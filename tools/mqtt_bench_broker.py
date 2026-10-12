#!/usr/bin/env python3
"""A small MQTT 3.1.1 broker for the bench -- a measuring instrument, not a product (M13).

    python3 mqtt_bench_broker.py [--port 1883] [--log broker.jsonl] [--user U --password-file F]

Stands in for the owner's broker (Mosquitto, Home Assistant's add-on) where none is installed, and records every
message it carries as one JSON line with the broker's own clock -- the evidence M13's acceptance needs: what the
presenter published (discovery configs, states, events), when a presenter's will fired, when the next presenter
first published. Standard library only.

What it does, from the MQTT 3.1.1 standard (OASIS, 2014): CONNECT/CONNACK (protocol level 4), PUBLISH at QoS 0 and
1 (PUBACK), SUBSCRIBE/SUBACK with '+' and '#' wildcards (granted QoS at most 1), UNSUBSCRIBE, PINGREQ/PINGRESP,
DISCONNECT, retained messages, the will message (sent when a connection ends without DISCONNECT, or when nothing
arrives for 1.5 x its keep-alive -- sections 3.1.2.5 and 3.1.2.10), a new connection with the same client id
replacing the old one. Not: QoS 2 (refused at SUBSCRIBE by granting 1; a QoS 2 PUBLISH is dropped and logged),
persistent sessions (every session is treated as clean), TLS.

Log lines: {"t": seconds, "ev": "connect"|"disconnect"|"will"|"publish"|"subscribe", ...}.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import threading
import time

LOCK = threading.Lock()
CLIENTS: dict[str, "Client"] = {}
RETAINED: dict[str, tuple[bytes, int]] = {}
T0 = time.time()
LOG = None
AUTH: tuple[str, str] | None = None


def log(ev: str, **kw) -> None:
    kw = {"t": round(time.time() - T0, 3), "wall": round(time.time(), 3), "ev": ev, **kw}
    line = json.dumps(kw, ensure_ascii=True)
    with LOCK:
        if LOG is not None:
            LOG.write(line + "\n")
            LOG.flush()
    print(line, flush=True)


def text(b: bytes) -> str:
    try:
        return b.decode("utf-8")
    except UnicodeDecodeError:
        return "hex:" + b.hex()


def matches(filt: str, topic: str) -> bool:
    f, t = filt.split("/"), topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t):
            return False
        if part != "+" and part != t[i]:
            return False
    return len(f) == len(t)


def enc_len(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def enc_str(s: bytes) -> bytes:
    return struct.pack("!H", len(s)) + s


class Client:
    def __init__(self, sock: socket.socket, addr) -> None:
        self.sock, self.addr = sock, addr
        self.id = ""
        self.subs: dict[str, int] = {}
        self.will: tuple[str, bytes, int, bool] | None = None
        self.keepalive = 0
        self.last_rx = time.time()
        self.alive = True
        self.wlock = threading.Lock()
        self.next_mid = 1

    def send(self, data: bytes) -> None:
        try:
            with self.wlock:
                self.sock.sendall(data)
        except OSError:
            self.alive = False

    def deliver(self, topic: str, payload: bytes, qos: int, retain: bool) -> None:
        q = min(qos, max((v for f, v in self.subs.items() if matches(f, topic)), default=-1))
        if q < 0:
            return
        hdr = 0x30 | (q << 1) | (1 if retain else 0)
        body = enc_str(topic.encode())
        if q:
            body += struct.pack("!H", self.next_mid)
            self.next_mid = self.next_mid % 65535 + 1
        body += payload
        self.send(bytes([hdr]) + enc_len(len(body)) + body)


def route(topic: str, payload: bytes, qos: int, retain: bool, src: str) -> None:
    log("publish", src=src, topic=topic, qos=qos, retain=retain, payload=text(payload))
    with LOCK:
        if retain:
            if payload:
                RETAINED[topic] = (payload, qos)
            else:
                RETAINED.pop(topic, None)
        targets = list(CLIENTS.values())
    for c in targets:
        c.deliver(topic, payload, qos, False)


def end(c: Client, clean: bool, why: str) -> None:
    with LOCK:
        if CLIENTS.get(c.id) is c:
            del CLIENTS[c.id]
        was = c.alive
        c.alive = False
    try:
        c.sock.close()
    except OSError:
        pass
    if not was and clean:
        return
    log("disconnect", client=c.id, clean=clean, why=why)
    if not clean and c.will is not None:
        topic, payload, qos, retain = c.will
        log("will", client=c.id, topic=topic, payload=text(payload))
        route(topic, payload, qos, retain, c.id + " (will)")


def recv_exact(sock: socket.socket, n: int) -> bytes:
    b = b""
    while len(b) < n:
        chunk = sock.recv(n - len(b))
        if not chunk:
            raise ConnectionError("closed")
        b += chunk
    return b


def read_packet(sock: socket.socket) -> tuple[int, bytes]:
    h = recv_exact(sock, 1)[0]
    mult, n = 1, 0
    while True:
        b = recv_exact(sock, 1)[0]
        n += (b & 0x7F) * mult
        if not b & 0x80:
            break
        mult *= 128
        if mult > 128 ** 3:
            raise ConnectionError("bad remaining length")
    return h, recv_exact(sock, n)


def take_str(b: bytes, at: int) -> tuple[bytes, int]:
    (n,) = struct.unpack_from("!H", b, at)
    return b[at + 2:at + 2 + n], at + 2 + n


def serve(c: Client) -> None:
    try:
        h, body = read_packet(c.sock)
        if h >> 4 != 1:
            raise ConnectionError("first packet is not CONNECT")
        name, at = take_str(body, 0)
        level, flags = body[at], body[at + 1]
        (c.keepalive,) = struct.unpack_from("!H", body, at + 2)
        at += 4
        cid, at = take_str(body, at)
        c.id = text(cid) or f"anon-{c.addr[0]}:{c.addr[1]}"
        if flags & 0x04:
            wt, at = take_str(body, at)
            wp, at = take_str(body, at)
            c.will = (text(wt), wp, (flags >> 3) & 3, bool(flags & 0x20))
        user = pw = None
        if flags & 0x80:
            u, at = take_str(body, at)
            user = text(u)
        if flags & 0x40:
            p, at = take_str(body, at)
            pw = text(p)
        if name != b"MQTT" or level != 4:
            c.send(b"\x20\x02\x00\x01")
            raise ConnectionError("unacceptable protocol level")
        if AUTH is not None and (user, pw) != AUTH:
            c.send(b"\x20\x02\x00\x05")
            log("connect", client=c.id, addr=c.addr[0], refused="not authorized")
            raise ConnectionError("not authorized")
        with LOCK:
            old = CLIENTS.get(c.id)
            CLIENTS[c.id] = c
        if old is not None:
            end(old, False, "replaced by a new connection with the same client id")
        c.send(b"\x20\x02\x00\x00")
        log("connect", client=c.id, addr=c.addr[0], keepalive=c.keepalive,
            will=None if c.will is None else {"topic": c.will[0], "payload": text(c.will[1])})
        while c.alive:
            h, body = read_packet(c.sock)
            c.last_rx = time.time()
            kind = h >> 4
            if kind == 3:  # PUBLISH
                qos, retain = (h >> 1) & 3, bool(h & 1)
                topic, at = take_str(body, 0)
                mid = None
                if qos:
                    (mid,) = struct.unpack_from("!H", body, at)
                    at += 2
                if qos == 2:
                    log("publish", src=c.id, topic=text(topic), dropped="QoS 2 is not supported here")
                    continue
                if qos == 1:
                    c.send(b"\x40\x02" + struct.pack("!H", mid))
                route(text(topic), body[at:], qos, retain, c.id)
            elif kind == 8:  # SUBSCRIBE
                (mid,) = struct.unpack_from("!H", body, 0)
                at, granted, new = 2, b"", []
                while at < len(body):
                    f, at = take_str(body, at)
                    q = min(body[at], 1)
                    at += 1
                    c.subs[text(f)] = q
                    granted += bytes([q])
                    new.append(text(f))
                c.send(bytes([0x90]) + enc_len(2 + len(granted)) + struct.pack("!H", mid) + granted)
                log("subscribe", client=c.id, filters=new)
                with LOCK:
                    ret = list(RETAINED.items())
                for t, (p, q) in ret:
                    if any(matches(f, t) for f in new):
                        c.deliver(t, p, q, True)
            elif kind == 10:  # UNSUBSCRIBE
                (mid,) = struct.unpack_from("!H", body, 0)
                at = 2
                while at < len(body):
                    f, at = take_str(body, at)
                    c.subs.pop(text(f), None)
                c.send(b"\xb0\x02" + struct.pack("!H", mid))
            elif kind == 12:  # PINGREQ
                c.send(b"\xd0\x00")
            elif kind == 14:  # DISCONNECT
                c.will = None
                end(c, True, "DISCONNECT")
                return
            elif kind == 4:  # PUBACK from a client: nothing kept to release
                pass
    except (ConnectionError, OSError, struct.error, IndexError) as exc:
        end(c, False, str(exc) or exc.__class__.__name__)


def watchdog() -> None:
    while True:
        time.sleep(0.25)
        now = time.time()
        with LOCK:
            cs = list(CLIENTS.values())
        for c in cs:
            if c.keepalive and now - c.last_rx > 1.5 * c.keepalive:
                end(c, False, f"no packet for 1.5 x keep-alive ({c.keepalive} s)")


def main() -> int:
    global LOG, AUTH
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--log", default="broker.jsonl")
    ap.add_argument("--user")
    ap.add_argument("--password-file")
    a = ap.parse_args()
    LOG = open(a.log, "a", encoding="utf-8")
    if a.user:
        with open(a.password_file, encoding="utf-8") as f:
            AUTH = (a.user, f.read().strip())
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", a.port))
    srv.listen(16)
    log("listening", port=a.port, auth=AUTH is not None)
    threading.Thread(target=watchdog, daemon=True).start()
    while True:
        s, addr = srv.accept()
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=serve, args=(Client(s, addr),), daemon=True).start()


if __name__ == "__main__":
    raise SystemExit(main())
