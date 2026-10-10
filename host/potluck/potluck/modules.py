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
(ARCHITECTURE section 7). 0xF0-0xFF is Potluck's own block, for test components.
"""

from __future__ import annotations

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
    out: list[ModuleSpec] = []
    for i, m in enumerate(mods):
        w = f"{where}.modules[{i}]"
        if not isinstance(m, dict):
            raise ModuleError(f"{w}: must be an object")
        name, typ, place, cfg = m.get("module"), m.get("type"), m.get("placement"), m.get("config", [])
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
        out.append(ModuleSpec(name, typ, place, tuple(fields), comp))
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
