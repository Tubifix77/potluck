"""M17: the hub announces itself on the house network by multicast DNS, so Home Assistant finds it with nothing typed.

A minimal responder with the standard library, from RFC 6762 (Multicast DNS) and RFC 6763 (DNS-SD), as registered in
the ledger: queries and responses on UDP port 5353 at 224.0.0.251; a response has ID 0, QR and AA set; the responder
announces with at least two unsolicited responses one second apart and says goodbye with TTL 0; records with a host
name (SRV, A) live 120 s, the others 75 minutes. The cache-flush bit is never set -- leaving it clear only makes a
neighbour treat a record as shared, which is always allowed, while setting it on a shared record (the service PTR) is
forbidden.

One service: `_potluck._tcp.local.`, instance `Potluck <system> <cell>`, at the `--listen` port, with TXT
`txtvers=1 cell= system= api=1 tls=1 fp=` (HUB-API.md, "Announced on the network").
"""

from __future__ import annotations

import socket
import struct
import threading
import time

MDNS_ADDR, MDNS_PORT = "224.0.0.251", 5353
SERVICE = "_potluck._tcp.local."
TYPE_A, TYPE_PTR, TYPE_TXT, TYPE_SRV, TYPE_ANY = 1, 12, 16, 33, 255
CLASS_IN = 1
TTL_HOST, TTL_OTHER = 120, 75 * 60


def encode_name(name: str) -> bytes:
    out = b""
    for label in name.rstrip(".").split("."):
        b = label.encode("utf-8")
        if not 1 <= len(b) <= 63:
            raise ValueError(f"a DNS label of {len(b)} bytes: {label!r}")
        out += bytes([len(b)]) + b
    return out + b"\x00"


def decode_name(msg: bytes, at: int) -> tuple[str, int]:
    """(name, offset after it), following compression pointers."""
    labels, jumped, end, hops = [], False, at, 0
    while True:
        if at >= len(msg):
            raise ValueError("a name runs off the message")
        n = msg[at]
        if n == 0:
            at += 1
            break
        if n & 0xC0 == 0xC0:
            if at + 1 >= len(msg):
                raise ValueError("a pointer runs off the message")
            ptr = ((n & 0x3F) << 8) | msg[at + 1]
            if not jumped:
                end = at + 2
            jumped = True
            hops += 1
            if hops > 16:
                raise ValueError("a pointer loop")
            at = ptr
            continue
        labels.append(msg[at + 1:at + 1 + n].decode("utf-8", "replace"))
        at += 1 + n
    return ".".join(labels) + ".", (end if jumped else at)


def rr(name: str, rtype: int, ttl: int, rdata: bytes) -> bytes:
    return encode_name(name) + struct.pack("!HHIH", rtype, CLASS_IN, ttl, len(rdata)) + rdata


def txt_rdata(items: dict[str, str]) -> bytes:
    out = b""
    for k, v in items.items():
        s = f"{k}={v}".encode("utf-8")
        if len(s) > 255:
            raise ValueError(f"a TXT string of {len(s)} bytes")
        out += bytes([len(s)]) + s
    return out


