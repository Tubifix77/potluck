"""The deploy manifest -- ARCHITECTURE.md section 7.4's one signed package per system.

    python -m potluck.manifest check examples/vent.json
    python -m potluck.manifest check examples/vent.json --resolved
    python -m potluck.manifest digest examples/vent.json

WHY THIS FILE EXISTS AT ALL

Section 7.4: "the deploy unit is the cluster application package -- one artifact per system, not per
node." Developers declare *constraints* and the build tool resolves placement, then freezes the
resolution into the signed manifest, "so what-runs-where is inspectable and reproducible, never
improvised at runtime". No application code ever names a node.

That makes the manifest the load-bearing document of the whole system: it carries the policies the
runtime deliberately refuses to guess -- which resources are strict about staleness (section 4 rule
2), what each actor does when the host goes away (section 8.3), which nodes may be borrowed for
background work (section 7.8) -- and the placement the reconciler will hold as desired state
(section 7.7). "Mechanism in the OS, policy in the manifest" is the sentence this file implements.

WHAT IT VALIDATES, AND WHY EACH CHECK EARNS ITS PLACE

Structure first, because a manifest is hand-written and a typo in a policy key is a policy that
silently reverts to a default. So **unknown keys are errors**, not warnings.

Then the two checks that are really about the firmware:

  * **Path hash collisions.** A node never sees a path string -- it stores a 32-bit FNV-1a hash
    (paths.py explains why). Two distinct paths that collide are indistinguishable on the node, and
    `NsError::HashCollision` exists precisely to name this as "a manifest error". A build is the only
    place it can be caught, because it is a property of the whole set of paths in the system.
  * **The 128-entry namespace cap per node** (section 14's mitigation for lookup cost). A manifest
    that declares more resources on a node than the node's table can hold does not fail at deploy --
    it fails at the 129th `declare`, at runtime, on one node.

What it deliberately does NOT check: whether each binding's latency class is satisfiable by its
placement. That is the Locality Contract check (section 4 rule 1, section 13-M4) and it needs a fleet
topology, not just this document. It gets its own module; this one only guarantees the data it needs
is present and well-formed.

FORMAT

JSON, because the manifest is signed (section 9.3) and the thing signed has to have exactly one byte
form. `canonical_bytes()` is that form: sorted keys, no incidental whitespace, ASCII-escaped, one
trailing newline. Two tools that agree on the manifest agree on its digest.
"""

from __future__ import annotations

import hashlib
import json
import sys
from dataclasses import dataclass, field
from typing import Any, Iterable

from .paths import path_hash

#: Section 14: "128-entry cap and perfect-hash the paths at build time from the manifest".
MAX_RESOURCES_PER_NODE = 128

#: Section 4's latency classes. L0 is the tightest (same-core), L4 the loosest (across the radio).
LATENCY_CLASSES = (0, 1, 2, 3, 4)

#: Section 7.8. `background` maps just above the idle task, so it consumes only unwanted cycles.
PRIORITIES = ("critical", "normal", "background")

#: Section 8.3. The OS ships the mechanism menu; the manifest picks one per actor.
HOST_LOSS_RESPONSES = ("continue", "hold", "controlled_stop", "safe_state")

#: Section 4 rule 2's per-resource toggle.
STALENESS_POLICIES = ("informative", "strict")

#: Section 4's table, column "may cross". The transport a hop uses is what decides the tightest class
#: that hop can carry, and section 4 rule 3 insists the class is "transport-derived, not aspirational".
TRANSPORTS = ("uart", "can", "espnow", "host")

RESOURCE_KINDS = ("sampled", "event")
ACCESS_MODES = ("read", "write", "read_write")

#: Section 7.8: "Battery and solar nodes are excluded from background pools by default and opt in via
#: manifest -- the section 1.2 home's garden node stays sleepy unless told otherwise."
POWER_SOURCES = ("mains", "battery", "solar")
SELF_POWERED = ("battery", "solar")

