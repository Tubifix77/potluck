"""The RTT histogram's bucket edges, mirroring
firmware/components/pot_link/include/pot/link_stats.hpp.

Duplicated deliberately, and tested against the firmware's own output in
tests/test_records.py: the node ships bucket *counts* with no edges, because
sending sixteen constants every ten seconds would be silly, so the host has to
know the edges. Which means they can drift. The test is what stops that.
"""

from __future__ import annotations

#: Upper edge of each bucket in microseconds; the last is the overflow bucket.
#: Chosen from ARCHITECTURE.md section 3's measured ESP-NOW distribution rather
#: than a power-of-two ladder -- dense around the ~2.8 ms floor and the tail to
#: 59 ms, coarse beyond the ~104 ms in-protocol retry ceiling.
RTT_BUCKET_EDGE_US: tuple[int, ...] = (
    1000,
    2000,
    3000,
    4000,
    6000,
    8000,
    11000,
    16000,
    22000,
    30000,
    42000,
    60000,
    85000,
    110000,
    200000,
    0xFFFFFFFF,
)

RTT_BUCKETS = len(RTT_BUCKET_EDGE_US)
assert RTT_BUCKETS == 16


def bucket_bounds(index: int) -> tuple[int, int | None]:
    """(lower, upper) in microseconds for a bucket. Upper is None for the overflow."""
    if not 0 <= index < RTT_BUCKETS:
        raise IndexError(index)
    lo = 0 if index == 0 else RTT_BUCKET_EDGE_US[index - 1]
    hi = RTT_BUCKET_EDGE_US[index]
    return lo, (None if hi == 0xFFFFFFFF else hi)


def describe_bucket(index: int) -> str:
    """A human label like '2.0-3.0 ms' or '>200 ms'."""
    lo, hi = bucket_bounds(index)
    if hi is None:
        return f">{lo / 1000:.0f} ms"
    return f"{lo / 1000:.1f}-{hi / 1000:.1f} ms"


def bucket_of(us: int) -> int:
    """Which bucket a sample lands in -- the same rule the firmware applies:
    the first bucket whose edge the sample does not exceed."""
    for i, edge in enumerate(RTT_BUCKET_EDGE_US):
        if us <= edge:
            return i
    return RTT_BUCKETS - 1


def percentile(hist: list[int] | tuple[int, ...], pct: int) -> tuple[int, int | None] | None:
    """The bucket interval containing a percentile, or None with no samples.

    An interval, not a number. Interpolating inside a bucket would invent
    precision the measurement does not have, and section 13-M0 asks for a
    measured figure. Mirrors pot::RttHistogram::percentile(), including the
    1-based ceiling rank, so the host and the node agree on which bucket p99 is.
    """
    total = sum(hist)
    if total == 0:
        return None
    pct = max(0, min(100, pct))
    rank = max(1, -(-total * pct // 100))  # ceiling division

    cumulative = 0
    for i, count in enumerate(hist):
        cumulative += count
        if cumulative >= rank:
            return bucket_bounds(i)
    return bucket_bounds(RTT_BUCKETS - 1)


def mean_bounds(hist: list[int] | tuple[int, ...]) -> tuple[float, float | None] | None:
    """Bounds on the mean, from the bucket edges.

    Returns (lower, upper), where upper is None if the overflow bucket is
    occupied -- an unbounded bucket makes the mean unbounded above, and saying so
    is more useful than picking a number for it.
    """
    total = sum(hist)
    if total == 0:
        return None
    lower = 0.0
    upper: float | None = 0.0
    for i, count in enumerate(hist):
        if count == 0:
            continue
        lo, hi = bucket_bounds(i)
        lower += lo * count
        if hi is None:
            upper = None
        elif upper is not None:
            upper += hi * count
    return (lower / total, None if upper is None else upper / total)
