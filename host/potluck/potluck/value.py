"""Typed values and readings -- ARCHITECTURE.md section 4 rule 2, host side.

Mirrors firmware/components/pot_ns/include/pot/value.hpp. Second independent implementation, for
the same reason as the frame codec and the serial framing: a wire type that exists once can only be
tested against itself.

Section 4 rule 2 is carried across the language boundary, not just the wire. `Reading` here has no
attribute that hands back the bare number:

    >>> r.value            # the Value, which knows its own type
    >>> r.number()         # (number, quality) -- never one without the other
    >>> r.usable()         # False for UNAVAILABLE and NO_DATA

A `float(reading)` would be the exact bug the section exists to prevent -- a location-transparent
read whose age got dropped on the floor -- so it is not implemented, and `__float__` raising is
deliberate rather than an oversight.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum

VALUE_BYTES_MAX = 8


class ValueType(IntEnum):
    NONE = 0
    BOOL = 1
    I32 = 2
    U32 = 3
    F32 = 4
    I64 = 5
    U64 = 6
    F64 = 7
    BYTES = 8

    @classmethod
    def name_of(cls, v: int) -> str:
        try:
            return cls(v).name
        except ValueError:
            return f"type-{v}"


#: struct format and width per type. BYTES and NONE have no fixed width.
_FMT = {
    ValueType.BOOL: ("<B", 1),
    ValueType.I32: ("<i", 4),
    ValueType.U32: ("<I", 4),
    ValueType.F32: ("<f", 4),
    ValueType.I64: ("<q", 8),
    ValueType.U64: ("<Q", 8),
    ValueType.F64: ("<d", 8),
}


def value_type_size(t: int) -> int:
    """Fixed width in bytes, or 0 for NONE and BYTES (which are length-delimited)."""
    entry = _FMT.get(ValueType(t) if t in set(ValueType) else None)  # type: ignore[arg-type]
    return entry[1] if entry else 0


class Unit(IntEnum):
    NONE = 0
    RATIO = 1
    PERCENT = 2
    METRE = 10
    MILLIMETRE = 11
    RADIAN = 20
    DEGREE = 21
    METRE_PER_SECOND2 = 30
    RADIAN_PER_SECOND = 31
    NEWTON = 40
    NEWTON_METRE = 41
    CELSIUS = 50
    KELVIN = 51
    PASCAL = 60
    VOLT = 70
    MILLIVOLT = 71
    AMPERE = 72
    MILLIAMPERE = 73
    WATT = 74
    SECOND = 80
    MILLISECOND = 81
    MICROSECOND = 82
    HERTZ = 90
    COUNT = 100
    BYTE = 101
    RAW = 200

    @classmethod
    def name_of(cls, v: int) -> str:
        try:
            return cls(v).name
        except ValueError:
            return f"unit-{v}"


#: How a unit is written next to a number. Empty for the dimensionless ones, because "42 NONE" is
#: worse than "42".
_UNIT_SUFFIX = {
    Unit.NONE: "",
    Unit.RATIO: "",
    Unit.COUNT: "",
    Unit.PERCENT: "%",
    Unit.METRE: "m",
    Unit.MILLIMETRE: "mm",
    Unit.RADIAN: "rad",
    Unit.DEGREE: "deg",
    Unit.METRE_PER_SECOND2: "m/s2",
    Unit.RADIAN_PER_SECOND: "rad/s",
    Unit.NEWTON: "N",
    Unit.NEWTON_METRE: "Nm",
    Unit.CELSIUS: "C",
    Unit.KELVIN: "K",
    Unit.PASCAL: "Pa",
    Unit.VOLT: "V",
    Unit.MILLIVOLT: "mV",
    Unit.AMPERE: "A",
    Unit.MILLIAMPERE: "mA",
    Unit.WATT: "W",
    Unit.SECOND: "s",
    Unit.MILLISECOND: "ms",
    Unit.MICROSECOND: "us",
    Unit.HERTZ: "Hz",
    Unit.BYTE: "B",
    Unit.RAW: "raw",
}


def unit_suffix(u: int) -> str:
    try:
        return _UNIT_SUFFIX.get(Unit(u), Unit.name_of(u))
    except ValueError:
        return f"unit-{u}"


class Quality(IntEnum):
    GOOD = 0
    STALE = 1
    UNAVAILABLE = 2
    NO_DATA = 3
    FAULTY = 4

    @classmethod
    def name_of(cls, v: int) -> str:
        try:
            return cls(v).name
        except ValueError:
            return f"quality-{v}"


def quality_has_value(q: int) -> bool:
    """STALE is included, deliberately -- that is section 4 rule 2's whole point.

    Anything that must refuse old data says so explicitly instead of leaning on this.
    """
    return q in (Quality.GOOD, Quality.STALE)


class StalenessPolicy(IntEnum):
    INFORMATIVE = 0
    STRICT = 1


class ResourceKind(IntEnum):
    SAMPLED = 0
    EVENT = 1


class Access(IntEnum):
    READ = 1
    WRITE = 2
    READ_WRITE = 3


class NsError(IntEnum):
    """Mirrors pot::NsError. The names are what potctl prints, so they are the node's words."""

    OK = 0
    NOT_FOUND = 1
    FULL = 2
    HASH_COLLISION = 3
    TYPE_MISMATCH = 4
    NOT_WRITABLE = 5
    NOT_READABLE = 6
    WRONG_OWNER = 7
    EVENT_NOT_CACHED = 8

    @classmethod
    def name_of(cls, v: int) -> str:
        try:
            return cls(v).name
        except ValueError:
            return f"ns-error-{v}"


