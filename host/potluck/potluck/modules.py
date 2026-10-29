"""M8.2 (PS-1): external modules -- an application's actors, described by its component.

An external component lives in the application's own repository and is built into a Potluck
firmware variant (tools\\build_firmware.ps1 -AppComponents). Next to its CMakeLists.txt it carries a
``potluck-modules.json`` that tells the host what its actors are called, which actor type numbers
they use, how they are placed, and how their configuration is laid out:

    {"component": "pot_passive_sensor", "types": [128, 143],
     "modules": [{"module": "ext:passive_sensor/rf_link", "type": 128, "placement": "every",
                  "config": [{"name": "window_ms", "kind": "u16", "default": 1000,
                              "min": 100, "max": 60000}]}]}

The description is declarative on purpose: Potluck's tooling never runs code from another
repository. ``placement`` is "pinned" (the manifest pins it), "every" (every node runs one) or
"portable" (the reconciler places it; Potluck writes the standard portable header in front of the
module's own bytes). Config kinds: u8 u16 u32 i8 i16 i32 f32 bool; "path" (a namespace path, sent as
its hash); "node" (a node id or label); "system_prefix" (no value: the FNV-1a state after
``potluck://<system>/``, from which the board can hash paths it builds itself).

Types: 0x00-0x7F are Potluck's built-ins; 0x80-0xFF are external, given out in blocks of 16
(the table in DEVELOPING-APPS.md, section 5). 0xF0-0xFF is Potluck's own block, for examples and tests.

M12: a module may list its ``outputs`` -- paths under ``potluck://<system>/`` with ``{node}`` (``node-<id>`` of
the node it runs on) and ``{actor}`` (the actor's name) -- and ``potluck apply`` reads each one back.
M20: a path starting with ``?`` is published only sometimes (a position while nobody is placed is not): it is
counted in the namespace like any other, but neither ``apply``'s verify nor the hub's "working" waits for it.
"""

from __future__ import annotations

NEAR_MAX = 4  # pot/position.hpp's kPositionNearMax: node-<id>/sys/near holds four u16 ids

import json
import struct
from dataclasses import dataclass
from typing import Any

from .paths import path_hash

FIRST_EXTERNAL_TYPE = 0x80
PLACEMENTS = ("pinned", "every", "portable")
_KINDS = {"u8": "<B", "u16": "<H", "u32": "<I", "i8": "<b", "i16": "<h", "i32": "<i", "f32": "<f",
          "bool": "<B", "path": "<I", "node": "<H", "system_prefix": "<I"}


class ModuleError(Exception):
    pass


#: M13: Potluck's optional board modules (ARCHITECTURE section 13's table), by the names a cluster file's
#: `firmware.modules` and a descriptor's `requires` use. pot/node_status.hpp's ModuleId numbers them.
BOARD_MODULES = ("sandbox", "extender", "wifi", "present", "pocket")


@dataclass(frozen=True)
class Field:
    name: str
    kind: str
    default: Any = None
    min: float | None = None
    max: float | None = None


@dataclass(frozen=True)
class ModuleSpec:
    module: str
    type: int
    placement: str
    fields: tuple[Field, ...]
    component: str
    # M12: the outputs an actor of this module publishes, for `potluck apply` to read back: paths under
    # potluck://<system>/, with {node} for "node-<id>" (the node it runs on) and {actor} for the actor's name.
    outputs: tuple[str, ...] = ()
    # M13: the board modules the component needs in the image (BOARD_MODULES): its descriptor's `requires`.
    requires: tuple[str, ...] = ()
    # Whether the descriptor has an "outputs" key at all: `[]` says "publishes nothing" (an LED driver), a missing
    # key says nothing -- apply --plan names only the latter as uncounted.
    outputs_declared: bool = False
    #: M20 (passive-sensor's D20): "peers": {"nearest": K} -- each board's {peer} outputs exist for its K nearest boards
    #: in its room only (node-<id>/sys/near, which `apply` computes), so they are counted for K, not every board. 0: all.
    peers_nearest: int = 0
    #: M20: "per_room": true -- a portable module `apply` runs once per room of the cluster file, as <actor>_<room>
    #: placed among that room's boards; a cluster file with no rooms keeps the one actor.
    per_room: bool = False
    #: M20 (review item 11): "panel" -- how the dashboard shows the module, from a closed vocabulary (PANEL_KINDS),
    #: its words as text only: {"kind": "room", "output": "act/{actor}/out", "label": ..., "on": ..., "off": ...}.
    #: Kept as sorted (key, value) pairs so the spec stays hashable.
    panel: tuple[tuple[str, str], ...] = ()
    #: 2026-10-20 (the Potluck Desk's Wiring window): "parts" -- what an actor of this module needs wired to its board,
    #: each {"part", "bus", "pins": {role: <config field>}, "guide"?}. A pin names the module's own config field that
    #: carries the GPIO, so the wiring guide and the firmware read one number: the actor's config, else the field's
    #: default. `apply --plan` refuses two parts, or a part and the firmware (potluck.pins), on one GPIO of a board.
    parts: tuple["Part", ...] = ()
    #: M24: "takes" -- the events from the PC an actor of this module takes on act/<actor>/in (pot/input.hpp): "key",
    #: "pointer", or both. The hub delivers POST /api/input's events to it, in order; the actor implements on_call.
    takes: tuple[str, ...] = ()
    #: 2026-10-29 (passive-sensor's D21): "writable" -- which of the module's own outputs a program outside the cell may
    #: be allowed to write (each one of its "outputs", never per link): the cluster file's `outside` may then mark them
    #: read_write, as for a board's `owns` and the built-ins' requests. The actor declares them ReadWrite on its board,
    #: which still decides every write.
    writable: tuple[str, ...] = ()


