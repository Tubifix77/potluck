"""A minimal Potluck node in Python, for testing the bridge with no hardware and no emulator.

This is NOT a second implementation of the node's policy -- pot::Node is the only one of those, and
the simulator in sim/ runs that same C++ code so the policy is never duplicated. This answers the
wire, nothing more: HELLO -> HELLO_ACK, READ -> REPLY, WRITE -> REPLY, and a heartbeat on a timer.

What it is for: proving the *bridge* correct. A bridge bug and a firmware bug look identical from the
host side, so there has to be one far end whose behaviour is known exactly. When the bridge talks to
this and to QEMU and gets the same answers, the framing, correlation and timeout logic are cleared
and anything left is the firmware's.

Deliberately not modelled here: staleness, access control, ownership, event queues. Those are
pot::Node's job and duplicating them would create a second place for section 4 rule 2 to be got
wrong -- exactly what the Reading type exists to prevent.
"""

from __future__ import annotations

import threading
import time

from . import frame as fr
from .ns_payloads import REPLY_TO_READ, REPLY_TO_WRITE, Read, Reply, Write
from .paths import path_hash
from .payloads import HELLO_FLAG_WANT_ACK, Heartbeat, Hello, HelloAck
from .serial_framing import SerialReassembler, write_serial_frame
from .sys_paths import sys_paths_for
from .transport import Transport
from .value import NsError, Quality, Reading, Unit, Value


