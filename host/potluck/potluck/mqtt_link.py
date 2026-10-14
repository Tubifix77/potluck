"""M14: the MQTT transport for the presenting role when the PC's node holds it (host/native/host_present.hpp).

pot_hostnode builds everything that is published -- the same present_core and namespace as a board -- and says what to
do on stderr: start a connection (S), publish (P), say goodbye and close (X). This carries it out with the standard
library (MQTT 3.1.1, QoS 0, nothing retained -- section 13), and hands back what only the connection knows: connected or
not, Home Assistant's birth message, and the commands on the cell's set topics.

The connection is the board's (pot_present/present.cpp): a will on the presenter's own availability topic (QoS 1, not
retained), the keep-alive the package names, reconnecting every 3 s, TLS against the CA the package pins when it says
TLS -- the broker's certificate checked against its IP address, as the boards' mbedTLS does. One thread owns the socket
(reads and writes), since a TLS socket is not safe to read in one thread while another writes.
"""

from __future__ import annotations

import base64
import socket
import ssl
import struct
import threading
import time
from typing import Callable

RECONNECT_S = 3.0
PINGREQ = bytes([0xC0, 0x00])
DISCONNECT = bytes([0xE0, 0x00])


def _len(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def _s(b: bytes) -> bytes:
    return struct.pack("!H", len(b)) + b


def _packet(h: int, body: bytes) -> bytes:
    return bytes([h]) + _len(len(body)) + body


def connect_packet(client_id: str, keepalive: int, will_topic: str, will: bytes, user: str | None = None,
                   password: str | None = None) -> bytes:
    flags = 0x02 | 0x04 | (1 << 3)  # clean session, a will, will QoS 1, not retained
    if user:
        flags |= 0x80 | (0x40 if password else 0)  # MQTT 3.1.1: user name, then password, after the will
    body = _s(b"MQTT") + bytes([4, flags]) + struct.pack("!H", keepalive)
    body += _s(client_id.encode()) + _s(will_topic.encode()) + _s(will)
    if user:
        body += _s(user.encode()) + (_s(password.encode()) if password else b"")
    return _packet(0x10, body)


def read_login(path: str) -> tuple[str, str]:
    """A broker login from a file of two lines -- user name, then password -- as tools/wifi_set.py reads Wi-Fi. The
    caller never prints either."""
    with open(path, encoding="utf-8-sig") as f:
        lines = [line.rstrip("\r\n") for line in f if line.strip("\r\n")]
    if len(lines) != 2:
        raise ValueError(f"{path}: two lines expected (the broker's user name, then its password)")
    return lines[0], lines[1]


def publish_packet(topic: str, payload: bytes) -> bytes:
    return _packet(0x30, _s(topic.encode()) + payload)  # QoS 0, not retained


def subscribe_packet(msg_id: int, filters: list[str]) -> bytes:
    body = struct.pack("!H", msg_id) + b"".join(_s(f.encode()) + b"\x00" for f in filters)
    return _packet(0x82, body)


def take_packet(buf: bytes) -> tuple[int, bytes, int] | None:
    """(header byte, body, bytes used) of the first whole packet in `buf`, or None while it is incomplete."""
    if len(buf) < 2:
        return None
    mult, n, i = 1, 0, 1
    while True:
        if i >= len(buf):
            return None
        b = buf[i]
        n += (b & 0x7F) * mult
        i += 1
        if not b & 0x80:
            break
        mult *= 128
        if i > 4:
            raise ConnectionError("a malformed packet length")
    if len(buf) < i + n:
        return None
    return buf[0], buf[i:i + n], i + n


class PresenterLink:
    """One holding of the role at a time. `control(text)` reaches pot_hostnode (a control record on its stdin)."""

    def __init__(self, control: Callable[[str], None], log: Callable[[str], None] = lambda m: None) -> None:
        self.control = control
        self.log = log
        self.lock = threading.Lock()
        self.cfg: dict | None = None
        self.connected = False
        self.generation = 0
        self.outq: list[bytes] = []
        self.goodbye: bytes | None = None
        self.counts = {"connects": 0, "published": 0, "publish_fail": 0, "commands": 0}

    # ---- from pot_hostnode ----
    def start(self, cfg: dict) -> None:
        self.stop()
        with self.lock:
            self.generation += 1
            self.cfg = cfg
            self.outq = []
            gen = self.generation
        threading.Thread(target=self._run, args=(gen,), name="potluck-present-mqtt", daemon=True).start()

    def publish(self, topic: str, payload: bytes) -> None:
        with self.lock:
            if not self.connected:
                return
            if len(self.outq) > 2000:  # a broker that stopped reading: drop, counted, as a board's full buffer does
                self.counts["publish_fail"] += 1
                return
            self.outq.append(publish_packet(topic, payload))

    def stop(self) -> None:
        """The role handed on: the connection thread says goodbye (its own availability offline, then a clean
        DISCONNECT, which suppresses the will -- as a board does) and ends."""
        with self.lock:
            cfg = self.cfg
            if cfg is None:
                return
            self.goodbye = publish_packet(cfg["will_topic"], base64.b64decode(cfg["will_b64"]))
            self.cfg = None
            self.generation += 1

    # ---- the connection ----
    def _open(self, cfg: dict):
        raw = socket.create_connection((cfg["host"], int(cfg["port"])), timeout=5)
        if not cfg.get("tls"):
            return raw
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        ctx.load_verify_locations(cadata=base64.b64decode(cfg["ca_b64"]))  # DER, the package's CA
        return ctx.wrap_socket(raw, server_hostname=cfg["host"])  # checked against the certificate's IP address

    def _run(self, gen: int) -> None:
        with self.lock:
            cfg = self.cfg
        while cfg is not None:
            sock = None
            try:
                sock = self._open(cfg)
                sock.sendall(connect_packet(cfg["client_id"], int(cfg["keepalive_s"]), cfg["will_topic"],
                                            base64.b64decode(cfg["will_b64"]), cfg.get("user"), cfg.get("pass")))
                sock.settimeout(5)
                buf = b""
                while (pkt := take_packet(buf)) is None:
                    chunk = sock.recv(4096)
                    if not chunk:
                        raise ConnectionError("closed by the broker")
                    buf += chunk
                h, body, used = pkt
                if h != 0x20 or len(body) < 2 or body[1] != 0:
                    raise ConnectionError(f"CONNACK refused ({body[1] if len(body) > 1 else '?'})")
                sock.sendall(subscribe_packet(1, [cfg["cmd_filter"], cfg["status_topic"]]))
                with self.lock:
                    if gen != self.generation:
                        self._bye(sock)
                        return
                    self.connected = True
                    self.counts["connects"] += 1
                self.log(f"presenting: connected to {cfg['host']}:{cfg['port']}{' (TLS)' if cfg.get('tls') else ''}")
                self.control("connected 1")
                self._serve(sock, cfg, gen, buf[used:])
            except (OSError, ConnectionError, ssl.SSLError, ValueError) as exc:
                self.log(f"presenting: {exc}")
            with self.lock:
                was, self.connected = self.connected, False
                ended = gen != self.generation
            if was:
                self.control("connected 0")
            if ended:
                return
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
            time.sleep(RECONNECT_S)
            with self.lock:
                cfg = self.cfg if gen == self.generation else None

    def _bye(self, sock) -> None:
        with self.lock:
            goodbye, self.goodbye = self.goodbye, None
        try:
            if goodbye is not None:
                sock.sendall(goodbye)
                time.sleep(0.1)
            sock.sendall(DISCONNECT)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass

    def _serve(self, sock, cfg: dict, gen: int, buf: bytes) -> None:
        keep = max(5, int(cfg["keepalive_s"]))
        sock.settimeout(0.05)
        last_ping = last_rx = time.monotonic()
        prefix = cfg["cmd_filter"][:-len("+/set")]  # potluck/<cell>/
        while True:
            with self.lock:
                ended = gen != self.generation
                out, self.outq = self.outq, []
            if ended:
                self._bye(sock)
                return
            for pkt in out:
                sock.sendall(pkt)
                self.counts["published"] += 1
            if time.monotonic() - last_ping > keep / 2:
                sock.sendall(PINGREQ)
                last_ping = time.monotonic()
            if time.monotonic() - last_rx > 1.5 * keep:
                raise ConnectionError("the broker went quiet")
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    raise ConnectionError("closed by the broker")
                buf += chunk
                last_rx = time.monotonic()
            except (socket.timeout, ssl.SSLWantReadError):
                pass
            while (pkt := take_packet(buf)) is not None:
                h, body, used = pkt
                buf = buf[used:]
                self._on_packet(h, body, cfg, prefix)

    def _on_packet(self, h: int, body: bytes, cfg: dict, prefix: str) -> None:
        if h >> 4 != 3:
            return  # SUBACK, PINGRESP
        tl = struct.unpack("!H", body[:2])[0]
        topic = body[2:2 + tl].decode("utf-8", "replace")
        at = 2 + tl + (2 if (h >> 1) & 3 else 0)
        payload = body[at:]
        if topic == cfg["status_topic"]:
            if payload == b"online":
                self.control("discovery")  # Home Assistant (re)started: the configs again
            return
        if topic.startswith(prefix) and topic.endswith("/set"):
            h8 = topic[len(prefix):-4]
            if len(h8) == 8 and all(c in "0123456789abcdef" for c in h8) and 0 < len(payload) < 24:
                self.counts["commands"] += 1
                self.control(f"cmd {h8} {base64.b64encode(payload).decode()}")


def reachable(host: str, port: int, timeout: float = 2.0) -> bool:
    """Whether the PC can open a connection to the broker: HELLO's connected bit for the PC's node."""
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False