@dataclass(frozen=True)
class Value:
    """A typed value exactly as the wire carries it: type, length, and 8 raw little-endian bytes."""

    type: int = int(ValueType.NONE)
    raw: bytes = b""

    # --- constructors ---------------------------------------------------------------------------
    @staticmethod
    def of_bool(v: bool) -> Value:
        return Value(int(ValueType.BOOL), bytes((1 if v else 0,)))

    @staticmethod
    def of_i32(v: int) -> Value:
        return Value(int(ValueType.I32), struct.pack("<i", v))

    @staticmethod
    def of_u32(v: int) -> Value:
        return Value(int(ValueType.U32), struct.pack("<I", v))

    @staticmethod
    def of_f32(v: float) -> Value:
        return Value(int(ValueType.F32), struct.pack("<f", v))

    @staticmethod
    def of_i64(v: int) -> Value:
        return Value(int(ValueType.I64), struct.pack("<q", v))

    @staticmethod
    def of_u64(v: int) -> Value:
        return Value(int(ValueType.U64), struct.pack("<Q", v))

    @staticmethod
    def of_f64(v: float) -> Value:
        return Value(int(ValueType.F64), struct.pack("<d", v))

    @staticmethod
    def of_bytes(b: bytes) -> Value:
        if len(b) > VALUE_BYTES_MAX:
            raise ValueError(f"a Value holds at most {VALUE_BYTES_MAX} bytes, got {len(b)}")
        return Value(int(ValueType.BYTES), bytes(b))

    # --- accessors ------------------------------------------------------------------------------
    @property
    def len(self) -> int:
        return len(self.raw)

    def number(self) -> int | float | bool | None:
        """The number, or None for NONE and BYTES. Type-faithful: no reinterpretation.

        A short `raw` returns None rather than zero-padding. A truncated value is a wire fault and
        inventing the missing bytes would turn it into a plausible reading.
        """
        t = self.type
        entry = _FMT.get(ValueType(t)) if t in set(ValueType) else None  # type: ignore[arg-type]
        if entry is None:
            return None
        fmt, width = entry
        if len(self.raw) < width:
            return None
        v = struct.unpack(fmt, self.raw[:width])[0]
        return bool(v) if t == ValueType.BOOL else v

    def to_wire(self) -> tuple[int, int, bytes]:
        """(type, len, 8 bytes zero-padded) -- what WritePayload and ReplyPayload carry."""
        return self.type, len(self.raw), self.raw.ljust(VALUE_BYTES_MAX, b"\x00")

    @staticmethod
    def from_wire(vtype: int, vlen: int, raw: bytes) -> Value:
        if vlen > VALUE_BYTES_MAX:
            vlen = VALUE_BYTES_MAX
        return Value(vtype, bytes(raw[:vlen]))

    def format(self) -> str:
        if self.type == ValueType.NONE:
            return "none"
        if self.type == ValueType.BYTES:
            return "0x" + self.raw.hex() if self.raw else "0x"
        n = self.number()
        if n is None:
            return f"<{ValueType.name_of(self.type)} truncated {len(self.raw)}B>"
        if isinstance(n, bool):
            return "true" if n else "false"
        if isinstance(n, float):
            return f"{n:g}"
        return str(n)

    def json(self):
        """A JSON-safe rendering: the number itself where there is one, else a hex string."""
        if self.type == ValueType.NONE:
            return None
        if self.type == ValueType.BYTES:
            return "0x" + self.raw.hex()
        return self.number()

    def __str__(self) -> str:
        return self.format()


@dataclass
class Reading:
    """Section 4 rule 2's tuple, entire. No attribute yields the number on its own."""

    value: Value = field(default_factory=Value)
    unit: int = int(Unit.NONE)
    timestamp_ms: int = 0
    age_ms: int = 0
    latency_class: int = 4
    quality: int = int(Quality.NO_DATA)

    def usable(self) -> bool:
        return quality_has_value(self.quality)

    def number(self) -> tuple[int | float | bool | None, int]:
        """(number, quality). The only way to the number, and the quality comes with it."""
        if not self.usable():
            return None, self.quality
        return self.value.number(), self.quality

    def __float__(self):
        raise TypeError(
            "a Reading does not convert to a float: section 4 rule 2 forbids handing back a bare "
            "value. Use reading.number(), which returns (number, quality)."
        )

    def format(self) -> str:
        """One line carrying every element of the tuple. Matches Reading::format() in the firmware."""
        q = Quality.name_of(self.quality)
        if not self.usable():
            return f"{q} (age {self.age_ms} ms, class L{self.latency_class})"
        suffix = unit_suffix(self.unit)
        num = self.value.format()
        shown = f"{num} {suffix}".rstrip() if suffix else num
        return f"{shown}  [{q}, age {self.age_ms} ms, ts {self.timestamp_ms}, class L{self.latency_class}]"

    def __str__(self) -> str:
        return self.format()
