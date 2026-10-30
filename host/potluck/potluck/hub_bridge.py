"""The hub's node port -- members from the house network (2026-10-30, a tablet; host/potluck/HUB-API.md).

A node that has no radio of the cell's -- an Android tablet on the house Wi-Fi -- joins through the hub: one TCP
connection to the node port, carrying Potluck Frames exactly as the cable does (write_serial_frame: COBS over the
frame and its CRC, then 0x00). The hub forwards them to the cabled board and back, byte for byte. It is trusted with
nothing: the cabled board gives such a node an address outside the cable's trusted family (02:00:00:01:<id>,
m0_main.cpp's kBridgedFirst..kBridgedLast), so the node signs its HELLO and tags its frames as a radio board does,
and every member checks them.

Before the frames, a login (the hub's own door, so a stranger cannot hold a node's place or load the cable):

    hub  -> node   potluck-node 1 <nonce: 64 hex>\\n
    node -> hub    <certificate: 224 hex> <signature: 128 hex>\\n        (within 10 s, one line)
    hub  -> node   ok <node id: 4 hex> <cabled board: 4 hex> <its MAC: 12 hex>\\n   then frames both ways
                   | refused <why>\\n and the hub closes

The certificate is the node's own (potluck.enrol --host-cert), from the cell's CA; the signature is the node's key over
LOGIN_DOMAIN || nonce || certificate. The cabled board's MAC is what that board signs its HELLO over
(pot/hello_auth.hpp): the node hands the board's frames to its Node from that address, or the board's HELLO does not
verify -- the hub learns it from the board's NODE_STATUS (version 3), and refuses a login until it has. Only the
cluster file's `bridge.nodes` may log in, one connection each: a
second login for the same node replaces the first (a tablet whose Wi-Fi dropped comes back before the old connection
times out). After `ok` every frame's source must be that node: any other is dropped, and so are frames over its rate.
"""

from __future__ import annotations

import os
import socket
import socketserver
import threading
import time
from typing import Callable

from . import ed25519_ref as ed
from .enrol import CERT_LEN, EnrolError, verify_cert
from .serial_framing import SerialReassembler

#: The ids a bridged node may have (m0_main.cpp's kBridgedFirst..kBridgedLast: hosts' ids, each given an untrusted
#: address of its own on the cable), how many one hub takes, and its port unless the cluster file says.
BRIDGED_FIRST, BRIDGED_LAST = 0x00F0, 0x00FD
BRIDGE_MAX = 4
BRIDGE_PORT = 8472  # beside the API's 8470 and the local relay's 8471

LOGIN_DOMAIN = b"potluck node port v1\x00"
LOGIN_TIMEOUT_S = 10.0
LINE_MAX = 600
#: A node's frames a second onto the cable, and its burst: a board beats 4 a second and answers what it is asked;
#: the cable (921600 baud) carries the whole cell, so one node may not fill it.
RATE_PER_S = 100.0
RATE_BURST = 200.0


def login_message(nonce: bytes, cert: bytes) -> bytes:
    return LOGIN_DOMAIN + nonce + cert


def check_login(line: bytes, nonce: bytes, ca_public: bytes, allowed: dict[int, str]) -> tuple[int | None, str]:
    """(node id, "") for a good answer to the challenge, else (None, why)."""
    parts = line.strip().split()
    if len(parts) != 2:
        return None, "expected '<certificate hex> <signature hex>'"
    try:
        cert, sig = bytes.fromhex(parts[0].decode("ascii")), bytes.fromhex(parts[1].decode("ascii"))
    except (ValueError, UnicodeDecodeError):
        return None, "not hex"
    if len(cert) != CERT_LEN or len(sig) != 64:
        return None, f"a certificate is {CERT_LEN} bytes and a signature 64"
    try:
        c = verify_cert(cert, ca_public)
    except EnrolError as exc:
        return None, str(exc)
    if c.node_id not in allowed:
        return None, f"node {c.node_id:04x} is not in this cell's bridge.nodes"
    if not ed.verify(c.node_pub, login_message(nonce, cert), sig):
        return None, "the signature does not verify with the certificate's key"
    return c.node_id, ""


