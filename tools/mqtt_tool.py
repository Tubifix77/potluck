#!/usr/bin/env python3
"""A minimal MQTT 3.1.1 client for the bench (M13): what Home Assistant does on the wire, by hand.

    python mqtt_tool.py --host H sub '<filter>' [--seconds N] [--out file.jsonl]
    python mqtt_tool.py --host H pub <topic> <payload> [--qos 0|1]

`sub` prints (and appends to --out) one JSON line per message: {"t": seconds since start, "topic", "payload"}.
`pub` sends one message and disconnects cleanly -- an automation's command. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time


def enc_len(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def s(b: bytes) -> bytes:
    return struct.pack("!H", len(b)) + b


def packet(h: int, body: bytes) -> bytes:
    return bytes([h]) + enc_len(len(body)) + body


def read_packet(sock: socket.socket) -> tuple[int, bytes]:
    def rx(n: int) -> bytes:
        b = b""
        while len(b) < n:
            c = sock.recv(n - len(b))
            if not c:
                raise ConnectionError("closed")
            b += c
        return b
    h = rx(1)[0]
    mult, n = 1, 0
    while True:
        b = rx(1)[0]
        n += (b & 0x7F) * mult
        if not b & 0x80:
            break
        mult *= 128
    return h, rx(n)


def connect(host: str, port: int, cid: str, keepalive: int = 30, user: str | None = None,
            password: str | None = None) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=10)
    flags = 0x02 | (0x80 if user else 0) | (0x40 if password else 0)
    body = s(b"MQTT") + bytes([4, flags]) + struct.pack("!H", keepalive) + s(cid.encode())
    if user:
        body += s(user.encode())
    if password:
        body += s(password.encode())
    sock.sendall(packet(0x10, body))
    h, b = read_packet(sock)
    if h >> 4 != 2 or b[1] != 0:
        raise ConnectionError(f"CONNACK refused: {b[1] if len(b) > 1 else '?'}")
    return sock


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--user")
    ap.add_argument("--password-file")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_sub = sub.add_parser("sub")
    p_sub.add_argument("filter")
    p_sub.add_argument("--seconds", type=float, default=0)
    p_sub.add_argument("--out")
    p_pub = sub.add_parser("pub")
    p_pub.add_argument("topic")
    p_pub.add_argument("payload")
    p_pub.add_argument("--qos", type=int, default=0)
    a = ap.parse_args()
    pw = open(a.password_file, encoding="utf-8").read().strip() if a.password_file else None
    sock = connect(a.host, a.port, f"bench-{a.cmd}-{int(time.time())}", user=a.user, password=pw)
    if a.cmd == "pub":
        body = s(a.topic.encode()) + (struct.pack("!H", 1) if a.qos else b"") + a.payload.encode()
        sock.sendall(packet(0x30 | (a.qos << 1), body))
        if a.qos:
            read_packet(sock)
        sock.sendall(b"\xe0\x00")
        sock.close()
        return 0
    sock.sendall(packet(0x82, struct.pack("!H", 1) + s(a.filter.encode()) + b"\x00"))
    read_packet(sock)  # SUBACK
    out = open(a.out, "a", encoding="utf-8") if a.out else None
    t0 = time.time()
    end = t0 + a.seconds if a.seconds else None
    last_ping = t0
    sock.settimeout(1.0)
    while end is None or time.time() < end:
        if time.time() - last_ping > 15:
            sock.sendall(b"\xc0\x00")
            last_ping = time.time()
        try:
            h, b = read_packet(sock)
        except socket.timeout:
            continue
        if h >> 4 != 3:
            continue
        (n,) = struct.unpack_from("!H", b, 0)
        topic = b[2:2 + n].decode("utf-8", "replace")
        at = 2 + n + (2 if (h >> 1) & 3 else 0)
        line = json.dumps({"t": round(time.time() - t0, 3), "wall": round(time.time(), 3), "topic": topic,
                           "payload": b[at:].decode("utf-8", "replace")})
        print(line, flush=True)
        if out:
            out.write(line + "\n")
            out.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