class FakeNode:
    """Answers the wire as a node would. Drive it with `pump()`, or `start()` for a thread."""

    def __init__(
        self,
        transport: Transport,
        *,
        node_id: int = 0x1001,
        boot_epoch: int = 7,
        hb_period_ms: int = 100,
        hb_miss_limit: int = 6,
        stale_after_ms: int = 5000,
    ) -> None:
        self.transport = transport
        self.node_id = node_id
        self.boot_epoch = boot_epoch
        self.hb_period_ms = hb_period_ms
        self.hb_miss_limit = hb_miss_limit
        self.stale_after_ms = stale_after_ms

        self.reassembler = SerialReassembler()
        self._seq = 0
        self._hb_seq = 0
        self._started = time.monotonic()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

        self.peers: dict[int, float] = {}
        self.reads_served = 0
        self.writes_served = 0
        self.writes_rejected = 0
        self.hellos_seen = 0
        self.heartbeats_seen = 0

        # The six built-ins every node declares, so the paths a real board offers are the paths this
        # answers. Values are (Value, written_at_monotonic, unit, writable).
        self._store: dict[int, tuple[Value, float, int, bool]] = {}
        now = time.monotonic()
        paths = sys_paths_for(node_id)
        defaults = [
            (Value.of_u32(213_400), int(Unit.BYTE), False),      # sys/heap-free
            (Value.of_u32(180_224), int(Unit.BYTE), False),      # sys/heap-largest
            (Value.of_u32(0), int(Unit.SECOND), False),          # sys/uptime
            (Value.of_u32(boot_epoch), int(Unit.COUNT), False),  # sys/boot-epoch
            (Value.of_u32(0), int(Unit.COUNT), False),           # sys/peers-alive
            (Value.of_i32(-58), int(Unit.NONE), False),          # sys/rssi
        ]
        for p, (v, unit, writable) in zip(paths, defaults):
            self._store[path_hash(p)] = (v, now, unit, writable)

        #: A writable path that no real board has, for exercising WRITE and its refusals.
        self.writable_path = f"potluck://lab/node-{node_id:04x}/test/setpoint"
        self._store[path_hash(self.writable_path)] = (Value.of_f32(0.0), now, int(Unit.VOLT), True)

    @property
    def uptime_ms(self) -> int:
        return int((time.monotonic() - self._started) * 1000)

    # -- lifecycle -------------------------------------------------------------------------------
    def start(self) -> None:
        if self._thread is not None:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, name="fake-node", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            self._thread = None

    def _loop(self) -> None:
        next_hb = time.monotonic() + self.hb_period_ms / 1000.0
        while not self._stop.is_set():
            self.pump()
            now = time.monotonic()
            if now >= next_hb:
                next_hb = now + self.hb_period_ms / 1000.0
                self.beat()
            time.sleep(0.002)

    def pump(self) -> int:
        """Handle whatever bytes are available. Returns the number of frames handled."""
        data = self.transport.read(4096)
        if not data:
            return 0
        n = 0
        for raw in self.reassembler.feed(data):
            self._on_frame(raw)
            n += 1
        return n

    # -- sending ---------------------------------------------------------------------------------
    def _send(self, opcode: int, payload: bytes, *, dst: int, msg_id: int = 0) -> None:
        self._seq = (self._seq + 1) & 0xFFFF
        raw = fr.encode(src=self.node_id, dst=dst, opcode=opcode, seq=self._seq,
                        msg_id=msg_id, payload=payload)
        self.transport.write(write_serial_frame(raw))

    def beat(self) -> None:
        """Broadcast a heartbeat, the way section 8.2's beacon does."""
        self._hb_seq = (self._hb_seq + 1) & 0xFFFFFFFF
        hb = Heartbeat(
            uptime_ms=self.uptime_ms,
            boot_epoch=self.boot_epoch,
            hb_seq=self._hb_seq,
            tx_frames=self._seq,
            tx_cb_ok=self._seq,
            tx_cb_fail=0,
            rx_frames=self.heartbeats_seen,
            rx_lost_seqgap=0,
            turnaround_us=0,
            ack_of_msg_id=0,
            rtt_min_us=None,
            rtt_max_us=None,
            free_dram_kib=208,
            espnow_version=2,
            hb_flags=0,
        )
        self._send(fr.Op.HEARTBEAT, hb.encode(), dst=fr.NODE_BROADCAST)

    # -- receiving -------------------------------------------------------------------------------
    def _on_frame(self, raw: bytes) -> None:
        try:
            f = fr.parse(raw)
        except fr.FrameError:
            return
        if f.dst != fr.NODE_BROADCAST and f.dst != self.node_id:
            return

        # Only HELLO admits a stranger -- the same rule Node::on_rx enforces, and the reason the
        # bridge has to say hello before it can read anything.
        known = f.src in self.peers
        if not known and f.opcode != fr.Op.HELLO:
            return

        if f.opcode == fr.Op.HELLO:
            self._on_hello(f)
        elif f.opcode == fr.Op.HEARTBEAT:
            self.heartbeats_seen += 1
            self.peers[f.src] = time.monotonic()
        elif f.opcode == fr.Op.READ:
            self._on_read(f)
        elif f.opcode == fr.Op.WRITE:
            self._on_write(f)
        elif f.opcode == fr.Op.BYE:
            self.peers.pop(f.src, None)

    def _on_hello(self, f: fr.Frame) -> None:
        self.hellos_seen += 1
        try:
            h = Hello.parse(f.payload)
        except ValueError:
            return
        self.peers[h.node_id or f.src] = time.monotonic()
        self._refresh_peers_alive()
        if h.wants_ack:
            ack = HelloAck(
                boot_epoch=self.boot_epoch,
                node_id=self.node_id,
                espnow_version=2,
                decision=0,
                hb_period_ms=self.hb_period_ms,
                hb_miss_limit=self.hb_miss_limit,
            )
            import struct
            payload = struct.pack("<IHBBBBH", ack.boot_epoch, ack.node_id, ack.espnow_version,
                                  ack.decision, ack.hb_period_ms // 10, ack.hb_miss_limit, 0)
            self._send(fr.Op.HELLO_ACK, payload, dst=h.node_id or f.src, msg_id=f.msg_id)

    def _reading_for(self, h: int) -> tuple[Reading, int]:
        entry = self._store.get(h)
        if entry is None:
            return Reading(quality=int(Quality.UNAVAILABLE)), int(NsError.NOT_FOUND)
        value, written_at, unit, _writable = entry
        age_ms = int((time.monotonic() - written_at) * 1000)
        quality = int(Quality.STALE) if age_ms > self.stale_after_ms else int(Quality.GOOD)
        return (
            Reading(value=value, unit=unit, timestamp_ms=int(written_at * 1000) & 0xFFFFFFFF,
                    age_ms=age_ms, latency_class=3, quality=quality),
            int(NsError.OK),
        )

    def _on_read(self, f: fr.Frame) -> None:
        try:
            req = Read.parse(f.payload)
        except ValueError:
            return
        self._refresh_uptime()
        r, status = self._reading_for(req.path_hash)
        rep = Reply.from_reading(req.path_hash, r, status=status, reply_to=REPLY_TO_READ)
        self._send(fr.Op.REPLY, rep.encode(), dst=f.src, msg_id=f.msg_id)
        self.reads_served += 1

    def _on_write(self, f: fr.Frame) -> None:
        try:
            req = Write.parse(f.payload)
        except ValueError:
            return
        entry = self._store.get(req.path_hash)
        if entry is None:
            status = int(NsError.NOT_FOUND)
        elif not entry[3]:
            status = int(NsError.NOT_WRITABLE)
        elif entry[0].type != req.value.type:
            status = int(NsError.TYPE_MISMATCH)
        else:
            self._store[req.path_hash] = (req.value, time.monotonic(), entry[2], True)
            status = int(NsError.OK)
        if status == NsError.OK:
            self.writes_served += 1
        else:
            self.writes_rejected += 1

        r, _ = self._reading_for(req.path_hash)
        rep = Reply.from_reading(req.path_hash, r, status=status, reply_to=REPLY_TO_WRITE)
        self._send(fr.Op.REPLY, rep.encode(), dst=f.src, msg_id=f.msg_id)

    # -- the built-ins that actually change ------------------------------------------------------
    def _refresh_uptime(self) -> None:
        h = path_hash(sys_paths_for(self.node_id)[2])
        if h in self._store:
            _v, _at, unit, w = self._store[h]
            self._store[h] = (Value.of_u32(self.uptime_ms // 1000), time.monotonic(), unit, w)

    def _refresh_peers_alive(self) -> None:
        h = path_hash(sys_paths_for(self.node_id)[4])
        if h in self._store:
            _v, _at, unit, w = self._store[h]
            self._store[h] = (Value.of_u32(len(self.peers)), time.monotonic(), unit, w)
