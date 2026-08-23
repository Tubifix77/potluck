"""M0 payload decoders, mirroring firmware/components/pot_frame/include/pot/payloads.hpp.

The struct format strings below are the layouts. They are written from the
offset tables in payloads.hpp, not generated from it, so a field reordered on the
firmware side fails a golden test here rather than producing plausible nonsense in
a capture.

Every one is fixed-size and little-endian, with no implicit padding -- the
firmware asserts that at compile time and `assert` below asserts the matching
size here.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

HELLO_FLAG_WANT_ACK = 0x01
HB_FLAG_IS_REPLY = 0x01

#: Sentinel for "no RTT sample yet" in the divide-by-8 quantised fields.
RTT_UNKNOWN_D8 = 0xFFFF

ADMIT_OK = 0x00

_HELLO = struct.Struct("<IIHHBBBB8s")
_HELLO_ACK = struct.Struct("<IHBBBBH")
_HEARTBEAT = struct.Struct("<IIIIIIIIIHHHHBBH")
_BYE = struct.Struct("<IHBB")
_ERR = struct.Struct("<HHB3s")

assert _HELLO.size == 24, _HELLO.size
assert _HELLO_ACK.size == 12, _HELLO_ACK.size
assert _HEARTBEAT.size == 48, _HEARTBEAT.size
assert _BYE.size == 8, _BYE.size
assert _ERR.size == 8, _ERR.size


def dequantise_us_d8(d8: int) -> int | None:
    """Undo the divide-by-8 quantisation, mapping the sentinel to None.

    None, not 0: "no sample yet" and "a sample of zero" are different facts, and
    section 13-M0 wants measured numbers.
    """
    if d8 == RTT_UNKNOWN_D8:
        return None
    return d8 << 3


class ShortPayload(ValueError):
    """Fewer bytes arrived than the opcode's fixed part requires."""


def _unpack(s: struct.Struct, payload: bytes, what: str) -> tuple:
    if len(payload) < s.size:
        raise ShortPayload(f"{what}: {len(payload)} bytes, need {s.size}")
    # Extra trailing bytes are ignored rather than rejected: that is how a
    # decoder written today keeps working when a later milestone appends a field.
    return s.unpack_from(payload, 0)


@dataclass(frozen=True, slots=True)
class Hello:
    boot_epoch: int
    caps: int
    node_id: int
    espnow_version: int
    hb_period_ms: int
    hb_miss_limit: int
    flags: int
    pubkey_fp: bytes

    @property
    def wants_ack(self) -> bool:
        return bool(self.flags & HELLO_FLAG_WANT_ACK)

    @property
    def dead_after_ms(self) -> int:
        """The peer's own death window -- section 8.2's period x misses."""
        return self.hb_period_ms * self.hb_miss_limit

    @classmethod
    def parse(cls, payload: bytes) -> Hello:
        (epoch, caps, node_id, _res0, ver, period_cs, misses, flags, fp) = _unpack(
            _HELLO, payload, "HELLO"
        )
        return cls(
            boot_epoch=epoch,
            caps=caps,
            node_id=node_id,
            espnow_version=ver,
            hb_period_ms=period_cs * 10,
            hb_miss_limit=misses,
            flags=flags,
            pubkey_fp=fp,
        )

    def encode(self) -> bytes:
        """The host sends this too -- potluck-bridge joins as an ordinary peer (section 8.1).

        hb_period_ms is carried in centiseconds, so a period that is not a multiple of 10 ms cannot
        be expressed. Rejecting it is deliberate: rounding here would mean the peer's death window
        silently differed from the one this host believes it announced.
        """
        if self.hb_period_ms % 10:
            raise ValueError(
                f"hb_period_ms must be a multiple of 10 (the wire field is centiseconds), "
                f"got {self.hb_period_ms}"
            )
        return _HELLO.pack(
            self.boot_epoch,
            self.caps,
            self.node_id,
            0,
            self.espnow_version,
            self.hb_period_ms // 10,
            self.hb_miss_limit,
            self.flags,
            self.pubkey_fp.ljust(8, b"\x00")[:8],
        )


@dataclass(frozen=True, slots=True)
class HelloAck:
    boot_epoch: int
    node_id: int
    espnow_version: int
    decision: int
    hb_period_ms: int
    hb_miss_limit: int

    @property
    def admitted(self) -> bool:
        return self.decision == ADMIT_OK

    @classmethod
    def parse(cls, payload: bytes) -> HelloAck:
        (epoch, node_id, ver, decision, period_cs, misses, _res0) = _unpack(
            _HELLO_ACK, payload, "HELLO_ACK"
        )
        return cls(
            boot_epoch=epoch,
            node_id=node_id,
            espnow_version=ver,
            decision=decision,
            hb_period_ms=period_cs * 10,
            hb_miss_limit=misses,
        )