#: A node id is a uint16 on the wire. 0 is "unset" and 0xFFFF is the broadcast destination
#: (frame.hpp's kNodeBroadcast), so neither can name a node.
NODE_ID_MIN = 0x0001
NODE_ID_MAX = 0xFFFE

#: Long enough for the deepest path in section 7.2's examples, short enough that a manifest cannot
#: smuggle a path no console will ever print in full.
MAX_PATH_LEN = 128

SCHEMA = 1


class ManifestError(Exception):
    """One problem, with the place in the document that has it.

    `where` is a JSON-ish path -- `actors[2].priority` -- because "invalid priority" without it sends
    a person reading a hundred-actor manifest hunting.
    """

    def __init__(self, where: str, message: str) -> None:
        super().__init__(f"{where}: {message}")
        self.where = where
        self.message = message


class ManifestErrors(Exception):
    """Every problem found, not the first one.

    A validator that stops at the first error turns fixing a manifest into as many build cycles as
    there are mistakes.
    """

    def __init__(self, errors: list[ManifestError]) -> None:
        super().__init__(f"{len(errors)} problem(s) in the manifest")
        self.errors = errors

    def report(self) -> str:
        return "\n".join(f"  {e.where}: {e.message}" for e in self.errors)


# ---------------------------------------------------------------------------------------------
# The declarations
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class ResourceSpec:
    """One thing that can be read or written, owned by exactly one node (section 7.2)."""

    path: str
    unit: str
    kind: str = "sampled"
    access: str = "read"
    latency_class: int = 4
    #: How old a value may be before `quality` stops being GOOD. 0 means "no bound declared", which
    #: is legal for an event but never for a strict resource -- see validate().
    staleness_bound_ms: int = 0
    staleness_policy: str = "informative"

    @property
    def hash(self) -> int:
        return path_hash(self.path)


@dataclass(frozen=True)
class NodeSpec:
    """A node the package expects to find, and what it brings to the potluck."""

    node_id: int
    label: str
    power: str = "mains"
    #: Free DRAM an actor may be placed against. A design figure from the build, not a measurement:
    #: the runtime reports the real number in every heartbeat and the reconciler uses that.
    headroom_bytes: int = 0
    #: Section 7.8's opt-in. Mains nodes are in the background pool unless they say otherwise;
    #: battery and solar nodes are out of it unless they say otherwise.
    allow_background: bool | None = None
    owns: tuple[ResourceSpec, ...] = ()

    def background_allowed(self) -> bool:
        if self.allow_background is not None:
            return self.allow_background
        return self.power not in SELF_POWERED


@dataclass(frozen=True)
class LinkSpec:
    """A transport between two nodes -- the topology the class arithmetic needs.

    Part of the package because section 7.4 ships "one artifact per system": the nodes the package
    expects to find, and how it expects to find them wired, are the same kind of statement. Without
    it, section 4 rule 3's "worst class of any transport on its path" has no path to look at.
    """

    a: int
    b: int
    transport: str

    def joins(self, x: int, y: int) -> bool:
        return (self.a == x and self.b == y) or (self.a == y and self.b == x)


@dataclass(frozen=True)
class ActorSpec:
    """A unit of application code, and the constraints that decide where it may live."""

    name: str
    module: str
    #: The actor's own class. Section 4 rule 1: it may only bind resources of this class or tighter,
    #: and an L0/L1 actor only on its own node. Checked by the locality checker, not here.
    latency_class: int
    needs: tuple[str, ...] = ()
    headroom_bytes: int = 0
    priority: str = "normal"
    #: Section 8.3 has no safe global default, so this is required for any actor that can write --
    #: see validate() for the argument.
    on_host_loss: str | None = None
    #: A hand-written override. Section 7.4: "hand-written node pins remain available as overrides,
    #: not as the norm."
    pin: int | None = None


