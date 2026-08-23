"""READ / WRITE / REPLY payloads -- ARCHITECTURE.md section 5.2, section 7.2, section 4 rule 2.

Mirrors firmware/components/pot_ns/include/pot/ns_payloads.hpp. Third place these three structs
exist (firmware header, firmware tests' golden bytes, here), and the offsets are asserted below
against the same numbers the firmware's static_asserts use, so a field reordered on one side fails
on the other.

REPLY answers both READ and WRITE; its `reply_to` says which. That makes a captured frame readable
without the correlation table that produced it, which is what section 7.6 needs from a capture --
a frame whose meaning depends on state the reader may have dropped is a frame that stops being
evidence.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

from .value import (
    VALUE_BYTES_MAX,
    NsError,
    Quality,
    Reading,
    Unit,
    Value,
    ValueType,
)

# Little-endian, in the firmware's field order. As with the frame header, the struct format *is* the
# layout definition -- there is no field-by-field unpacking to drift out of order.
_READ = struct.Struct("<IHH")
_WRITE = struct.Struct("<IBBH8s")
_REPLY = struct.Struct("<IIIHBBBBBB8s")

READ_SIZE = 8
WRITE_SIZE = 16
REPLY_SIZE = 28

assert _READ.size == READ_SIZE, _READ.size
assert _WRITE.size == WRITE_SIZE, _WRITE.size
assert _REPLY.size == REPLY_SIZE, _REPLY.size


class ShortNsPayload(ValueError):
    """A payload too short for the opcode it claims to be."""


def _unpack(s: struct.Struct, payload: bytes, what: str) -> tuple:
    if len(payload) < s.size:
        raise ShortNsPayload(f"{what} needs {s.size} bytes, got {len(payload)}")
    return s.unpack_from(payload, 0)


# ---------------------------------------------------------------------------------------------
# READ -- 0x10, 8 bytes
# ---------------------------------------------------------------------------------------------


@dataclass
class Read:
    path_hash: int
    flags: int = 0

    def encode(self) -> bytes:
        return _READ.pack(self.path_hash, self.flags, 0)

    @classmethod
    def parse(cls, payload: bytes) -> Read:
        path_hash, flags, _reserved0 = _unpack(_READ, payload, "READ")
        return cls(path_hash=path_hash, flags=flags)

    def __str__(self) -> str:
        return f"READ hash=0x{self.path_hash:08x}"


# ---------------------------------------------------------------------------------------------
# WRITE -- 0x11, 16 bytes
# ---------------------------------------------------------------------------------------------


@dataclass
class Write:
    path_hash: int
    value: Value

    def encode(self) -> bytes:
        vtype, vlen, raw = self.value.to_wire()
        return _WRITE.pack(self.path_hash, vtype, vlen, 0, raw)

    @classmethod
    def parse(cls, payload: bytes) -> Write:
        path_hash, vtype, vlen, _reserved0, raw = _unpack(_WRITE, payload, "WRITE")
        return cls(path_hash=path_hash, value=Value.from_wire(vtype, vlen, raw))

    def __str__(self) -> str:
        return f"WRITE hash=0x{self.path_hash:08x} value={self.value.format()}"


# ---------------------------------------------------------------------------------------------
# REPLY -- 0x21, 28 bytes. Answers both.
# ---------------------------------------------------------------------------------------------

REPLY_TO_READ = 0x10
REPLY_TO_WRITE = 0x11


@dataclass
class Reply:
    path_hash: int
    timestamp_ms: int
    age_ms: int
    unit: int
    reply_to: int
    status: int
    quality: int
    latency_class: int
    value: Value

    def encode(self) -> bytes:
        vtype, vlen, raw = self.value.to_wire()
        return _REPLY.pack(
            self.path_hash,
            self.timestamp_ms,
            self.age_ms,
            self.unit,
            self.reply_to,
            self.status,
            self.quality,
            self.latency_class,
            vtype,
            vlen,
            raw,
        )

    @classmethod
    def parse(cls, payload: bytes) -> Reply:
        (
            path_hash,
            timestamp_ms,
            age_ms,
            unit,
            reply_to,
            status,
            quality,
            latency_class,
            vtype,
            vlen,
            raw,
        ) = _unpack(_REPLY, payload, "REPLY")
        return cls(
            path_hash=path_hash,
            timestamp_ms=timestamp_ms,
            age_ms=age_ms,
            unit=unit,
            reply_to=reply_to,
            status=status,
            quality=quality,
            latency_class=latency_class,
            value=Value.from_wire(vtype, vlen, raw),
        )

    @staticmethod
    def from_reading(path_hash: int, r: Reading, status: int = int(NsError.OK),
                     reply_to: int = REPLY_TO_READ) -> Reply:
        return Reply(
            path_hash=path_hash,
            timestamp_ms=r.timestamp_ms,
            age_ms=r.age_ms,
            unit=r.unit,
            reply_to=reply_to,
            status=status,
            quality=r.quality,
            latency_class=r.latency_class,
            value=r.value,
        )

    def reading(self) -> Reading:
        """Section 4 rule 2's tuple, reconstructed. The inverse of from_reading().

        A REPLY whose status is not OK carries no value, so the Value is dropped rather than
        presented alongside an error -- a number next to NOT_FOUND is exactly the unmarked reading
        the section bans.
        """
        ok = self.status == NsError.OK
        return Reading(
            value=self.value if ok else Value(),
            unit=self.unit,
            timestamp_ms=self.timestamp_ms,
            age_ms=self.age_ms,
            latency_class=self.latency_class,
            quality=self.quality if ok else int(Quality.UNAVAILABLE),
        )

    @property
    def ok(self) -> bool:
        return self.status == NsError.OK

    def __str__(self) -> str:
        to = "READ" if self.reply_to == REPLY_TO_READ else (
            "WRITE" if self.reply_to == REPLY_TO_WRITE else f"op-0x{self.reply_to:02x}")
        if not self.ok:
            return (f"REPLY to {to} hash=0x{self.path_hash:08x} "
                    f"status={NsError.name_of(self.status)}")
        return (f"REPLY to {to} hash=0x{self.path_hash:08x} "
                f"{self.reading().format()}")


# ---------------------------------------------------------------------------------------------
# CALL / CAST -- 0x20 and 0x22, an 8-byte header plus opaque arguments
# ---------------------------------------------------------------------------------------------

_CALL = struct.Struct("<IHH")
CALL_SIZE = 8
assert _CALL.size == CALL_SIZE, _CALL.size


@dataclass
class Call:
    """An actor invocation. The result returns as an ordinary REPLY, carrying a Value.

    Results larger than a Value are not returned at all: the callee publishes them to a namespace
    path and the reply says only that they are there. That keeps messages small and costs the REPLY
    codec nothing -- see ns_payloads.hpp for the same note on the firmware side.

    CAST is this exact layout with no reply expected; the opcode carries the difference, so there is
    one class rather than two identical ones.
    """

    path_hash: int
    args: bytes = b""
    flags: int = 0

    def encode(self) -> bytes:
        return _CALL.pack(self.path_hash, self.flags, len(self.args)) + self.args

    @classmethod
    def parse(cls, payload: bytes) -> Call:
        path_hash, flags, arg_len = _unpack(_CALL, payload, "CALL")
        # The declared length must be backed by bytes that actually arrived. Rejected rather than
        # clamped: a silently truncated argument list is the same class of fault as an unmarked
        # stale value.
        if len(payload) < CALL_SIZE + arg_len:
            raise ShortNsPayload(
                f"CALL declares {arg_len} argument bytes but the payload holds "
                f"{len(payload) - CALL_SIZE}"
            )
        return cls(path_hash=path_hash, flags=flags, args=bytes(payload[CALL_SIZE:CALL_SIZE + arg_len]))

    def __str__(self) -> str:
        return f"CALL hash=0x{self.path_hash:08x} args={len(self.args)}B"


def describe_unit(u: int) -> str:
    return Unit.name_of(u)


def describe_type(t: int) -> str:
    return ValueType.name_of(t)
