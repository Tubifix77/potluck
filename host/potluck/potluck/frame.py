"""Potluck Frame v1 decoder -- ARCHITECTURE.md section 5.1, 5.2, 5.4.

This is a *second, independent* implementation of the codec in
firmware/components/pot_frame. That duplication is the point. A single
implementation can only be tested for self-consistency: encode, decode, compare,
and a header field silently swapped for its neighbour passes every time. Two
implementations written from the specification, checked against the same golden
bytes, actually test the specification -- and section 14 says "keep the wire
format specified and versioned independently of the implementation", which is
hard to claim when there is one implementation.

Kept to the standard library so it runs anywhere Python does.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum

MAGIC = 0xD9
VERSION = 1
HEADER_SIZE = 16
AUTH_TAG_SIZE = 8

FLAG_FRAG = 0x08
FLAG_ACKREQ = 0x04
FLAG_AUTH = 0x02
FLAG_RESERVED = 0x01
FLAG_MASK = 0x0F

NODE_UNPROVISIONED = 0x0000
NODE_BROADCAST = 0xFFFF

CLASS_MAX = 4  # section 4 defines L0..L4
PRIORITY_MAX = 31

ESPNOW_V2_LINK_MTU = 1470
ESPNOW_V1_LINK_MTU = 250
MAX_PAYLOAD_V2 = ESPNOW_V2_LINK_MTU - HEADER_SIZE - AUTH_TAG_SIZE  # 1446
MAX_PAYLOAD_V1 = ESPNOW_V1_LINK_MTU - HEADER_SIZE - AUTH_TAG_SIZE  # 226

# Little-endian, in section 5.1's field order. The struct format is the whole
# header definition -- there is no field-by-field unpacking to get out of order.
_HEADER = struct.Struct("<BBHHBBHHHH")
assert _HEADER.size == HEADER_SIZE


class Op(IntEnum):
    """Opcodes -- section 5.2. M0 implements the membership set and ERR; M1 adds the namespace.

    Only opcodes the firmware actually implements appear here. The rest stay commented out in
    firmware/components/pot_frame/include/pot/opcodes.hpp so the numbering is reserved without
    implying the behaviour exists -- and this list follows the same rule, because a host tool that
    names an opcode it cannot possibly receive is a tool that lies about what the fleet can do.
    """

    HELLO = 0x01
    HELLO_ACK = 0x02
    HEARTBEAT = 0x03
    BYE = 0x04
    READ = 0x10
    WRITE = 0x11
    CALL = 0x20
    REPLY = 0x21
    CAST = 0x22
    SAFE_STATE = 0x50
    ERR = 0x7F

    @classmethod
    def name_of(cls, value: int) -> str:
        try:
            return cls(value).name
        except ValueError:
            return f"0x{value:02x}"


#: section 5.4: "No fragmentation for SAFE_STATE, HEARTBEAT, or HELLO."
NO_FRAG_OPCODES = frozenset({Op.SAFE_STATE, Op.HEARTBEAT, Op.HELLO})


class FrameError(Exception):
    """A frame that is not what section 5 describes.

    The `reason` strings match pot::frame_error_str() in the firmware, so a
    rejection counted on the node and one counted here are the same word.
    """

    def __init__(self, reason: str, detail: str = "") -> None:
        super().__init__(f"{reason}: {detail}" if detail else reason)
        self.reason = reason
        self.detail = detail


@dataclass(frozen=True, slots=True)
class Frame:
    """A decoded Potluck Frame."""

    magic: int
    ver_flags: int
    src: int
    dst: int
    opcode: int
    lclass_pri: int
    seq: int
    msg_id: int
    frag_off: int
    total_len: int
    payload: bytes
    auth_tag: bytes | None

    # -- section 5.1 packed fields ------------------------------------------
    @property
    def version(self) -> int:
        return self.ver_flags >> 4

    @property
    def flags(self) -> int:
        return self.ver_flags & FLAG_MASK

    @property
    def is_frag(self) -> bool:
        return bool(self.flags & FLAG_FRAG)

    @property
    def wants_ack(self) -> bool:
        return bool(self.flags & FLAG_ACKREQ)

    @property
    def has_auth(self) -> bool:
        return bool(self.flags & FLAG_AUTH)

    @property
    def lclass(self) -> int:
        return self.lclass_pri >> 5

    @property
    def priority(self) -> int:
        return self.lclass_pri & 0x1F

    @property
    def opcode_name(self) -> str:
        return Op.name_of(self.opcode)

    def __str__(self) -> str:
        bits = []
        if self.is_frag:
            bits.append("FRAG")
        if self.wants_ack:
            bits.append("ACKREQ")
        if self.has_auth:
            bits.append("AUTH")
        flags = "|".join(bits) if bits else "-"
        return (
            f"{self.opcode_name} 0x{self.src:04x}->0x{self.dst:04x} "
            f"seq={self.seq} msg={self.msg_id} L{self.lclass}p{self.priority} "
            f"{flags} len={len(self.payload)}"
        )


def parse(buf: bytes, max_payload: int = MAX_PAYLOAD_V2) -> Frame:
    """Decode and validate one frame, or raise FrameError.

    `max_payload` is the transport profile cap pinned for the peer these bytes
    came from (section 5.3). Pass MAX_PAYLOAD_V1 for a peer not known to be
    v2.0: a v1.0 receiver truncates a longer frame rather than rejecting it, so
    the cap has to be enforced on the way in as well as on the way out.

    Every check here mirrors one in pot::parse(). Keeping the two in step is
    what tests/test_frame.py's golden vectors are for.
    """
    if len(buf) < HEADER_SIZE:
        raise FrameError("too_short", f"{len(buf)} bytes, need {HEADER_SIZE}")

    (
        magic,
        ver_flags,
        src,
        dst,
        opcode,
        lclass_pri,
        seq,
        msg_id,
        frag_off,
        total_len,
    ) = _HEADER.unpack_from(buf, 0)

    if magic != MAGIC:
        raise FrameError("bad_magic", f"0x{magic:02x}")
    if (ver_flags >> 4) != VERSION:
        raise FrameError("bad_version", str(ver_flags >> 4))

    flags = ver_flags & FLAG_MASK
    if flags & FLAG_RESERVED:
        # section 5.1 says bit 0 must be 0. Rejecting it now is what lets a
        # future version claim it.
        raise FrameError("reserved_flag_set")
    if (lclass_pri >> 5) > CLASS_MAX:
        raise FrameError("bad_class", str(lclass_pri >> 5))

    has_auth = bool(flags & FLAG_AUTH)
    overhead = HEADER_SIZE + (AUTH_TAG_SIZE if has_auth else 0)
    if len(buf) < overhead:
        raise FrameError("missing_auth_tag", f"{len(buf)} bytes, need {overhead}")

    payload_len = len(buf) - overhead
    if payload_len > max_payload:
        raise FrameError("payload_too_long", f"{payload_len} > {max_payload}")

    is_frag = bool(flags & FLAG_FRAG)
    if is_frag and opcode in NO_FRAG_OPCODES:
        raise FrameError("frag_not_allowed", Op.name_of(opcode))

    # The length check that catches the section 5.3 truncation hazard: a v1.0
    # receiver handed a longer v2.0 frame may deliver only its first 250 bytes,
    # and a silently shortened frame parsed as if whole is worse than a dropped
    # one.
    if is_frag:
        if frag_off + payload_len > total_len:
            raise FrameError(
                "length_mismatch", f"frag_off {frag_off} + {payload_len} > total_len {total_len}"
            )
        if payload_len == 0:
            raise FrameError("length_mismatch", "empty fragment")
    else:
        if frag_off != 0:
            raise FrameError("length_mismatch", f"frag_off {frag_off} without FRAG")
        if payload_len != total_len:
            raise FrameError(
                "length_mismatch", f"{payload_len} payload bytes but total_len {total_len}"
            )

    payload = bytes(buf[HEADER_SIZE : HEADER_SIZE + payload_len])
    auth_tag = bytes(buf[HEADER_SIZE + payload_len :]) if has_auth else None

    return Frame(
        magic=magic,
        ver_flags=ver_flags,
        src=src,
        dst=dst,
        opcode=opcode,
        lclass_pri=lclass_pri,
        seq=seq,
        msg_id=msg_id,
        frag_off=frag_off,
        total_len=total_len,
        payload=payload,
        auth_tag=auth_tag,
    )


def encode(
    *,
    src: int = NODE_UNPROVISIONED,
    dst: int = NODE_BROADCAST,
    opcode: int = 0,
    lclass: int = 3,
    priority: int = 0,
    seq: int = 0,
    msg_id: int = 0,
    payload: bytes = b"",
    ack_req: bool = False,
    auth: bool = False,
    frag: bool = False,
    frag_off: int = 0,
    total_len: int | None = None,
) -> bytes:
    """Build a frame. Used by the tests and by replay; the firmware sends the
    real traffic.

    Mirrors pot::encode(), including deriving total_len for an unfragmented
    frame rather than trusting the caller -- which is the commonest way to emit
    a frame your own parser would reject.
    """
    if lclass > CLASS_MAX or priority > PRIORITY_MAX:
        raise FrameError("bad_argument", f"L{lclass} p{priority}")
    if frag and opcode in NO_FRAG_OPCODES:
        raise FrameError("frag_not_allowed", Op.name_of(opcode))

    if frag:
        if total_len is None:
            raise FrameError("bad_argument", "a fragment needs total_len")
        if len(payload) == 0 or frag_off + len(payload) > total_len:
            raise FrameError("length_mismatch", f"{frag_off} + {len(payload)} vs {total_len}")
    else:
        total_len = len(payload)
        frag_off = 0

    flags = 0
    if frag:
        flags |= FLAG_FRAG
    if ack_req:
        flags |= FLAG_ACKREQ
    if auth:
        flags |= FLAG_AUTH

    header = _HEADER.pack(
        MAGIC,
        (VERSION << 4) | flags,
        src,
        dst,
        opcode,
        (lclass << 5) | (priority & 0x1F),
        seq,
        msg_id,
        frag_off,
        total_len,
    )
    # section 14: the auth_tag bytes are reserved from day one; M5 fills them.
    # Zeroing rather than omitting keeps the MTU arithmetic in section 5.3 true.
    tag = bytes(AUTH_TAG_SIZE) if auth else b""
    return header + payload + tag