@dataclass(frozen=True)
class Manifest:
    system: str
    min_core_version: int
    nodes: tuple[NodeSpec, ...]
    actors: tuple[ActorSpec, ...]
    #: Section 7.2: "logical paths are bindings, in the Plan 9 sense". logical -> canonical.
    bindings: dict[str, str] = field(default_factory=dict)
    #: How the nodes are wired. Optional, but the locality checker cannot do its job without it.
    links: tuple[LinkSpec, ...] = ()
    #: The frozen resolution: actor name -> node id. Absent until the build tool resolves placement.
    placement: dict[str, int] = field(default_factory=dict)
    schema: int = SCHEMA

    # -- lookups ------------------------------------------------------------------------------

    def node(self, node_id: int) -> NodeSpec | None:
        for n in self.nodes:
            if n.node_id == node_id:
                return n
        return None

    def actor(self, name: str) -> ActorSpec | None:
        for a in self.actors:
            if a.name == name:
                return a
        return None

    def owner_of(self, path: str) -> NodeSpec | None:
        """Which node owns the resource at `path`, following a binding if that is what it is."""
        target = self.bindings.get(path, path)
        for n in self.nodes:
            for r in n.owns:
                if r.path == target:
                    return n
        return None

    def resource(self, path: str) -> ResourceSpec | None:
        target = self.bindings.get(path, path)
        for n in self.nodes:
            for r in n.owns:
                if r.path == target:
                    return r
        return None

    def placement_of(self, actor: ActorSpec) -> int | None:
        """Where this actor runs: the frozen placement, or its hand-written pin."""
        if actor.name in self.placement:
            return self.placement[actor.name]
        return actor.pin

    def link_between(self, a: int, b: int) -> LinkSpec | None:
        for link in self.links:
            if link.joins(a, b):
                return link
        return None

    def is_resolved(self) -> bool:
        return all(self.placement_of(a) is not None for a in self.actors)

    def all_paths(self) -> list[str]:
        """Every path the system names: canonical resources and logical binding names alike.

        Both go in, because both are hashed and a logical name that collides with a canonical one is
        just as unresolvable on the node.
        """
        out: list[str] = []
        for n in self.nodes:
            out.extend(r.path for r in n.owns)
        out.extend(self.bindings.keys())
        return out

    # -- serialisation ------------------------------------------------------------------------

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema": self.schema,
            "system": self.system,
            "min_core_version": self.min_core_version,
            "nodes": [
                {
                    "node_id": n.node_id,
                    "label": n.label,
                    "power": n.power,
                    "headroom_bytes": n.headroom_bytes,
                    **({} if n.allow_background is None else {"allow_background": n.allow_background}),
                    "owns": [
                        {
                            "path": r.path,
                            "unit": r.unit,
                            "kind": r.kind,
                            "access": r.access,
                            "latency_class": r.latency_class,
                            "staleness_bound_ms": r.staleness_bound_ms,
                            "staleness_policy": r.staleness_policy,
                        }
                        for r in n.owns
                    ],
                }
                for n in self.nodes
            ],
            "actors": [
                {
                    "name": a.name,
                    "module": a.module,
                    "latency_class": a.latency_class,
                    "needs": list(a.needs),
                    "headroom_bytes": a.headroom_bytes,
                    "priority": a.priority,
                    **({} if a.on_host_loss is None else {"on_host_loss": a.on_host_loss}),
                    **({} if a.pin is None else {"pin": a.pin}),
                }
                for a in self.actors
            ],
            "bindings": dict(self.bindings),
            "links": [{"a": l.a, "b": l.b, "transport": l.transport} for l in self.links],
            "placement": dict(self.placement),
        }

    def canonical_bytes(self) -> bytes:
        """The exact bytes that get signed (section 9.3).

        Sorted keys and no incidental whitespace, so re-emitting a manifest cannot change its
        identity. A signature over "whatever the editor saved" is a signature over formatting.
        """
        text = json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"), ensure_ascii=True)
        return (text + "\n").encode("ascii")

    def digest(self) -> str:
        return hashlib.sha256(self.canonical_bytes()).hexdigest()


# ---------------------------------------------------------------------------------------------
# Parsing. Strict, and every complaint carries its location.
# ---------------------------------------------------------------------------------------------


