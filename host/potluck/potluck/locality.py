"""The Locality Contract check -- ARCHITECTURE.md section 4, and half of section 13-M4.

    python -m potluck.locality check manifests/home.json

M4's acceptance test is two sentences. This is the first one, in full:

    "a manifest binding an L1 actor to a remote resource fails the build with a readable error
     naming both ends."

That sentence is the reason the project exists. Section 4's opening line is *"transparent naming,
explicit cost, checked before it ships"*, and its whole argument is that a single-system image which
hides the seams is a system that passes every bench test and fails in a wall. The class is part of
the type; binding across an incompatible class is a build error, not a runtime surprise.

So this file is the thing that makes section 4 a contract rather than a convention. Everything else
in the repository can be right and, without this, an L1 control loop can still end up reading a
sensor across a radio -- and nothing will say so until the deadline is missed in the field.

THE FOUR RULES, AND WHICH ONES BELONG HERE

Rule 1 -- an actor declares its class, may bind only resources of its class or tighter, and an
L0/L1 actor only on its own node. **Here.**

Rule 2 -- a read never returns a bare or unmarked value. Runtime, and the firmware's namespace
already enforces it.

Rule 3 -- class is transport-derived, not aspirational: the ceiling of a binding is the worst class
of any transport on its path. **Here**, from the manifest's `links`. And with it v1's single-hop
boundary condition, because "two L3 hops at <500 ms each compose to <1 s, which is not L3".

Rule 4 -- demotion is loud. Runtime, on the node, plus host-side percentile auditing.

Section 4 is precise about the division of labour and it is worth repeating: class **ceilings** are
static, so a linter on a laptop proves a binding never crosses a hop type its class does not budget
for. Class **satisfaction** is environmental -- the same manifest passes at 3 m and fails at 60 m
across section 3's PDR cliff -- and that is rule 4's job at runtime. This tool is the laptop half. It
cannot tell you the link will be good enough; it can tell you the link is the wrong *kind*.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass

from .manifest import Manifest, ManifestErrors, load

#: Section 4's table, read as "the tightest class this hop can carry".
#:
#:   L0  < 100 us   may cross nothing -- same silicon
#:   L1  < 10 ms    may cross nothing -- same node
#:   L2  < 20 ms    one wired hop (CAN / UART)
#:   L3  < 500 ms   one wireless hop (ESP-NOW)
#:   L4  best effort, no deadline -- anything, including host and internet
TRANSPORT_CLASS = {
    "uart": 2,
    "can": 2,
    "espnow": 3,
    "host": 4,
}

#: No hop at all. L0 and L1 both mean "does not leave this node", so a same-node binding can serve
#: any class -- the class is then decided by the code, not by the fabric.
SAME_NODE_CLASS = 0

CLASS_DESCRIPTION = {
    0: "L0 (<100 us, same silicon)",
    1: "L1 (<10 ms, same node)",
    2: "L2 (<20 ms, one wired hop)",
    3: "L3 (<500 ms, one wireless hop)",
    4: "L4 (best effort, anything)",
}


def describe_class(c: int) -> str:
    return CLASS_DESCRIPTION.get(c, f"L{c}")


@dataclass(frozen=True)
class LocalityError:
    """One rejected binding, with both ends named.

    "Naming both ends" is not a nicety -- it is the acceptance criterion. An error that says
    "locality violation in actor 3" tells a person nothing they can act on; the useful error names
    the actor, its class, the resource, and the two nodes involved.
    """

    actor: str
    resource: str
    message: str

    def __str__(self) -> str:
        return f"{self.actor} -> {self.resource}: {self.message}"


class LocalityViolation(Exception):
    def __init__(self, errors: list[LocalityError]) -> None:
        super().__init__(f"{len(errors)} locality contract violation(s)")
        self.errors = errors

    def report(self) -> str:
        return "\n".join(f"  {e}" for e in self.errors)


def binding_ceiling(m: Manifest, from_node: int, to_node: int) -> tuple[int | None, str]:
    """The tightest class a binding between these two nodes can carry, and why.

    Returns (class, explanation). A class of None means the binding cannot be made at all -- there
    is no path, or the only path needs more than one hop, which v1 does not do.
    """
    if from_node == to_node:
        return SAME_NODE_CLASS, "same node, no hop"

    link = m.link_between(from_node, to_node)
    if link is not None:
        return TRANSPORT_CLASS[link.transport], f"one {link.transport} hop"

    # Is it reachable at all, and if so how far? Section 4's v1 boundary condition: "v1 paths are
    # single-hop -- rule 3's worst-transport arithmetic is only correct when a path has one
    # transport", so a two-hop route is not a worse class, it is not a route.
    hops = _hops_between(m, from_node, to_node)
    if hops is None:
        return None, "no transport connects these nodes"
    return None, (f"the shortest route is {hops} hops; v1 paths are single-hop, because two "
                  f"<500 ms hops compose to <1 s, which is not L3 -- forwarding is v2 work")


def _hops_between(m: Manifest, a: int, b: int) -> int | None:
    """Breadth-first hop count, only to tell "unreachable" from "too far"."""
    if a == b:
        return 0
    seen = {a}
    frontier = [a]
    depth = 0
    while frontier:
        depth += 1
        nxt: list[int] = []
        for node in frontier:
            for link in m.links:
                other = link.b if link.a == node else (link.a if link.b == node else None)
                if other is None or other in seen:
                    continue
                if other == b:
                    return depth
                seen.add(other)
                nxt.append(other)
        frontier = nxt
    return None


def check(m: Manifest) -> list[LocalityError]:
    """Every binding in the manifest, against section 4's rules 1 and 3."""
    errors: list[LocalityError] = []

    for actor in m.actors:
        where = m.placement_of(actor)
        if where is None:
            # Unresolved placement is not a locality error: constraints without a resolution are the
            # *input* to the build tool. manifest.py's --resolved is what insists on one.
            continue
        home = m.node(where)
        home_label = home.label if home is not None else f"0x{where:04x}"

        for path in actor.needs:
            resource = m.resource(path)
            owner = m.owner_of(path)
            if resource is None or owner is None:
                # manifest.py already reports an unresolvable need; nothing to say about its class.
                continue

            # -- rule 1, second half: an L0/L1 actor binds only on its own node ------------------
            if actor.latency_class <= 1 and owner.node_id != where:
                errors.append(LocalityError(
                    actor.name, path,
                    f"actor is {describe_class(actor.latency_class)} and runs on "
                    f"'{home_label}' (0x{where:04x}), but this resource is owned by "
                    f"'{owner.label}' (0x{owner.node_id:04x}). "
                    f"{describe_class(actor.latency_class)} may not leave its own node, so this "
                    f"binding cannot be made -- pin the actor to '{owner.label}', or declare it a "
                    f"looser class and accept the deadline that comes with it"))
                continue

            # -- rule 1, first half: only resources of the actor's class or tighter --------------
            if resource.latency_class > actor.latency_class:
                errors.append(LocalityError(
                    actor.name, path,
                    f"actor is {describe_class(actor.latency_class)} but the resource is declared "
                    f"{describe_class(resource.latency_class)}, owned by '{owner.label}' "
                    f"(0x{owner.node_id:04x}). An actor may only bind resources of its own class or "
                    f"tighter; a tight loop reading a loose resource is a deadline nobody wrote down"))
                continue

            # -- rule 3: the class is what the transport can carry, not what the author hoped ----
            ceiling, why = binding_ceiling(m, where, owner.node_id)
            if ceiling is None:
                errors.append(LocalityError(
                    actor.name, path,
                    f"'{home_label}' (0x{where:04x}) cannot reach '{owner.label}' "
                    f"(0x{owner.node_id:04x}): {why}"))
                continue
            if actor.latency_class < ceiling:
                errors.append(LocalityError(
                    actor.name, path,
                    f"actor is {describe_class(actor.latency_class)} on '{home_label}' "
                    f"(0x{where:04x}) and the resource is on '{owner.label}' "
                    f"(0x{owner.node_id:04x}), reached by {why} -- which is "
                    f"{describe_class(ceiling)} at best. The class of a binding is what the "
                    f"transport can carry, not what the actor declares"))

    errors.extend(_check_headroom(m))
    return errors