class Announcer:
    def __init__(self, system: str, cell: str, port: int, fp: str, ip: str, hostname: str | None = None) -> None:
        self.instance = f"Potluck {system} {cell}.{SERVICE}"
        self.host = (hostname or f"potluck-hub-{cell}") + ".local."
        self.port, self.ip = port, ip
        self.txt = {"txtvers": "1", "cell": cell, "system": system, "api": "1", "tls": "1", "fp": fp}
        self.sock: socket.socket | None = None
        self.stop_evt = threading.Event()
        self.counts = {"queries": 0, "answers": 0}

    # ---- records ----
    def records(self, ttl_scale: int = 1, ttl_max: int | None = None) -> dict[int, bytes]:
        def ttl(t: int) -> int:
            t *= ttl_scale
            return min(t, ttl_max) if ttl_max is not None else t

        srv = struct.pack("!HHH", 0, 0, self.port) + encode_name(self.host)
        return {
            TYPE_PTR: rr(SERVICE, TYPE_PTR, ttl(TTL_OTHER), encode_name(self.instance)),
            TYPE_SRV: rr(self.instance, TYPE_SRV, ttl(TTL_HOST), srv),
            TYPE_TXT: rr(self.instance, TYPE_TXT, ttl(TTL_OTHER), txt_rdata(self.txt)),
            TYPE_A: rr(self.host, TYPE_A, ttl(TTL_HOST), socket.inet_aton(self.ip)),
        }

    def response(self, answers: list[bytes], additionals: list[bytes] = ()) -> bytes:
        hdr = struct.pack("!HHHHHH", 0, 0x8400, 0, len(answers), 0, len(additionals))  # ID 0; QR and AA
        return hdr + b"".join(answers) + b"".join(additionals)

    def full(self, ttl_scale: int = 1) -> bytes:
        r = self.records(ttl_scale)
        return self.response([r[TYPE_PTR]], [r[TYPE_SRV], r[TYPE_TXT], r[TYPE_A]])

    def answer(self, query: bytes, legacy: bool = False) -> bytes | None:
        """The response to one query, or None if it asks nothing of ours. `legacy`: the query came from a port other
        than 5353 -- a one-shot resolver (RFC 6762 6.7): answered by unicast with its ID, its question, TTLs of 10 s
        at most."""
        if len(query) < 12:
            return None
        qid, flags, qd, _an, _ns, _ar = struct.unpack("!HHHHHH", query[:12])
        if flags & 0x8000:
            return None  # a response, not a query
        at, wanted = 12, set()
        for _ in range(qd):
            name, at = decode_name(query, at)
            if at + 4 > len(query):
                return None
            qtype, _qclass = struct.unpack("!HH", query[at:at + 4])
            at += 4
            n = name.lower()
            if n == SERVICE.lower() and qtype in (TYPE_PTR, TYPE_ANY):
                wanted.add(TYPE_PTR)
            elif n == self.instance.lower() and qtype in (TYPE_SRV, TYPE_TXT, TYPE_ANY):
                wanted.update({TYPE_SRV, TYPE_TXT} if qtype == TYPE_ANY else {qtype})
            elif n == self.host.lower() and qtype in (TYPE_A, TYPE_ANY):
                wanted.add(TYPE_A)
        if not wanted:
            return None
        r = self.records(ttl_max=10 if legacy else None)
        answers = [r[t] for t in (TYPE_PTR, TYPE_SRV, TYPE_TXT, TYPE_A) if t in wanted]
        extra = [r[t] for t in (TYPE_SRV, TYPE_TXT, TYPE_A) if t not in wanted and TYPE_PTR in wanted]
        if not legacy:
            return self.response(answers, extra)
        question = query[12:at]
        hdr = struct.pack("!HHHHHH", qid, 0x8400, qd, len(answers), 0, len(extra))
        return hdr + question + b"".join(answers) + b"".join(extra)

    # ---- the socket ----
    def start(self) -> None:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", MDNS_PORT))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     socket.inet_aton(MDNS_ADDR) + socket.inet_aton(self.ip))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(self.ip))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        s.settimeout(1.0)
        self.sock = s
        threading.Thread(target=self._serve, name="potluck-mdns", daemon=True).start()

    def _send(self, data: bytes) -> None:
        try:
            self.sock.sendto(data, (MDNS_ADDR, MDNS_PORT))
        except OSError:
            pass

    def _serve(self) -> None:
        for _ in range(2):  # RFC 6762 8.3: at least two unsolicited responses, one second apart
            self._send(self.full())
            if self.stop_evt.wait(1.0):
                return
        while not self.stop_evt.is_set():
            try:
                data, addr = self.sock.recvfrom(9000)
            except socket.timeout:
                continue
            except OSError:
                return
            legacy = addr[1] != MDNS_PORT
            try:
                resp = self.answer(data, legacy=legacy)
            except ValueError:
                continue
            self.counts["queries"] += 1
            if resp is not None:
                self.counts["answers"] += 1
                if legacy:
                    try:
                        self.sock.sendto(resp, addr)  # straight back to a one-shot resolver (RFC 6762 6.7)
                    except OSError:
                        pass
                else:
                    self._send(resp)

    def stop(self) -> None:
        if self.sock is None:
            return
        self._send(self.full(ttl_scale=0))  # goodbye: TTL 0 (RFC 6762 10.1)
        self.stop_evt.set()
        time.sleep(0.05)
        self.sock.close()
        self.sock = None


def lan_ip(towards: str = "192.168.0.1") -> str:
    """This PC's address on the network that reaches `towards` (no packet is sent: UDP connect only picks a route)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((towards, 9))
        return s.getsockname()[0]
    finally:
        s.close()