def _require_keys(d: dict, allowed: Iterable[str], required: Iterable[str], where: str,
                  errors: list[ManifestError]) -> None:
    allowed_set = set(allowed)
    for k in d:
        if k not in allowed_set:
            # Not a warning. A misspelled `staleness_policy` is a resource that silently reverts to
            # informative, which is the one failure section 4 rule 2 exists to prevent.
            errors.append(ManifestError(f"{where}.{k}", "unknown key"))
    for k in required:
        if k not in d:
            errors.append(ManifestError(where, f"missing required key '{k}'"))


def _enum(value: Any, allowed: tuple[str, ...], where: str,
          errors: list[ManifestError]) -> str | None:
    if not isinstance(value, str) or value not in allowed:
        errors.append(ManifestError(where, f"must be one of {', '.join(allowed)}; got {value!r}"))
        return None
    return value


def _int(value: Any, where: str, errors: list[ManifestError], *, low: int | None = None,
         high: int | None = None) -> int | None:
    # bool is an int in Python, and a `true` where a count belongs is a mistake worth naming.
    if isinstance(value, bool) or not isinstance(value, int):
        errors.append(ManifestError(where, f"must be an integer; got {value!r}"))
        return None
    if low is not None and value < low:
        errors.append(ManifestError(where, f"must be at least {low}; got {value}"))
        return None
    if high is not None and value > high:
        errors.append(ManifestError(where, f"must be at most {high}; got {value}"))
        return None
    return value


def _str(value: Any, where: str, errors: list[ManifestError]) -> str | None:
    if not isinstance(value, str) or value == "":
        errors.append(ManifestError(where, f"must be a non-empty string; got {value!r}"))
        return None
    return value


def check_path(path: Any, where: str, errors: list[ManifestError]) -> str | None:
    """A path is `potluck://<cluster>/<segment>/...`, ASCII, no empty segments, no trailing slash.

    Checked here rather than left to the node, because the node never sees the string: it gets a
    hash. A malformed path hashes perfectly well and fails as a read that finds nothing.
    """
    if not isinstance(path, str) or not path:
        errors.append(ManifestError(where, f"must be a non-empty string; got {path!r}"))
        return None
    if not path.startswith("potluck://"):
        errors.append(ManifestError(where, f"must start with 'potluck://'; got {path!r}"))
        return None
    if len(path) > MAX_PATH_LEN:
        errors.append(ManifestError(where, f"longer than {MAX_PATH_LEN} characters"))
        return None
    if not path.isascii():
        # The project spent a release removing a non-ASCII character from every tool it touched.
        errors.append(ManifestError(where, "must be ASCII"))
        return None
    rest = path[len("potluck://"):]
    if rest.endswith("/"):
        errors.append(ManifestError(where, "must not end with '/'"))
        return None
    segments = rest.split("/")
    if len(segments) < 2:
        errors.append(ManifestError(where, "needs at least a cluster and one segment"))
        return None
    for seg in segments:
        if seg == "":
            errors.append(ManifestError(where, "has an empty path segment"))
            return None
        if any(c.isspace() for c in seg):
            errors.append(ManifestError(where, "has whitespace in a path segment"))
            return None
    return path


_RESOURCE_KEYS = ("path", "unit", "kind", "access", "latency_class", "staleness_bound_ms",
                  "staleness_policy")
_NODE_KEYS = ("node_id", "label", "power", "headroom_bytes", "allow_background", "owns")
_ACTOR_KEYS = ("name", "module", "latency_class", "needs", "headroom_bytes", "priority",
               "on_host_loss", "pin")
_TOP_KEYS = ("schema", "system", "min_core_version", "nodes", "actors", "bindings", "links",
            "placement")
_LINK_KEYS = ("a", "b", "transport")