@dataclass(frozen=True)
class Part:
    part: str                          # what it is, e.g. "SHT41"
    bus: str                           # PART_BUSES
    pins: tuple[tuple[str, str], ...]  # (role, config field), e.g. (("scl", "scl_gpio"), ("sda", "sda_gpio"))
    guide: str = ""                    # how to wire it, as text


#: How a part is connected. "gpio" is any single pin (a button, a relay input, a 1-Wire data line).
PART_BUSES = ("i2c", "spi", "uart", "adc", "onewire", "gpio")
PART_TEXT_MAX = 40
PART_GUIDE_MAX = 400
#: An ESP32-S3's GPIOs are 0..48 (its technical reference); which of them a board leaves free is its datasheet's.
GPIO_MAX = 48


def _text(v: Any, cap: int) -> bool:
    return isinstance(v, str) and len(v) <= cap and all(" " <= ch <= "~" or ch == "\n" for ch in v)


def check_parts(p: Any, fields: list["Field"], where: str) -> tuple[Part, ...]:
    if not isinstance(p, list):
        raise ModuleError(f"{where}: 'parts' is a list of {{\"part\", \"bus\", \"pins\", \"guide\"?}}")
    ints = {f.name for f in fields if f.kind in ("u8", "u16", "i8", "i16", "u32", "i32")}
    out = []
    for i, x in enumerate(p):
        w = f"{where}.parts[{i}]"
        if not isinstance(x, dict) or not set(x) <= {"part", "bus", "pins", "guide"}:
            raise ModuleError(f"{w}: is {{\"part\", \"bus\", \"pins\", \"guide\"?}}")
        if not _text(x.get("part"), PART_TEXT_MAX) or not x["part"].strip():
            raise ModuleError(f"{w}: 'part' is a name, at most {PART_TEXT_MAX} printable characters")
        if x.get("bus") not in PART_BUSES:
            raise ModuleError(f"{w}: 'bus' is one of {list(PART_BUSES)}")
        pins = x.get("pins")
        if not isinstance(pins, dict) or not pins:
            raise ModuleError(f"{w}: 'pins' maps each role to the module's config field that carries its GPIO")
        for role, field in pins.items():
            if not _text(role, PART_TEXT_MAX) or field not in ints:
                raise ModuleError(f"{w}: pin {role!r} must name one of the module's integer config fields "
                                  f"({sorted(ints) or 'it has none'}), which carries the GPIO")
        if not _text(x.get("guide", ""), PART_GUIDE_MAX):
            raise ModuleError(f"{w}: 'guide' is text, at most {PART_GUIDE_MAX} printable characters")
        out.append(Part(x["part"], x["bus"], tuple(sorted(pins.items())), x.get("guide", "")))
    return tuple(out)


def actor_pins(spec: ModuleSpec, cfg: dict[str, Any]) -> list[tuple[str, str, int | None]]:
    """(part, role, GPIO) for every pin of an actor's parts: its config, else the field's default; None if neither."""
    defaults = {f.name: f.default for f in spec.fields}
    out = []
    for p in spec.parts:
        for role, field in p.pins:
            v = cfg.get(field, defaults.get(field))
            out.append((p.part, role, v if isinstance(v, int) and not isinstance(v, bool) else None))
    return out


_REGISTRY: dict[str, ModuleSpec] = {}


def clear() -> None:
    _REGISTRY.clear()


def get(module: str) -> ModuleSpec | None:
    return _REGISTRY.get(module)


def registered() -> list[ModuleSpec]:
    return list(_REGISTRY.values())


