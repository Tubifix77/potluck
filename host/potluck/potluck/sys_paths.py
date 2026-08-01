"""The built-in resource paths every Potluck node owns.

Mirrors firmware/components/pot_ns/include/pot/sys_resources.hpp. A node ships only the 32-bit hash
of a path, so the host needs the strings to turn an `ns` record back into something readable. If
these spellings drift from the firmware's, every resource shows as `<0x...>` -- which is why
tests/test_sys_paths.py checks them against the hashes the firmware's own tests assert.
"""

from __future__ import annotations

from .paths import PathTable, path_hash

#: Suffixes, in the same order as the SysResource enum. Order is not on the wire -- only the hash
#: is -- but keeping it aligned makes the two files diffable.
SYS_SUFFIXES: tuple[str, ...] = (
    "sys/heap-free",
    "sys/heap-largest",
    "sys/uptime",
    "sys/boot-epoch",
    "sys/peers-alive",
    "sys/rssi",
)

#: Human labels and the unit a reader should expect, for display only.
SYS_LABELS: dict[str, str] = {
    "sys/heap-free": "free internal DRAM",
    "sys/heap-largest": "largest free block",
    "sys/uptime": "uptime",
    "sys/boot-epoch": "boot epoch",
    "sys/peers-alive": "peers alive",
    "sys/rssi": "worst peer RSSI",
}


def sys_path(node_id: int, suffix: str) -> str:
    """The canonical path for a built-in resource on a node."""
    return f"potluck://lab/node-{node_id:04x}/{suffix}"


def sys_paths_for(node_id: int) -> list[str]:
    return [sys_path(node_id, s) for s in SYS_SUFFIXES]


def table_for_nodes(node_ids) -> PathTable:
    """A PathTable covering the built-ins of every node given.

    The host learns node ids from the `boot` and `link` records it is already reading, so a capture
    becomes self-describing without anyone supplying a manifest.
    """
    t = PathTable()
    for n in node_ids:
        for p in sys_paths_for(n):
            t.add(p)
    return t


def describe(path: str) -> str:
    """A human label for a known built-in, else the path itself."""
    for suffix, label in SYS_LABELS.items():
        if path.endswith("/" + suffix):
            return label
    return path


def sys_path_for_hash(node_id: int, h: int) -> str | None:
    """The built-in path on `node_id` whose hash is `h`, or None if it is not a built-in.

    A node ships only the hash, so this is the reverse direction: six candidates per node, hashed and
    compared. None rather than a guess -- an application resource's hash is not resolvable from here,
    and inventing a name for it would put a wrong label on a real reading.
    """
    for suffix in SYS_SUFFIXES:
        p = sys_path(node_id, suffix)
        if path_hash(p) == h:
            return p
    return None


__all__ = [
    "SYS_SUFFIXES",
    "SYS_LABELS",
    "sys_path",
    "sys_paths_for",
    "sys_path_for_hash",
    "table_for_nodes",
    "describe",
    "path_hash",
]