@dataclass(frozen=True, slots=True)
class Heartbeat:
    uptime_ms: int
    boot_epoch: int
    hb_seq: int
    tx_frames: int
    tx_cb_ok: int
    tx_cb_fail: int
    rx_frames: int
    rx_lost_seqgap: int
    turnaround_us: int
    ack_of_msg_id: int
    rtt_min_us: int | None
    rtt_max_us: int | None
    free_dram_kib: int
    espnow_version: int
    hb_flags: int

    @property
    def is_reply(self) -> bool:
        return bool(self.hb_flags & HB_FLAG_IS_REPLY)

    @property
    def tx_pdr(self) -> float | None:
        """Outbound delivery ratio as the *sender* measured it.

        None when nothing has been attempted. Never a default of 1.0: an
        unmeasured link reports unknown.
        """
        attempted = self.tx_cb_ok + self.tx_cb_fail
        return (self.tx_cb_ok / attempted) if attempted else None

    @property
    def rx_pdr(self) -> float | None:
        """Inbound delivery ratio as the sender measured it, from seq gaps."""
        expected = self.rx_frames + self.rx_lost_seqgap
        return (self.rx_frames / expected) if expected else None

    @classmethod
    def parse(cls, payload: bytes) -> Heartbeat:
        (
            uptime_ms,
            boot_epoch,
            hb_seq,
            tx_frames,
            tx_cb_ok,
            tx_cb_fail,
            rx_frames,
            rx_lost_seqgap,
            turnaround_us,
            ack_of_msg_id,
            rtt_min_d8,
            rtt_max_d8,
            free_dram_kib,
            espnow_version,
            hb_flags,
            _res0,
        ) = _unpack(_HEARTBEAT, payload, "HEARTBEAT")
        return cls(
            uptime_ms=uptime_ms,
            boot_epoch=boot_epoch,
            hb_seq=hb_seq,
            tx_frames=tx_frames,
            tx_cb_ok=tx_cb_ok,
            tx_cb_fail=tx_cb_fail,
            rx_frames=rx_frames,
            rx_lost_seqgap=rx_lost_seqgap,
            turnaround_us=turnaround_us,
            ack_of_msg_id=ack_of_msg_id,
            rtt_min_us=dequantise_us_d8(rtt_min_d8),
            rtt_max_us=dequantise_us_d8(rtt_max_d8),
            free_dram_kib=free_dram_kib,
            espnow_version=espnow_version,
            hb_flags=hb_flags,
        )


    def encode(self) -> bytes:
        """The host heartbeats too, so it stays Alive in the node's peer table (section 8.2).

        A host that only ever said HELLO is marked Dead after period x misses. The node keeps
        answering it -- a dead peer's slot is not freed -- but its own `sys/peers-alive` then
        undercounts, and a capture of that shows a fleet quietly disagreeing with itself. Cheaper to
        heartbeat than to explain.

        rtt_min_us / rtt_max_us of None encode the RTT_UNKNOWN_D8 sentinel: "no sample yet" and "a
        sample of zero" are different facts, and section 13-M0 wants measured numbers.
        """
        def q(v: int | None) -> int:
            return RTT_UNKNOWN_D8 if v is None else min(v >> 3, RTT_UNKNOWN_D8 - 1)

        return _HEARTBEAT.pack(
            self.uptime_ms,
            self.boot_epoch,
            self.hb_seq,
            self.tx_frames,
            self.tx_cb_ok,
            self.tx_cb_fail,
            self.rx_frames,
            self.rx_lost_seqgap,
            self.turnaround_us,
            self.ack_of_msg_id,
            q(self.rtt_min_us),
            q(self.rtt_max_us),
            self.free_dram_kib,
            self.espnow_version,
            self.hb_flags,
            0,
        )


@dataclass(frozen=True, slots=True)
class Bye:
    boot_epoch: int
    node_id: int
    reason: int

    @classmethod
    def parse(cls, payload: bytes) -> Bye:
        epoch, node_id, reason, _res0 = _unpack(_BYE, payload, "BYE")
        return cls(boot_epoch=epoch, node_id=node_id, reason=reason)

    def encode(self) -> bytes:
        """The host says this too, so a bridge shutting down is recorded as Left, not Dead."""
        return _BYE.pack(self.boot_epoch, self.node_id, self.reason, 0)


@dataclass(frozen=True, slots=True)
class Err:
    code: int
    ref_msg_id: int
    detail: str

    @classmethod
    def parse(cls, payload: bytes) -> Err:
        code, ref, detail_len, _res0 = _unpack(_ERR, payload, "ERR")
        detail = payload[_ERR.size : _ERR.size + detail_len].decode("utf-8", "replace")
        return cls(code=code, ref_msg_id=ref, detail=detail)


#: Opcode value -> payload class, for decoding a frame's body by its opcode.
#:
#: Built by merging in the namespace payloads rather than importing them at module scope, because
#: ns_payloads imports value.py and this module is the one the frame decoder pulls in first. The
#: import lives inside the merge so a circular import cannot appear later by accident.
PAYLOAD_BY_OPCODE = {
    0x01: Hello,
    0x02: HelloAck,
    0x03: Heartbeat,
    0x04: Bye,
    0x7F: Err,
}


def _register_ns_payloads() -> None:
    from .ns_payloads import Call, Read, Reply, Write

    PAYLOAD_BY_OPCODE.setdefault(0x10, Read)
    PAYLOAD_BY_OPCODE.setdefault(0x11, Write)
    PAYLOAD_BY_OPCODE.setdefault(0x20, Call)
    PAYLOAD_BY_OPCODE.setdefault(0x21, Reply)
    PAYLOAD_BY_OPCODE.setdefault(0x22, Call)


_register_ns_payloads()


def decode_payload(opcode: int, payload: bytes):
    """Decode a frame body, or return None for an opcode this tool does not implement."""
    cls = PAYLOAD_BY_OPCODE.get(opcode)
    if cls is None:
        return None
    return cls.parse(payload)