def _parse_resource(d: Any, where: str, errors: list[ManifestError]) -> ResourceSpec | None:
    if not isinstance(d, dict):
        errors.append(ManifestError(where, "must be an object"))
        return None
    _require_keys(d, _RESOURCE_KEYS, ("path", "unit"), where, errors)
    path = check_path(d.get("path"), f"{where}.path", errors)
    unit = _str(d.get("unit"), f"{where}.unit", errors)
    kind = _enum(d.get("kind", "sampled"), RESOURCE_KINDS, f"{where}.kind", errors)
    access = _enum(d.get("access", "read"), ACCESS_MODES, f"{where}.access", errors)
    policy = _enum(d.get("staleness_policy", "informative"), STALENESS_POLICIES,
                   f"{where}.staleness_policy", errors)
    cls = _int(d.get("latency_class", 4), f"{where}.latency_class", errors, low=0, high=4)
    bound = _int(d.get("staleness_bound_ms", 0), f"{where}.staleness_bound_ms", errors, low=0)
    if None in (path, unit, kind, access, policy) or cls is None or bound is None:
        return None
    return ResourceSpec(path=path, unit=unit, kind=kind, access=access, latency_class=cls,
                        staleness_bound_ms=bound, staleness_policy=policy)


def _parse_node(d: Any, where: str, errors: list[ManifestError]) -> NodeSpec | None:
    if not isinstance(d, dict):
        errors.append(ManifestError(where, "must be an object"))
        return None
    _require_keys(d, _NODE_KEYS, ("node_id", "label"), where, errors)
    node_id = _int(d.get("node_id"), f"{where}.node_id", errors, low=NODE_ID_MIN, high=NODE_ID_MAX)
    label = _str(d.get("label"), f"{where}.label", errors)
    power = _enum(d.get("power", "mains"), POWER_SOURCES, f"{where}.power", errors)
    headroom = _int(d.get("headroom_bytes", 0), f"{where}.headroom_bytes", errors, low=0)

    allow = d.get("allow_background")
    if allow is not None and not isinstance(allow, bool):
        errors.append(ManifestError(f"{where}.allow_background", f"must be true or false; got {allow!r}"))
        allow = None

    owns_raw = d.get("owns", [])
    owns: list[ResourceSpec] = []
    if not isinstance(owns_raw, list):
        errors.append(ManifestError(f"{where}.owns", "must be a list"))
    else:
        for i, r in enumerate(owns_raw):
            parsed = _parse_resource(r, f"{where}.owns[{i}]", errors)
            if parsed is not None:
                owns.append(parsed)

    if node_id is None or label is None or power is None or headroom is None:
        return None
    return NodeSpec(node_id=node_id, label=label, power=power, headroom_bytes=headroom,
                    allow_background=allow, owns=tuple(owns))


def _parse_actor(d: Any, where: str, errors: list[ManifestError]) -> ActorSpec | None:
    if not isinstance(d, dict):
        errors.append(ManifestError(where, "must be an object"))
        return None
    _require_keys(d, _ACTOR_KEYS, ("name", "module", "latency_class"), where, errors)
    name = _str(d.get("name"), f"{where}.name", errors)
    module = _str(d.get("module"), f"{where}.module", errors)
    cls = _int(d.get("latency_class"), f"{where}.latency_class", errors, low=0, high=4)
    headroom = _int(d.get("headroom_bytes", 0), f"{where}.headroom_bytes", errors, low=0)
    priority = _enum(d.get("priority", "normal"), PRIORITIES, f"{where}.priority", errors)

    on_host_loss = d.get("on_host_loss")
    if on_host_loss is not None:
        on_host_loss = _enum(on_host_loss, HOST_LOSS_RESPONSES, f"{where}.on_host_loss", errors)

    pin = d.get("pin")
    if pin is not None:
        pin = _int(pin, f"{where}.pin", errors, low=NODE_ID_MIN, high=NODE_ID_MAX)

    needs_raw = d.get("needs", [])
    needs: list[str] = []
    if not isinstance(needs_raw, list):
        errors.append(ManifestError(f"{where}.needs", "must be a list"))
    else:
        for i, n in enumerate(needs_raw):
            p = check_path(n, f"{where}.needs[{i}]", errors)
            if p is not None:
                needs.append(p)

    if name is None or module is None or cls is None or headroom is None or priority is None:
        return None
    return ActorSpec(name=name, module=module, latency_class=cls, needs=tuple(needs),
                     headroom_bytes=headroom, priority=priority, on_host_loss=on_host_loss, pin=pin)


