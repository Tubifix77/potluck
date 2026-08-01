"""Path hashing, mirroring firmware/components/pot_ns/include/pot/namespace.hpp.

A node never sees a path string. ARCHITECTURE.md section 14's mitigation for namespace lookup cost
is to "perfect-hash the paths at build time from the manifest", so the node stores a 32-bit hash and
the *host* keeps the strings. That split only works if both sides compute exactly the same number
from the same string -- if they ever disagree, every read misses and nothing says why.

So this is a second implementation, written from the specification in namespace.hpp, and
tests/test_namespace.py checks it against the same reference values the C++ tests use. Same
reasoning as the two frame decoders: a constant duplicated across a boundary is a constant that
drifts unless something checks it.
"""

from __future__ import annotations

from dataclasses import dataclass

FNV1A32_OFFSET = 2166136261
FNV1A32_PRIME = 16777619
_MASK32 = 0xFFFFFFFF

#: A hash of 0 marks a free slot in the node's table, so it can never be a valid path.
FREE_SLOT = 0


def path_hash(path: str) -> int:
    """FNV-1a/32 over the UTF-8 bytes of `path`.

    Chosen over anything cleverer because it is four lines, needs no tables, and can be
    reimplemented correctly in another language in five minutes -- which is exactly what this file
    is.
    """
    h = FNV1A32_OFFSET
    for b in path.encode("utf-8"):
        h = ((h ^ b) * FNV1A32_PRIME) & _MASK32
    return h


@dataclass(frozen=True, slots=True)
class PathEntry:
    """A path and its hash, as the host-side manifest holds them."""

    path: str
    hash: int


class PathTable:
    """The host's side of the namespace: hash -> path.

    Lets a capture or a `read` be reported with the name a human typed rather than a number. The
    node cannot do this, by design, and pretending otherwise would mean shipping the strings to a
    part with 128 entries of 40 bytes each.
    """

    def __init__(self, paths: list[str] | None = None) -> None:
        self._by_hash: dict[int, str] = {}
        for p in paths or []:
            self.add(p)

    def add(self, path: str) -> PathEntry:
        h = path_hash(path)
        if h == FREE_SLOT:
            # 1-in-4-billion, and the node rejects it, so the host must too rather than register a
            # path that can never be looked up.
            raise ValueError(f"{path!r} hashes to 0, which marks a free slot; rename it")
        existing = self._by_hash.get(h)
        if existing is not None and existing != path:
            # A real collision between two paths a human chose. The node refuses this at declare
            # time; catching it here means the manifest fails to build rather than the fleet
            # aliasing two resources.
            raise ValueError(f"hash collision: {path!r} and {existing!r} both hash to 0x{h:08x}")
        self._by_hash[h] = path
        return PathEntry(path, h)

    def resolve(self, h: int) -> str | None:
        """The path for a hash, or None if this table has never seen it."""
        return self._by_hash.get(h)

    def name_or_hash(self, h: int) -> str:
        """The path if known, otherwise the hash rendered so it is obviously a hash."""
        return self._by_hash.get(h, f"<0x{h:08x}>")

    def __len__(self) -> int:
        return len(self._by_hash)

    def __contains__(self, path: str) -> bool:
        return path_hash(path) in self._by_hash