def _check_headroom(m: Manifest) -> list[LocalityError]:
    """Section 7.4's other constraint: an actor "needs >= 40 KB headroom".

    A design figure checked against a design figure -- the manifest's declared headroom, not a
    measurement. The runtime reports the real number in every heartbeat and the reconciler uses that;
    this only catches a placement that could not fit even on paper.
    """
    errors: list[LocalityError] = []
    used: dict[int, int] = {}
    for actor in m.actors:
        where = m.placement_of(actor)
        if where is None:
            continue
        used[where] = used.get(where, 0) + actor.headroom_bytes

    for node_id, total in sorted(used.items()):
        node = m.node(node_id)
        if node is None or node.headroom_bytes == 0:
            continue  # no figure declared; nothing to check against
        if total > node.headroom_bytes:
            names = ", ".join(a.name for a in m.actors if m.placement_of(a) == node_id)
            errors.append(LocalityError(
                names, f"node 0x{node_id:04x}",
                f"the actors placed on '{node.label}' ask for {total} B of headroom and the node "
                f"declares {node.headroom_bytes} B"))
    return errors


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) < 2 or args[0] != "check":
        print("usage: python -m potluck.locality check <manifest.json>", file=sys.stderr)
        return 2
    try:
        m = load(args[1])
    except ManifestErrors as exc:
        print(f"{args[1]}: {exc}", file=sys.stderr)
        print(exc.report(), file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"{args[1]}: {exc}", file=sys.stderr)
        return 2

    errors = check(m)
    bindings = sum(len(a.needs) for a in m.actors)
    if errors:
        print(f"{args[1]}: {len(errors)} locality contract violation(s) "
              f"in {bindings} binding(s)", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    print(f"{m.system}: {bindings} binding(s) satisfy the locality contract")
    for actor in m.actors:
        where = m.placement_of(actor)
        node = m.node(where) if where is not None else None
        label = node.label if node is not None else "unplaced"
        print(f"  {actor.name:<16} {describe_class(actor.latency_class):<28} on {label}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
