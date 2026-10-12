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
than one. Look is every GET; write is POST /api/write, only what the package lets the outside write, passed to the
owning board marked from outside, which checks again; deploy is never here.

Local only by default (127.0.0.1); the house network and phones are M16's login, not version 1's.
"""

from __future__ import annotations

import argparse
import json
import os
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
from .ns_payloads import Write
from .paths import path_hash
from .serial_framing import SerialReassembler, write_serial_frame
from .transport import open_transport
from .value import Quality, Value, unit_suffix

API_VERSION = 1
DEFAULT_API = "127.0.0.1:8470"
DEFAULT_RELAY = "127.0.0.1:8471"
DASHBOARD_DIR = Path(__file__).resolve().parent / "dashboard"

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
    def __init__(self, sock: socket.socket, name: str) -> None:
        self.sock = sock
        self.name = name
        self.lock = threading.Lock()
        self.alive = True

    def send_raw(self, raw: bytes) -> None:
        wire = write_serial_frame(raw)
        with self.lock:
            try:
                self.sock.sendall(wire)
            except OSError:
                self.alive = False


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
        self.relay_counts = {"clients": 0, "requests": 0, "replies": 0, "unmatched": 0, "fanned_out": 0,
                             "node_frames_out": 0, "node_frames_in": 0}

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

    def _reply_probe(self, f: fr.Frame, recv: float) -> None:
        if self.node_client is None:  # with a host node attached, it is the PC's node and answers its own probes
            super()._reply_probe(f, recv)

    # -- frames from the cell ------------------------------------------------------------------------
    def _on_raw_frame(self, raw: bytes) -> None:
        try:
            f = fr.parse(raw)
        except fr.FrameError:
            super()._on_raw_frame(raw)
            return
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
        if f.opcode in (fr.Op.CALL, fr.Op.CAST) or (node is None and f.opcode == fr.Op.READ):
            if f.dst == HOST_NODE_ID:
                # The cell asks the PC (an agent's service, M8): every tool hears it; the one offering it answers.
                self._fan_out(raw)
        super()._on_raw_frame(raw)

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
        if self.node_client is not None:
            return  # the host node is the PC's node now; a second HELLO for 0x00fe would read as another incarnation
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
            # when M10's process was killed on the cable -- the hub says nothing for 0x00fe until a tool's HELLO.
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
                    data = self.request.recv(4096)
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

    def __init__(self, hub: Hub, *, cluster=None, package=None, pull_period_ms: int = 2000,
                 telemetry_period_ms: int = 10000) -> None:
        self.hub = hub
        self.cluster = cluster
        self.package = package
        self.pull_period_ms = pull_period_ms
        # M15: how often each board's telemetry is pulled over the radio (0: never). Its own rate, longer than the
        # inventory's: it is the larger load, and M15's [MEASURE] is the rate that leaves M0's figures intact.
        self.telemetry_period_ms = telemetry_period_ms
        self._telemetry_due = 0.0
        self.telemetry_counts = {"pulls": 0, "replies": 0, "failures": 0, "events": 0}
        self.link_deaths: dict[tuple[int, int], int] = {}  # (board, peer) -> peer_dead events pulled since the hub started
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.boards: dict[int, dict] = {}
        self.resources: dict[str, dict] = {}
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
            self.events.append({"seq": self.seq, "at": _hub_ms(), "board": f"{board:04x}" if board else None,
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

    def resource_paths(self) -> list[str]:
        if self.package is None:
            return []
        from .apply import expected
        _, _, reads = expected(self.package.manifest, self.board_ids())
        return reads

    # one pull: every board's NODE_STATUS, every resource's READ
    def pull(self) -> None:
        from .apply import node_status
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
                    tel = b.get("tel") or {}
                    if prev in ("alive", "asleep") and tel.get("health") is not None and tel["health"].long_window:
                        # It declared a long window (M5.1): not answering is what an asleep board does.
                        if prev != "asleep":
                            b["state"], b["since_at"] = "asleep", _hub_ms()
                    elif prev in ("alive", "asleep"):
                        b["state"], b["since_at"] = "dead", _hub_ms()
                    elif prev == "unknown":
                        b["state"] = "unknown"
                    self.errors.append(time.monotonic())
                else:
                    self.rtts.append(time.monotonic() - t0)
                    if prev != "alive":
                        b["state"], b["since_at"] = "alive", _hub_ms()
                    if b.get("epoch_up") is not None and st.uptime_s < b["epoch_up"]:
                        self._event_locked("board_rebooted", n, {"up_s": st.uptime_s})
                    b["epoch_up"] = st.uptime_s
                    b["last_ok"] = time.monotonic()
                    b["status"] = st
                    b["cabled"] = n == cabled
                if prev != b["state"]:
                    self._event_locked("board_" + b["state"], n, {"was": prev})
        for path in self.resource_paths():
            t0 = time.monotonic()
            try:
                r = self.hub.read(path, timeout=1.5)
            except BridgeError:
                self.errors.append(time.monotonic())
                continue
            self.rtts.append(time.monotonic() - t0)
            with self.lock:
                old = self.resources.get(path)
                q = Quality.name_of(r.quality)
                self.resources[path] = {"reading": r, "quality": q, "at": _hub_ms()}
                if old is not None and old["quality"] != q:
                    self._event_locked("quality_changed", None, {"path": path, "from": old["quality"], "to": q})
        if self.telemetry_period_ms and time.monotonic() >= self._telemetry_due:
            self._telemetry_due = time.monotonic() + self.telemetry_period_ms / 1000
            self.pull_telemetry()
        with self.lock:
            self.frames_at.append((time.monotonic(), self.hub.stats.rx_frames + self.hub.stats.tx_frames))

    def pull_telemetry(self) -> None:
        """M15: every answering board's health, links, actors and new events, over the radio, one pull for every
        viewer. A board that does not answer TELEMETRY (firmware before M15) keeps what NODE_STATUS says."""
        from . import telemetry as tm

        for n in self.board_ids():
            with self.lock:
                b = self.boards.get(n)
                if b is None or b["state"] != "alive" or b.get("tel_unsupported"):
                    continue
                since = b.get("tel", {}).get("events_next", 1) - 1
            self.telemetry_counts["pulls"] += 1
            try:
                h = tm.health(self.hub, n)
                ls = tm.links(self.hub, n)
                acts = tm.actors(self.hub, n)
                evs, nxt = tm.events(self.hub, n, since)
                if nxt <= since:  # the board's log started again: it rebooted since the last pull
                    evs, nxt = tm.events(self.hub, n, 0)
            except (BridgeError, tm.TelemetryError) as exc:
                self.telemetry_counts["failures"] += 1
                with self.lock:
                    b = self.boards.get(n)
                    if b is not None:
                        b["tel_failures"] = b.get("tel_failures", 0) + 1
                        if b["tel_failures"] >= 3 and not b.get("tel"):
                            b["tel_unsupported"] = True  # never answered: firmware before M15
                            self._event_locked("telemetry_unsupported", n, {"why": str(exc)[:120]})
                continue
            self.telemetry_counts["replies"] += 1
            self.telemetry_counts["events"] += len(evs)
            with self.lock:
                b = self.boards.get(n)
                if b is None:
                    continue
                b["tel_failures"] = 0
                b["tel"] = {"health": h, "links": ls, "actors": acts, "events_next": nxt, "at": _hub_ms()}
                for e in evs:
                    if e.kind_name == "peer_dead":
                        self.link_deaths[(n, e.node)] = self.link_deaths.get((n, e.node), 0) + 1
                    self._event_locked("board_" + e.kind_name, n,
                                       {"peer": f"{e.node:04x}", "a": e.a, "b": e.b, "board_ms": e.at_ms,
                                        "board_seq": e.seq})

    def _event_locked(self, kind: str, board: int | None, detail: dict) -> None:
        self.seq += 1
        self.events.append({"seq": self.seq, "at": _hub_ms(), "board": f"{board:04x}" if board else None,
                            "kind": kind, "detail": detail})

    # the JSON the API serves (HUB-API.md)
    def snapshot(self) -> dict:
        with self.lock:
            boards = [self._board_json(n, b) for n, b in sorted(self.boards.items())]
            resources = [self._resource_json(p, r) for p, r in self.resources.items()]
            events_tail = list(self.events)[-1:]
        apps = self._apps_json(boards, resources)
        return {
            "api": API_VERSION,
            "at": _hub_ms(),
            "hub": {"version": self.version, "up_ms": int((time.monotonic() - self.started) * 1000),
                    "port": getattr(self.hub.transport, "port", None) or self.hub.transport.description,
                    "link": "up" if self.hub.peer_node_id is not None else "down", "link_detail": "",
                    "frames_rx": self.hub.stats.rx_frames, "frames_tx": self.hub.stats.tx_frames,
                    "pull_period_ms": self.pull_period_ms, "clock_error_ms": self._clock_error_ms(),
                    "telemetry_period_ms": self.telemetry_period_ms, "telemetry": dict(self.telemetry_counts),
                    "relay": dict(self.hub.relay_counts)},
            "cell": self._cell_json(boards),
            "boards": boards,
            "apps": apps,
            "resources": resources,
            "integrations": self._integrations_json(boards, resources),
            "deploys": list(self.deploys),
            "signals": self._signals_json(boards),
            "last_event_seq": events_tail[0]["seq"] if events_tail else 0,
        }

    def _clock_error_ms(self) -> int | None:
        if not self.rtts:
            return None
        xs = sorted(self.rtts)
        return int(xs[len(xs) // 2] * 1000 / 2)  # half a median round trip: the uncertainty of mapping a board's clock

    def _board_json(self, n: int, b: dict) -> dict:
        st = b.get("status")
        out = {"node": f"{n:04x}", "label": self.label_of(n), "state": b["state"], "since_at": b["since_at"],
               "silent_ms": int((time.monotonic() - b["last_ok"]) * 1000) if b.get("last_ok") else None,
               "rssi": None, "cabled": bool(b.get("cabled"))}
        if st is not None:
            out.update({
                "firmware": {"version": st.fw_version, "counter": st.fw_running, "state": st.fw_state},
                "package": {"counter": st.pkg_counter, "state": "on_trial" if st.pending else "confirmed",
                            "live_applies": st.live_applies},
                "roles": st.role_names(),
                "modules": [{"name": m, "state": s, "reason": r} for m, s, r in st.modules],
                "actors": list(st.actors),
                "up_ms": st.uptime_s * 1000,
                "cluster": st.cluster,
                "heap": None,   # M15 telemetry, below, when the board answers it
                "links": [],
            })
        tel = b.get("tel")
        if tel is not None:
            h = tel["health"]
            out["heap"] = {"internal_free": h.int_free, "internal_min": h.int_min, "internal_largest": h.int_largest,
                           "psram_free": h.ext_free, "psram_min": h.ext_min}
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
                            "released": a.released} for a in tel["actors"]]}
            radio = [k.rssi for k in tel["links"] if k.rssi and not (k.flags & 1)]
            out["rssi"] = max(radio) if radio else None
        return out

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
                "age_ms": rd.age_ms + max(0, _hub_ms() - r["at"]), "latency_class": rd.latency_class,
                "exposed": outside is not None, "outside_writable": outside == "read_write",
                "safety_relevant": self._safety(path)}

    def _safety(self, path: str) -> bool:
        if self.package is None:
            return False
        r = self.package.manifest.resource(path)
        return bool(r is not None and r.safety_relevant)

    def _cell_json(self, boards: list[dict]) -> dict:
        pk = next((b["package"]["counter"] for b in boards if b.get("package")), None)
        fw = next((b["firmware"]["version"] for b in boards if b.get("firmware")), None)
        presenter = next((b["node"] for b in boards if "present" in b.get("actors", [])), None)
        return {"system": self.cluster.system if self.cluster is not None else "lab", "cell_id": self.cell_id,
                "channel": None, "package": pk, "firmware": fw, "other_cell_frames": None, "presenter": presenter}

    def _apps_json(self, boards: list[dict], resources: list[dict]) -> list[dict]:
        if self.package is None:
            return []
        from . import modules as mods
        from .apply import expected
        quality = {r["path"]: r["quality"] for r in resources}
        running_here = {b["node"]: set(b.get("actors", [])) for b in boards if b["state"] == "alive"}
        out = []
        m = self.package.manifest
        for a in m.actors:
            spec = mods.get(a.module)
            portable = a.module.startswith("guest:") or a.module in ("builtin:ticker", "builtin:present") or (
                spec is not None and spec.placement == "portable")
            every = spec is not None and spec.placement == "every"
            kind = a.module.split(":", 1)[1].split("/")[-1] if ":" in a.module else a.module
            kind = {"die_temp": "die_temp", "overheat_alarm": "guest"}.get(kind, kind)
            if a.module.startswith("guest:"):
                kind = "guest"
            pin = m.placement_of(a)
            nodes = [f"{pin:04x}"] if pin is not None and not portable else [b["node"] for b in boards]
            running = [n for n, acts in running_here.items() if kind in acts and (portable or every or n in nodes)]
            outs = [p for p in expected(m, [int(n, 16) for n in nodes])[2] if f"/act/{a.name}/" in p or
                    (pin is not None and f"/node-{pin:04x}/" in p and not portable)]
            bad_outs = [p for p in outs if quality.get(p) not in (None, "GOOD")]
            if portable:
                ok, why = len(running) == 1, ("" if len(running) == 1 else f"running on {len(running)} boards")
            else:
                want = nodes if not every else [b["node"] for b in boards if b["state"] == "alive"]
                ok = all(n in running for n in want)
                why = "" if ok else "not running on " + ", ".join(n for n in want if n not in running)
            if ok and bad_outs:
                ok, why = False, "output " + bad_outs[0].split("/", 3)[3] + " is " + quality.get(bad_outs[0], "?")
            out.append({"name": a.name, "module": a.module,
                        "placement": "portable" if portable else ("every" if every else "pinned"),
                        "nodes": nodes, "running_on": running, "working": ok, "detail": why, "outputs": outs})
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
        slots = max((len(b.get("actors", [])) / 8 for b in alive), default=None)
        return {"latency_p50_ms": p50, "latency_p99_ms": p99, "traffic_frames_s": traffic, "errors_min": errors,
                "saturation": {"peers": round(len(boards) / 21, 2) if boards else None, "namespace": None,
                               "actor_slots": round(slots, 2) if slots is not None else None, "internal_ram": None,
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
    try:
        import subprocess
        return subprocess.run(["git", "rev-parse", "--short=7", "HEAD"], cwd=Path(__file__).parent,
                              capture_output=True, text=True, timeout=5).stdout.strip() or "unknown"
    except Exception:
        return "unknown"


# ---- the API ---------------------------------------------------------------------------------------------

def serve_api(model: Model, addr: str) -> ThreadingHTTPServer:
    host, _, p = addr.rpartition(":")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a) -> None:  # quiet: the hub's own log is its events
            pass

        def _json(self, code: int, obj: Any) -> None:
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            path, _, query = self.path.partition("?")
            if path == "/api/model":
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
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self) -> None:
            if self.path != "/api/write":
                self._json(404, {"error": "not found"})
                return
            try:
                n = int(self.headers.get("Content-Length", "0"))
                req = json.loads(self.rfile.read(n) or b"{}")
                path, value = req["path"], req["value"]
            except (ValueError, KeyError):
                self._json(400, {"ok": False, "result": "expected {\"path\": ..., \"value\": ...}"})
                return
            self._json(*write_outside(model, path, value))

    srv = ThreadingHTTPServer((host or "127.0.0.1", int(p)), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, name="potluck-hub-api", daemon=True).start()
    return srv


def write_outside(model: Model, path: str, value: Any) -> tuple[int, dict]:
    """POST /api/write: only a resource the package lets the outside write, never a safety-relevant one; sent to the
    owning board as a WRITE marked from outside, which checks the package again."""
    cl = model.cluster
    mode = cl.outside.get(path) if cl is not None else None
    if mode != "read_write" or model._safety(path):
        model.record("outside_write", None, {"path": path, "value": value, "result": "refused: not writable from outside"})
        return 403, {"ok": False, "result": "not writable from outside"}
    with model.lock:
        cur = model.resources.get(path)
    if isinstance(value, bool):
        v = Value.of_bool(value)
    elif isinstance(value, int):
        v = Value.of_i32(value)
    elif isinstance(value, float):
        v = Value.of_f32(value)
    else:
        return 400, {"ok": False, "result": "a bool or a number"}
    name = path.split("/", 3)[3]
    owner = int(name[5:9], 16) if name.startswith("node-") else None
    if owner is None:
        return 400, {"ok": False, "result": "only a board's own resource can be written from outside"}
    h = path_hash(path)
    try:
        raw = model.hub._request(fr.Op.WRITE, Write(path_hash=h, value=v, from_outside=True).encode(), h,
                                 timeout=2.0, dst=owner if owner != model.hub.peer_node_id else None)
    except RequestTimeout:
        model.record("outside_write", owner, {"path": path, "value": value, "result": "no answer"})
        return 504, {"ok": False, "result": "the owning board did not answer"}
    from .value import NsError
    ok = raw.status == NsError.OK
    result = "applied" if ok else NsError.name_of(raw.status).lower()
    model.record("outside_write", owner, {"path": path, "value": value, "result": result})
    del cur
    return (200 if ok else 403), {"ok": ok, "result": result}


# ---- the program ----------------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="potluck-hub", description=__doc__.splitlines()[0])
    ap.add_argument("--port", help="the serial port of the PC's adapter (e.g. COM6)")
    ap.add_argument("--tcp", help="or a TCP endpoint carrying the frame link (an emulator)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--cluster", help="the cluster file: boards, labels, the package, what the outside may see")
    ap.add_argument("--api", default=DEFAULT_API)
    ap.add_argument("--relay", default=DEFAULT_RELAY)
    ap.add_argument("--pull-ms", type=int, default=2000)
    ap.add_argument("--telemetry-ms", type=int, default=10000,
                    help="M15: how often each board's telemetry is pulled over the radio (0: never)")
    a = ap.parse_args(argv)
    cluster = package = None
    if a.cluster:
        from .apply import ApplyError, default_cache, load_cluster, plan_package
        try:
            cluster = load_cluster(a.cluster)
            package = plan_package(cluster, default_cache(), lambda m: print(f"[hub] {m}", flush=True))
        except ApplyError as exc:
            print(f"hub: {exc}")
            return 2
        a.port = a.port or cluster.port
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

    hub = Hub(open_transport(port=a.port, tcp=a.tcp, baud=a.baud))
    hub.start()
    model = Model(hub, cluster=cluster, package=package, pull_period_ms=a.pull_ms,
                  telemetry_period_ms=a.telemetry_ms)
    lease = None
    if hub.hello(timeout=3.0) is None:
        print(f"hub: no board answers on {a.port or a.tcp}; serving anyway, and retrying")
    else:
        # In the cell: its hub lease, held by the lowest-id board. Another PC's resident hub holds it -> refused,
        # naming that hub, before anything is served.
        try:
            lease = CellLease.take(hub, KIND_HUB, holder_name("hub"), log=lambda m: print(f"[hub] {m}", flush=True))
        except LeaseRefused as exc:
            print(f"REFUSED, nothing changed: {exc} -- one resident hub per cell")
            hub.close()
            return 2
    relay = serve_relay(hub, a.relay)
    api = serve_api(model, a.api)
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
            if lease is not None and lease.lost is not None:
                # The arbiter changed and another PC's hub took the lease there first: this one stands down.
                model.record("hub_lease_lost", None, {"holder": lease.lost.name, "arbiter": lease.lost.arbiter})
                print(f"[hub] standing down: the cell's hub lease is {lease.lost.holder_line()}", flush=True)
                break
            model.pull()
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
        relay.shutdown()
        api.shutdown()
        hub.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