def parse(doc: Any, *, require_placement: bool = False) -> Manifest:
    """Parse and validate. Raises ManifestErrors carrying every problem found."""
    errors: list[ManifestError] = []
    if not isinstance(doc, dict):
        raise ManifestErrors([ManifestError("<document>", "must be a JSON object")])

    _require_keys(doc, _TOP_KEYS, ("system", "nodes", "actors"), "<manifest>", errors)

    schema = _int(doc.get("schema", SCHEMA), "schema", errors, low=1)
    if schema is not None and schema != SCHEMA:
        errors.append(ManifestError("schema", f"this tool understands schema {SCHEMA}, not {schema}"))
    system = _str(doc.get("system"), "system", errors)
    min_core = _int(doc.get("min_core_version", 1), "min_core_version", errors, low=1)

    nodes: list[NodeSpec] = []
    nodes_raw = doc.get("nodes", [])
    if not isinstance(nodes_raw, list) or not nodes_raw:
        errors.append(ManifestError("nodes", "must be a non-empty list"))
    else:
        for i, n in enumerate(nodes_raw):
            parsed = _parse_node(n, f"nodes[{i}]", errors)
            if parsed is not None:
                nodes.append(parsed)

    actors: list[ActorSpec] = []
    actors_raw = doc.get("actors", [])
    if not isinstance(actors_raw, list):
        errors.append(ManifestError("actors", "must be a list"))
    else:
        for i, a in enumerate(actors_raw):
            parsed = _parse_actor(a, f"actors[{i}]", errors)
            if parsed is not None:
                actors.append(parsed)

    bindings: dict[str, str] = {}
    bindings_raw = doc.get("bindings", {})
    if not isinstance(bindings_raw, dict):
        errors.append(ManifestError("bindings", "must be an object of logical -> canonical"))
    else:
        for logical, canonical in bindings_raw.items():
            lp = check_path(logical, f"bindings['{logical}']", errors)
            cp = check_path(canonical, f"bindings['{logical}'] target", errors)
            if lp is not None and cp is not None:
                bindings[lp] = cp

    links: list[LinkSpec] = []
    links_raw = doc.get("links", [])
    if not isinstance(links_raw, list):
        errors.append(ManifestError("links", "must be a list"))
    else:
        for i, l in enumerate(links_raw):
            where = f"links[{i}]"
            if not isinstance(l, dict):
                errors.append(ManifestError(where, "must be an object"))
                continue
            _require_keys(l, _LINK_KEYS, ("a", "b", "transport"), where, errors)
            a = _int(l.get("a"), f"{where}.a", errors, low=NODE_ID_MIN, high=NODE_ID_MAX)
            b = _int(l.get("b"), f"{where}.b", errors, low=NODE_ID_MIN, high=NODE_ID_MAX)
            transport = _enum(l.get("transport"), TRANSPORTS, f"{where}.transport", errors)
            if a is None or b is None or transport is None:
                continue
            if a == b:
                errors.append(ManifestError(where, "a node is not wired to itself"))
                continue
            links.append(LinkSpec(a=a, b=b, transport=transport))

    placement: dict[str, int] = {}
    placement_raw = doc.get("placement", {})
    if not isinstance(placement_raw, dict):
        errors.append(ManifestError("placement", "must be an object of actor -> node id"))
    else:
        for actor_name, node_id in placement_raw.items():
            nid = _int(node_id, f"placement['{actor_name}']", errors, low=NODE_ID_MIN,
                       high=NODE_ID_MAX)
            if nid is not None:
                placement[actor_name] = nid

    if system is None or min_core is None:
        raise ManifestErrors(errors)

    m = Manifest(system=system, min_core_version=min_core, nodes=tuple(nodes),
                 actors=tuple(actors), bindings=bindings, links=tuple(links),
                 placement=placement, schema=SCHEMA)
    errors.extend(cross_check(m, require_placement=require_placement))
    if errors:
        raise ManifestErrors(errors)
    return m


