"""Section 7.7's reconciler, the host's half -- M6.

The node decides at run time where a portable actor runs (firmware/components/pot_reconcile). What the
build decides, and freezes into the signed image, is the *input* to that decision: which actors are
portable, which nodes each may run on, and how strongly each node attracts it (data gravity, section
7.4). This module derives those, and ranks the nodes the same way the node does, so a person reading
a package can see where every portable actor will start and where it goes when a node dies.

    python -m potluck.reconcile manifests/m6-ticker.json

PORTABILITY IS DERIVED, NOT DECLARED (section 7.7). An actor is portable when nothing ties it to one
node's silicon:

  * not pinned -- a pin is a hand-written override, and it wins (section 7.4);
  * its own class is L3 or looser, and so is every resource it binds -- an L0-L2 binding is pinned by
    physics;
  * it binds nothing writable -- "actuator-owning actors are never portable regardless of class"
    (ADR-006), which is what keeps actuation exactly-once while activation is only at-least-once.

ELIGIBILITY AND GRAVITY. Eligible: every node the package declares with the headroom the actor asks
for, and -- for `background` priority -- a node in the background pool (section 7.8). Gravity: one, plus
one for every resource the actor binds that the node owns, so the actor prefers to sit where its data
is and "touches the radio never, not rarely". Ties are broken by rendezvous hashing (Thaler and
Ravishankar 1998), with the same weight function as the node: MurmurHash3's fmix32 (public domain).
"""

from __future__ import annotations

import sys
from dataclasses import dataclass

from .manifest import ActorSpec, Manifest
from .paths import path_hash

#: deploy.hpp's kPortableNode: the declaration's node field for "placed at run time".
PORTABLE_NODE = 0xFFFE
#: deploy.hpp's kMaxEligible and reconcile.hpp's kMaxPortable.
MAX_ELIGIBLE = 8
MAX_PORTABLE = 8
#: The modules that can be portable on this firmware (ADR-003 Tier 0).
PORTABLE_MODULES = ("builtin:ticker",)


def fmix32(h: int) -> int:
    h &= 0xFFFFFFFF
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h


def hrw_weight(key: int, node: int) -> int:
    """reconcile.cpp's hrw_weight, bit for bit."""
    return fmix32(fmix32(key) ^ (node & 0xFFFF))


def output_path(m: Manifest, a: ActorSpec) -> str:
    """Where a ticker publishes. Derived from the system and actor names, so it cannot collide with a
    node's canonical resources (those live under node-<id>/)."""
    return f"potluck://{m.system}/act/{a.name}/out"


def portability(m: Manifest, a: ActorSpec) -> tuple[bool, str]:
    """(portable, why). `why` names the binding or rule that pins it, for the package report."""
    if a.pin is not None:
        return False, f"pinned to 0x{a.pin:04x}"
    if a.latency_class < 3:
        return False, f"declares class L{a.latency_class}; portable actors are L3 or looser"
    for path in a.needs:
        r = m.resource(path)
        if r is None:
            return False, f"binds {path}, which no node in the package owns"
        if r.latency_class < 3:
            return False, f"binds {path} at L{r.latency_class}: pinned by physics"
        if r.access != "read":
            return False, f"binds {path} for writing: actuator-owning actors are never portable (ADR-006)"
    return True, "portable"


def eligible(m: Manifest, a: ActorSpec) -> list[tuple[int, int]]:
    """[(node_id, gravity)] in manifest order."""
    if a.pin is not None:
        return [(a.pin, 1)]
    out: list[tuple[int, int]] = []
    for n in m.nodes:
        if n.headroom_bytes < a.headroom_bytes:
            continue
        if a.priority == "background" and not n.background_allowed():
            continue
        owned = {r.path for r in n.owns}
        gravity = 1 + sum(1 for p in a.needs if m.bindings.get(p, p) in owned)
        out.append((n.node_id, min(gravity, 255)))
    return out


def rank(key: int, nodes: list[tuple[int, int]], live: set[int] | None = None) -> list[int]:
    """reconcile.cpp's rank_nodes: gravity, then weight, then id, best first."""
    cands = [(g, hrw_weight(key, n), n) for n, g in nodes if live is None or n in live]
    cands.sort(reverse=True)
    return [n for _, _, n in cands]


@dataclass(frozen=True)
class PortablePlan:
    actor: str
    key: int
    path: str
    nodes: tuple[tuple[int, int], ...]
    order: tuple[int, ...]


def plan(m: Manifest) -> list[PortablePlan]:
    out = []
    for a in m.actors:
        if a.module not in PORTABLE_MODULES:
            continue
        path = output_path(m, a)
        key = path_hash(path)
        nodes = tuple(eligible(m, a))
        out.append(PortablePlan(a.name, key, path, nodes, tuple(rank(key, list(nodes)))))
    return out


def main(argv: list[str] | None = None) -> int:
    from .manifest import ManifestErrors, load

    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1:
        print(__doc__.strip().splitlines()[0])
        print("usage: python -m potluck.reconcile <manifest.json>")
        return 2
    try:
        m = load(args[0])
    except ManifestErrors as e:
        print(e.report())
        return 1
    for a in m.actors:
        ok, why = portability(m, a)
        print(f"{a.name}: {why}")
    for p in plan(m):
        order = " > ".join(f"0x{n:04x}" for n in p.order)
        print(f"  {p.actor} -> {p.path} (key {p.key}): starts on 0x{p.order[0]:04x}; order {order}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
