"""potluck-bridge -- the host's frame link to a node. ARCHITECTURE.md section 7.1, section 5.3, M2.

The host is an ordinary peer, not a console. It has a MAC (kHostMac, 02:00:00:00:00:FE), a node id
derived from that MAC by the same rule the firmware uses, it says HELLO to be admitted, and it
heartbeats so it stays Alive in the node's peer table. Section 8.1 says a mode transition is a
non-event, and that is only true if a host is not a special case above the transport -- the moment
the firmware needs an `if (from_host)` the claim is dead.

    with Bridge.open(tcp="127.0.0.1:5555") as br:
        br.hello()
        r = br.read("potluck://lab/node-1001/sys/uptime")
        print(r.format())          # 41 s  [GOOD, age 812 ms, ts 41230, class L3]

WHAT OWNS WHAT
    Section 7.1 forbids anything above the bridge opening a serial port. At M2 the bridge is a
    library that potctl uses in-process, so there is exactly one owner and the rule holds trivially.
    The socket boundary arrives with potluck-agent at M8; nothing here assumes in-process, so that is an
    added transport rather than a rewrite.

CORRELATION
    msg_id, which the node echoes into its REPLY. A reply for a msg_id nobody is waiting on is
    counted as `replies_unmatched` rather than dropped silently, because the commonest cause is a
    timeout set too short and that is worth seeing.

CLOCKS
    Every duration here is measured on one clock -- the host's, with time.monotonic. There is no
    subtraction of a node timestamp from a host timestamp anywhere in this file. The REPLY's
    timestamp_ms and age_ms are the *node's* reckoning and are passed through untouched.
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Callable

from . import frame as fr
from .capture import CaptureWriter
from .ns_payloads import REPLY_TO_READ, REPLY_TO_WRITE, Read, Reply, Write
from .paths import path_hash
from .payloads import HELLO_FLAG_WANT_ACK, Bye, Heartbeat, Hello, HelloAck, decode_payload
from .serial_framing import SerialReassembler, write_serial_frame
from .transport import Transport, open_transport
from .value import NsError, Quality, Reading, Value

#: The reserved MAC the firmware routes to the UART -- pot::kHostMac in serial_port.cpp.
HOST_MAC = bytes((0x02, 0x00, 0x00, 0x00, 0x00, 0xFE))

#: The host's node id, derived from HOST_MAC by the firmware's own rule: the low two bytes, big-endian.
#: Pinned as a constant rather than recomputed, because the number appears in captures and in the
#: node's peer table and must not drift.
HOST_NODE_ID = 0x00FE

DEFAULT_HB_PERIOD_MS = 100
DEFAULT_HB_MISS_LIMIT = 6


class BridgeError(Exception):
    """The link is unusable, or a request could not be sent."""


class RequestTimeout(BridgeError):
    """No REPLY arrived inside the deadline. The request may still have been acted on."""


@dataclass
class BridgeStats:
    tx_frames: int = 0
    tx_bytes: int = 0
    rx_frames: int = 0
    rx_bytes: int = 0
    bad_frames: int = 0
    replies_matched: int = 0
    replies_unmatched: int = 0
    timeouts: int = 0
    heartbeats_rx: int = 0
    heartbeats_tx: int = 0
    hellos_rx: int = 0
    errs_rx: int = 0

    def as_dict(self) -> dict[str, int]:
        return dict(vars(self))


@dataclass
class _Pending:
    msg_id: int
    op: int
    path_hash: int
    sent_monotonic: float
    done: threading.Event = field(default_factory=threading.Event)
    reply: Reply | None = None
    rtt_s: float | None = None


class Bridge:
    """Owns the transport, the framing, and the request/reply correlation."""

    def __init__(
        self,
        transport: Transport,
        *,
        node_id: int = HOST_NODE_ID,
        capture: CaptureWriter | None = None,
        heartbeat: bool = True,
        hb_period_ms: int = DEFAULT_HB_PERIOD_MS,
        hb_miss_limit: int = DEFAULT_HB_MISS_LIMIT,
        on_frame: Callable[[fr.Frame, str], None] | None = None,
        on_log: Callable[[str], None] | None = None,
    ) -> None:
        self.transport = transport
        self.node_id = node_id
        self.capture = capture
        self.heartbeat = heartbeat
        self.hb_period_ms = hb_period_ms
        self.hb_miss_limit = hb_miss_limit
        self.on_frame = on_frame
        self.on_log = on_log

        self.stats = BridgeStats()
        self.reassembler = SerialReassembler()
        #: node_id of whoever answered, once known. A bridge does not assume it knows the far end.
        self.peer_node_id: int | None = None
        self.peer_boot_epoch: int | None = None
        self.peer_hb_period_ms: int | None = None
        self.peer_hb_miss_limit: int | None = None
        self.last_heartbeat: Heartbeat | None = None
        self.last_heartbeat_at: float | None = None

        self._started = time.monotonic()
        self._seq = 0
        self._msg_id = 0
        self._pending: dict[int, _Pending] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._reader: threading.Thread | None = None
        self._hb_thread: threading.Thread | None = None
        self._hb_seq = 0

    # -- construction ----------------------------------------------------------------------------
    @classmethod
    def open(cls, *, port: str | None = None, tcp: str | None = None, baud: int = 921600,
             **kwargs) -> Bridge:
        return cls(open_transport(port=port, tcp=tcp, baud=baud), **kwargs)

    def __enter__(self) -> Bridge:
        self.start()
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- lifecycle -------------------------------------------------------------------------------
    def start(self) -> None:
        if self._reader is not None:
            return
        self._stop.clear()
        self._reader = threading.Thread(target=self._read_loop, name="potluck-bridge-rx", daemon=True)
        self._reader.start()
        if self.heartbeat:
            self._hb_thread = threading.Thread(target=self._hb_loop, name="potluck-bridge-hb",
                                               daemon=True)
            self._hb_thread.start()

    def close(self) -> None:
        self._stop.set()
        for t in (self._reader, self._hb_thread):
            if t is not None:
                t.join(timeout=2.0)
        self._reader = None
        self._hb_thread = None
        # Release anyone still waiting, rather than leaving them on a deadline that will now never
        # be answered by anything.
        with self._lock:
            for p in self._pending.values():
                p.done.set()
            self._pending.clear()
        self.transport.close()

    @property
    def uptime_ms(self) -> int:
        return int((time.monotonic() - self._started) * 1000)

    # -- sending ---------------------------------------------------------------------------------
    def _next_seq(self) -> int:
        with self._lock:
            self._seq = (self._seq + 1) & 0xFFFF
            return self._seq

    def _next_msg_id(self) -> int:
        # Never 0: the firmware uses msg_id 0 for "no correlation", so a request that used it could
        # never be matched to its reply.
        with self._lock:
            self._msg_id = (self._msg_id + 1) & 0xFFFF or 1
            return self._msg_id

    def send_frame(self, opcode: int, payload: bytes, *, dst: int = fr.NODE_BROADCAST,
                   msg_id: int = 0, ack_req: bool = False, lclass: int = 3,
                   priority: int = 0) -> None:
        raw = fr.encode(
            src=self.node_id,
            dst=dst,
            opcode=opcode,
            lclass=lclass,
            priority=priority,
            seq=self._next_seq(),
            msg_id=msg_id,
            payload=payload,
            ack_req=ack_req,
        )
        wire = write_serial_frame(raw)
        try:
            self.transport.write(wire)
        except Exception as exc:
            raise BridgeError(f"write to {self.transport.description} failed: {exc}") from exc
        self.stats.tx_frames += 1
        self.stats.tx_bytes += len(wire)
        self._tee(raw, "tx")

    def _tee(self, raw: bytes, direction: str) -> None:
        if self.capture is not None:
            self.capture.write_frame(
                node_id=self.node_id if direction == "tx" else self.peer_node_id,
                direction=direction,
                raw=raw,
                peer_mac=":".join(f"{b:02x}" for b in HOST_MAC),
            )
        if self.on_frame is not None:
            try:
                f = fr.parse(raw)
            except fr.FrameError:
                return
            self.on_frame(f, direction)

    # -- membership ------------------------------------------------------------------------------
    def hello(self, *, timeout: float = 2.0) -> HelloAck | None:
        """Announce this host and wait for HELLO_ACK. Returns None if the node did not answer.

        Broadcast, exactly as a node's own HELLO is: the host does not know the far end's node id
        before it has been told, and inventing one is how a bridge ends up unable to talk to any
        board but the one it was written against.
        """
        h = Hello(
            boot_epoch=0,  # a host has no NVS incarnation; 0 says so rather than faking one
            caps=0,
            node_id=self.node_id,
            espnow_version=0,  # not on a radio at all, and pretending otherwise would skew section 5.3
            hb_period_ms=self.hb_period_ms,
            hb_miss_limit=self.hb_miss_limit,
            flags=HELLO_FLAG_WANT_ACK,
            pubkey_fp=b"",
        )
        deadline = time.monotonic() + timeout
        self.send_frame(fr.Op.HELLO, h.encode(), dst=fr.NODE_BROADCAST)
        while time.monotonic() < deadline:
            if self.peer_node_id is not None:
                return HelloAck(
                    boot_epoch=self.peer_boot_epoch or 0,
                    node_id=self.peer_node_id,
                    espnow_version=0,
                    decision=0,
                    hb_period_ms=self.peer_hb_period_ms or 0,
                    hb_miss_limit=self.peer_hb_miss_limit or 0,
                )
            time.sleep(0.01)
        return None

    def bye(self) -> None:
        """Leave deliberately, so the node records Left rather than Dead (section 8.2).

        boot_epoch 0: a host has no NVS incarnation to count. Saying 0 is honest; inventing one
        would make the node's epoch-monotonicity check compare against a number that means nothing.
        """
        if self.peer_node_id is None:
            return
        self.send_frame(
            fr.Op.BYE,
            Bye(boot_epoch=0, node_id=self.node_id, reason=0).encode(),
            dst=self.peer_node_id,
        )

    # -- the namespace ---------------------------------------------------------------------------
    def read(self, path: str | int, *, timeout: float = 1.0, dst: int | None = None) -> Reading:
        """READ one resource and return section 4 rule 2's tuple.

        Raises RequestTimeout rather than returning an empty Reading. A Reading is a statement about
        a resource; fabricating one for a request that was never answered would be the unmarked
        value the section exists to forbid. UNAVAILABLE means the node said so.
        """
        h = path if isinstance(path, int) else path_hash(path)
        rep = self._request(fr.Op.READ, Read(path_hash=h).encode(), h, timeout=timeout, dst=dst)
        return rep.reading()

    def write(self, path: str | int, value: Value, *, timeout: float = 1.0,
              dst: int | None = None) -> Reply:
        """WRITE one resource. Returns the REPLY, including a refusal.

        The REPLY is returned rather than a bool because the node answers *what happened* -- a write
        refused as NOT_WRITABLE, WRONG_OWNER or TYPE_MISMATCH is three different conversations, and
        collapsing them to False loses the one that tells you the manifest is wrong.
        """
        h = path if isinstance(path, int) else path_hash(path)
        return self._request(fr.Op.WRITE, Write(path_hash=h, value=value).encode(), h,
                             timeout=timeout, dst=dst)

    def _request(self, opcode: int, payload: bytes, path_h: int, *, timeout: float,
                 dst: int | None) -> Reply:
        target = dst if dst is not None else self.peer_node_id
        if target is None:
            raise BridgeError(
                "no peer yet: call hello() first, or pass dst= if you know the node id"
            )
        msg_id = self._next_msg_id()
        pend = _Pending(msg_id=msg_id, op=opcode, path_hash=path_h,
                        sent_monotonic=time.monotonic())

        # Registered *before* the send, never after. The reply can arrive on the reader thread
        # between send_frame() returning and this dict being written, and then it has nowhere to go.
        # The same hazard bit the firmware's own request path.
        with self._lock:
            self._pending[msg_id] = pend
        try:
            self.send_frame(opcode, payload, dst=target, msg_id=msg_id, ack_req=True)
        except Exception:
            with self._lock:
                self._pending.pop(msg_id, None)
            raise

        if not pend.done.wait(timeout):
            with self._lock:
                self._pending.pop(msg_id, None)
            self.stats.timeouts += 1
            raise RequestTimeout(
                f"{fr.Op.name_of(opcode)} hash=0x{path_h:08x} msg={msg_id}: "
                f"no REPLY in {timeout:.3f} s"
            )
        if pend.reply is None:
            raise BridgeError("the link closed while the request was outstanding")
        return pend.reply

    def last_rtt_s(self, msg_id: int) -> float | None:  # pragma: no cover - diagnostics
        with self._lock:
            p = self._pending.get(msg_id)
        return p.rtt_s if p else None

    # -- receiving -------------------------------------------------------------------------------
    def _read_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data = self.transport.read(4096)
            except Exception as exc:  # pragma: no cover - transport dependent
                if self.on_log:
                    self.on_log(f"read failed: {exc}")
                break
            if not data:
                if getattr(self.transport, "closed_by_peer", False):
                    if self.on_log:
                        self.on_log("the far end closed the link")
                    break
                time.sleep(0.005)
                continue
            self.stats.rx_bytes += len(data)
            for raw in self.reassembler.feed(data):
                self._on_raw_frame(raw)
            self.stats.bad_frames = (
                self.reassembler.bad_crc
                + self.reassembler.cobs_invalid
                + self.reassembler.too_long
            )

    def _on_raw_frame(self, raw: bytes) -> None:
        self.stats.rx_frames += 1
        self._tee(raw, "rx")
        try:
            f = fr.parse(raw)
        except fr.FrameError:
            self.stats.bad_frames += 1
            return

        # A frame addressed to someone else is not ours to act on, even arriving down our own wire.
        if f.dst != fr.NODE_BROADCAST and f.dst != self.node_id:
            return

        if f.opcode == fr.Op.REPLY:
            self._on_reply(f)
            return
        if f.opcode == fr.Op.HELLO_ACK:
            self._on_hello_ack(f)
            return
        if f.opcode == fr.Op.HELLO:
            self.stats.hellos_rx += 1
            self._learn_peer_from_hello(f)
            return
        if f.opcode == fr.Op.HEARTBEAT:
            self._on_heartbeat(f)
            return
        if f.opcode == fr.Op.ERR:
            self.stats.errs_rx += 1
            if self.on_log:
                err = decode_payload(f.opcode, f.payload)
                self.on_log(f"node reported {err}")
            return

    def _on_reply(self, f: fr.Frame) -> None:
        try:
            rep = Reply.parse(f.payload)
        except ValueError:
            self.stats.bad_frames += 1
            return
        with self._lock:
            pend = self._pending.pop(f.msg_id, None)
        if pend is None:
            self.stats.replies_unmatched += 1
            return
        # A round trip, measured end to end on this host's clock. Never a one-way figure: the two
        # clocks are unrelated and the difference would be a number with no meaning.
        pend.rtt_s = time.monotonic() - pend.sent_monotonic
        pend.reply = rep
        self.stats.replies_matched += 1
        if self.peer_node_id is None:
            self.peer_node_id = f.src
        pend.done.set()

    def _on_hello_ack(self, f: fr.Frame) -> None:
        try:
            ack = HelloAck.parse(f.payload)
        except ValueError:
            self.stats.bad_frames += 1
            return
        self.peer_node_id = ack.node_id or f.src
        self.peer_boot_epoch = ack.boot_epoch
        self.peer_hb_period_ms = ack.hb_period_ms
        self.peer_hb_miss_limit = ack.hb_miss_limit
        if self.on_log:
            verdict = "admitted" if ack.admitted else f"refused (decision {ack.decision})"
            self.on_log(
                f"node 0x{self.peer_node_id:04x} epoch {ack.boot_epoch} {verdict}; "
                f"its death window is {ack.hb_period_ms} ms x {ack.hb_miss_limit}"
            )

    def _learn_peer_from_hello(self, f: fr.Frame) -> None:
        try:
            h = Hello.parse(f.payload)
        except ValueError:
            return
        if self.peer_node_id is None:
            self.peer_node_id = h.node_id or f.src
            self.peer_boot_epoch = h.boot_epoch
            self.peer_hb_period_ms = h.hb_period_ms
            self.peer_hb_miss_limit = h.hb_miss_limit

    def _on_heartbeat(self, f: fr.Frame) -> None:
        self.stats.heartbeats_rx += 1
        try:
            hb = Heartbeat.parse(f.payload)
        except ValueError:
            self.stats.bad_frames += 1
            return
        self.last_heartbeat = hb
        self.last_heartbeat_at = time.monotonic()
        if self.peer_node_id is None:
            self.peer_node_id = f.src
        if self.peer_boot_epoch is not None and hb.boot_epoch > self.peer_boot_epoch:
            if self.on_log:
                self.on_log(
                    f"node 0x{f.src:04x} rebooted: epoch {self.peer_boot_epoch} -> {hb.boot_epoch}"
                )
        if hb.boot_epoch:
            self.peer_boot_epoch = hb.boot_epoch

    # -- heartbeating ----------------------------------------------------------------------------
    def _hb_loop(self) -> None:
        period = self.hb_period_ms / 1000.0
        while not self._stop.wait(period):
            if self.peer_node_id is None:
                continue
            self._hb_seq = (self._hb_seq + 1) & 0xFFFFFFFF
            hb = Heartbeat(
                uptime_ms=self.uptime_ms,
                boot_epoch=0,
                hb_seq=self._hb_seq,
                tx_frames=self.stats.tx_frames,
                # A UART has no MAC-layer ACK, so there is no delivery signal to report. Both
                # counters stay 0, which makes tx_pdr None -- "unmeasured", not "perfect". Reporting
                # 1.0 here would put a fabricated PDR into section 13-M0's numbers.
                tx_cb_ok=0,
                tx_cb_fail=0,
                rx_frames=self.stats.rx_frames,
                rx_lost_seqgap=0,
                turnaround_us=0,
                ack_of_msg_id=0,
                rtt_min_us=None,
                rtt_max_us=None,
                free_dram_kib=0,  # a host's free memory is not comparable to a node's; 0 = n/a
                espnow_version=0,
                hb_flags=0,
            )
            try:
                self.send_frame(fr.Op.HEARTBEAT, hb.encode(), dst=self.peer_node_id)
                self.stats.heartbeats_tx += 1
            except BridgeError:
                # The link went away; the reader loop will notice and stop. Not fatal here.
                pass

    # -- convenience -----------------------------------------------------------------------------
    def read_many(self, paths: list[str], *, timeout: float = 1.0) -> dict[str, Reading | str]:
        """READ several resources, keeping a failure next to the path that produced it.

        A dict of path -> Reading-or-error string rather than raising on the first miss: reading six
        resources and being told only about the first that timed out is a worse diagnostic than
        seeing which five worked.
        """
        out: dict[str, Reading | str] = {}
        for p in paths:
            try:
                out[p] = self.read(p, timeout=timeout)
            except RequestTimeout as exc:
                out[p] = f"TIMEOUT ({exc})"
            except BridgeError as exc:
                out[p] = f"ERROR ({exc})"
        return out

    def describe(self) -> str:
        peer = f"0x{self.peer_node_id:04x}" if self.peer_node_id is not None else "unknown"
        return (f"bridge 0x{self.node_id:04x} on {self.transport.description}, "
                f"peer {peer}")