def cross_check(m: Manifest, *, require_placement: bool = False) -> list[ManifestError]:
    """The checks that need the whole document rather than one field."""
    errors: list[ManifestError] = []

    # Duplicate node ids. Two nodes with one id is two nodes answering to the same destination.
    seen_ids: dict[int, int] = {}
    for i, n in enumerate(m.nodes):
        if n.node_id in seen_ids:
            errors.append(ManifestError(f"nodes[{i}].node_id",
                                        f"0x{n.node_id:04x} is already used by nodes[{seen_ids[n.node_id]}]"))
        else:
            seen_ids[n.node_id] = i

    seen_actors: dict[str, int] = {}
    for i, a in enumerate(m.actors):
        if a.name in seen_actors:
            errors.append(ManifestError(f"actors[{i}].name",
                                        f"'{a.name}' is already used by actors[{seen_actors[a.name]}]"))
        else:
            seen_actors[a.name] = i

    # Section 14's cap, and duplicate resource paths, per node.
    for i, n in enumerate(m.nodes):
        if len(n.owns) > MAX_RESOURCES_PER_NODE:
            errors.append(ManifestError(
                f"nodes[{i}].owns",
                f"{len(n.owns)} resources on one node; the node's table holds "
                f"{MAX_RESOURCES_PER_NODE}, so the rest would fail to declare at runtime"))
        seen_paths: dict[str, int] = {}
        for j, r in enumerate(n.owns):
            if r.path in seen_paths:
                errors.append(ManifestError(f"nodes[{i}].owns[{j}].path",
                                            f"already declared at owns[{seen_paths[r.path]}]"))
            else:
                seen_paths[r.path] = j

    # The same path owned by two different nodes: section 7.2 gives every resource exactly one owner.
    owners: dict[str, int] = {}
    for i, n in enumerate(m.nodes):
        for r in n.owns:
            if r.path in owners and owners[r.path] != n.node_id:
                errors.append(ManifestError(
                    f"nodes[{i}].owns",
                    f"'{r.path}' is also owned by node 0x{owners[r.path]:04x}; a resource has one owner"))
            owners[r.path] = n.node_id

    # A strict resource with no staleness bound cannot be strict about anything: strict means
    # "deliver an error instead of a value past the bound", and there is no bound to be past.
    for i, n in enumerate(m.nodes):
        for j, r in enumerate(n.owns):
            if r.staleness_policy == "strict" and r.staleness_bound_ms == 0:
                errors.append(ManifestError(
                    f"nodes[{i}].owns[{j}]",
                    "staleness_policy 'strict' needs a staleness_bound_ms above 0; "
                    "strict means 'refuse past the bound' and there is no bound"))
            if r.kind == "event" and r.staleness_policy == "strict":
                # Section 7.2: event entries have queue semantics and READ is not how you get them,
                # so a staleness policy on one is a policy that never applies.
                errors.append(ManifestError(
                    f"nodes[{i}].owns[{j}]",
                    "an event resource has queue semantics, so a staleness policy never applies"))

    # Path hash collisions across everything the system names. This is the check that cannot be made
    # anywhere else: it is a property of the whole set, and the node cannot see it at all.
    by_hash: dict[int, str] = {}
    for p in m.all_paths():
        h = path_hash(p)
        if h in by_hash and by_hash[h] != p:
            errors.append(ManifestError(
                "paths",
                f"'{p}' and '{by_hash[h]}' both hash to 0x{h:08x}; the node stores only the hash, "
                f"so these two paths are the same resource to it. Rename one"))
        by_hash[h] = p

    # Bindings must land on something real, and must not shadow a canonical path.
    for logical, canonical in m.bindings.items():
        if m.owner_of(canonical) is None:
            errors.append(ManifestError(f"bindings['{logical}']",
                                        f"binds to '{canonical}', which no node declares"))
        if any(r.path == logical for n in m.nodes for r in n.owns):
            errors.append(ManifestError(f"bindings['{logical}']",
                                        "a logical name may not also be a declared resource path"))

    # Every need must resolve, or the actor is bound to something that does not exist.
    for i, a in enumerate(m.actors):
        for j, need in enumerate(a.needs):
            if m.owner_of(need) is None:
                errors.append(ManifestError(
                    f"actors[{i}].needs[{j}]",
                    f"'{need}' is neither a declared resource nor a binding"))

    # Section 8.3: there is no safe global default for an actor that can write. An actuator fed by
    # host-computed setpoints and left to `continue` keeps integrating a dead host's last command,
    # which the architecture calls "not autonomous, unattended". So it must be stated.
    for i, a in enumerate(m.actors):
        if a.on_host_loss is not None:
            continue
        writes = [n for n in a.needs
                  if (r := m.resource(n)) is not None and r.access in ("write", "read_write")]
        if writes:
            errors.append(ManifestError(
                f"actors[{i}]",
                f"binds writable resource '{writes[0]}' and must declare on_host_loss "
                f"({', '.join(HOST_LOSS_RESPONSES)}); there is no safe default for something "
                f"that can move the world"))

    # Placement and pins must name a node the package expects to find.
    for i, a in enumerate(m.actors):
        if a.pin is not None and m.node(a.pin) is None:
            errors.append(ManifestError(f"actors[{i}].pin",
                                        f"0x{a.pin:04x} is not a node this manifest declares"))
        if a.name in m.placement and a.pin is not None and m.placement[a.name] != a.pin:
            errors.append(ManifestError(
                f"placement['{a.name}']",
                f"frozen to 0x{m.placement[a.name]:04x} but the actor pins itself to 0x{a.pin:04x}"))
    for name, node_id in m.placement.items():
        if m.actor(name) is None:
            errors.append(ManifestError(f"placement['{name}']",
                                        "places an actor this manifest does not declare"))
        if m.node(node_id) is None:
            errors.append(ManifestError(f"placement['{name}']",
                                        f"places it on 0x{node_id:04x}, which is not a declared node"))

    # Section 7.8's opt-in, checked against where the actor actually ends up.
    for i, a in enumerate(m.actors):
        if a.priority != "background":
            continue
        where = m.placement_of(a)
        if where is None:
            continue
        n = m.node(where)
        if n is not None and not n.background_allowed():
            errors.append(ManifestError(
                f"actors[{i}]",
                f"is background work placed on '{n.label}', a {n.power} node; harvesting idle "
                f"cycles denies sleep, so set allow_background on that node or place it elsewhere"))

    if require_placement:
        for i, a in enumerate(m.actors):
            if m.placement_of(a) is None:
                errors.append(ManifestError(
                    f"actors[{i}]",
                    "has no placement and no pin; a manifest is only deployable once the build "
                    "tool has frozen where every actor runs"))

    return errors