class Bridged:
    """One logged-in node on the node port."""

    def __init__(self, sock: socket.socket, node_id: int, address: str, client_cls) -> None:
        self.node_id = node_id
        self.address = address
        self.client = client_cls(sock, f"node-{node_id:04x}@{address}")
        self.since_at = int(time.time() * 1000)
        self.epoch: int | None = None
        self.tokens = RATE_BURST
        self.t = time.monotonic()
        self.dropped = {"wrong_src": 0, "rate": 0}

    def send_raw(self, raw: bytes) -> None:
        self.client.send_raw(raw)

    def take(self) -> bool:
        now = time.monotonic()
        self.tokens = min(RATE_BURST, self.tokens + (now - self.t) * RATE_PER_S)
        self.t = now
        if self.tokens < 1.0:
            self.dropped["rate"] += 1
            return False
        self.tokens -= 1.0
        return True

    def close(self) -> None:
        self.client.alive = False
        try:
            self.client.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def _readline(sock: socket.socket, deadline: float) -> bytes | None:
    buf = b""
    while b"\n" not in buf:
        left = deadline - time.monotonic()
        if left <= 0 or len(buf) > LINE_MAX:
            return None
        sock.settimeout(left)
        try:
            chunk = sock.recv(LINE_MAX)
        except (socket.timeout, OSError):
            return None
        if not chunk:
            return None
        buf += chunk
    line, _, rest = buf.partition(b"\n")
    return line if not rest else None  # frames before "ok" are a client that did not wait: refused


def serve_node_port(hub, addr: tuple[str, int], ca_public: bytes, allowed: dict[int, str],
                    record: Callable[[str, dict], None],
                    cable_end: Callable[[], tuple[int, bytes] | None]) -> socketserver.ThreadingTCPServer:
    """The node port, on its own threads. `hub` is potluck.hub.Hub (attach_bridged / drop_bridged / from_bridged);
    `cable_end` the cabled board's (id, MAC) or None while not known (potluck.hub.Model.cable_end)."""
    from .hub import _Client

    class Handler(socketserver.BaseRequestHandler):
        def handle(self) -> None:
            sock = self.request
            address = self.client_address[0]
            nonce = os.urandom(32)
            try:
                sock.sendall(b"potluck-node 1 " + nonce.hex().encode("ascii") + b"\n")
            except OSError:
                return
            line = _readline(sock, time.monotonic() + LOGIN_TIMEOUT_S)
            node, why = (None, "no answer to the challenge") if line is None else \
                check_login(line, nonce, ca_public, allowed)
            end = cable_end() if node is not None else None
            if node is not None and end is None:
                node, why = None, "the cabled board's address is not known yet (its firmware, or the hub just started)"
            if node is None:
                record("bridged_refused", {"from": address, "why": why})
                try:
                    sock.sendall(b"refused " + why.encode("ascii", "replace")[:200] + b"\n")
                except OSError:
                    pass
                return
            try:
                sock.sendall(b"ok %04x %04x %s\n" % (node, end[0], end[1].hex().encode("ascii")))
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                return
            b = Bridged(sock, node, address, _Client)
            hub.attach_bridged(b)
            ra = SerialReassembler()
            try:
                while b.client.alive:
                    try:
                        data = sock.recv(4096)
                    except socket.timeout:
                        continue  # a quiet node (the timeout is for sends to it)
                    if not data:
                        break
                    for raw in ra.feed(data):
                        hub.from_bridged(b, raw)
            except OSError:
                pass
            finally:
                hub.drop_bridged(b)

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = False  # the relay's 127.0.0.1 port must never be shared by accident
        daemon_threads = True

    srv = Server(addr, Handler)
    threading.Thread(target=srv.serve_forever, name="potluck-hub-node-port", daemon=True).start()
    return srv