def is_placed_at_run_time(module: str) -> bool:
    """True for modules the manifest need not pin: every-node and portable ones."""
    if module.startswith("guest:"):
        return True  # M7: a guest actor is portable (potluck.guest)
    s = _REGISTRY.get(module)
    return s is not None and s.placement in ("every", "portable")


def is_portable(module: str) -> bool:
    if module.startswith("guest:"):
        return True  # M7
    s = _REGISTRY.get(module)
    return s is not None and s.placement == "portable"


#: The panel kinds the dashboard draws. "room": one per room (a per_room module), the room shaded and its words
#: shown when the output is true or non-zero; "value": the output's value under the label.
PANEL_KINDS = ("room", "value")
PANEL_TEXT_MAX = 40


def check_panel(p: Any, outputs: list[str], per_room: bool, where: str) -> tuple[tuple[str, str], ...]:
    if not isinstance(p, dict) or not set(p) <= {"kind", "output", "label", "on", "off"}:
        raise ModuleError(f"{where}: 'panel' is {{\"kind\", \"output\", \"label\"?, \"on\"?, \"off\"?}}")
    if p.get("kind") not in PANEL_KINDS:
        raise ModuleError(f"{where}: a panel's kind is one of {list(PANEL_KINDS)}")
    if p["kind"] == "room" and not per_room:
        raise ModuleError(f"{where}: a \"room\" panel is for a per_room module")
    if p.get("output") not in [o.lstrip("?") for o in outputs] or "{node}" in str(p.get("output")):
        raise ModuleError(f"{where}: a panel's output is one of the module's outputs under act/{{actor}}/")
    for k in ("label", "on", "off"):
        v = p.get(k, "")
        if not isinstance(v, str) or len(v) > PANEL_TEXT_MAX or not all(" " <= ch <= "~" for ch in v):
            raise ModuleError(f"{where}: a panel's {k} is text, at most {PANEL_TEXT_MAX} printable characters")
    return tuple(sorted((k, str(v)) for k, v in p.items()))


def parse_descriptor(doc: Any, where: str = "<descriptor>") -> list[ModuleSpec]:
    if not isinstance(doc, dict):
        raise ModuleError(f"{where}: must be a JSON object")
    comp = doc.get("component")
    types = doc.get("types")
    mods = doc.get("modules")
    if not isinstance(comp, str) or not comp:
        raise ModuleError(f"{where}: 'component' must be the component's directory name")
    if (not isinstance(types, list) or len(types) != 2 or not all(isinstance(t, int) for t in types)
            or not FIRST_EXTERNAL_TYPE <= types[0] <= types[1] <= 0xFF):
        raise ModuleError(f"{where}: 'types' must be [first, last] inside 0x80..0xFF")
    if not isinstance(mods, list) or not mods:
        raise ModuleError(f"{where}: 'modules' must be a non-empty list")
    req = doc.get("requires", [])
    if not isinstance(req, list) or not all(isinstance(r, str) and r in BOARD_MODULES for r in req):
        raise ModuleError(f"{where}: 'requires' must be a list of Potluck's board modules, from {list(BOARD_MODULES)}")
    out: list[ModuleSpec] = []
    for i, m in enumerate(mods):
        w = f"{where}.modules[{i}]"
        if not isinstance(m, dict):
            raise ModuleError(f"{w}: must be an object")
        name, typ, place, cfg = m.get("module"), m.get("type"), m.get("placement"), m.get("config", [])
        outs = m.get("outputs", [])
        if not isinstance(outs, list) or not all(isinstance(o, str) and o.lstrip("?") and
                                                 not o.lstrip("?").startswith("potluck://") for o in outs):
            raise ModuleError(f"{w}: 'outputs' must be a list of paths under potluck://<system>/, "
                              f"such as \"{{node}}/my/out\" or \"act/{{actor}}/out\"")
        if not isinstance(name, str) or not name.startswith("ext:"):
            raise ModuleError(f"{w}: 'module' must be a name starting with 'ext:'")
        if not isinstance(typ, int) or not types[0] <= typ <= types[1]:
            raise ModuleError(f"{w}: type {typ} is outside the component's block {types}")
        if place not in PLACEMENTS:
            raise ModuleError(f"{w}: placement must be one of {PLACEMENTS}")
        if not isinstance(cfg, list):
            raise ModuleError(f"{w}: 'config' must be a list")
        fields = []
        for j, f in enumerate(cfg):
            fw = f"{w}.config[{j}]"
            if not isinstance(f, dict) or not isinstance(f.get("name"), str) or f.get("kind") not in _KINDS:
                raise ModuleError(f"{fw}: needs a name and a kind, one of {sorted(_KINDS)}")
            fields.append(Field(f["name"], f["kind"], f.get("default"), f.get("min"), f.get("max")))
        peers = m.get("peers")
        k = 0
        if peers is not None:
            k = peers.get("nearest") if isinstance(peers, dict) and set(peers) == {"nearest"} else None
            if not isinstance(k, int) or isinstance(k, bool) or not 1 <= k <= NEAR_MAX:
                raise ModuleError(f"{w}: 'peers' is {{\"nearest\": K}}, K 1..{NEAR_MAX} (a board's set travels as "
                                  f"node-<id>/sys/near, eight bytes)")
        per_room = m.get("per_room", False)
        if not isinstance(per_room, bool) or (per_room and place != "portable"):
            raise ModuleError(f"{w}: 'per_room' is true or false, and only for a portable module")
        panel = check_panel(m["panel"], outs, per_room, w) if "panel" in m else ()
        parts = check_parts(m["parts"], fields, w) if "parts" in m else ()
        takes = m.get("takes", [])
        if (not isinstance(takes, list) or len(set(takes)) != len(takes) or
                any(t not in ("key", "pointer") for t in takes)):
            raise ModuleError(f"{w}: 'takes' lists the events it takes from the PC: \"key\", \"pointer\" or both")
        writable = m.get("writable", [])
        plain = [o.lstrip("?") for o in outs]
        if (not isinstance(writable, list) or not all(isinstance(x, str) for x in writable) or
                any(x not in plain or "{peer}" in x for x in writable)):
            raise ModuleError(f"{w}: 'writable' lists some of the module's own 'outputs' (not per-link ones) that a "
                              f"program outside the cell may be allowed to write")
        out.append(ModuleSpec(name, typ, place, tuple(fields), comp, tuple(outs), tuple(req), "outputs" in m, k,
                              per_room, panel, parts, tuple(takes), tuple(writable)))
    return out