def load(path: str, *, require_placement: bool = False) -> Manifest:
    with open(path, "r", encoding="utf-8") as f:
        try:
            doc = json.load(f)
        except json.JSONDecodeError as exc:
            raise ManifestErrors([ManifestError(f"line {exc.lineno}", exc.msg)]) from exc
    return parse(doc, require_placement=require_placement)


# ---------------------------------------------------------------------------------------------
# Command line, so a build can run it
# ---------------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd = args[0]
    if cmd not in ("check", "digest") or len(args) < 2:
        print("usage: python -m potluck.manifest {check|digest} <file> [--resolved]",
              file=sys.stderr)
        return 2
    require_placement = "--resolved" in args
    try:
        m = load(args[1], require_placement=require_placement)
    except ManifestErrors as exc:
        print(f"{args[1]}: {exc}", file=sys.stderr)
        print(exc.report(), file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"{args[1]}: {exc}", file=sys.stderr)
        return 2

    if cmd == "digest":
        print(m.digest())
        return 0

    print(f"{m.system}: {len(m.nodes)} node(s), {len(m.actors)} actor(s), "
          f"{len(m.all_paths())} path(s)")
    print(f"  placement: {'frozen' if m.is_resolved() else 'not resolved'}")
    print(f"  digest:    {m.digest()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
