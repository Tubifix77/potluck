"""potluck-hub -- one program on the PC owns the link to the cell (ARCHITECTURE section 13, M14; section 7.1).

    python -m potluck.hub --port COM6 [--cluster keys/desk-m13.json] [--api 127.0.0.1:8470] [--relay 127.0.0.1:8471]

Until M14 every tool opened the serial port itself, so only one could run at a time: the PC could be a placement
host, or offer a service, or be watched, never two at once. The hub holds the port and is the PC's one identity in
the cell (node 0x00fe, as every tool's bridge was). Two things hang off it:

THE RELAY (`--relay`, default 127.0.0.1:8471). A tool's own Bridge connects here instead of to the port
(`Bridge.open(port=...)` does so by itself while a hub holds that port: hub_endpoint()). The hub answers the tool's
HELLO from what the cell told the hub, swallows the tool's heartbeats and probe answers (the hub heartbeats for the
PC), and passes every request on with a msg_id of its own, so two tools' requests never collide and each REPLY goes
back to the tool that asked. Requests the cell sends the PC (an agent's services, M8) go to every tool; the one that
offers the service answers.

THE PLACEMENT HOST (M10) is one client of a different kind: pot_hostnode is an enrolled node, so the PC's node in the
cell while it is attached (`python -m potluck.hostnode --hub`). It is told apart by its HELLO, which carries a boot
epoch (a tool's carries 0). Its frames go to the cable byte for byte -- its frames to other boards carry auth tags
over the header, so nothing in them may change -- and the hub stands back: no HELLO and no probe answers of its own,
which would read to the cabled board as another incarnation of the same node. msg_ids are split instead of
re-numbered: the host node numbers its requests 1..0x7FFF (NodeConfig::msg_id_max), the hub its own and its tools'
from 0x8000. Everything the cell sends the PC goes to the host node except the agent's CALLs and CASTs.

THE MODEL AND ITS API (`--api`, default 127.0.0.1:8470; host/potluck/HUB-API.md). Every pull period the hub asks each
board for its NODE_STATUS and reads the package's resources, and keeps one model: boards, apps, resources,
integrations, deploys, events, the four golden signals. Every viewer -- the dashboard (M16), Home Assistant's
integration (M17), the owner's programs (M18) -- reads that one model, so ten viewers cost the cell no more radio
than one. Look is every GET, and POST /api/interest (a program asks the hub to read, while it keeps asking, what the
package's reads leave out: per-link, sometimes-published and sys/ resources); write is POST /api/write, only what the
package lets the outside write, passed to the owning board marked from outside, which checks again; deploy is never
here.

Local only by default (127.0.0.1), with no login: the PC's own user. The house network and phones are `--listen`,
over TLS and behind a login only (M16, potluck.hubauth). `--record` keeps the model for `--replay` (potluck.hubreplay).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import socket
import socketserver
import struct
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from . import frame as fr
from .bridge import HOST_NODE_ID, Bridge, BridgeError, RequestTimeout
from .payloads import HELLO_FLAG_WANT_ACK, Hello
from .ns_payloads import CALL_FORWARD, Call, Reply, Write
from .paths import path_hash
from .serial_framing import SerialReassembler, write_serial_frame
from .transport import open_transport
from .value import NsError, Quality, Value, unit_suffix

API_VERSION = 1
DEFAULT_API = "127.0.0.1:8470"
DEFAULT_RELAY = "127.0.0.1:8471"
DASHBOARD_DIR = Path(__file__).resolve().parent / "dashboard"
# M20 (review item 11): the dashboard's Content-Security-Policy. Its own files only: no inline script, nothing from
# another site, not framed by another site; inline style attributes are allowed for the resource bars' widths (a style
# cannot run code). Checked in the browser against hub_mock on 2026-10-19: no violation on the dashboard or the wizard.
DASHBOARD_CSP = ("default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self'; "
                 "connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'")

#: Requests a tool sends that the cell answers with a REPLY correlated by msg_id: the hub re-numbers these.
_REQUEST_OPS = frozenset({fr.Op.READ, fr.Op.WRITE, fr.Op.CALL, fr.Op.DEPLOY_BEGIN, fr.Op.DEPLOY_CHUNK,
                          fr.Op.DEPLOY_COMMIT, fr.Op.DEPLOY_ABORT, fr.Op.FW_BEGIN, fr.Op.FW_CHUNK, fr.Op.FW_COMMIT,
                          fr.Op.FW_STATUS, fr.Op.NODE_STATUS, fr.Op.LEASE, fr.Op.TELEMETRY})


# ---- where a running hub can be found ---------------------------------------------------------------

def state_file() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/.cache")
    return Path(base) / "potluck" / "hub.json"


def hub_endpoint(port: str | None) -> str | None:
    """The relay's "host:port" if a live hub holds `port` (or any port, when None); else None. What lets
    `Bridge.open(port=...)` go through the hub without the tool knowing (M14: on demand, a tool needs nothing more)."""
    try:
        st = json.loads(state_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if port is not None and str(st.get("port", "")).upper() != str(port).upper():
        return None
    host, _, p = str(st.get("relay", "")).rpartition(":")
    try:
        with socket.create_connection((host or "127.0.0.1", int(p)), timeout=0.5):
            return st["relay"]
    except (OSError, ValueError):
        return None


# ---- the relay -------------------------------------------------------------------------------------------

class _Client:
    #: A tool that stops reading is dropped after this long rather than holding the hub's reader, which sends it
    #: the cell's frames (2026-10-21: a host node hung on the owner's PC and stopped reading its relay socket).
    SEND_TIMEOUT_S = 5.0

    def __init__(self, sock: socket.socket, name: str) -> None:
        self.sock = sock
        self.name = name
        self.lock = threading.Lock()
        self.alive = True
        sock.settimeout(self.SEND_TIMEOUT_S)  # the relay's recv loop reads on through a quiet tool's timeouts

    def send_raw(self, raw: bytes) -> None:
        if not self.alive:
            return
        wire = write_serial_frame(raw)
        with self.lock:
            try:
                self.sock.sendall(wire)
            except OSError:  # socket.timeout too: part of a frame may be out, so the stream is over either way
                self.alive = False
                try:
                    self.sock.shutdown(socket.SHUT_RDWR)  # its handler's recv ends, and drops it
                except OSError:
                    pass


@dataclass
class _Relayed:
    client: _Client
    msg_id: int
    at: float


class Hub(Bridge):
    """The PC's one bridge to the cell, relaying tools' frames and keeping the model."""

    RELAY_TTL_S = 30.0  # a relayed request with no REPLY after this is forgotten (deploy steps wait up to ~10 s)

    HUB_MSG_ID_BASE = 0x8000  # the hub's requests (its own and its tools'); a host node behind it keeps below

    def __init__(self, transport, **kw) -> None:
        super().__init__(transport, **kw)
        self.record = lambda kind, board, detail: None  # the model replaces it
        self._relayed: dict[int, _Relayed] = {}
        self._clients: list[_Client] = []
        self._clients_lock = threading.Lock()
        self.node_client: _Client | None = None  # M10's placement host, while attached
        self.services_for = lambda: None  # M23: the PC's named services (potluck.services.PcServices), from the model
        from .bulk import Bulk
        from .events import Inputs
        self.inputs = Inputs(self._call_app)  # M24: key and pointer events on their way to apps
        self.bulk = Bulk(self._call_app)  # M25: files the PC offers, on their way to the apps that asked
        self._node_window_s = 6.0  # its declared death window (HELLO), for the silence after it goes
        self._quiet_until = 0.0     # after it went: say nothing as 0x00fe until the cell has declared it dead
        self.relay_counts = {"clients": 0, "requests": 0, "replies": 0, "unmatched": 0, "fanned_out": 0,
                             "node_frames_out": 0, "node_frames_in": 0}

    def attach(self, transport) -> None:
        """M20's first run: from no cell (NullTransport) to the first board's port. The reader reads self.transport on
        every pass, so the swap takes effect at its next read."""
        old, self.transport = self.transport, transport
        try:
            old.close()
        except Exception:  # noqa: BLE001 -- a null transport, or a port already gone
            pass

    def _next_msg_id(self) -> int:
        with self._lock:
            self._msg_id = self._msg_id + 1 if self.HUB_MSG_ID_BASE <= self._msg_id < 0xFFFF else self.HUB_MSG_ID_BASE
            return self._msg_id

    def send_raw(self, raw: bytes) -> None:
        """A frame onto the cable exactly as it is (the host node's: its auth tag covers the header)."""
        wire = write_serial_frame(raw)
        with self._tx_lock:
            try:
                self.transport.write(wire)
            except Exception as exc:
                raise BridgeError(f"write to {self.transport.description} failed: {exc}") from exc
            self.stats.tx_frames += 1
            self.stats.tx_bytes += len(wire)
        self._tee(raw, "tx")

    def _may_heartbeat(self) -> bool:
        # With a host node attached it is the PC's node and beats for itself; after it went, the hub keeps silent for
        # its death window, so the cabled board declares it dead as every other board does (found by review: the
        # hub's own heartbeats kept 0x00fe alive on the cabled board alone, and the reconcilers disagreed).
        return self.node_client is None and time.monotonic() >= self._quiet_until

    def _reply_probe(self, f: fr.Frame, recv: float) -> None:
        if self._may_heartbeat():
            super()._reply_probe(f, recv)

    # -- frames from the cell ------------------------------------------------------------------------
    def _on_raw_frame(self, raw: bytes) -> None:
        try:
            f = fr.parse(raw)
        except fr.FrameError:
            super()._on_raw_frame(raw)
            return
        if f.opcode == fr.Op.REPLY:
            self._input_reply(f)  # M24: an app's answer to a batch of events -- noted, and passed on as before
        if f.opcode == fr.Op.REPLY and f.msg_id != 0:
            with self._lock:
                r = self._relayed.pop(f.msg_id, None)
            if r is not None:
                self.stats.rx_frames += 1
                self.relay_counts["replies"] += 1
                r.client.send_raw(_renumber(raw, f, r.msg_id))
                return
        node = self.node_client
        if node is not None and f.opcode not in (fr.Op.CALL, fr.Op.CAST):
            if f.opcode == fr.Op.REPLY and f.msg_id < self.HUB_MSG_ID_BASE:
                self.stats.rx_frames += 1
                self.relay_counts["node_frames_in"] += 1
                node.send_raw(raw)  # the host node's own request answered
                return
            if f.opcode != fr.Op.REPLY:
                self.relay_counts["node_frames_in"] += 1
                node.send_raw(raw)  # HELLO, heartbeats, the cell's READs and WRITEs of the PC's resources, ...
                if f.opcode == fr.Op.HELLO_ACK:
                    return  # the cell answering the host node's HELLO: not the hub's to learn from
        if f.opcode == fr.Op.CALL and f.dst == HOST_NODE_ID and self._serve_call(f):
            return  # M23: one of the PC's own services -- answered here, never also fanned out to tools
        if f.opcode in (fr.Op.CALL, fr.Op.CAST) or (node is None and f.opcode == fr.Op.READ):
            if f.dst == HOST_NODE_ID:
                # The cell asks the PC (an agent's service, M8): every tool hears it; the one offering it answers.
                self._fan_out(raw)
        super()._on_raw_frame(raw)

    def _call_app(self, owner: int, path_hash: int, args: bytes) -> bool:
        """M24: one CALL of an app's input (a batch of events; M25: a chunk of a file), through the cabled board: for another board it is marked to
        be passed on (ns_payloads.hpp kCallForward, the member first), and the board brings the answer back -- as an
        outside write is passed on. (Through the PC's node it took 741 ms at the 99th percentile on the bench,
        2026-10-25: that node's own round trip and its pending slots, shared with presenting.) False: no board yet."""
        peer = self.peer_node_id
        if peer is None:
            return False
        if owner == peer:
            payload = Call(path_hash=path_hash, args=args).encode()
        else:
            payload = Call(path_hash=path_hash, args=struct.pack("<H", owner) + args, flags=CALL_FORWARD).encode()
        self.send_frame(fr.Op.CALL, payload, dst=peer, msg_id=self._next_msg_id(), lclass=4)
        return True

    def _input_reply(self, f: fr.Frame) -> None:
        try:
            rep = Reply.parse(bytes(f.payload))
        except ValueError:
            return
        if rep.reply_to != fr.Op.CALL:
            return
        t = self.bulk.by_path(rep.path_hash)  # M25: a chunk's answer -- the offset the app expects next
        if t is not None:
            if rep.status == int(NsError.OK):
                self.bulk.ack(t, int.from_bytes(bytes(rep.value.raw[:4]), "little"))
            else:
                self.bulk.fail(t)  # the app refused a chunk: no such reader there
            return
        q = self.inputs.by_path(rep.path_hash)  # from the app's board, or from the cabled board passing it on
        if q is None:
            return
        if rep.status == int(NsError.OK):
            q.ack(int.from_bytes(bytes(rep.value.raw[:4]), "little") & 0xFFFF, time.time())
        else:
            q.refuse()

    def _serve_call(self, f: fr.Frame) -> bool:
        """M23: a board's CALL of one of the PC's named services (the cluster file's `pc_services`): answered here.
        A slow one (speech) on a thread of its own, so the reader never waits on it. False if not one of ours."""
        svc = self.services_for()
        if svc is None:
            return False
        try:
            call = Call.parse(bytes(f.payload))
        except ValueError:
            return False
        name = svc.name_of(call.path_hash)
        if name is None:
            return False

        def answer() -> None:
            value = svc.serve(name, f.src, bytes(call.args))
            rep = Reply(path_hash=call.path_hash, timestamp_ms=self.uptime_ms & 0xFFFFFFFF, age_ms=0, unit=0,
                        reply_to=fr.Op.CALL, status=int(NsError.OK), quality=int(Quality.GOOD), latency_class=4,
                        value=value)
            node = self.node_client
            if node is not None and node.alive:
                # A board off the cable checks the end-to-end tag on what the PC's node sends it, and only pot_hostnode
                # holds that key: an untagged reply from here was dropped by C (2026-10-24, the first speech on the
                # cell: said aloud, C's result "gone"). So the host node sends it -- the reply goes down its link
                # marked as its own, and potluck.hostnode turns it into a "reply" record for it (reply_record).
                node.send_raw(fr.encode(src=HOST_NODE_ID, dst=f.src, opcode=fr.Op.REPLY, lclass=4, msg_id=f.msg_id,
                                        payload=rep.encode()))
                return
            try:  # no host node: the cabled board, the cable's own end, takes it untagged; another writes it off
                self.send_frame(fr.Op.REPLY, rep.encode(), dst=f.src, msg_id=f.msg_id, lclass=4)
            except BridgeError:
                pass  # the link went away; the board writes the call off as its provider gone

        if svc.slow(name):
            threading.Thread(target=answer, name=f"potluck-svc-{name}", daemon=True).start()
        else:
            answer()
        return True

    def _fan_out(self, raw: bytes) -> None:
        with self._clients_lock:
            clients = list(self._clients)
        for c in clients:
            c.send_raw(raw)
            self.relay_counts["fanned_out"] += 1

    # -- frames from a tool ----------------------------------------------------------------------------
    def from_client(self, c: _Client, raw: bytes) -> None:
        try:
            f = fr.parse(raw)
        except fr.FrameError:
            return
        if c is self.node_client:
            self.relay_counts["node_frames_out"] += 1
            try:
                self.send_raw(raw)
            except BridgeError:
                pass
            return
        if f.opcode == fr.Op.HELLO and _hello_epoch(f) != 0:
            # An enrolled node's HELLO: the placement host (M10). One at a time -- it is the PC's one node.
            if self.node_client is not None:
                self.record("host_node_refused", None, {"client": c.name, "holder": self.node_client.name})
                c.alive = False
                try:
                    c.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                return
            self.node_client = c
            try:
                h = Hello.parse(bytes(f.payload))
                self._node_window_s = max(1.0, h.hb_period_ms * h.hb_miss_limit / 1000.0)
            except (ValueError, struct.error):
                self._node_window_s = 6.0
            self.record("host_node", None, {"client": c.name, "attached": True, "epoch": _hello_epoch(f)})
            self.relay_counts["node_frames_out"] += 1
            try:
                self.send_raw(raw)
            except BridgeError:
                pass
            return
        if f.opcode == fr.Op.HELLO:
            # A tool says HELLO when it starts and after the cabled board rebooted ("a board that rebooted forgets the
            # host"): so the hub says its own HELLO to the cell too -- the board re-admits the PC, which stays one host
            # to it -- and answers the tool from what the cell told the hub (found on the bench: a firmware rollout
            # through the hub stalled on the cabled board, which never re-admitted the PC after its reboot).
            if self.node_client is None:
                self.say_hello()
            if self.peer_node_id is not None:
                ack = struct.pack("<IHBBBBH", self.peer_boot_epoch or 0, self.peer_node_id, 2, 0,
                                  (self.peer_hb_period_ms or 100) // 10, self.peer_hb_miss_limit or 6, 0)
                c.send_raw(fr.encode(src=self.peer_node_id, dst=HOST_NODE_ID, opcode=fr.Op.HELLO_ACK, lclass=3,
                                     priority=0, seq=0, msg_id=f.msg_id, payload=ack))
            return
        if f.opcode in (fr.Op.HEARTBEAT, fr.Op.BYE, fr.Op.HELLO_ACK):
            return  # the hub heartbeats for the PC; a tool leaving is not the PC leaving
        if f.opcode in _REQUEST_OPS and f.msg_id != 0:
            mid = self._next_msg_id()
            now = time.monotonic()
            with self._lock:
                self._relayed[mid] = _Relayed(c, f.msg_id, now)
                for k in [k for k, v in self._relayed.items() if now - v.at > self.RELAY_TTL_S]:
                    del self._relayed[k]
                    self.relay_counts["unmatched"] += 1
            self.relay_counts["requests"] += 1
            self.send_frame(f.opcode, bytes(f.payload), dst=f.dst, msg_id=mid, ack_req=f.wants_ack,
                            lclass=f.lclass, priority=f.priority)
            return
        # Anything else -- a tool answering the cell's request (REPLY), a CAST -- goes on as it is.
        self.send_frame(f.opcode, bytes(f.payload), dst=f.dst, msg_id=f.msg_id, ack_req=f.wants_ack,
                        lclass=f.lclass, priority=f.priority)

    def say_hello(self) -> None:
        if not self._may_heartbeat():
            return  # the host node is (or was, a death window ago) the PC's node; a HELLO now reads as its incarnation
        h = Hello(boot_epoch=0, caps=0, node_id=self.node_id, espnow_version=0, hb_period_ms=self.hb_period_ms,
                  hb_miss_limit=self.hb_miss_limit, flags=HELLO_FLAG_WANT_ACK, pubkey_fp=b"")
        try:
            self.send_frame(fr.Op.HELLO, h.encode(), dst=fr.NODE_BROADCAST)
        except BridgeError:
            pass

    def _on_heartbeat(self, f: fr.Frame) -> None:
        before = self.peer_boot_epoch
        super()._on_heartbeat(f)
        # The cabled board rebooted: it forgot the PC. Say HELLO again rather than wait for a tool to.
        if before is not None and self.peer_boot_epoch is not None and self.peer_boot_epoch > before:
            self.say_hello()
            self.record("cabled_board_rebooted", f.src, {"epoch": self.peer_boot_epoch})

    def add_client(self, c: _Client) -> None:
        with self._clients_lock:
            self._clients.append(c)
            self.relay_counts["clients"] = len(self._clients)

    def drop_client(self, c: _Client) -> None:
        with self._clients_lock:
            if c in self._clients:
                self._clients.remove(c)
            self.relay_counts["clients"] = len(self._clients)
        if c is self.node_client:
            # The placement host went (killed, or stopped): the cell finds it dead by its missed heartbeats, exactly as
            # when M10's process was killed on the cable -- the hub says nothing for 0x00fe for its death window
            # (and a margin), then speaks for the PC again.
            self._quiet_until = time.monotonic() + self._node_window_s + 1.0
            self.node_client = None
            self.record("host_node", None, {"client": c.name, "attached": False})
        with self._lock:
            for k in [k for k, v in self._relayed.items() if v.client is c]:
                del self._relayed[k]


def _hello_epoch(f: fr.Frame) -> int:
    try:
        return Hello.parse(bytes(f.payload)).boot_epoch
    except (ValueError, struct.error):
        return 0


def _renumber(raw: bytes, f: fr.Frame, msg_id: int) -> bytes:
    return fr.encode(src=f.src, dst=f.dst, opcode=f.opcode, lclass=f.lclass, priority=f.priority, seq=f.seq,
                     msg_id=msg_id, payload=bytes(f.payload), ack_req=f.wants_ack)


def serve_relay(hub: Hub, addr: str) -> socketserver.ThreadingTCPServer:
    host, _, p = addr.rpartition(":")

    class Handler(socketserver.BaseRequestHandler):
        def handle(self) -> None:
            c = _Client(self.request, f"{self.client_address[0]}:{self.client_address[1]}")
            self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            hub.add_client(c)
            hub.record("relay_client", None, {"client": c.name, "connected": True})
            ra = SerialReassembler()
            try:
                while c.alive:
                    try:
                        data = self.request.recv(4096)
                    except socket.timeout:
                        continue  # a quiet tool (the timeout is for sends to it)
                    if not data:
                        break
                    for raw in ra.feed(data):
                        hub.from_client(c, raw)
            except OSError:
                pass
            finally:
                hub.drop_client(c)
                hub.record("relay_client", None, {"client": c.name, "connected": False})

    socketserver.ThreadingTCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer((host or "127.0.0.1", int(p)), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, name="potluck-hub-relay", daemon=True).start()
    return srv


# ---- the model ------------------------------------------------------------------------------------------

def _hub_ms() -> int:
    return int(time.time() * 1000)


class Model:
    """What the hub knows, kept by the poller and read by the API. One lock; snapshots are plain JSON values."""

    DEAD_AFTER_MISSES = 3  # consecutive unanswered NODE_STATUS polls before the model calls a board dead

    def __init__(self, hub: Hub, *, cluster=None, package=None, pull_period_ms: int = 2000,
                 telemetry_period_ms: int = 10000, telemetry_log=None) -> None:
        self.hub = hub
        # M15: every pull's health and links as JSON lines (--telemetry-log), so the radio's figures can be measured
        # with no console connected to any board (tools/m15_report.py --hub).
        self.telemetry_log = telemetry_log
        self.recorder = None  # M16: potluck.hubreplay.Recorder, when --record names a directory
        self.pairing = None  # M17: potluck.hubauth.Pairing, when --listen serves the house network
        from .hub_clients import FairShare, Reports

        self.fair = FairShare()  # M18: each program's share of what the hub may ask of the cell
        self.tls_fp = None  # M17: SHA-256 of the --listen certificate, shown so a person can compare it
        self.reports = Reports()  # M18: what the integrations say of themselves
        self.cluster = cluster
        self.package = package
        self._pkg_seen: int | None = None  # the cell's package counter the hub's package was planned for
        self._replanning = False
        self._pins_for: tuple[str | None, dict | None] = (None, None)  # (firmware version, its pin map)
        self.restart_requested = False  # POST /api/setup/listen: the main loop stops cleanly, the tray starts a new hub
        self.pull_period_ms = pull_period_ms
        # M15: how often each board's telemetry is pulled over the radio (0: never). Its own rate, longer than the
        # inventory's: it is the larger load, and M15's [MEASURE] is the rate that leaves M0's figures intact.
        self.telemetry_period_ms = telemetry_period_ms
        self._telemetry_due = 0.0
        self.telemetry_counts = {"pulls": 0, "replies": 0, "failures": 0, "events": 0}
        self.job_times: dict[int, dict[str, tuple[int, int]]] = {}  # M22: a board's last finished run of each kind
        self._services = None  # M23: (system, names, PcServices) -- kept while the cluster file names the same ones
        hub.services_for = self.pc_services
        self.link_deaths: dict[tuple[int, int], int] = {}  # (board, peer) -> peer_dead events pulled since the hub started
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.first_pull: float | None = None  # when the hub first looked at the cell: where a quiet start is counted
        # every board the cell file names, from the start: "0 of 3 heard", not "0 of 0", before the first pull
        self.boards: dict[int, dict] = {n: {"state": "unknown", "since_at": _hub_ms(), "last_ok": None}
                                        for n, _ in (cluster.boards if cluster is not None else [])}
        self.resources: dict[str, dict] = {}
        self.interest: dict[str, dict[str, float]] = {}  # on-demand reads: path -> {client: until (monotonic)}
        self.events: deque[dict] = deque(maxlen=5000)
        self.seq = 0
        self.rtts: deque[float] = deque(maxlen=200)
        self.errors: deque[float] = deque(maxlen=500)
        self.frames_at: deque[tuple[float, int]] = deque(maxlen=120)
        self.deploys: list[dict] = []
        self.version = _version()
        self.cell_id = _cell_id(cluster)
        hub.record = self.record  # the relay records through the model

    # events
    def record(self, kind: str, board: int | None, detail: dict) -> None:
        with self.lock:
            self.seq += 1
            self.events.append({"seq": self.seq, "at": _hub_ms(), "board": f"{board:04x}" if board else "hub",
                                "kind": kind, "detail": detail})

    # which boards, which resources
    def board_ids(self) -> list[int]:
        if self.cluster is not None and self.cluster.boards:
            return [n for n, _ in self.cluster.boards]
        return [self.hub.peer_node_id] if self.hub.peer_node_id else []

    def label_of(self, node: int) -> str:
        if self.cluster is not None:
            for n, lab in self.cluster.boards:
                if n == node:
                    return lab
        return f"node-{node:04x}"

    def room_of(self, node: int) -> str | None:
        """M20: the room the cluster file puts this board in, or None."""
        return getattr(self.cluster, "rooms", {}).get(node) if self.cluster is not None else None

    def resource_paths(self) -> list[str]:
        if self.package is None:
            return []
        from .apply import expected
        _, _, reads = expected(self.package.manifest, self.board_ids(), verify=False)  # every output, inboxes' too
        return reads

    # On-demand reads (POST /api/interest): paths a client asks the hub to read besides the package's, for as long as
    # it keeps asking. The package's own reads leave out what a board keeps per link ({peer} outputs), what an actor
    # publishes only sometimes (? outputs) and a board's built-in sys/ resources -- reading all of them all the time
    # would cost a READ each per pull for every viewer's sake (a 20-board cell with a per-link app: hundreds), and ~70
    # reads a second already stretches the heartbeat's round trip (DEVELOPING-APPS.md). So they are read only while
    # someone looks. Asked for by the Potluck Desk, found on the owner's cell 2026-10-18.
    INTEREST_PER_CLIENT = 32
    INTEREST_MAX = 64
    INTEREST_TTL_S = (5, 120)

    def _system(self) -> str:
        if self.package is not None:
            return self.package.manifest.system
        return getattr(self.cluster, "system", None) or "lab"

    def _interest_patterns(self) -> list[re.Pattern]:
        """What a client may ask for: an output some module of the package declares -- {node} and {peer} any board,
        {actor} that actor, a ? output too -- or a board's built-in sys/ resource. Nothing else: an on-demand read is
        a look at what the cell already publishes, never a probe of arbitrary paths."""
        from . import modules as mods
        node = "node-[0-9a-f]{4}"
        prefix = re.escape(f"potluck://{self._system()}/")
        pats = [re.compile(prefix + node + r"/sys/[a-z0-9/_-]+")]
        for a in (self.package.manifest.actors if self.package is not None else []):
            spec = mods.get(a.module)
            for o in (getattr(spec, "outputs", None) or []):
                t = re.escape(o.lstrip("?"))
                t = t.replace(re.escape("{actor}"), re.escape(a.name)).replace(re.escape("{node}"), node)
                t = t.replace(re.escape("{peer}"), node)
                pats.append(re.compile(prefix + t))
        return pats

    def want(self, client: str, paths: list, ttl_s: float) -> tuple[list[str], list[dict]]:
        """A client's on-demand reads, renewed for ttl_s seconds (clamped to INTEREST_TTL_S). Returns (accepted,
        refused, each with why). Asking again renews; a path nobody renews is dropped at its expiry, its value too."""
        lo, hi = self.INTEREST_TTL_S
        until = time.monotonic() + min(max(float(ttl_s), lo), hi)
        pats = self._interest_patterns()
        accepted, refused = [], []
        with self.lock:
            mine = {p for p, who in self.interest.items() if client in who}
            for p in paths:
                if not isinstance(p, str) or not any(pt.fullmatch(p) for pt in pats):
                    refused.append({"path": p, "result": "not an output of this cell's package or a board's sys/"})
                elif p not in mine and len(mine) >= self.INTEREST_PER_CLIENT:
                    refused.append({"path": p, "result": f"at most {self.INTEREST_PER_CLIENT} per program"})
                elif p not in self.interest and len(self.interest) >= self.INTEREST_MAX:
                    refused.append({"path": p, "result": f"at most {self.INTEREST_MAX} in all"})
                else:
                    self.interest.setdefault(p, {})[client] = until
                    mine.add(p)
                    accepted.append(p)
        return accepted, refused

    def interest_paths(self) -> list[str]:
        """The on-demand paths still wanted, beyond the package's own; an expired one is dropped, and so is its value
        unless the package reads it anyway."""
        now = time.monotonic()
        package = set(self.resource_paths())
        with self.lock:
            for p in list(self.interest):
                who = {c: t for c, t in self.interest[p].items() if t > now}
                if who:
                    self.interest[p] = who
                else:
                    del self.interest[p]
                    if p not in package:
                        self.resources.pop(p, None)
            return [p for p in self.interest if p not in package]

    def _interest_json(self) -> dict:
        with self.lock:
            clients = {c for who in self.interest.values() for c in who}
            return {"paths": len(self.interest), "clients": len(clients), "max": self.INTEREST_MAX,
                    "per_client": self.INTEREST_PER_CLIENT}

    # one pull: every board's NODE_STATUS, every resource's READ
    def pull(self) -> None:
        from .apply import node_status
        if self.first_pull is None:
            self.first_pull = time.monotonic()
        if time.monotonic() < self.hub._quiet_until:
            return  # a host node just went: any frame from 0x00fe would keep it alive on the cabled board
        cabled = self.hub.peer_node_id
        for n in self.board_ids():
            t0 = time.monotonic()
            try:
                st = node_status(self.hub, n, timeout=2.0, tries=1)
            except BridgeError:
                st = None
            with self.lock:
                b = self.boards.setdefault(n, {"state": "unknown", "since_at": _hub_ms(), "last_ok": None})
                prev = b["state"]
                if st is None:
                    b["misses"] = b.get("misses", 0) + 1
                tel = b.get("tel") or {}
                if st is None and b["misses"] < self.DEAD_AFTER_MISSES and prev in ("alive", "asleep"):
                    # One unanswered poll is a busy cable, not a dead board (found on the bench, M0-LOG session 42:
                    # with a placement host, the agent, a watch and a plan at once, single polls timed out and the
                    # model flickered dead/alive).
                    self.errors.append(time.monotonic())
                elif st is None:
                    if prev == "asleep" and self._cabled_sees_dead(n):
                        # Asleep is a declared long window, not forever: once the cabled board's own view has
                        # declared it dead (its window ran out), so does the hub (found by review).
                        b["state"], b["since_at"] = "dead", _hub_ms()
                    elif prev in ("alive", "asleep") and tel.get("health") is not None and tel["health"].long_window:
                        # It declared a long window (M5.1): not answering is what an asleep board does.
                        if prev != "asleep":
                            b["state"], b["since_at"] = "asleep", _hub_ms()
                    elif prev in ("alive", "asleep"):
                        b["state"], b["since_at"] = "dead", _hub_ms()
                    elif prev == "unknown":
                        b["state"] = "unknown"
                    self.errors.append(time.monotonic())
                else:
                    b["misses"] = 0
                    self.rtts.append(time.monotonic() - t0)
                    if prev != "alive":
                        b["state"], b["since_at"] = "alive", _hub_ms()
                    if b.get("epoch_up") is not None and st.uptime_s < b["epoch_up"]:
                        self._event_locked("board_rebooted", n, {"up_s": st.uptime_s})
                    b["epoch_up"] = st.uptime_s
                    b["last_ok"] = time.monotonic()
                    self._note_deploy_locked(n, b.get("status"), st)
                    b["status"] = st
                    b["cabled"] = n == cabled
                if prev != b["state"]:
                    self._event_locked("board_" + b["state"], n, {"was": prev})
        self._follow_package()
        for path in self.resource_paths() + self.interest_paths():  # the package's, then what a viewer asked for
            t0 = time.monotonic()
            try:
                r = self.hub.read(path, timeout=1.5)
                if r.quality == Quality.STALE:
                    # Another board's value comes from the cabled board's copy, which a read refreshes: its age is the
                    # hub's own pass over the cell. With 42 resources at ~100 ms a read that pass is 4-5 s, at a 5 s
                    # bound (found 2026-10-19, after the store's app added three: the earlier alarm's outputs flipped
                    # STALE and its app "not working"). A second read now answers from the refreshed copy -- measured
                    # 5220 -> 1326 ms and 2162 -> 226 ms -- and is what is kept: a reading, not a guess.
                    self.rtts.append(time.monotonic() - t0)
                    t0 = time.monotonic()
                    r = self.hub.read(path, timeout=1.5)
            except BridgeError:
                self.errors.append(time.monotonic())
                continue
            self.rtts.append(time.monotonic() - t0)
            self._record(path, r)
        if self.telemetry_period_ms and time.monotonic() >= self._telemetry_due:
            self._telemetry_due = time.monotonic() + self.telemetry_period_ms / 1000
            self.pull_telemetry()
        with self.lock:
            self.frames_at.append((time.monotonic(), self.hub.stats.rx_frames + self.hub.stats.tx_frames))

    def _follow_package(self) -> None:
        """When the cell's package changes, the hub plans it again from the cluster file: its apps, resources and panels
        follow what the boards run. Found 2026-10-19: the package was planned once at start, so after the owner's Apply
        of a store app the hub knew neither the new app nor its outputs. Planned off the pull's thread (it may export an
        app's repository); until it is done the hub shows the package it had."""
        if self.cluster is None or getattr(self.cluster, "path", None) is None:
            return
        with self.lock:
            counters = [b["status"].pkg_counter for b in self.boards.values()
                        if b.get("status") is not None and b["state"] == "alive"]
        if not counters:
            return
        now = max(counters)
        if self._pkg_seen is None:
            self._pkg_seen = now
            return
        if now == self._pkg_seen or self._replanning:
            return
        self._pkg_seen = now
        self._replanning = True
        threading.Thread(target=self._replan, args=(now,), daemon=True, name="hub-replan").start()

    def _replan(self, counter: int) -> None:
        from .apply import default_cache, load_cluster, plan_package
        try:
            c = load_cluster(str(self.cluster.path))
            pkg = plan_package(c, default_cache(), lambda msg: None)
        except Exception as exc:  # noqa: BLE001 -- the hub keeps what it had and says why
            self.record("hub_package", None, {"counter": counter, "error": str(exc)[:300]})
        else:
            self.cluster, self.package = c, pkg
            self.record("hub_package", None, {"counter": counter,
                                              "actors": len(pkg.manifest.actors) if pkg is not None else 0})
        finally:
            self._replanning = False

    def _record(self, path: str, r) -> None:
        """One reading of a resource, kept with its settled quality.

        The quality as read is shown as it is. Its *settled* quality changes only when a new quality holds for
        SETTLE_READS reads in a row: another board's value reaches the hub through the cabled board's copy, refreshed
        at the hub's previous read (3-4 s ago with ~35 resources), so a value published every second can read STALE
        once under a 5 s bound and GOOD the next time. App health and the quality_changed event follow the settled
        quality (found 2026-10-16: an everyday cell's dashboard flapped "not working" on single late reads)."""
        with self.lock:
            old = self.resources.get(path)
            q = Quality.name_of(r.quality)
            streak = old.get("streak", 1) + 1 if old is not None and old["quality"] == q else 1
            settled = old.get("settled") if old is not None else None
            first = old.get("first", time.monotonic()) if old is not None else time.monotonic()
            if settled is None:
                # Starting: GOOD that holds is where it starts; anything else only once it outlasts STARTUP_S. The
                # cabled board answers the first reads from copies it last refreshed before the hub started, so the
                # first reads of another board's value are STALE by construction (found 2026-10-16: two STALE reads
                # settled, the quiet start ended at ~7 s, and the values read GOOD at 12-13 s). Where a resource starts
                # is not a change, so no event.
                if (streak >= self.SETTLE_READS and q == "GOOD") or time.monotonic() - first >= self.STARTUP_S:
                    settled = q
            elif settled != q and (streak >= self.SETTLE_READS or (settled == "NO_DATA" and q == "GOOD")):
                # NO_DATA to GOOD at once: a value's first write (an inbox's program starting) is no late read -- held
                # back, an app read "nothing written yet" ~20 s after its values were GOOD (the Potluck Desk, 2026-10-21).
                self._event_locked("quality_changed", None, {"path": path, "from": settled, "to": q})
                settled = q
            self.resources[path] = {"reading": r, "quality": q, "at": _hub_ms(), "streak": streak,
                                    "settled": settled, "first": first}

    def pull_telemetry(self) -> None:
        """M15: every answering board's health, links, actors and new events, over the radio, one pull for every
        viewer. A board that does not answer TELEMETRY (firmware before M15) keeps what NODE_STATUS says."""
        from . import telemetry as tm

        for n in self.board_ids():
            with self.lock:
                b = self.boards.get(n)
                if b is None or b["state"] != "alive":
                    continue
                if b.get("tel_unsupported") and time.monotonic() < b.get("tel_retry_at", 0.0):
                    continue  # asked again every few minutes: a board stalled when the hub started, or upgraded since
                old = b.get("tel") or {}
                since = old.get("events_next", 1) - 1
                old_epoch = old["health"].boot_epoch if old.get("health") is not None else None
                have_reset = b.get("reset") is not None and b.get("reset_epoch") == old_epoch
                have_chip = b.get("chip") is not None and b.get("chip_epoch") == old_epoch
                st = b.get("status")
                mc = bool(st is not None and {"mc_job", "mc_lender"} & set(getattr(st, "actors", ()) or ()))
            self.telemetry_counts["pulls"] += 1
            try:
                h = tm.health(self.hub, n)
                if old_epoch is not None and h.boot_epoch != old_epoch:
                    since = 0  # it rebooted: its event log started again at 1 (found by review: counting missed it)
                # once a boot -- and again on the next pull while a read over the radio went unanswered
                reset = self._read_reset(n) if (h.boot_epoch != old_epoch or not have_reset) else None
                ls = tm.links(self.hub, n)
                acts = tm.actors(self.hub, n)
                evs, nxt = tm.events(self.hub, n, since)
                if nxt <= since:  # the log is behind where we were: it rebooted, and the epoch did not say so
                    evs, nxt = tm.events(self.hub, n, 0)
                try:
                    auth = tm.auth(self.hub, n)
                except (BridgeError, tm.TelemetryError):
                    auth = None  # firmware before 2026-10-19 does not know the section, or no answer: not reported
                chip = None
                if h.boot_epoch != old_epoch or not have_chip:  # M21: what the board is -- once a boot
                    try:
                        chip = tm.chip(self.hub, n)
                    except (BridgeError, tm.TelemetryError):
                        chip = None  # firmware before M21, or no answer: asked again on the next pull
                compute = None
                if mc:  # M22: borrowed cores -- only a board that runs a lender or a job is asked
                    try:
                        compute = tm.compute(self.hub, n)
                    except (BridgeError, tm.TelemetryError):
                        compute = None  # firmware before M22, or no answer
            except (BridgeError, tm.TelemetryError) as exc:
                self.telemetry_counts["failures"] += 1
                with self.lock:
                    b = self.boards.get(n)
                    if b is not None:
                        b["tel_failures"] = b.get("tel_failures", 0) + 1
                        if b["tel_failures"] >= 3 and not b.get("tel"):
                            if not b.get("tel_unsupported"):
                                self._event_locked("telemetry_unsupported", n, {"why": str(exc)[:120]})
                            b["tel_unsupported"] = True  # not answering: firmware before M15, or stalled now
                            b["tel_retry_at"] = time.monotonic() + 300.0
                continue
            self.telemetry_counts["replies"] += 1
            self.telemetry_counts["events"] += len(evs)
            if self.telemetry_log is not None:
                rec = {"rec": "tel", "host_ts": time.time(), "node": n, "epoch": h.boot_epoch, "up_s": h.uptime_s,
                       "flags": h.flag_names(), "int_free": h.int_free, "int_min": h.int_min, "auth": auth,
                       "links": [{"peer": k.node, "state": k.state_name, "rx_lost": k.rx_lost, "rx_bcast": k.rx_bcast,
                                  "rx_frames": k.rx_frames, "tx_cb_ok": k.tx_cb_ok, "tx_cb_fail": k.tx_cb_fail,
                                  "rtt_p50_ms": k.rtt_p50_ms, "rtt_p99_ms": k.rtt_p99_ms,
                                  "rtt_timeouts": k.rtt_timeouts} for k in ls]}
                self.telemetry_log.write(json.dumps(rec) + "\n")
                self.telemetry_log.flush()
            with self.lock:
                b = self.boards.get(n)
                if b is None:
                    continue
                b["tel_failures"] = 0
                b["tel_unsupported"] = False
                b["tel"] = {"health": h, "links": ls, "actors": acts, "events_next": nxt, "at": _hub_ms(), "auth": auth,
                            "compute": compute}
                job = (compute or {}).get("job")
                if job and not job["running"] and not job["stopped"] and job["run"]:
                    # M22: a run that ended with its last unit -- kept by kind, so a speed-up is measured, not guessed
                    times = self.job_times.setdefault(n, {})
                    times["lend" if job["lend"] else "alone"] = (job["run"], job["elapsed_ms"])
                if chip is not None:
                    b["chip"], b["chip_epoch"] = chip, h.boot_epoch
                if reset is not None:
                    if old_epoch is not None and h.boot_epoch != old_epoch:  # it restarted while the hub watched
                        self._event_locked("board_restarted", n, {"reason": reset, "epoch": h.boot_epoch})
                    b["reset"], b["reset_epoch"] = reset, h.boot_epoch
                elif h.boot_epoch != old_epoch:
                    b["reset"] = None  # a new boot whose reason is not known yet: never the last boot's
                # The first answer this hub gets from a board carries the board's kept history: those events
                # happened before the hub was watching. They are logged as `earlier`, at the time they happened --
                # not as news (found 2026-10-16: every hub start replayed half an hour of probe timeouts as new).
                earlier = old_epoch is None
                now_ms, up_ms = _hub_ms(), h.uptime_s * 1000
                for e in evs:
                    if e.kind_name == "peer_dead" and not earlier:
                        self.link_deaths[(n, e.node)] = self.link_deaths.get((n, e.node), 0) + 1
                    self._event_locked("board_" + e.kind_name, n,
                                       {"peer": f"{e.node:04x}", "a": e.a, "b": e.b, "board_ms": e.at_ms,
                                        "board_seq": e.seq},
                                       at=now_ms - max(0, up_ms - e.at_ms) if earlier else None, earlier=earlier)

    def _read_reset(self, n: int) -> str | None:
        """Why board n last started (its sys/reset), by name; None from firmware before 2026-10-16 or no answer. Read
        once an incarnation: a board that restarts by itself -- the task watchdog, the radio fail-stop, a brownout --
        is seen from the PC, not only on its console (M0-LOG session 46)."""
        from .sys_paths import reset_name, sys_path
        system = self.cluster.system if self.cluster is not None else "lab"
        try:
            # through the cabled board, as every READ the hub makes: it adopts another board's entry on demand, so the
            # first answer may be NO_DATA -- the next pull reads it (addressed to the board itself, it never answered)
            r = self.hub.read(sys_path(n, "sys/reset", system), timeout=1.0)
        except BridgeError:
            return None
        v = r.value.number() if r.usable() else None
        return reset_name(int(v)) if isinstance(v, int) and not isinstance(v, bool) else None

    def _cabled_sees_dead(self, n: int) -> bool:
        """Under self.lock: the cabled board's last telemetry has a link to n, and it says dead."""
        cab = self.boards.get(self.hub.peer_node_id or -1) or {}
        for k in (cab.get("tel") or {}).get("links", []):
            if k.node == n:
                return k.state_name == "dead"
        return False

    def _note_deploy_locked(self, n: int, old, st) -> None:
        """Under self.lock: a board's firmware or package counter moved -- a deploy, whoever ran it (`apply` through
        this hub, or a board catching up from a peer). The hub sends no deploy itself, so this is where `deploys`
        learns of one (it stayed empty until 2026-10-14). One entry per (kind, counter), settled once every board
        the hub hears runs it confirmed."""
        if old is None:
            return  # the hub's first look at this board: what it runs is not news
        for kind, before, now in (("firmware", old.fw_running, st.fw_running), ("package", old.pkg_counter, st.pkg_counter)):
            if now == before or not now:
                continue
            d = next((x for x in self.deploys if x["kind"] == kind and x["counter"] == now), None)
            if d is None:
                d = {"at": _hub_ms(), "kind": kind, "counter": now, "result": "running", "detail": ""}
                self.deploys.append(d)
                del self.deploys[:-20]
            d.setdefault("_boards", set()).add(n)
        self._settle_deploys_locked(n, st)

    def _settle_deploys_locked(self, n: int, st) -> None:
        for d in self.deploys:
            if d["result"] != "running":
                continue
            others = {m: b.get("status") for m, b in self.boards.items() if b.get("state") == "alive" and m != n}
            others[n] = st
            on = [m for m, s in others.items() if s is not None and
                  (s.fw_running if d["kind"] == "firmware" else s.pkg_counter) == d["counter"]]
            d["detail"] = f"{len(on)} of {len(others)} board(s)"
            settled = len(on) == len(others) and all(
                (others[m].fw_state == "confirmed") if d["kind"] == "firmware" else not others[m].pending for m in on)
            if settled:
                d["result"] = "confirmed" if d["kind"] == "firmware" else "verified"
                d["detail"] = f"{'live' if d['kind'] == 'package' else 'confirmed'} on {len(on)} board(s)"
                self._event_locked("deploy", None, {k: v for k, v in d.items() if not k.startswith("_")})

    def _event_locked(self, kind: str, board: int | None, detail: dict, at: int | None = None,
                      earlier: bool = False) -> None:
        self.seq += 1
        ev = {"seq": self.seq, "at": _hub_ms() if at is None else at, "board": f"{board:04x}" if board else "hub",
              "kind": kind, "detail": detail}
        if earlier:
            ev["earlier"] = True  # from a board's history, before this hub was watching
        self.events.append(ev)

    # the JSON the API serves (HUB-API.md)
    def snapshot(self) -> dict:
        with self.lock:
            boards = [self._board_json(n, b) for n, b in sorted(self.boards.items())]
            resources = [self._resource_json(p, r) for p, r in self.resources.items()]
            events_tail = list(self.events)[-1:]
            deploys = [{k: v for k, v in d.items() if not k.startswith("_")} for d in self.deploys]
        apps = self._apps_json(boards, resources)
        starting = self.starting()
        if starting:
            for a in apps:
                if not a["working"]:
                    a["working"], a["detail"] = None, "starting: the hub has not seen the whole cell yet"
        panels = self._panels_json(resources)
        return {
            "api": API_VERSION,
            "at": _hub_ms(),
            "hub": {"version": self.version, "up_ms": int((time.monotonic() - self.started) * 1000),
                    "port": getattr(self.hub.transport, "port", None) or self.hub.transport.description,
                    "link": "up" if self.hub.peer_node_id is not None else "down", "link_detail": "",
                    "frames_rx": self.hub.stats.rx_frames, "frames_tx": self.hub.stats.tx_frames,
                    "pull_period_ms": self.pull_period_ms, "clock_error_ms": self._clock_error_ms(),
                    "telemetry_period_ms": self.telemetry_period_ms, "telemetry": dict(self.telemetry_counts),
                    "relay": dict(self.hub.relay_counts), "retention": self._retention_json(),
                    "pairing": self.pairing.pending() if self.pairing is not None else [],
                    "clients": self.fair.snapshot(), "tls_fp": self.tls_fp, "starting": starting,
                    "interest": self._interest_json()},
            "cell": self._cell_json(boards),
            "boards": boards,
            "apps": apps,
            "panels": panels,
            "resources": resources,
            "integrations": self._integrations_json(boards, resources) + self.reports.snapshot(),
            "deploys": deploys,
            "signals": self._signals_json(boards),
            "last_event_seq": events_tail[0]["seq"] if events_tail else 0,
        }

    def _retention_json(self) -> dict:
        """M16: how much history this hub keeps -- events in memory, and its recording on disk if it keeps one."""
        with self.lock:
            oldest = self.events[0]["at"] if self.events else None
            held = len(self.events)
        out = {"events_kept": self.events.maxlen, "events_held": held, "oldest_event_at": oldest, "recording": None}
        if self.recorder is not None:
            out["recording"] = self.recorder.retention()
        return out

    def _clock_error_ms(self) -> int | None:
        if not self.rtts:
            return None
        xs = sorted(self.rtts)
        return int(xs[len(xs) // 2] * 1000 / 2)  # half a median round trip: the uncertainty of mapping a board's clock

    def _board_json(self, n: int, b: dict) -> dict:
        st = b.get("status")
        pos = getattr(self.cluster, "positions", {}).get(n) if self.cluster is not None else None
        out = {"node": f"{n:04x}", "label": self.label_of(n), "room": self.room_of(n),
               "position": list(pos) if pos else None, "state": b["state"],
               "since_at": b["since_at"],
               "silent_ms": int((time.monotonic() - b["last_ok"]) * 1000) if b.get("last_ok") else None,
               "rssi": None, "cabled": bool(b.get("cabled")),
               "chip": b.get("chip")}  # M21: {model, model_id, revision, cores, flash_mb, psram_mb, features} or null
        if st is not None:
            out.update({
                "firmware": {"version": st.fw_version, "counter": st.fw_running, "state": st.fw_state},
                "package": {"counter": st.pkg_counter, "state": "on_trial" if st.pending else "confirmed",
                            "live_applies": st.live_applies},
                "roles": st.role_names(),
                "modules": [{"name": m, "state": s, "reason": r} for m, s, r in st.modules],
                "actors": list(st.actors),
                # the board cut its list of names short (64 bytes): every running actor has one name in a whole list
                "actors_cut": len(st.actors) < st.pinned_running + st.portable_here,
                "pinned_running": st.pinned_running,
                "up_ms": st.uptime_s * 1000,
                "reset": b.get("reset"),
                "cluster": st.cluster,
                "heap": None,   # M15 telemetry, below, when the board answers it
                "links": [],
            })
        tel = b.get("tel")
        if tel is not None:
            h = tel["health"]
            out["heap"] = {"internal_free": h.int_free, "internal_min": h.int_min, "internal_largest": h.int_largest,
                           "internal_total_kib": h.int_total_kib or None, "psram_free": h.ext_free,
                           "psram_min": h.ext_min}
            out["links"] = [{"peer": f"{k.node:04x}", "state": k.state_name, "delivery": k.delivery,
                             "rtt_p50_ms": k.rtt_p50_ms, "rtt_p99_ms": k.rtt_p99_ms, "rssi": k.rssi if k.rssi else None,
                             "deaths": self.link_deaths.get((n, k.node), 0),
                             "lost": k.rx_lost, "misses": k.misses, "cable": bool(k.flags & 1),
                             "window_ms": k.hb_period_ms * k.miss_limit} for k in tel["links"]]
            out["telemetry"] = {
                "at": tel["at"], "channel": h.channel, "flags": h.flag_names(), "up_ms": h.uptime_s * 1000,
                "epoch": h.boot_epoch, "peers_alive": h.peers_alive, "peers_dead": h.peers_dead,
                "deaths": h.deaths, "revivals": h.revivals, "reboots_seen": h.reboots_seen,
                "rx_total": h.rx_total, "rx_rejected": h.rx_rejected, "rx_queue_dropped": h.rx_queue_dropped,
                "tx_total": h.tx_total, "tx_enqueue_err": h.tx_enqueue_err, "events_dropped": h.events_dropped,
                "writes_served": h.writes_served, "writes_rejected": h.writes_rejected,
                "actors_pinned": h.actors_pinned, "actors_portable_here": h.actors_portable_here,
                "link_stack_free": h.link_stack_free,
                "actors": [{"key": f"{a.key:08x}", "running_here": bool(a.flags & 1), "owner": f"{a.owner:04x}",
                            "assigned": f"{a.assigned:04x}", "activations": a.activations, "fenced": a.fenced,
                            "released": a.released} for a in tel["actors"]],
                "auth": tel.get("auth"),
                # M22: M9's lender here (accepted, refused, served, unit_ms), or null
                "lend": (tel.get("compute") or {}).get("lender")}
            radio = [k.rssi for k in tel["links"] if k.rssi and not (k.flags & 1)]
            out["rssi"] = max(radio) if radio else None
        return out

    #: reads in a row a new quality must hold before it counts as the resource's settled quality
    SETTLE_READS = 2
    #: the longest a hub start is quiet: until every board has answered and every resource has settled, or this long
    STARTUP_S = 30.0

    def starting(self) -> bool:
        """The hub has not yet seen the whole cell once: some board has not answered yet, or some resource has not held
        one quality for SETTLE_READS reads. Never longer than STARTUP_S after the hub first looked at the cell -- a fault
        is never hidden past that. Counted from the first pull, not the start: a hub restarted after a kill waits up to
        a minute for its old lease to lapse before it serves anything (found 2026-10-16: the window had run out during
        that wait, and the first model the dashboard saw showed every app not working)."""
        if self.first_pull is None:
            return True
        if time.monotonic() - self.first_pull >= self.STARTUP_S:
            return False
        with self.lock:
            if any(b.get("status") is None for b in self.boards.values()):
                return True
            have = dict(self.resources)
        return any(have.get(p, {}).get("settled") is None for p in self.resource_paths())

    def _resource_json(self, path: str, r: dict) -> dict:
        rd = r["reading"]
        name = path.split("/", 3)[3] if path.count("/") >= 3 else path
        outside = self.cluster.outside.get(path) if self.cluster is not None else None
        owner = name[5:9] if name.startswith("node-") else None
        value: Any = None
        if rd.usable():
            value = rd.value.number()
            if isinstance(value, float):
                value = round(value, 6)
        unit = unit_suffix(rd.unit)
        return {"path": path, "name": name, "owner": owner, "value": value, "unit": unit, "quality": r["quality"],
                "settled_quality": r.get("settled") or "STARTING",
                "age_ms": rd.age_ms + max(0, _hub_ms() - r["at"]), "latency_class": rd.latency_class,
                "exposed": outside is not None, "outside_writable": outside == "read_write",
                "safety_relevant": self._safety(path),
                # M18: what a write must be (a reader cannot tell 20 from 20.0 in JSON); null until it first has a value
                "type": _TYPE_NAMES.get(rd.value.type),
                # read because a viewer asked (POST /api/interest); under self.lock, as snapshot() calls this. getattr:
                # a replay (potluck.hubreplay) and the tests' stand-ins have no interest of their own
                "on_demand": path in getattr(self, "interest", {})}

    def _safety(self, path: str) -> bool:
        if self.package is None:
            return False
        r = self.package.manifest.resource(path)
        return bool(r is not None and r.safety_relevant)

    def _cell_json(self, boards: list[dict]) -> dict:
        pk = next((b["package"]["counter"] for b in boards if b.get("package")), None)
        fw = next((b["firmware"]["version"] for b in boards if b.get("firmware")), None)
        presenter = next((b["node"] for b in boards if "present" in b.get("actors", [])), None)
        if presenter is None and self.package is not None:
            # 2026-10-21: the PC's host node holds the role (M14's present.hub) -- no board runs it, and every board's
            # telemetry names 00fe the owner of the presenting actor
            from . import reconcile as rc
            for a in self.package.manifest.actors:
                if a.module == "builtin:present":
                    key = f"{path_hash(rc.output_path(self.package.manifest, a)):08x}"
                    owners = {t["owner"] for b in boards for t in (b.get("telemetry") or {}).get("actors", [])
                              if t.get("key") == key}
                    if owners == {"00fe"}:
                        presenter = "00fe"
        update = self._update_json(fw)
        if fw != self._pins_for[0]:  # the pins the boards' build uses (potluck.pins), looked up once a version
            from .pins import for_version
            self._pins_for = (fw, for_version(fw))
        return {"system": self.cluster.system if self.cluster is not None else "lab", "cell_id": self.cell_id,
                "channel": None, "package": pk, "firmware": fw, "pins": self._pins_for[1], "update": update,
                "other_cell_frames": None,
                "presenter": presenter,
                "jobs": self._jobs_json(),
                "services": self.pc_services().snapshot() if self.pc_services() is not None else [],
                "bulk": self._bulk_json(),  # M25
                "inputs": (self.hub.inputs.snapshot()  # M24
                           if getattr(getattr(self, "hub", None), "inputs", None) is not None else []),
                "plan": getattr(self.cluster, "plan", None) if self.cluster is not None else None}

    def pc_services(self):
        """M23: the services this PC offers (the cluster file's `pc_services`), or None. One object per configuration,
        so a cluster file reloaded for another reason keeps the counters."""
        names = tuple(getattr(self.cluster, "pc_services", ()) or ()) if self.cluster is not None else ()
        if not names:
            return None
        system = getattr(self.cluster, "system", "lab")
        folder = getattr(self.cluster, "bulk_folder", None)  # M25: what `file` serves from
        if self._services is None or self._services[:3] != (system, names, folder):
            from .services import PcServices
            hub = getattr(self, "hub", None)
            on_file = ((lambda caller, path, name, data: hub.bulk.start(caller, path, name, data, self._bulk_rate()))
                       if hub is not None else None)
            self._services = (system, names, folder, PcServices(system, list(names), folder, on_file))
        return self._services[3]

    def _bulk_rate(self) -> float | None:
        """M25: the cluster file's bulk.rate (chunks a second), read afresh at each transfer: tuned without a restart."""
        try:
            doc = json.loads(Path(self.cluster.path).read_text(encoding="utf-8-sig"))
            r = (doc.get("bulk") or {}).get("rate")
            return float(r) if isinstance(r, (int, float)) and 1 <= r <= 200 else None
        except (OSError, ValueError, AttributeError):
            return None

    def _bulk_json(self) -> dict:
        """M25: the folder the PC offers, its files, and the reads of them (potluck.bulk)."""
        svc = self.pc_services()
        bulk = getattr(getattr(self, "hub", None), "bulk", None)
        return {"folder": getattr(self.cluster, "bulk_folder", None) if self.cluster is not None else None,
                "files": svc.files() if svc is not None else [], "reads": bulk.snapshot() if bulk is not None else []}

    def _jobs_json(self) -> list[dict]:
        """M22: each board's job on borrowed cores (M9's mc_job), from its telemetry: the run in progress or the last.
        `speedup` is measured -- the board's last run alone over its last run with lending -- or null until both ran."""
        out = []
        with self.lock:
            for n, b in sorted(self.boards.items()):
                job = ((b.get("tel") or {}).get("compute") or {}).get("job")
                if not job:
                    continue
                times = self.job_times.get(n, {})
                alone, lend = times.get("alone"), times.get("lend")
                speed = round(alone[1] / lend[1], 2) if alone and lend and lend[1] else None
                lenders = [dict(x, label=self.label_of(int(x["node"], 16))) for x in job["lenders"]]
                out.append(dict(job, id=f"{n:04x}/{job['run']}", borrower=f"{n:04x}", label=self.label_of(n),
                                lenders=lenders, redone=job["refused"] + job["lost"], speedup=speed))
        return out

    def _update_json(self, boards_fw: str | None) -> dict:
        """2026-10-20, the owner: an update of the boards as its own thing, not hidden in an Apply. The firmware the
        boards run (most of them), the one the installed Potluck carries (its release, setup.packaged_firmware), and
        whether they differ -- or a board runs another version than the rest."""
        from .setup import packaged_firmware
        available = None
        root = packaged_firmware()
        if root is not None and root.is_dir():
            found = []
            for d in root.iterdir():
                try:
                    meta = json.loads((d / "release.json").read_text(encoding="utf-8"))
                    found.append((d.stat().st_mtime, str(meta["version"])))
                except (OSError, ValueError, KeyError):
                    continue
            if found:
                available = max(found)[1]
        with self.lock:
            versions = {b["status"].fw_version for b in self.boards.values()
                        if b.get("status") is not None and b["state"] == "alive"}
        mixed = len(versions) > 1
        return {"boards": boards_fw, "available": available, "mixed": mixed,
                "pending": available is not None and (mixed or (boards_fw is not None and boards_fw != available))}

    def _panels_json(self, resources: list[dict]) -> list[dict]:
        """M20's app panels: what each app's descriptor asks the dashboard to show (modules.PANEL_KINDS), one entry per
        actor -- per room for a per_room module -- and only for a resource the package marks exposed (review item 11)."""
        if self.package is None:
            return []
        from . import modules as mods
        from .apply import room_slug
        res = {r["path"]: r for r in resources}
        rooms = sorted(set(getattr(self.cluster, "rooms", {}).values())) if self.cluster is not None else []
        m = self.package.manifest
        out = []
        for a in m.actors:
            spec = mods.get(a.module)
            if spec is None or not spec.panel:
                continue
            p = dict(spec.panel)
            path = f"potluck://{m.system}/" + p["output"].replace("{actor}", a.name)
            r = res.get(path)
            if r is None or not r.get("exposed"):
                continue
            room = next((x for x in rooms if a.name.endswith("_" + room_slug(x))), None) if spec.per_room else None
            v = r.get("value")
            on = (bool(v) if isinstance(v, (bool, int, float)) else None) if r.get("quality") == "GOOD" else None
            out.append({"kind": p["kind"], "app": a.name, "room": room, "resource": path, "label": p.get("label", ""),
                        "on_text": p.get("on", ""), "off_text": p.get("off", ""), "value": v,
                        "quality": r.get("quality"), "on": on})
        return out

    def _apps_json(self, boards: list[dict], resources: list[dict]) -> list[dict]:
        if self.package is None:
            return []
        from . import modules as mods
        from .apply import actor_reads
        quality = {r["path"]: r.get("settled_quality", r["quality"]) for r in resources}  # one late read is not a fault
        running_here = {b["node"]: set(b.get("actors", [])) for b in boards if b["state"] == "alive"}
        out = []
        m = self.package.manifest

        def placed(a) -> tuple[bool, bool, str]:  # (portable, every, the kind name NODE_STATUS lists it by)
            spec = mods.get(a.module)
            portable = a.module.startswith("guest:") or a.module in ("builtin:ticker", "builtin:present") or (
                spec is not None and spec.placement == "portable")
            every = spec is not None and spec.placement == "every"
            kind = a.module.split(":", 1)[1].split("/")[-1] if ":" in a.module else a.module
            kind = {"die_temp": "die_temp", "overheat_alarm": "guest"}.get(kind, kind)
            if a.module.startswith("guest:"):
                kind = "guest"
            return portable, every, kind

        # A board's list of actor names is cut at 64 bytes (pot/node_status.hpp: "may be cut short"). Found 2026-10-26:
        # C's eight pinned actors filled it with seven names, and pos_8160 read "not running on 8160" while its
        # position actor ran. A cut list whose board runs as many pinned actors as the package pins there names them all.
        from .apply import expected
        want = expected(m, [int(n, 16) for n in running_here], verify=False)[0]
        for b in boards:
            n = b["node"]
            if n in running_here and b.get("actors_cut") and b.get("pinned_running", 0) >= want.get(int(n, 16), 0):
                for a in m.actors:
                    portable, every, kind = placed(a)
                    if not portable and (every or m.placement_of(a) == int(n, 16)):
                        running_here[n].add(kind)
        for a in m.actors:
            spec = mods.get(a.module)
            portable, every, kind = placed(a)
            pin = m.placement_of(a)
            nodes = [f"{pin:04x}"] if pin is not None and not portable else [b["node"] for b in boards]
            running = [n for n, acts in running_here.items() if kind in acts and (portable or every or n in nodes)]
            if portable:
                # The boards' reconcilers name each portable actor's owner by its output's key: by actor, not by kind.
                # Found 2026-10-19: two guests of one module -- the store's overheat_alarm on A, the earlier alarm on
                # B -- read as one app "running on 2 nodes", and the other was not listed at all.
                from . import reconcile as rc
                key = f"{path_hash(rc.output_path(m, a)):08x}"
                seen = [(b["node"], t) for b in boards for t in (b.get("telemetry") or {}).get("actors", [])
                        if t.get("key") == key]
                if seen:
                    running = sorted({n for n, t in seen if t.get("running_here") and n in running_here})
            if portable and not running:
                # M14: a placement host runs it -- NODE_STATUS sees boards only, but every board's telemetry names the
                # owner its reconciler accepts (found on the bench: "running on 0 boards" while the PC ran the ticker).
                from . import reconcile as rc
                key = f"{path_hash(rc.output_path(m, a)):08x}"
                owners = {t["owner"] for b in boards for t in (b.get("telemetry") or {}).get("actors", [])
                          if t["key"] == key}
                if owners == {"00fe"}:
                    running = ["00fe"]
            outs = actor_reads(m, a, [int(n, 16) for n in nodes])[1]  # its own outputs, not its board's
            bad_outs = [p for p in outs if quality.get(p) not in (None, "GOOD", "STARTING")]
            if portable:
                ok, why = len(running) == 1, ("" if len(running) == 1 else f"running on {len(running)} nodes")
            else:
                want = nodes if not every else [b["node"] for b in boards if b["state"] == "alive"]
                ok = all(n in running for n in want)
                why = "" if ok else "not running on " + ", ".join(n for n in want if n not in running)
            if a.module == "builtin:present" and running == ["00fe"]:
                # 2026-10-21: the PC's host node holds the role. Its output reaches the hub as NO_DATA (a board's replica
                # of an output the PC owns is not refreshed -- open, M0-LOG session 48), so the host node's own report of
                # its broker connection is what says it works.
                rep = next((r for r in self.reports.snapshot() if r.get("name") == "presenting (MQTT, this PC)"), None)
                st = rep.get("state") if rep else None
                ok, why, bad_outs = st == "connected", ("" if st == "connected" else
                                                        f"this PC holds the role: {st or 'no report from it'}"), []
            if ok and bad_outs:
                ok, why = False, "output " + bad_outs[0].split("/", 3)[3] + " is " + quality.get(bad_outs[0], "?")
                if a.module == "builtin:inbox":  # 2026-10-20: its values are its program's to write
                    q = quality.get(bad_outs[0])
                    why = ("nothing written yet by its program" if q == "NO_DATA" else
                           "its program stopped writing" if q == "STALE" else why)
            parts = []
            if spec is not None and spec.parts:  # what it needs wired, with the GPIOs its config gives (2026-10-20)
                gpios = {(p, r): g for p, r, g in mods.actor_pins(spec, a.config)}
                parts = [{"part": p.part, "bus": p.bus, "pins": {r: gpios.get((p.part, r)) for r, _f in p.pins},
                          "guide": p.guide} for p in spec.parts]
            out.append({"name": a.name, "module": a.module,
                        "placement": "portable" if portable else ("every" if every else "pinned"),
                        "nodes": nodes, "running_on": running, "working": ok, "detail": why, "outputs": outs,
                        "parts": parts, "takes": list(takes_of(a.module))})
        return out

    def _integrations_json(self, boards: list[dict], resources: list[dict]) -> list[dict]:
        out = []
        for b in boards:
            for mod in b.get("modules", []):
                if mod["name"] == "present":
                    out.append({"name": "presenting (MQTT)", "kind": "board", "where": b["node"],
                                "state": "connected" if mod["state"] == "active" else mod["state"],
                                "since_at": b["since_at"], "detail": mod["reason"]})
        return out

    def _signals_json(self, boards: list[dict]) -> dict:
        xs = sorted(self.rtts)
        p50 = round(xs[len(xs) // 2] * 1000, 1) if xs else None
        p99 = round(xs[min(len(xs) - 1, int(len(xs) * 0.99))] * 1000, 1) if xs else None
        traffic = None
        if len(self.frames_at) >= 2:
            (t0, n0), (t1, n1) = self.frames_at[0], self.frames_at[-1]
            traffic = round((n1 - n0) / max(0.001, t1 - t0), 1)
        now = time.monotonic()
        errors = sum(1 for t in self.errors if now - t < 60)
        alive = [b for b in boards if b.get("actors") is not None]
        # pinned actors against pot/actor.hpp's 8 slots (the list of names may be cut short, and names portable ones too)
        slots = max((b.get("pinned_running", len(b.get("actors", []))) / 8 for b in alive), default=None)
        # M15: the worst board's internal heap used at its low-water mark, from telemetry.
        ram = [1 - b["heap"]["internal_min"] / (b["heap"]["internal_total_kib"] * 1024) for b in boards
               if b.get("heap") and b["heap"].get("internal_total_kib")]
        return {"latency_p50_ms": p50, "latency_p99_ms": p99, "traffic_frames_s": traffic, "errors_min": errors,
                "saturation": {"peers": round(len(boards) / 21, 2) if boards else None, "namespace": None,
                               "actor_slots": round(slots, 2) if slots is not None else None,
                               "internal_ram": round(max(ram), 2) if ram else None,
                               "airtime": None}}


def _cell_id(cluster) -> str | None:
    """The cell's identity, as the boards put it in every outside id: the CA fingerprint's 8 hex digits."""
    if cluster is None:
        return None
    try:
        from .enrol import ca_fingerprint
        from .signing import read_key
        return ca_fingerprint(read_key(str(cluster.keys["ca"])).public).hex()
    except Exception:
        return None


def _version() -> str:
    if getattr(sys, "frozen", False):  # the package: tools/build_package.ps1 writes the commit it was built from
        try:
            return (Path(sys.executable).parent / "version.txt").read_text(encoding="ascii").strip() or "unknown"
        except OSError:
            return "unknown"
    try:
        import subprocess
        return subprocess.run(["git", "rev-parse", "--short=7", "HEAD"], cwd=Path(__file__).parent,
                              capture_output=True, text=True, timeout=5).stdout.strip() or "unknown"
    except Exception:
        return "unknown"


# ---- the API ---------------------------------------------------------------------------------------------

def serve_api(model: Model, addr: str, sessions=None, tls=None, pairing=None) -> ThreadingHTTPServer:
    """The API. With `sessions` (potluck.hubauth.Sessions) every request but the login page needs a login, and a write
    needs the write right (M16); without, it is the PC's own user on 127.0.0.1. `tls`: an ssl.SSLContext."""
    host, _, p = addr.rpartition(":")
    from . import hubauth

    PUBLIC = {"/", "/index.html", "/app.js", "/plan.js", "/library.js", "/style.css", "/potluck.svg", "/potluck-small.svg", "/api/login", "/api/whoami", "/api/hello", "/hub-certificate.crt"}
    setup_state: dict = {}  # M20's setup wizard: its jobs, made on first use (owner actions, 127.0.0.1 only)

    def setup_jobs():
        from . import setup as st
        if "jobs" not in setup_state:
            setup_state["jobs"] = st.Jobs()
        return setup_state["jobs"]

    def store_json() -> dict:
        """M20's store as the Library shows it: on or off, and the apps installed from it."""
        from . import store as so
        cl = getattr(model, "cluster", None)
        if cl is None:
            return {"on": False, "apps": []}
        reg = so.load_registry(Path(cl.path).parent)
        return {"on": bool(reg["on"]), "apps": [dict(v, name=k) for k, v in sorted(reg["apps"].items())]}

    def library_json() -> dict:
        """M20's library as the page shows it: each app on or off, Home Assistant (the cell's presenting role) with its
        broker, and per resource the package names whether the outside may see it ("read") or also control it
        ("read_write"), and whether it could be controlled at all."""
        cl = getattr(model, "cluster", None)
        if cl is None:
            return {"apps": [], "home_assistant": None, "resources": []}
        snap = model.snapshot()
        m = getattr(getattr(model, "package", None), "manifest", None)
        prefix = f"potluck://{cl.system}/"
        from .manifest import writable_actor_paths
        inboxes = writable_actor_paths(m) if m is not None else set()  # writable by the actor's own declaration
        writers = getattr(cl, "writers", {}) or {}  # 2026-10-20: which programs may write it (null: any paired writer)
        res = []
        for r in snap["resources"]:
            decl = m.resource(r["path"]) if m is not None else None
            mode = cl.outside.get(r["path"], "off")
            w = writers.get(r["path"])
            res.append({"path": r["path"], "name": r["name"], "mode": mode if isinstance(mode, str) else "read_write",
                        "controllable": bool(r["path"] in inboxes or (decl is not None and
                                             decl.access in ("write", "read_write") and not decl.safety_relevant)),
                        "writers": list(w) if w else None})
        for path, mode in cl.outside.items():  # a rule for a resource the package no longer names (an app off)
            if not any(x["path"] == path for x in res):
                w = writers.get(path)
                res.append({"path": path, "name": path[len(prefix):] if path.startswith(prefix) else path,
                            "mode": mode, "controllable": False, "missing": True, "writers": list(w) if w else None})
        pr = cl.present
        return {"apps": [{"name": a.name, "enabled": a.enabled, "built_in": bool(a.components)} for a in cl.apps],
                "home_assistant": {"on": pr is not None, "broker": (pr or {}).get("broker"),
                                   "tls": bool((pr or {}).get("tls_ca"))},
                "resources": sorted(res, key=lambda x: x["name"])}

    def cell_keys() -> tuple[str, str] | None:
        """The CA key and the node registry beside the cluster's ca.pub (potluck.enrol's defaults)."""
        cl = getattr(model, "cluster", None)
        if cl is None or "ca" not in getattr(cl, "keys", {}):
            return None
        folder = Path(cl.keys["ca"]).parent
        return str(folder / "ca.key"), str(folder / "nodes.json")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a) -> None:  # quiet: the hub's own log is its events
            pass

        def setup(self) -> None:
            if tls is not None:
                self.request.settimeout(10)
                self.request.do_handshake()
                self.request.settimeout(None)
            super().setup()

        def end_headers(self) -> None:
            if sessions is not None:
                self.send_header("X-Frame-Options", "DENY")
                self.send_header("Referrer-Policy", "no-referrer")
                self.send_header("X-Content-Type-Options", "nosniff")
            super().end_headers()

        def _json(self, code: int, obj: Any, cookie: str | None = None) -> None:
            self._drain()
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            if cookie is not None:
                self.send_header("Set-Cookie", cookie)
            self.end_headers()
            self.wfile.write(body)

        def _who(self) -> tuple[str, str] | None:
            """(name, right) of this request: the PC's own user without a login, else a paired program's token (M17)
            or the session's."""
            if sessions is None:
                return "local", "write"
            token = hubauth.bearer_token(self.headers.get("Authorization"))
            if token is not None:
                return sessions.users.check_token(token)
            return sessions.get(hubauth.cookie_token(self.headers.get("Cookie")))

        def _allowed(self, path: str) -> bool:
            if sessions is None or path in PUBLIC or self._who() is not None:
                return True
            if path.startswith("/api/pair/") and pairing is not None:
                return True  # a program waiting for the owner's answer has no token yet (M17)
            self._json(401, {"error": "login", "result": "log in first"})
            return False

        def _client(self) -> str:
            """M18: whose share a request is -- a login or token by name; on 127.0.0.1 what the program calls itself
            (X-Potluck-Client), else its address and port's host."""
            if sessions is not None:
                who = self._who()
                if who is not None:
                    return who[0]
            return self.headers.get("X-Potluck-Client", "")[:64] or self.client_address[0]

        def _fair(self, kind: str) -> bool:
            fair = getattr(model, "fair", None)  # a replay has none: nothing behind it to protect
            if fair is None:
                return True
            wait = fair.take(self._client(), kind)
            if wait <= 0:
                return True
            body = json.dumps({"ok": False, "result": f"slow down: this program is over its share ({kind}s; "
                                                      f"HUB-API.md, fair share)"}).encode()
            self.send_response(429)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Retry-After", str(max(1, int(wait + 0.999))))
            self.end_headers()
            self.wfile.write(body)
            return False

        def _setup_cell(self, path: str) -> None:
            """M20's setup wizard, the cell's side: a board into the cluster file (or its room), and `apply`."""
            from . import setup as st
            cl = getattr(model, "cluster", None)
            if cl is None:
                self._json(409, {"ok": False, "result": "the hub was started without a cluster file"})
                return
            try:
                req = self._body()
            except ValueError:
                self._json(400, {"ok": False, "result": "expected a JSON object"})
                return
            if path == "/api/setup/broker_login":
                # 2026-10-20 (the Potluck Desk, ROADMAP item 6): a login for a broker that took anyone -- the hub's own
                # login file in tray.json, and each presenting board's over its USB console; then the tray's restart.
                from . import hubtls
                ports = req.get("ports", [])
                if not isinstance(ports, list) or not all(isinstance(p, str) and p for p in ports):
                    self._json(400, {"ok": False, "result": "expected {\"user\", \"password\", \"ports\"?: [...]}"})
                    return
                own = getattr(model.hub.transport, "port", None)
                try:
                    user, password = st.check_broker_login(req.get("user"), req.get("password"))
                    if own is not None and own in ports:
                        raise st.SetupError(f"{own} is the hub's own link to the cell")
                except st.SetupError as exc:
                    self._json(409, {"ok": False, "result": str(exc)})
                    return
                hubtls.set_broker_login(user, password)
                jobs = setup_jobs()
                started = {}
                for p in ports:
                    try:
                        started[p] = jobs.start("broker_login", p,
                                                lambda pct, p=p: {"saved": st.set_present_login(p, user, password)})
                    except st.SetupError as exc:
                        started[p] = None
                        model.record("broker_login", None, {"port": p, "result": str(exc)})
                restart = hubtls.runs_under_tray(cl.path)
                if restart:
                    def later(ids=[j for j in started.values() if j]):
                        for _ in range(600):  # the boards' console jobs first: a restart would cut them off
                            if all((jobs.get(j) or {}).get("state") != "running" for j in ids):
                                break
                            time.sleep(0.2)
                        model.restart_requested = True
                    threading.Thread(target=later, daemon=True, name="broker-login-restart").start()
                model.record("broker_login", None, {"ports": ports, "hub": True})
                self._json(200, {"ok": True, "jobs": started, "file": str(hubtls.login_path()),
                                 "restart": ("the hub stops when the boards' jobs end and the tray starts it again with "
                                             "the login" if restart else
                                             "the tray does not run this hub: start it again with --broker-login")})
                return
            if path == "/api/setup/listen":
                # 2026-10-20 (the Potluck Desk, HOME-ASSISTANT.md part B steps 1-2): the hub on the house network, or
                # back to the PC only -- its certificate made or kept, the tray's hub_args, then a restart by the tray.
                from . import hubtls
                on, address = req.get("on"), req.get("address")
                if not isinstance(on, bool) or not (address is None or isinstance(address, str)):
                    self._json(400, {"ok": False, "result": "expected {\"on\": true|false, \"address\"?: \"<IPv4>\"}"})
                    return
                try:
                    res = hubtls.set_listen(on, address)
                except ValueError as exc:
                    self._json(409, {"ok": False, "result": str(exc)})
                    return
                restart = hubtls.runs_under_tray(cl.path)
                res["restart"] = ("the hub stops now and the tray starts it again with these settings, within about "
                                  "half a minute" if restart else
                                  "the tray does not run this hub: start it again yourself for the change to apply")
                model.record("hub_listen", None, {"on": on, "address": res["address"], "certificate": res["certificate"]})
                self._json(200, {"ok": True, **res})
                if restart:
                    model.restart_requested = True
                return
            if path == "/api/setup/tree":
                # M19/M20's large setup: the cells proposed from what the boards measure (potluck.survey); with
                # "write": true, put into the cluster file -- every screen writes the cluster file, every step is apply.
                from . import survey as sv
                try:
                    chargers = {int(str(x), 16) for x in req.get("chargers", [])}
                    snap = model.snapshot()
                    boards, links, rooms = sv.links_from_model(snap)
                    p = sv.propose(boards, links, chargers, rooms)
                    unheard = [str(b.get("label") or b.get("node")) for b in snap.get("boards", [])
                               if b.get("state") not in (None, "alive")]
                    if unheard:
                        p.notes.append("not counted, not heard now: " + ", ".join(unheard))
                    if req.get("write"):
                        sv.write(cl.path, p)
                        from .apply import load_cluster
                        model.cluster = load_cluster(str(cl.path))
                except (st.SetupError, ValueError) as exc:
                    self._json(409, {"ok": False, "result": str(exc)})
                    return
                self._json(200, {"ok": True, "proposal": p.to_json(), "written": bool(req.get("write"))})
                return
            if path == "/api/setup/board":
                try:
                    if "plan" in req:
                        st.set_plan(cl.path, req["plan"])
                        entry = None
                    else:
                        entry = st.add_board(cl.path, str(req["node"]), req.get("label"), req.get("room"),
                                             req.get("position"))
                    from .apply import load_cluster
                    model.cluster = load_cluster(str(cl.path))  # the dashboard shows the new label and room
                except (st.SetupError, KeyError, ValueError) as exc:
                    self._json(409, {"ok": False, "result": str(exc)})
                    return
                self._json(200, {"ok": True, "board": entry})
                return
            if path == "/api/setup/job":
                code, body = setup_job(model, req)  # M22: start or stop the job on borrowed cores
                self._json(code, body)
                return
            if path == "/api/setup/update":
                # 2026-10-20, the owner: an update of the boards is not an Apply of the library. The same run (apply
                # brings firmware and package to what the cell's file and this Potluck say), as a job of its own.
                up = model._update_json(model.snapshot()["cell"]["firmware"])
                if not up["pending"]:
                    self._json(200, {"ok": True, "job": None, "result": f"the boards already run {up['boards']}"
                                     if up["available"] else "this Potluck carries no firmware to update to"})
                    return
            jobs = setup_jobs()
            try:
                phrase = req.get("passphrase")
                jid = jobs.start("update" if path == "/api/setup/update" else "apply", "cell", lambda pct: st.run_apply(
                    cl.path, pct, passphrase=str(phrase) if phrase else None))
            except st.SetupError as exc:
                self._json(409, {"ok": False, "result": str(exc)})
                return
            self._json(202, {"ok": True, "job": jid})

        def _setup_library(self, path: str) -> None:
            """M20's library: apps on and off, the broker tried, what Home Assistant may see and control."""
            from . import setup as st
            from .apply import ApplyError, load_cluster
            cl = getattr(model, "cluster", None)
            if cl is None:
                self._json(409, {"ok": False, "result": "make the cell first"})
                return
            try:
                req = self._body()
                if path == "/api/setup/broker":
                    import base64
                    broker = str(req.get("broker") or (cl.present or {}).get("broker") or "")
                    ca = (cl.present or {}).get("tls_ca")
                    use_tls = bool(req.get("tls", ca is not None))
                    from .hubtls import hub_login
                    login = hub_login()  # 2026-10-20: the hub's own, once it has one -- "a login is set" vs "refused"
                    res = st.broker_test(broker, base64.b64decode(ca) if (use_tls and ca) else None,
                                         *(login or (None, None)))
                    self._json(200, dict(res, with_login=login is not None))
                    return
                if "app" in req:
                    st.set_app(cl.path, str(req["app"]), bool(req.get("enabled", True)))
                elif "outside" in req and isinstance(req["outside"], dict):
                    st.set_outside(cl.path, {str(k): str(v) for k, v in req["outside"].items()})
                else:
                    self._json(400, {"ok": False, "result": "expected {\"app\", \"enabled\"} or {\"outside\": {...}}"})
                    return
                model.cluster = load_cluster(str(cl.path))
            except (st.SetupError, ApplyError, ValueError, KeyError) as exc:
                self._json(409, {"ok": False, "result": str(exc)})
                return
            self._json(200, dict(library_json(), ok=True))

        def _setup_store(self, path: str) -> None:
            """M20's store, the owner's on 127.0.0.1 only: switch it on, look at an app in a GitHub repository (the one
            request the hub makes outside), install it -- the author pinned to the repository and certified under the
            cell's CA, the vault unlocked for that one action -- or remove it. Install and remove write the cluster file;
            the Library's Apply sends them."""
            from . import store as so
            from .apply import ApplyError, load_cluster
            cl = getattr(model, "cluster", None)
            if cl is None:
                self._json(409, {"ok": False, "result": "make the cell first"})
                return
            cell = Path(cl.path).parent
            try:
                req = self._body()
                if path == "/api/setup/store":
                    so.set_on(cell, bool(req.get("on")))
                    self._json(200, dict(store_json(), ok=True))
                    return
                if not so.load_registry(cell)["on"]:
                    self._json(409, {"ok": False, "result": "the store is off: turn it on in the Library first"})
                    return
                if path == "/api/setup/store/remove":
                    so.remove(cl.path, str(req["name"]))
                    model.cluster = load_cluster(str(cl.path))
                    self._json(200, dict(store_json(), ok=True))
                    return
                if path == "/api/setup/store/feed":
                    feed = so.fetch_feed()
                    have = {a["repo"] for a in so.load_registry(cell)["apps"].values()}
                    for a in feed["apps"]:
                        a["installed"] = so.source(a["repo"], a["dir"]) in have
                    self._json(200, dict(feed, ok=True))
                    return
                repo, ref, folder = str(req["repo"]), str(req.get("ref") or "HEAD"), str(req.get("dir") or "")
                app, module = so.fetch(repo, ref, folder=folder)
                if path == "/api/setup/store/look":
                    reg = so.load_registry(cell)
                    pinned = reg["authors"].get(so.source(repo, folder))
                    author = ("new" if pinned is None else
                              "pinned" if pinned["key"] == app.author_key.hex() else "changed")
                    self._json(200, {"ok": True, "name": app.name, "description": app.description,
                                     "author_key": app.author_key.hex(), "author": author, "sha256": app.sha256,
                                     "inputs": list(app.inputs), "outputs": app.outputs,
                                     "candidates": so.candidates(app, model.snapshot()["resources"])})
                    return
                if path != "/api/setup/store/install":
                    self._json(404, {"error": "not found"})
                    return
                keys = cell_keys()
                if keys is None:
                    raise so.StoreError("the cell has no CA key to certify the author with")
                phrase = req.get("passphrase")

                def certify(author: int, key: bytes) -> bytes:
                    from . import enrol as en, keystore
                    from .signing import read_key
                    vault = Path(keys[0]).parent / "vault.json"
                    try:
                        if phrase and vault.exists():  # the owner's keys, unlocked for this one action
                            keystore.unlock(vault, str(phrase))
                        return en.build_cert(read_key(keys[0]), author, key, role=en.ROLE_GUEST)
                    finally:
                        keystore.lock()
                r = so.install(cl.path, repo, ref, app, module, [str(b) for b in req.get("bindings", [])], certify,
                               folder=folder)
                model.cluster = load_cluster(str(cl.path))
                self._json(200, dict(store_json(), ok=True, installed=r))
            except (so.StoreError, ApplyError, ValueError, KeyError, OSError) as exc:
                self._json(409, {"ok": False, "result": str(exc)})

        def _setup_first(self, path: str) -> None:
            """M20's first run: make the cell (keys sealed under the owner's passphrase, an empty cluster file), then
            attach the hub to the first board's own USB port once that board is flashed and enrolled."""
            from . import setup as st
            from .apply import ApplyError, default_cache, load_cluster
            from .transport import NullTransport, SerialTransport
            try:
                req = self._body()
            except ValueError:
                self._json(400, {"ok": False, "result": "expected a JSON object"})
                return
            try:
                if path == "/api/setup/newcell":
                    if getattr(model, "cluster", None) is not None:
                        self._json(409, {"ok": False, "result": "this hub already runs a cell"})
                        return
                    system = str(req.get("system", ""))
                    folder = default_cache().parent / "cells" / system  # where the tray looks (tray.cell_file)
                    cluster = st.new_cell(folder, system, str(req.get("passphrase", "")))
                    model.cluster = load_cluster(str(cluster))
                    self._json(200, {"ok": True, "cluster": str(cluster)})
                    return
                port = str(req["port"])
                if not isinstance(model.hub.transport, NullTransport):
                    self._json(409, {"ok": False, "result": "the hub is already attached to the cell"})
                    return
                cl = getattr(model, "cluster", None)
                if cl is None:
                    self._json(409, {"ok": False, "result": "make the cell first"})
                    return
                doc = json.loads(Path(cl.path).read_text(encoding="utf-8"))
                doc["port"], doc["baud"] = port, st.CONSOLE_BAUD  # the board's own USB cable: the console's rate
                st._write_cluster(Path(cl.path), doc)
                model.cluster = load_cluster(str(cl.path))
                model.hub.attach(SerialTransport(port, st.CONSOLE_BAUD))
                ack = model.hub.hello(timeout=3.0)
                self._json(200, {"ok": True, "node": f"{ack.node_id:04x}" if ack else None})
            except (st.SetupError, ApplyError, KeyError, OSError, ValueError) as exc:
                self._json(409, {"ok": False, "result": str(exc)})

        def _setup_post(self, path: str) -> None:
            """M20's setup wizard: probe, flash, enrol, Wi-Fi, identify -- each a job the dashboard follows."""
            from . import setup as st
            if path in ("/api/setup/board", "/api/setup/apply", "/api/setup/tree", "/api/setup/listen",
                        "/api/setup/broker_login", "/api/setup/update", "/api/setup/job"):
                self._setup_cell(path)
                return
            if path in ("/api/setup/library", "/api/setup/broker"):
                self._setup_library(path)
                return
            if path.startswith("/api/setup/store"):
                self._setup_store(path)
                return
            if path in ("/api/setup/newcell", "/api/setup/attach"):
                self._setup_first(path)
                return
            try:
                req = self._body()
                port = str(req["port"])
            except (ValueError, KeyError):
                self._json(400, {"ok": False, "result": "expected {\"port\": ...}"})
                return
            if port == getattr(model.hub.transport, "port", None):
                self._json(409, {"ok": False, "result": "that is the hub's own link to the cell"})
                return
            jobs = setup_jobs()
            try:
                if path == "/api/setup/probe":
                    cl = getattr(model, "cluster", None)

                    def probe_job(pct, port=port, cluster=cl.path if cl is not None else None):
                        r = st.probe(port)
                        # a board the cell had, wiped and plugged in again: the wizard asks the owner (2026-10-23)
                        r["known"] = st.known_board(cluster, r.get("mac")) if cluster else None
                        return r
                    jid = jobs.start("probe", port, probe_job)
                elif path == "/api/setup/identify":
                    secs = int(req.get("seconds", 20))
                    jid = jobs.start("identify", port, lambda pct: st.identify(port, secs))
                elif path == "/api/setup/flash":
                    from .apply import default_cache
                    build = Path(str(req["build"])).resolve()
                    root = default_cache().resolve()
                    if root not in build.parents:  # only a build the hub's own tools made
                        self._json(403, {"ok": False, "result": f"a build under {root}, nothing else"})
                        return
                    parts = st.flash_parts(build)
                    jid = jobs.start("flash", port, lambda pct: st.flash(port, parts, pct) or {"flashed": str(build)})
                elif path == "/api/setup/reset":
                    jid = jobs.start("reset", port, lambda pct: st.reset(port))
                elif path == "/api/setup/wifi":
                    ssid, pw = st.check_wifi(req.get("ssid"), req.get("password"))  # 409 with why, before any job
                    jid = jobs.start("wifi", port, lambda pct: {"saved": st.set_wifi(port, ssid, pw)})
                elif path == "/api/setup/enrol":
                    keys = cell_keys()
                    if keys is None:
                        self._json(409, {"ok": False, "result": "the hub was started without a cluster file"})
                        return
                    ca_key, registry = keys
                    passphrase = req.get("passphrase")
                    wanted = req.get("node_id")
                    replace = bool(req.get("replace")) and bool(wanted)  # the owner said: the cell's board, reinstalled

                    def run(pct, ca_key=ca_key, registry=registry, passphrase=passphrase, wanted=wanted, replace=replace):
                        from . import keystore
                        vault = Path(ca_key).parent / "vault.json"
                        try:
                            if passphrase and vault.exists():  # the owner's keys, unlocked for this one action
                                keystore.unlock(vault, str(passphrase))
                            return st.enrol_board(port, ca_key, registry, int(wanted, 16) if wanted else None, replace)
                        finally:
                            keystore.lock()
                    jid = jobs.start("enrol", port, run)
                else:
                    self._json(404, {"error": "not found"})
                    return
            except (st.SetupError, ValueError, KeyError) as exc:
                self._json(409, {"ok": False, "result": str(exc)})
                return
            self._json(202, {"ok": True, "job": jid})

        def _local_host_ok(self) -> bool:
            """On the PC's own address (no logins: whoever is there is the owner), only a request that names it: a page
            elsewhere could otherwise reach it through DNS rebinding (a name of its own resolving to 127.0.0.1).
            M20 review #5."""
            if sessions is not None:
                return True
            host = (self.headers.get("Host") or "").rsplit(":", 1)[0].strip("[]").lower()
            if host in ("127.0.0.1", "localhost", "::1"):
                return True
            self._json(403, {"ok": False, "result": "this address answers only to 127.0.0.1 or localhost"})
            return False

        def do_GET(self) -> None:
            path, _, query = self.path.partition("?")
            if not self._local_host_ok():
                return
            if not self._allowed(path):
                return
            if path.startswith("/api/") and not self._fair("read"):
                return
            if path.startswith("/api/pair/") and sessions is not None and pairing is not None:
                got = pairing.fetch(path[len("/api/pair/"):])
                if got is None:
                    self._json(404, {"error": "not found", "result": "no such request (expired, or already fetched)"})
                else:
                    self._json(200, got)
                return
            if path == "/hub-certificate.crt":
                # 2026-10-19: the hub's own certificate (already a CA naming its addresses), with no login -- nothing
                # secret, its fingerprint is announced too. A phone that trusts it once loads the page without a
                # certificate error, and only then does Chrome offer to save the password (none since Chrome 37).
                der = getattr(model, "tls_der", None)
                if der is None:
                    self._json(404, {"error": "not found", "result": "this hub has no certificate (started without --listen)"})
                    return
                self.send_response(200)
                self.send_header("Content-Type", "application/x-x509-ca-cert")
                self.send_header("Content-Disposition", 'attachment; filename="potluck-hub.crt"')
                self.send_header("Content-Length", str(len(der)))
                self.end_headers()
                self.wfile.write(der)
                return
            if path == "/api/hello":
                # M17: who this hub is, with no login -- nothing secret: what it also announces on the network.
                cl = getattr(model, "cluster", None)
                self._json(200, {"api": API_VERSION, "cell_id": getattr(model, "cell_id", None),
                                 "system": cl.system if cl is not None else None,
                                 "version": getattr(model, "version", None),
                                 "tls_fp": getattr(model, "tls_fp", None)})
                return
            if path.startswith("/api/setup/"):
                if sessions is not None:  # owner actions: the PC's own address, never --listen (Review of M20 #5)
                    self._json(404, {"error": "not found"})
                    return
                from . import setup as st
                if path == "/api/setup/ports":
                    own = getattr(model.hub.transport, "port", None)
                    try:
                        self._json(200, {"ports": st.list_ports(exclude=(own,) if own else ())})
                    except ImportError:  # a hub run from source without pyserial: say so rather than drop the request
                        self._json(200, {"ports": [], "result": "this hub cannot list USB ports (pyserial is not "
                                                                 "installed)"})
                elif path == "/api/setup/build":
                    # the firmware the cell runs (most of its boards), and its build on this PC, for new boards
                    vers = [b.get("firmware", {}).get("version") for b in model.snapshot()["boards"]]
                    vers = [v for v in vers if v]
                    if not vers:  # M20's first run: no board yet -- the newest firmware built on this PC
                        try:
                            self._json(200, {"version": None, "build": str(st.newest_build()),
                                             "note": "the newest firmware built on this PC"})
                        except st.SetupError as exc:
                            self._json(409, {"ok": False, "result": str(exc)})
                        return
                    v = max(set(vers), key=vers.count)
                    try:
                        self._json(200, {"version": v, "build": str(st.cell_build(v))})
                    except st.SetupError as exc:
                        self._json(409, {"ok": False, "version": v, "result": str(exc)})
                elif path == "/api/setup/cell":
                    cl, keys = getattr(model, "cluster", None), cell_keys()
                    self._json(200, {"system": cl.system if cl is not None else None,
                                     "boards": [{"node": f"{n:04x}", "label": lab, "room": model.room_of(n)}
                                                for n, lab in (cl.boards if cl is not None else [])],
                                     "max_boards": st.MAX_BOARDS,
                                     "vault": bool(keys) and (Path(keys[0]).parent / "vault.json").exists()})
                elif path == "/api/setup/library":
                    self._json(200, library_json())
                elif path == "/api/setup/store":
                    self._json(200, store_json())
                elif path == "/api/setup/jobs":
                    self._json(200, {"jobs": setup_jobs().all()})
                elif path.startswith("/api/setup/jobs/"):
                    j = setup_jobs().get(path.rsplit("/", 1)[1])
                    self._json(200 if j else 404, j or {"error": "no such job"})
                else:
                    self._json(404, {"error": "not found"})
                return
            if path == "/api/whoami":
                who = self._who()
                self._json(200, {"api": API_VERSION, "login": sessions is not None,
                                 "name": who[0] if who else None, "right": who[1] if who else None})
            elif path == "/api/model":
                self._json(200, model.snapshot())
            elif path == "/api/events":
                since = None
                for kv in query.split("&"):
                    k, _, v = kv.partition("=")
                    if k == "since" and v.isdigit():
                        since = int(v)
                with model.lock:
                    evs = list(model.events)
                    nxt = model.seq + 1
                evs = [e for e in evs if since is None or e["seq"] >= since][-200 if since is None else None:]
                self._json(200, {"api": API_VERSION, "next": nxt, "events": evs})
            elif path == "/api/stream":
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "close")
                self.end_headers()
                last = 0
                try:
                    while True:
                        snap = model.snapshot()
                        self.wfile.write(b"event: model\ndata: " + json.dumps(snap).encode() + b"\n\n")
                        with model.lock:
                            evs = [e for e in model.events if e["seq"] > last]
                        if evs:
                            last = evs[-1]["seq"]
                            body = {"api": API_VERSION, "next": last + 1, "events": evs[-200:]}  # GET /api/events' shape
                            self.wfile.write(b"event: events\ndata: " + json.dumps(body).encode() + b"\n\n")
                        self.wfile.flush()
                        time.sleep(model.pull_period_ms / 1000)
                except OSError:
                    return
            else:
                self._static(path)

        def _static(self, path: str) -> None:
            name = "index.html" if path in ("/", "") else path.lstrip("/")
            f = (DASHBOARD_DIR / name).resolve()
            if DASHBOARD_DIR.resolve() not in f.parents or not f.is_file():
                self._json(404, {"error": "not found"})
                return
            body = f.read_bytes()
            ctype = {".html": "text/html; charset=utf-8", ".js": "text/javascript", ".css": "text/css",
                     ".svg": "image/svg+xml", ".json": "application/json"}.get(f.suffix, "application/octet-stream")
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")  # revalidate: an upgraded hub's page must not run old files
            self.send_header("Content-Security-Policy", DASHBOARD_CSP)
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(body)

        def _body(self) -> dict:
            n = int(self.headers.get("Content-Length", "0"))
            if n > 4096:
                raise ValueError("too large")
            self._body_read = True
            return json.loads(self.rfile.read(n) or b"{}")

        def _drain(self) -> None:
            """A reply sent before the request's body was read reads the body first. Unread, it is taken for the next
            request on a kept-alive connection, and on Windows the client is often reset (WinError 10053) instead of
            getting the reply -- found by the Potluck Desk 2026-10-18 on an unknown path's 404."""
            if getattr(self, "_body_read", True):
                return
            self._body_read = True
            try:
                n = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                return
            if 0 < n <= 65536:
                self.rfile.read(n)
            elif n > 65536:
                self.close_connection = True  # too much to read for a refusal: end the connection after the reply

        def do_POST(self) -> None:
            self._body_read = False  # per request: a kept-alive connection carries several
            if not self._local_host_ok():
                return
            if self.headers.get("X-Potluck-Request") != "1":
                # A page on another site can make a browser POST here -- with its cookie behind a login, or to the PC's
                # own address with no login at all (it used to be exempt: M20 review #5) -- but not with a custom
                # header unless this server allowed it (CORS), which it never does.
                self._json(403, {"ok": False, "result": "missing X-Potluck-Request"})
                return
            if not self._fair("read"):
                return
            if self.path == "/api/integrations":
                # M18: an integration says how it is; any login or token may (it changes nothing in the cell).
                if self._who() is None:
                    self._json(401, {"error": "login", "result": "log in first"})
                    return
                try:
                    req = self._body()
                    ok = model.reports.report(str(req["name"]), str(req["state"]), str(req.get("detail", "")),
                                              self._client())
                except (ValueError, KeyError, AttributeError):
                    ok = False
                if not ok:
                    self._json(400, {"ok": False, "result": "expected {\"name\", \"state\": connected|disconnected|"
                                                            "error|idle, \"detail\"}"})
                    return
                self._json(200, {"ok": True})
                return
            if self.path == "/api/pair" and sessions is not None and pairing is not None:
                # M17: a program asks to be paired; the owner answers on the PC (POST /api/pairing on 127.0.0.1).
                try:
                    req = self._body()
                except ValueError:
                    req = {}
                pid = pairing.request(str(req.get("name", "")), str(req.get("right", "look")), self.client_address[0])
                if pid is None:
                    self._json(429, {"ok": False, "result": "too many pairing requests waiting"})
                    return
                model.record("pair_request", None, {"name": req.get("name"), "from": self.client_address[0]})
                self._json(202, {"pair_id": pid, "expires_in_s": hubauth.Pairing.TTL_S})
                return
            if self.path == "/api/pairing" and sessions is None and pairing is not None:
                # The owner's answer -- the PC's own address only, never on --listen.
                try:
                    req = self._body()
                    pid, approve = str(req["pair_id"]), bool(req["approve"])
                except (ValueError, KeyError):
                    self._json(400, {"ok": False, "result": "expected {\"pair_id\": ..., \"approve\": true|false}"})
                    return
                ok = pairing.answer(pid, approve)
                model.record("pair_answer", None, {"pair_id": pid, "approve": approve, "ok": ok})
                self._json(200 if ok else 404, {"ok": ok})
                return
            if self.path.startswith("/api/setup/") and sessions is None:
                self._setup_post(self.path)
                return
            if self.path == "/api/integrations/forget" and sessions is None:
                # The owner's, on the PC's own address only: a program that stopped for good, or a test, stops being
                # listed as down. A connected one is refused (409).
                try:
                    name = str(self._body()["name"])
                except (ValueError, KeyError):
                    self._json(400, {"ok": False, "result": "expected {\"name\": ...}"})
                    return
                ok = model.reports.forget(name)
                model.record("integration_forgotten", None, {"name": name, "ok": ok})
                self._json(200 if ok else 409, {"ok": ok})
                return
            if self.path == "/api/login" and sessions is not None:
                try:
                    req = self._body()
                    name, password = str(req["name"]), str(req["password"])
                except (ValueError, KeyError):
                    self._json(400, {"ok": False, "result": "expected {\"name\": ..., \"password\": ...}"})
                    return
                token, result = sessions.login(name, password, self.client_address[0])
                model.record("login", None, {"name": name, "from": self.client_address[0],
                                             "result": "ok" if token else result})
                if token is None:
                    self._json(403, {"ok": False, "result": result})
                    return
                self._json(200, {"ok": True, "name": name, "right": result},
                           cookie=f"{hubauth.COOKIE}={token}; Path=/; HttpOnly; Secure; SameSite=Strict; "
                                  f"Max-Age={hubauth.SESSION_S}")
                return
            if self.path == "/api/logout" and sessions is not None:
                sessions.logout(hubauth.cookie_token(self.headers.get("Cookie")))
                self._json(200, {"ok": True}, cookie=f"{hubauth.COOKIE}=; Path=/; HttpOnly; Secure; SameSite=Strict; "
                                                     f"Max-Age=0")
                return
            if self.path == "/api/replay" and hasattr(model, "control"):
                # M16: moving a replay (potluck.hubreplay) -- a look right is enough, it changes nothing in a cell.
                if self._who() is None:
                    self._json(401, {"error": "login", "result": "log in first"})
                    return
                try:
                    req = self._body()
                    model.control(seek_at=req.get("seek_at"), speed=req.get("speed"), paused=req.get("paused"))
                except (ValueError, TypeError):
                    self._json(400, {"ok": False, "result": "expected {seek_at, speed, paused}"})
                    return
                self._json(200, {"ok": True, "replay": model.snapshot()["hub"]["replay"]})
                return
            if self.path == "/api/interest" and hasattr(model, "want"):
                # On-demand reads (Model.want): a look right is enough -- it reads what the cell already publishes and
                # changes nothing in it. The values arrive in the model like any other resource, "on_demand": true.
                if self._who() is None:
                    self._json(401, {"error": "login", "result": "log in first"})
                    return
                try:
                    req = self._body()
                    paths, ttl = req["paths"], req.get("ttl_s", 30)
                    if not isinstance(paths, list) or not isinstance(ttl, (int, float)) or isinstance(ttl, bool) \
                            or not math.isfinite(ttl):
                        raise ValueError
                except (ValueError, KeyError, AttributeError):
                    self._json(400, {"ok": False, "result": "expected {\"paths\": [...], \"ttl_s\": seconds}"})
                    return
                accepted, refused = model.want(self._client(), paths, ttl)
                lo, hi = model.INTEREST_TTL_S
                self._json(200, {"ok": True, "accepted": accepted, "refused": refused,
                                 "ttl_s": min(max(float(ttl), lo), hi)})
                return
            if self.path not in ("/api/write", "/api/input"):
                self._json(404, {"error": "not found"})
                return
            who = self._who()
            if who is None:
                self._json(401, {"error": "login", "result": "log in first"})
                return
            if who[1] != "write":
                model.record("outside_write", None, {"by": who[0], "result": "refused: this login may only look"})
                self._json(403, {"ok": False, "result": "this login may look, not write"})
                return
            if not self._fair("input" if self.path == "/api/input" else "write"):
                return
            if self.path == "/api/input":
                try:
                    req = self._body()
                except ValueError:
                    req = None
                self._json(*input_events(model, req))  # M24
                return
            try:
                req = self._body()
                path, value = req["path"], req["value"]
            except (ValueError, KeyError):
                self._json(400, {"ok": False, "result": "expected {\"path\": ..., \"value\": ...}"})
                return
            self._json(*write_outside(model, path, value, by=None if sessions is None else who[0]))

    srv = ThreadingHTTPServer((host or "127.0.0.1", int(p)), Handler)
    if tls is not None:
        # The handshake runs in the connection's own thread (Handler.setup), so a slow client never holds accept().
        srv.socket = tls.wrap_socket(srv.socket, server_side=True, do_handshake_on_connect=False)
        srv.handle_error = lambda request, client_address: None  # a scanner's failed handshake is not news
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, name="potluck-hub-api", daemon=True).start()
    return srv


def takes_of(module: str) -> tuple[str, ...]:
    """M24: the events an app of this module takes -- a built-in's own, or an external module's "takes"."""
    from . import events as ev
    from . import modules as mods
    if module in ev.BUILTIN_TAKES:
        return ev.BUILTIN_TAKES[module]
    spec = mods.get(module)
    return tuple(getattr(spec, "takes", ()) or ())


def input_events(model, req) -> tuple[int, dict]:
    """M24: POST /api/input {"app", "events": [...]} -- queued for the app's input, numbered, and delivered in order
    (potluck.events). 202 with the first event's number; 400 a bad request, 404 no such app, 409 an app that takes no
    events or has no board, 429 too many waiting."""
    from . import deploy as dp
    from . import events as ev
    if not isinstance(req, dict) or not isinstance(req.get("app"), str) or "events" not in req:
        return 400, {"ok": False, "result": 'expected {"app": ..., "events": [...]}'}
    pkg = getattr(model, "package", None)
    m = pkg.manifest if pkg is not None else None
    a = next((x for x in (m.actors if m is not None else []) if x.name == req["app"]), None)
    if a is None:
        return 404, {"ok": False, "result": "no such app in this cell's package"}
    takes = takes_of(a.module)
    if not takes:
        return 409, {"ok": False, "result": "this app takes no events (its module does not say \"takes\")"}
    owner = m.placement_of(a)
    if owner is None:
        return 409, {"ok": False, "result": "this app has no fixed board to deliver to (pin it)"}
    try:
        evs = ev.check_events(req["events"], takes)
        q = model.hub.inputs.queue(a.name, owner, path_hash(dp.input_path(m.system, a.name)), takes)
        first = q.put(evs, time.time())
    except ev.InputError as exc:
        code = 429 if "already waiting" in str(exc) else 400
        return code, {"ok": False, "result": str(exc)}
    return 202, {"ok": True, "first": first, "count": len(evs), "board": f"{owner:04x}"}


_TYPE_NAMES = {1: "bool", 2: "i32", 3: "u32", 4: "f32", 5: "i64", 6: "u64", 7: "f64", 8: "bytes"}


def _inbox_type(model, path: str) -> int:
    """The declared value type of a value written from outside before the hub has read it -- an inbox's, a speak
    actor's request, a job's control (pot/value.hpp's ValueType code) -- or 0."""
    pkg = getattr(model, "package", None)
    if pkg is None:
        return 0
    from . import deploy as dp
    m = pkg.manifest
    for a in m.actors:
        node = m.placement_of(a)
        if (a.module == "builtin:speak" and path == dp.speak_paths(m.system, a.name)[0]) or (
                a.module == "builtin:file_hash" and path == dp.file_hash_paths(m.system, a.name)["go"]) or (
                a.module == "builtin:mc_job" and node is not None and path == dp.mc_job_ctl_path(m.system, node)):
            return 3  # U32: M23's request and M22's job control, written before the hub has read them
        if a.module != "builtin:inbox":
            continue
        try:
            for n, typ, _unit in dp.inbox_outputs(a, a.name):
                if dp.inbox_path(m.system, a.name, n) == path:
                    return typ
        except dp.DeployError:
            continue
    return 0


def _inbox_owner(model, path: str) -> int | None:
    """The board an inbox is pinned to, if `path` is one of its values (builtin:inbox): written from outside like a
    board's own resource. Found 2026-10-21 (the owner's phone-app session, reading HUB-API.md): only node-XXXX/ paths
    were let through, so every inbox write was refused."""
    pkg = getattr(model, "package", None)
    if pkg is None:
        return None
    from . import deploy as dp
    m = pkg.manifest
    for a in m.actors:
        if (a.module == "builtin:speak" and path == dp.speak_paths(m.system, a.name)[0]) or (
                a.module == "builtin:file_hash" and path == dp.file_hash_paths(m.system, a.name)["go"]):
            return m.placement_of(a)  # M23's say, M25's go -- a board's own resource too
        if a.module != "builtin:inbox":
            continue
        try:
            if path in {dp.inbox_path(m.system, a.name, n) for n, *_ in dp.inbox_outputs(a, a.name)}:
                return m.placement_of(a)
        except dp.DeployError:
            continue
    return None


def setup_job(model: Model, req: dict) -> tuple[int, dict]:
    """M22: POST /api/setup/job {"run": "lend" | "alone" | "stop", "board"?} -- start or stop the cell's job on borrowed
    cores (an mc_job with "control": true) by writing its control resource, through the same checks as any outside
    write: the cluster file's `outside` must let it be written."""
    from . import deploy as dp
    runs = {"lend": 1, "alone": 2, "stop": 3}
    what = str(req.get("run", ""))
    if what not in runs:
        return 400, {"ok": False, "result": 'expected {"run": "lend" | "alone" | "stop"}'}
    m = model.package.manifest if getattr(model, "package", None) is not None else None
    nodes = [m.placement_of(a) for a in (m.actors if m is not None else [])
             if a.module == "builtin:mc_job" and a.config.get("control")]
    want = req.get("board")
    if want:
        nodes = [x for x in nodes if x is not None and f"{x:04x}" == str(want).lower()]
    if not nodes or nodes[0] is None:
        return 404, {"ok": False, "result": "no job with a control resource in this cell's package"}
    code, body = write_outside(model, dp.mc_job_ctl_path(m.system, nodes[0]), runs[what])
    return code, dict(body, board=f"{nodes[0]:04x}")


def write_outside(model: Model, path: str, value: Any, by: str | None = None) -> tuple[int, dict]:
    """POST /api/write: only a resource the package lets the outside write, never a safety-relevant one; sent to the
    owning board as a WRITE marked from outside, which checks the package again. M18: a resource whose cluster file
    names its writers may be written only by those programs (`by`: the login or token's name; None is the PC's own
    user on 127.0.0.1, the owner)."""
    cl = model.cluster
    mode = cl.outside.get(path) if cl is not None else None
    paths = getattr(model, "resource_paths", None)
    if callable(paths) and getattr(model, "package", None) is not None and path not in paths():
        # 2026-10-21: not a resource of this cell's package -- an app that is off, or values not provisioned yet (an
        # inbox before its Apply); a program can tell this from a refusal.
        return 404, {"ok": False, "result": "no such resource in this cell (not in its package: an app that is off, or "
                                            "not provisioned yet)"}
    writers = getattr(cl, "writers", {}).get(path) if cl is not None else None
    if writers and by is not None and by not in writers:
        model.record("outside_write", None, {"resource": path, "path": path, "value": value, "by": by,
                                             "result": "refused: not one of its writers"})
        why = "not writable by this program"
        base, _, n = by.rpartition(" ")
        if n.isdigit() and base in writers:
            # Paired again without revoking first, it got "<name> 2" (pairing never reuses a name) -- found by the
            # Potluck Desk, 2026-10-20: every write refused while the dashboard showed the program paired.
            why = (f"not writable by this program: it is paired as {by!r} and the writers list names {base!r} -- revoke "
                   f"the old pairing and pair again, or add {by!r} to the cluster file's writers")
        return 403, {"ok": False, "result": why}
    if mode != "read_write" or model._safety(path):
        model.record("outside_write", None, {"resource": path, "path": path, "value": value, "result": "refused: not writable from outside"})
        return 403, {"ok": False, "result": "not writable from outside"}
    with model.lock:
        cur = model.resources.get(path)
    # M18: a number is written as the resource's own type when the hub knows it, so a client never has to tell 20 from
    # 20.0 (JSON cannot): 20 to a float resource is 20.0, 20.0 to an integer one is 20.
    known = cur["reading"].value.type if cur is not None and cur.get("reading") is not None else 0
    if not known:
        # 2026-10-21: nothing read yet -- an inbox's value is NO_DATA until its program first writes it -- so its
        # declared type, from the package (a phone's first heart rate of 72 must go out as the f32 it is).
        known = _inbox_type(model, path)
    whole = isinstance(value, (int, float)) and not isinstance(value, bool) and float(value).is_integer()
    if isinstance(value, bool):
        v = Value.of_bool(value)
    elif isinstance(value, (int, float)) and known == 4:  # F32
        v = Value.of_f32(float(value))
    elif isinstance(value, (int, float)) and known == 7:  # F64
        v = Value.of_f64(float(value))
    elif whole and known in (2, 3):  # I32, U32
        v = Value.of_u32(int(value)) if known == 3 and hasattr(Value, "of_u32") else Value.of_i32(int(value))
    elif whole and known in (5, 6):  # I64, U64 (an inbox's step count, say)
        v = Value.of_u64(int(value)) if known == 6 else Value.of_i64(int(value))
    elif isinstance(value, int):
        v = Value.of_i32(value)
    elif isinstance(value, float):
        v = Value.of_f32(value)
    else:
        return 400, {"ok": False, "result": "a bool or a number"}
    name = path.split("/", 3)[3]
    owner = int(name[5:9], 16) if name.startswith("node-") else _inbox_owner(model, path)
    if owner is None:
        return 400, {"ok": False, "result": "only a board's own resource or an inbox's value can be written from outside"}
    h = path_hash(path)
    try:
        # To the cabled board, which passes a write for another board's resource to its owner and returns the owner's
        # answer (Node::proxy_write). Addressed to the owner itself it was never answered: the PC on the cable is no
        # peer of the other boards (found 2026-10-18 by the Potluck Desk: every write to B's LED timed out).
        raw = model.hub._request(fr.Op.WRITE, Write(path_hash=h, value=v, from_outside=True).encode(), h,
                                 timeout=2.0, dst=None)
    except RequestTimeout:
        model.record("outside_write", owner, {"resource": path, "path": path, "value": value, "result": "no answer"})
        return 504, {"ok": False, "result": "the owning board did not answer"}
    except BridgeError:
        # No board on the PC's link yet (the hub started before the cabled board answered, or the cable is out): an
        # answer, not a dropped connection -- found 2026-10-24 smoke-testing M22's job route on the packaged hub.
        model.record("outside_write", owner, {"resource": path, "path": path, "value": value, "result": "no board"})
        return 503, {"ok": False, "result": "no board answers on the PC's link"}
    from .value import NsError
    ok = raw.status == NsError.OK
    result = "applied" if ok else NsError.name_of(raw.status).lower()
    if not ok and result == "wrong_owner" and owner != model.hub.peer_node_id:
        # The cabled board did not pass it on: firmware before it could (2026-10-18), or the owner is not alive there.
        result = "the board on the cable could not pass it to its owner (owner not alive, or firmware too old)"
    model.record("outside_write", owner, {"resource": path, "path": path, "value": value, "result": result})
    del cur
    if not ok and raw.status == NsError.NOT_FOUND:
        # The package has it, the board not (yet): mid-Apply. Absent, not forbidden -- a 403 here told a program to stop
        # writing a value it should send again (found by the owner's phone-app session, 2026-10-21).
        return 404, {"ok": False, "result": result}
    return (200 if ok else 403), {"ok": ok, "result": result}


# ---- the program ----------------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    if os.name == "nt":
        # No Windows error box from a program the hub starts (an apply's ESP-IDF build, esptool): a tool missing a DLL
        # fails instead of waiting, unseen, for a click -- found 2026-10-19, an apply from the Desk stopped on one from
        # qemu-system-xtensa (libiconv-2.dll) during ESP-IDF's tool check. SEM_FAILCRITICALERRORS |
        # SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX, inherited by every child process.
        try:
            import ctypes
            ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)
        except (AttributeError, OSError):
            pass
    ap = argparse.ArgumentParser(prog="potluck-hub", description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="the serial port of the PC's adapter (e.g. COM6)")
    ap.add_argument("--tcp", help="or a TCP endpoint carrying the frame link (an emulator)")
    ap.add_argument("--baud", type=int, default=None,
                    help="the link's rate: default the cluster file's \"baud\" for its own port, else 921600")
    ap.add_argument("--cluster", help="the cluster file: boards, labels, the package, what the outside may see")
    ap.add_argument("--api", default=DEFAULT_API)
    ap.add_argument("--relay", default=DEFAULT_RELAY)
    ap.add_argument("--pull-ms", type=int, default=2000)
    ap.add_argument("--telemetry-ms", type=int, default=10000,
                    help="M15: how often each board's telemetry is pulled over the radio (0: never)")
    ap.add_argument("--telemetry-log", help="M15: append every pull's health and links to this file, as JSON lines")
    ap.add_argument("--listen", help="M16: also serve the dashboard on this address (e.g. 0.0.0.0:8443) for phones on "
                                     "the PC's own network -- TLS and a login only (potluck.hubauth)")
    ap.add_argument("--tls-cert", help="M16: the certificate (PEM) --listen serves")
    ap.add_argument("--tls-key", help="M16: its private key (PEM)")
    ap.add_argument("--record", help="M16: keep the model and events, each pull period, in hourly files in this "
                                     "directory (potluck.hubreplay)")
    ap.add_argument("--record-hours", type=int, default=24, help="M16: how many hours of recording to keep")
    ap.add_argument("--replay", help="M16: serve this recording through the API instead of a cell (no port)")
    ap.add_argument("--speed", type=float, default=1.0, help="M16: replay speed")
    ap.add_argument("--broker-login", help="M17: a file of two lines, the broker's user name and password, for a "
                                           "broker that wants a login (never printed)")
    ap.add_argument("--no-ha-entities", action="store_true",
                    help="M17: do not publish the hub's own entities (board health, firmware) to the cluster's broker")
    a = ap.parse_args(argv)
    lan = None
    if a.listen:
        # Refused before anything is touched: never a dashboard on the network without encryption and a login.
        import ssl

        from .hubauth import Sessions, Users

        users = Users()
        if not (a.tls_cert and a.tls_key):
            print("hub: --listen needs --tls-cert and --tls-key: the dashboard is never served unencrypted beyond "
                  "this PC")
            return 2
        if not users.users:
            # M17: a program pairs with a token approved on this PC, so no login is needed for that -- but nobody can
            # log in on the page until one exists.
            print(f"[hub] no login yet ({users.path}): programs can pair (approve them on http://{a.api}/); for a "
                  f"phone or another computer's browser: python -m potluck.hubauth add <name> --right look", flush=True)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        try:
            ctx.load_cert_chain(a.tls_cert, a.tls_key)
        except (OSError, ssl.SSLError) as exc:
            print(f"hub: the certificate or key could not be loaded: {exc}")
            return 2
        from .hubauth import sessions_file
        lan = (Sessions(users, sessions_file()), ctx)
    if a.replay:
        from .hubreplay import serve_replay

        try:
            return serve_replay(a.replay, a.api, a.speed, a.listen, *(lan or (None, None)))
        except (OSError, ValueError) as exc:
            print(f"hub: {exc}")
            return 2
    cluster = package = None
    if a.cluster:
        from .apply import ApplyError, default_cache, load_cluster, plan_package
        try:
            cluster = load_cluster(a.cluster)
            package = plan_package(cluster, default_cache(), lambda m: print(f"[hub] {m}", flush=True))
        except ApplyError as exc:
            print(f"hub: {exc}")
            return 2
        if not a.port and cluster.port:
            a.port = cluster.port
            # the cluster file's rate goes with its port: 115200 for a board's own USB cable (the setup wizard
            # writes that), 921600 for an adapter on GPIO17/18 -- found 2026-10-16, a tray-started hub on a
            # wizard-made cell opened its port at 921600
            a.baud = a.baud or cluster.baud
    a.baud = a.baud or 921600
    # One resident hub per cell (M14). On this PC: the state file names a running one, found by its relay answering.
    if hub_endpoint(None) is not None:
        try:
            st = json.loads(state_file().read_text(encoding="utf-8"))
        except (OSError, ValueError):
            st = {}
        print(f"REFUSED, nothing changed: a hub already runs on this PC (pid {st.get('pid')}, holding "
              f"{st.get('port')}, API http://{st.get('api')}/) -- use it, or stop it first")
        return 2
    from .lease import KIND_HUB, CellLease, LeaseRefused, holder_name

    setup_mode = not (a.port or a.tcp)
    if setup_mode:
        # M20's first run: no cell yet. Only the PC's own address, and only the wizard has anything to do.
        if lan is not None:
            print("hub: no cell yet -- set it up first on this PC (http://127.0.0.1:8470/setup.html); --listen comes after")
            return 2
        from .transport import NullTransport
        hub = Hub(NullTransport())
    else:
        hub = Hub(open_transport(port=a.port, tcp=a.tcp, baud=a.baud))
    hub.start()
    model = Model(hub, cluster=cluster, package=package, pull_period_ms=a.pull_ms,
                  telemetry_period_ms=a.telemetry_ms,
                  telemetry_log=open(a.telemetry_log, "a", encoding="utf-8") if a.telemetry_log else None)
    lease = None
    if setup_mode:
        print(f"[hub] no cell yet: set one up at http://{a.api}/setup.html", flush=True)
    elif hub.hello(timeout=3.0) is None:
        print(f"hub: no board answers on {a.port or a.tcp}; serving anyway, and retrying")
    else:
        # In the cell: its hub lease, held by the lowest-id board. Another PC's resident hub holds it -> refused,
        # naming that hub, before anything is served.
        me = holder_name("hub")
        for attempt in range(2):
            try:
                lease = CellLease.take(hub, KIND_HUB, me, strict=False, log=lambda m: print(f"[hub] {m}", flush=True))
                break
            except LeaseRefused as exc:
                if attempt == 0 and exc.reply.name == me:
                    # This PC's own hub, gone without releasing (killed): none answers here (checked above), so its
                    # lease is only waiting to lapse -- wait it out rather than refuse a restart (found on the bench,
                    # M0-LOG session 42: a killed hub's restart was refused for up to a minute).
                    wait_s = exc.reply.remaining_ms / 1000 + 1
                    print(f"[hub] the cell's hub lease is still this PC's last hub's (killed?); taking it over in "
                          f"{wait_s:.0f} s", flush=True)
                    time.sleep(wait_s)
                    continue
                print(f"REFUSED, nothing changed: {exc} -- one resident hub per cell")
                hub.close()
                return 2
    if a.record:
        from .hubreplay import Recorder

        model.recorder = Recorder(a.record, a.record_hours)
    relay = serve_relay(hub, a.relay)
    pairing = None
    announcer = None
    if lan is not None:
        from .hubauth import Pairing

        pairing = Pairing(lan[0].users)  # M17: programs ask on --listen, the owner answers on 127.0.0.1
        model.pairing = pairing
    api = serve_api(model, a.api, pairing=pairing)
    if lan is not None:
        serve_api(model, a.listen, sessions=lan[0], tls=lan[1], pairing=pairing)
        print(f"[hub] dashboard for the network at https://{a.listen}/ (login required; "
              f"{len(lan[0].users.users)} login(s), {len(lan[0].users.tokens)} paired program(s))", flush=True)
        # M17: announced by multicast DNS, so Home Assistant finds it (hub_mdns.py).
        import hashlib
        import ssl

        from .hub_mdns import Announcer, lan_ip

        try:
            der = ssl.PEM_cert_to_DER_cert(open(a.tls_cert, encoding="ascii").read())
            model.tls_fp = hashlib.sha256(der).hexdigest()
            model.tls_der = der  # GET /hub-certificate.crt: a phone trusts it once, then saves passwords (2026-10-19)
            towards = str((cluster.present or {}).get("broker", "192.168.0.1")).partition(":")[0] if cluster else \
                "192.168.0.1"
            ip = lan_ip(towards)
            if os.environ.get("POTLUCK_HUB_NO_ANNOUNCE"):  # a test's hub: never announced on the owner's network
                raise ValueError("POTLUCK_HUB_NO_ANNOUNCE is set")
            announcer = Announcer(cluster.system if cluster else "lab", model.cell_id, int(a.listen.rpartition(":")[2]),
                                  hashlib.sha256(der).hexdigest(), ip)
            announcer.start()
            print(f"[hub] announced as {announcer.instance} at {ip}", flush=True)
        except (OSError, ValueError) as exc:
            print(f"[hub] not announced on the network: {exc}", flush=True)
    extras = None
    if cluster is not None and cluster.present and not a.no_ha_entities:
        # M17: the hub's own entities in Home Assistant, on the presenter's devices (hub_extras.py).
        from .hub_extras import Extras

        from .mqtt_link import read_login

        try:
            login = read_login(a.broker_login) if a.broker_login else None
        except (OSError, ValueError) as exc:
            print(f"hub: --broker-login: {exc}")
            return 2
        extras = Extras(model, lambda m: print(f"[hub] {m}", flush=True), login)
        extras.start()
    st = {"pid": os.getpid(), "port": a.port or a.tcp, "relay": a.relay, "api": a.api, "started": _hub_ms()}
    state_file().parent.mkdir(parents=True, exist_ok=True)
    state_file().write_text(json.dumps(st), encoding="utf-8")
    model.record("hub_started", None, {"port": st["port"], "api": a.api, "relay": a.relay})
    print(f"[hub] holding {st['port']}; API http://{a.api}/  relay {a.relay}", flush=True)
    try:
        while True:
            t0 = time.monotonic()
            if hub.peer_node_id is None:
                hub.hello(timeout=2.0)
            if model.restart_requested:  # POST /api/setup/listen: a clean stop (the lease released), the tray restarts it
                print("[hub] stopping for a restart with new settings (POST /api/setup/listen)", flush=True)
                break
            if lease is not None and lease.lost is not None:
                # The arbiter changed and another PC's hub took the lease there first: this one stands down.
                model.record("hub_lease_lost", None, {"holder": lease.lost.name, "arbiter": lease.lost.arbiter})
                print(f"[hub] standing down: the cell's hub lease is {lease.lost.holder_line()}", flush=True)
                break
            model.pull()
            if model.recorder is not None or extras is not None:
                snap = model.snapshot()
                if model.recorder is not None:
                    with model.lock:
                        evs = list(model.events)
                    model.recorder.write(snap, evs)
                if extras is not None:
                    extras.tick(snap)
            time.sleep(max(0.0, a.pull_ms / 1000 - (time.monotonic() - t0)))
    except KeyboardInterrupt:
        pass
    finally:
        try:
            if json.loads(state_file().read_text(encoding="utf-8")).get("pid") == os.getpid():
                state_file().unlink()
        except (OSError, ValueError):
            pass
        if lease is not None:
            lease.release()
        if extras is not None:
            extras.stop()
        if announcer is not None:
            announcer.stop()
        relay.shutdown()
        api.shutdown()
        hub.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