def register(specs: list[ModuleSpec]) -> None:
    """Add modules to the registry. Refused, with nothing added: a module name or a type number that
    another registered component already uses."""
    for s in specs:
        if s.module in _REGISTRY:
            raise ModuleError(f"module {s.module} is described twice")
        for other in _REGISTRY.values():
            if other.type == s.type:
                raise ModuleError(f"type 0x{s.type:02x} is used by both {other.component} ({other.module}) "
                                  f"and {s.component} ({s.module})")
    names = [s.module for s in specs]
    if len(set(names)) != len(names) or len({s.type for s in specs}) != len(specs):
        raise ModuleError("a descriptor names one module or one type twice")
    for s in specs:
        _REGISTRY[s.module] = s


def load(path: str) -> list[ModuleSpec]:
    with open(path, "r", encoding="utf-8") as f:
        specs = parse_descriptor(json.load(f), path)
    register(specs)
    return specs


def _node_id(m, v, where: str) -> int:
    if isinstance(v, int):
        return v
    for n in m.nodes:
        if n.label == v:
            return n.node_id
    raise ModuleError(f"{where}: no node '{v}' in the manifest")


def encode_config(spec: ModuleSpec, cfg: dict[str, Any], m, where: str) -> bytes:
    """The module's own config bytes, in the order its descriptor lists them (little-endian)."""
    known = {f.name for f in spec.fields if f.kind != "system_prefix"}
    extra = set(cfg) - known - ({"period_ms"} if spec.placement == "portable" else set())
    if extra:
        raise ModuleError(f"{where}: unknown config key(s) {sorted(extra)} for {spec.module}")
    out = b""
    for f in spec.fields:
        if f.kind == "system_prefix":
            out += struct.pack("<I", path_hash(f"potluck://{m.system}/"))
            continue
        v = cfg.get(f.name, f.default)
        if v is None:
            raise ModuleError(f"{where}: {spec.module} needs config '{f.name}'")
        if f.kind == "path":
            if not isinstance(v, str):
                raise ModuleError(f"{where}: '{f.name}' must be a namespace path")
            out += struct.pack("<I", path_hash(m.bindings.get(v, v)))
            continue
        if f.kind == "node":
            out += struct.pack("<H", _node_id(m, v, where))
            continue
        if f.kind == "bool":
            v = 1 if v else 0
        if (f.min is not None and v < f.min) or (f.max is not None and v > f.max):
            raise ModuleError(f"{where}: {f.name}={v} is outside {f.min}..{f.max}")
        try:
            out += struct.pack(_KINDS[f.kind], v)
        except struct.error as e:
            raise ModuleError(f"{where}: {f.name}={v} does not fit {f.kind}: {e}") from None
    return out
