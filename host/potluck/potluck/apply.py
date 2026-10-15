"""M12: potluck apply -- from one cluster file to a cell running it, verified.

    python -m potluck.apply <cluster.json> [--port COM6] [--plan] [--cache DIR]
    python -m potluck.apply <cluster.json> inventory [--port COM6]

A cluster file says what the cell should be: its name, its boards, the firmware they run (Potluck at a
commit, plus any applications' components and the extender), and the package (manifest fragments, the
applications' module descriptors, guest bundles). `apply` makes the cell that, in this order:

  1. plan, changing nothing: compose one manifest from the fragments, compile and check it (every actor,
     every guest's author signature under the cluster CA), find the keys, export and build the firmware
     (or reuse the cached build of the same sources), and read every board's inventory. Anything wrong
     stops here, with the reason, before any board is touched. `--plan` stops here on purpose.
  2. firmware, where it differs: a board whose running image is not the build (by the ELF's SHA-256) gets
     it over the cell (M11, one board at a time, the cabled board last), at a counter above every board's
     floor; a board that rolls back stops the rollout.
  3. the package: signed at a counter above every board's, sent through the cabled board to the cell.
     Boards take it live when they can (M12: nothing reboots, unchanged actors keep their state), else by
     rebooting into it; either way on trial, reverting by themselves if it fails.
  4. verify: every board runs the package, confirmed; each one's actors are the ones the package gives it;
     every portable actor runs exactly once in the cell; every output the manifest names reads back with
     a value. Then a report: what changed, which boards rebooted, and how long each step took.

Exit codes: 0 verified; 2 refused before any change (the plan failed); 1 changed and not verified.

The cluster file (paths relative to it; "git" is a local repository or a URL, "rev" a commit or tag):

    {"schema": "potluck-cluster/1", "system": "lab", "port": "COM6",
     "boards": [{"node": "6300", "label": "A"}, {"node": "7368", "label": "B"}],
     "keys": {"ca": "keys/ca.pub", "deploy_key": "keys/deploy.key", "deploy_cert": "keys/deploy.cert",
              "deploy_bcert": "keys/deploy.bcert"},
     "firmware": {"potluck": {"rev": "HEAD"}, "extra": ["CONFIG_POT_WASM=y"],
                  "extender": {"git": "...", "rev": "...", "components": "firmware/components"}},
     "apps": [{"name": "hello", "git": ".", "rev": "HEAD", "components": "examples/apps"}],
     "package": {"manifests": ["examples/apps/pot_hello/hello.json"], "guests_root": "."},
     "verify": {"timeout_s": 240}}

"firmware" may instead be {"image": "<.bin>", "elf": "<.elf>"} to use a build made elsewhere. An app's
"components" directory is built into the firmware (its actors are compiled in, ADR-003 Tier 0) and every
potluck-modules.json under it describes its modules to the host; its "manifests" (paths inside the app's
repository) are fragments of the package. A fragment's boards are matched to the cluster's by node id or by
label; one written for another cluster name is re-homed under this one.
"""

from __future__ import annotations

import argparse
import contextlib
import copy
import hashlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCHEMA = "potluck-cluster/1"
REPO = Path(__file__).resolve().parents[3]  # host/potluck/potluck/apply.py -> the Potluck repository

ROLE_NAMES = {0x01: "extender-build", 0x02: "extender", 0x04: "sandbox", 0x08: "reconciler", 0x10: "relay",
              0x20: "enrolled", 0x40: "cabled"}
FW_STATES = {0: "factory", 1: "confirmed", 2: "on_trial"}
#: M13: pot/node_status.hpp's ModuleId, ModuleState and ModuleReason -- the words the inventory prints.
MODULE_NAMES = {1: "sandbox", 2: "extender", 3: "wifi", 4: "present", 5: "pocket"}
AMPDU_TX_KEY = "CONFIG_ESP_WIFI_AMPDU_TX_ENABLED="
AMPDU_TX_OFF = AMPDU_TX_KEY + "n"

MODULE_STATES = {1: "active", 2: "idle", 3: "failed"}
MODULE_REASONS = {0: "", 1: "no credentials", 2: "no network", 3: "standby", 4: "nothing here uses it",
                  5: "out of memory", 6: "refused", 7: "error", 8: "another module is the connection"}


class ApplyError(Exception):
    """A plan that cannot be carried out; raised before any board changes."""


# ---- M13: Potluck's optional board modules ---------------------------------------------------------------

@dataclass(frozen=True)
class BoardModule:
    """One of Potluck's optional board modules (ARCHITECTURE section 13's table): what a cluster file's
    `firmware.modules` turns on, what it needs, and which archives its cost is counted from."""
    kconfig: tuple[str, ...]
    archives: tuple[str, ...]
    requires: tuple[str, ...] = ()
    #: at least one of these too (the presenter needs a network connection: Potluck's station or the extender's)
    requires_any: tuple[str, ...] = ()
    #: section 13's class: "a security liability" or "filler" -- printed by --plan, never a reason to refuse.
    why_optional: str = "filler"


#: Built so far. The extender is not here: it comes from its own repository (`firmware.extender`), and is
#: named "extender" in the inventory and in a descriptor's `requires`.
BOARD_MODULES = {
    "sandbox": BoardModule(("CONFIG_POT_WASM=y",), ("libpot_wasm.a",), why_optional="filler"),
    # M13: Potluck's own Wi-Fi station -- puts the board on a house network, with its credentials on the board.
    "wifi": BoardModule(("CONFIG_POT_WIFI=y",), ("libpot_wifi.a",), why_optional="a security liability"),
    # M13: the presenting role -- the cell's exposed resources to an MQTT broker; ESP-IDF's managed MQTT client.
    "present": BoardModule(("CONFIG_POT_PRESENT=y",), ("libpot_present.a", "libespressif__mqtt.a"),
                           requires_any=("wifi", "extender"), why_optional="a security liability"),
    # M13.1: the pocket page -- the board's own access point and a read-only page for a phone in reach.
    "pocket": BoardModule(("CONFIG_POT_POCKET=y",), ("libpot_pocket.a",), why_optional="a security liability"),
}
EXTENDER_ARCHIVES = ("libpme_hotspot.a",)


# ---- the inventory -----------------------------------------------------------------------------------

@dataclass
class NodeStatus:
    node: int
    fw_running: int
    fw_floor: int
    fw_state: str
    fw_version: str
    elf_sha: bytes
    slot: int
    pending: int
    trial_boots: int
    pkg_counter: int
    pkg_floor: int
    pkg_digest: bytes
    roles: int
    live_applies: int
    ck_writes: int
    uptime_s: int
    pinned_running: int
    portable_total: int
    portable_here: int
    cluster: str
    actors: list[str]
    #: M13 (version 2): (module, state, reason) for each of Potluck's optional modules the image carries.
    modules: list[tuple[str, str, str]] = field(default_factory=list)

    def module_words(self) -> str:
        return ",".join(f"{m}:{s}" + (f" ({r})" if r else "") for m, s, r in self.modules) or "-"

    def role_names(self) -> list[str]:
        return [n for bit, n in ROLE_NAMES.items() if self.roles & bit]

    def line(self) -> str:
        if not self.slot:
            pkg = "no package"
        elif self.pkg_counter == 0:
            pkg = "built-in behaviour (its package does not run on this firmware)"
        else:
            pkg = f"package {self.pkg_counter}" + (" ON TRIAL" if self.pending else "")
        return (f"0x{self.node:04x}  fw {self.fw_version} (counter {self.fw_running}, floor {self.fw_floor}, "
                f"{self.fw_state})  {pkg} (floor {self.pkg_floor})  cluster '{self.cluster}'  "
                f"up {self.uptime_s} s  live {self.live_applies}  ck writes {self.ck_writes}  "
                f"roles {','.join(self.role_names()) or '-'}  "
                f"actors {','.join(self.actors) or '-'} ({self.portable_here}/{self.portable_total} portable here)  "
                f"modules {self.module_words()}")


FIXED_LEN = 87


def parse_node_status(raw: bytes) -> NodeStatus | None:
    """pot/node_status.hpp's reply, version 1 or 2; None for 'unreachable' ({0, target}) or anything else."""
    if len(raw) < FIXED_LEN + 2 or raw[0] not in (1, 2):
        return None
    v2 = raw[0] == 2
    node, fw_run, fw_floor, fw_state = struct.unpack_from("<HIIB", raw, 1)
    version = raw[12:44].split(b"\0", 1)[0].decode("ascii", "replace")
    elf = raw[44:52]
    slot, pending, trial = raw[52], raw[53], raw[54]
    pkg, pkg_floor = struct.unpack_from("<II", raw, 55)
    digest = raw[63:71]
    roles = raw[71]
    live, ck, up = struct.unpack_from("<III", raw, 72)
    pinned, ptotal, phere = raw[84], raw[85], raw[86]
    at = FIXED_LEN
    cl = raw[at]
    cluster = raw[at + 1:at + 1 + cl].decode("ascii", "replace")
    at += 1 + cl
    if at >= len(raw):
        return None
    al = raw[at]
    actors = raw[at + 1:at + 1 + al].decode("ascii", "replace")
    at += 1 + al
    modules: list[tuple[str, str, str]] = []
    if v2:
        if at >= len(raw):
            return None
        mc = raw[at]
        at += 1
        if at + 3 * mc != len(raw):
            return None
        for i in range(mc):
            mid, ms, mr = raw[at + 3 * i:at + 3 * i + 3]
            modules.append((MODULE_NAMES.get(mid, f"module-{mid}"), MODULE_STATES.get(ms, f"state-{ms}"),
                            MODULE_REASONS.get(mr, f"reason-{mr}")))
    elif at != len(raw):
        return None
    return NodeStatus(node, fw_run, fw_floor, FW_STATES.get(fw_state, str(fw_state)), version, elf, slot, pending,
                      trial, pkg, pkg_floor, digest, roles, live, ck, up, pinned, ptotal, phere, cluster,
                      [a for a in actors.split(",") if a], modules)


def node_status(b, target: int, *, timeout: float = 3.0, tries: int = 2) -> NodeStatus | None:
    """One board's inventory through the cabled board; None if it does not answer or runs firmware from
    before M12 (it has no NODE_STATUS)."""
    from . import frame as fr
    from .bridge import BridgeError

    for _ in range(tries):
        try:
            raw = b.deploy_step(fr.Op.NODE_STATUS, struct.pack("<H", target), timeout=timeout)
        except BridgeError:
            continue
        st = parse_node_status(raw)
        if st is not None:
            return st
    return None


def fw_status(b, target: int) -> dict | None:
    """M11's FW_STATUS, which boards from before M12 answer too."""
    from . import fw

    try:
        r = fw.status(b, target)
    except Exception:  # noqa: BLE001 - unreachable is an answer here
        return None
    return r if r.get("status") == "ok" and r.get("node") == target else None


# ---- the cluster file ---------------------------------------------------------------------------------

@dataclass
class Source:
    name: str
    git: str
    rev: str
    components: str | None = None
    manifests: tuple[str, ...] = ()
    #: M20's library: an app switched off keeps its components in the image -- so switching is a package change, no
    #: new firmware -- but its manifests leave the package.
    enabled: bool = True
    #: M19: in a tree, the cell this app's portable actors live in (Review of M19 #3); None: wherever they may run.
    cell: str | None = None


@dataclass
class Cluster:
    path: Path
    system: str
    port: str | None
    baud: int
    boards: list[tuple[int, str]]
    keys: dict[str, Path]
    firmware: dict[str, Any] | None
    potluck: Source
    extender: Source | None
    apps: list[Source]
    extra: list[str]
    manifests: list[tuple[Path, Path]]  # (fragment, root for its relative paths)
    modules: list[Path]
    guests_root: Path
    verify_timeout_s: float
    verify_reads: list[str] = field(default_factory=list)
    #: M13: what systems outside the cell may do with each resource -- the owner's choice, so it lives here and
    #: not in an app's fragment. Canonical paths (potluck://<system>/...) -> "read" | "read_write".
    outside: dict[str, str] = field(default_factory=dict)
    #: M13: Potluck's board modules in the image: those `firmware.modules` names, the sandbox when `extra` turns
    #: it on directly, and the extender when `firmware.extender` is given.
    board_modules: tuple[str, ...] = ()
    #: M13: the cluster file's `present` block -- the presenting role's broker, keep-alive and period -- or None.
    present: dict[str, Any] | None = None
    #: M18: resource path -> the programs that may write it through the hub; absent: any program with the write right.
    writers: dict[str, tuple[str, ...]] = field(default_factory=dict)
    #: M20: the room each board lives in (a board's `room`), shown by the dashboard -- not its identity, which is `label`.
    rooms: dict[int, str] = field(default_factory=dict)
    #: M20: where each board sits on the floor plan, metres from its top-left corner (x right, y down); each
    #: becomes a pinned builtin:position actor, so the boards hold their own positions with no PC.
    positions: dict[int, tuple[float, float]] = field(default_factory=dict)
    #: M20: the floor plan the dashboard draws -- its size in metres and the rooms as rectangles -- or None. For
    #: people only: nothing on a board depends on it (the boards' positions above are what travels).
    plan: dict[str, Any] | None = None
    #: M19: the cells (groups) each board belongs to, by name -- one, or two for a foreman. A cluster file with none
    #: is one cell as before; with cells, a board admits only boards of its own cells.
    cells: dict[int, tuple[str, ...]] = field(default_factory=dict)
    #: M19 (Review #2): a cell's own death window in ms (an upper cell of foremen, far apart, a slower one); absent:
    #: its members are judged by their own announced windows, as in one cell.
    cell_windows: dict[str, int] = field(default_factory=dict)
    #: M19: a cell's parent -- the upper cell its foreman (and the standby) joins at run time; absent: a cell that
    #: does not federate.
    parents: dict[str, str] = field(default_factory=dict)
    #: M19 (Review of M19 #4): the boards that may hold their cell's foreman role (`"foreman": true` -- on mains
    #: power); the reconciler assigns it among them, never the owner.
    foremen: tuple[int, ...] = ()
    #: M19: what each federating cell's foreman publishes upward, export name -> {kind: argument, "unit", "type"}
    #: (pot/foreman.hpp); `alive` is always one of them.
    exports: dict[str, dict[str, dict[str, str]]] = field(default_factory=dict)
    #: M19: how often a cell's foreman publishes its summary, ms (default FOREMAN_PERIOD_MS).
    cell_periods: dict[str, int] = field(default_factory=dict)
    #: M19: boards the PC hosts as stub nodes for a tree's bench tests (`"stub": true`, ids STUB_FIRST..STUB_LAST): in
    #: the tree and the package, never reached through the cable -- potluck.stubs gives them the package.
    stubs: frozenset[int] = frozenset()
    #: M19: package.manifests entries that name the cell their portable actors live in (index -> cell).
    manifest_cells: dict[int, str] = field(default_factory=dict)


#: M19: a board's unicast peers -- ESP-NOW's 20 less the broadcast entry (node.hpp's kMaxUnicastPeers).
PEER_CAP = 19
#: M19: the node ids a stub board may have (m0_main.cpp's kStubFirst..kStubLast: the cable gives each its own address).
STUB_FIRST, STUB_LAST = 0xF000, 0xFDFF
#: M19: a foreman's summary period, ms, unless the cell says (cells.<name>.period_ms).
FOREMAN_PERIOD_MS = 2000
#: M19: pot/foreman.hpp's limits -- the boards a foreman's config names, its exports besides `alive`.
FOREMAN_MAX_MEMBERS = 20
FOREMAN_MAX_EXPORTS = 7
EXPORT_KINDS = ("copy", "max", "min", "sum", "avg", "count")
#: M19 (Review of M19 #6): what one child cell puts into its upper cell -- its foreman. The standby stays warm in its
#: own cell and joins on taking the role; a handover's newcomer takes the old foreman's slot once it is dead (a full
#: table reclaims a dead peer's, node.cpp), which the handover's bound -- the upper window plus 10 s -- allows.
CHILD_SEATS = 1


def _rel(base: Path, p: str) -> Path:
    q = Path(os.path.expandvars(os.path.expanduser(p)))
    return q if q.is_absolute() else (base / q).resolve()


def _source(d: Any, where: str, base: Path, default_name: str) -> Source:
    if not isinstance(d, dict):
        raise ApplyError(f"{where}: must be an object")
    extra = set(d) - {"name", "git", "rev", "components", "manifests", "enabled", "cell"}
    if extra:
        raise ApplyError(f"{where}: unknown key(s) {sorted(extra)}")
    git = d.get("git", ".")
    if not isinstance(git, str) or not git:
        raise ApplyError(f"{where}.git: a local repository or a URL")
    if "://" not in git and not git.startswith("git@"):
        git = str(_rel(base, git))
    rev = d.get("rev", "HEAD")
    mans = d.get("manifests", [])
    if not isinstance(rev, str) or not isinstance(mans, list) or not all(isinstance(x, str) for x in mans):
        raise ApplyError(f"{where}: 'rev' is a string and 'manifests' a list of paths")
    enabled = d.get("enabled", True)
    if not isinstance(enabled, bool):
        raise ApplyError(f"{where}.enabled: true or false")
    cell = d.get("cell")
    if cell is not None and (not isinstance(cell, str) or not 1 <= len(cell) <= 31):
        raise ApplyError(f"{where}.cell: the name of the cell its portable actors live in (M19)")
    s = Source(str(d.get("name", default_name)), git, rev, d.get("components"), tuple(mans), enabled)
    s.cell = cell
    return s


def check_plan(plan: Any) -> dict[str, Any] | None:
    """M20: the floor plan, {"width": m, "height": m, "rooms": [{"name", "x", "y", "w", "h"}]}, metres from its
    top-left corner, x right, y down. Returns it normalised, or None when the file has none."""
    if plan is None:
        return None

    def num(v, lo, hi):
        return isinstance(v, (int, float)) and not isinstance(v, bool) and lo <= v <= hi
    if not isinstance(plan, dict) or set(plan) - {"width", "height", "rooms"} or \
            not num(plan.get("width"), 1, 1000) or not num(plan.get("height"), 1, 1000):
        raise ApplyError("plan: {\"width\": m, \"height\": m, \"rooms\": [...]}, each side 1..1000 m")
    w, h = float(plan["width"]), float(plan["height"])
    rooms = []
    raw = plan.get("rooms", [])
    if not isinstance(raw, list) or len(raw) > 64:
        raise ApplyError("plan.rooms: a list of at most 64 rooms")
    for i, r in enumerate(raw):
        if not isinstance(r, dict) or set(r) != {"name", "x", "y", "w", "h"} or not isinstance(r["name"], str) or \
                not 1 <= len(r["name"]) <= 40 or not r["name"].isprintable() or \
                not (num(r["x"], 0, w) and num(r["y"], 0, h) and num(r["w"], 0.1, w) and num(r["h"], 0.1, h)) or \
                r["x"] + r["w"] > w + 1e-6 or r["y"] + r["h"] > h + 1e-6:
            raise ApplyError(f"plan.rooms[{i}]: {{\"name\", \"x\", \"y\", \"w\", \"h\"}} inside the plan, a name of 1..40 characters")
        rooms.append({"name": r["name"], "x": float(r["x"]), "y": float(r["y"]), "w": float(r["w"]), "h": float(r["h"])})
    return {"width": w, "height": h, "rooms": rooms}


def load_cluster(path: str) -> Cluster:
    p = Path(path).resolve()
    try:
        doc = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ApplyError(f"{path}: {exc}") from None
    base = p.parent
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        raise ApplyError(f"{path}: not a cluster file (\"schema\": \"{SCHEMA}\")")
    allowed = {"schema", "system", "port", "baud", "boards", "keys", "firmware", "apps", "package", "verify",
               "comment", "outside", "present", "plan", "cells"}
    extra = set(doc) - allowed
    if extra:
        raise ApplyError(f"{path}: unknown key(s) {sorted(extra)}")
    system = doc.get("system", "lab")
    if not isinstance(system, str) or not 1 <= len(system) <= 31 or any(c in system for c in " /") or \
            not system.isascii() or not system.isprintable():
        raise ApplyError("system: a cluster name is 1..31 printable ASCII characters, without spaces or '/'")
    boards = []
    rooms: dict[int, str] = {}
    positions: dict[int, tuple[float, float]] = {}
    cells: dict[int, tuple[str, ...]] = {}
    foremen: list[int] = []
    stubs: set[int] = set()
    for i, bd in enumerate(doc.get("boards", [])):
        if not isinstance(bd, dict) or "node" not in bd:
            raise ApplyError(f"boards[{i}]: {{\"node\": \"<hex id>\", \"label\": \"...\"}}")
        nid = int(bd["node"], 16) if isinstance(bd["node"], str) else int(bd["node"])
        boards.append((nid, str(bd.get("label", f"node-{nid:04x}"))))
        room = bd.get("room")  # M20: where the board lives, for people; its identity stays `label`
        if room is not None:
            if not isinstance(room, str) or not 1 <= len(room) <= 40 or not room.isprintable():
                raise ApplyError(f"boards[{i}].room: a room name, 1..40 printable characters")
            rooms[nid] = room
        pos = bd.get("position")  # M20: [x, y] in metres on the floor plan
        if pos is not None:
            if not (isinstance(pos, list) and len(pos) == 2 and
                    all(isinstance(v, (int, float)) and not isinstance(v, bool) and 0 <= v <= 1000 for v in pos)):
                raise ApplyError(f"boards[{i}].position: [x, y], metres from the plan's top-left corner, 0..1000")
            positions[nid] = (float(pos[0]), float(pos[1]))
        cl_raw = bd.get("cells")  # M19: ["house"], or ["house", "upper"] for a board in two
        if cl_raw is not None:
            if not (isinstance(cl_raw, list) and 1 <= len(cl_raw) <= 2 and len(set(cl_raw)) == len(cl_raw) and
                    all(isinstance(v, str) and 1 <= len(v) <= 31 and v.isprintable() for v in cl_raw)):
                raise ApplyError(f"boards[{i}].cells: one or two cell names, 1..31 printable characters each")
            cells[nid] = tuple(cl_raw)
        st = bd.get("stub", False)  # M19: a stub node the PC hosts (potluck.stubs), for a tree's bench tests
        if not isinstance(st, bool) or (st and not STUB_FIRST <= nid <= STUB_LAST):
            raise ApplyError(f"boards[{i}].stub: true for a stub node, whose id is {STUB_FIRST:04x}..{STUB_LAST:04x}")
        if st:
            stubs.add(nid)
        fm = bd.get("foreman", False)  # M19: may hold its cell's foreman role (mains power)
        if not isinstance(fm, bool):
            raise ApplyError(f"boards[{i}].foreman: true or false")
        if fm:
            foremen.append(nid)
    if len({n for n, _ in boards}) != len(boards) or len({lab for _, lab in boards}) != len(boards):
        raise ApplyError("boards: a node id or a label appears twice")
    keys_raw = doc.get("keys", {})
    keys = {}
    for k, default in (("ca", "keys/ca.pub"), ("deploy_key", "keys/deploy.key"), ("deploy_cert", "keys/deploy.cert"),
                       ("deploy_bcert", "keys/deploy.bcert")):
        keys[k] = _rel(base, keys_raw.get(k, default))
    fwd = doc.get("firmware", {})
    if fwd is not None and not isinstance(fwd, dict):
        raise ApplyError("firmware: an object, or null to leave the firmware alone")
    potluck = _source((fwd or {}).get("potluck", {"git": str(REPO)}), "firmware.potluck", base, "potluck")
    extender = _source(fwd["extender"], "firmware.extender", base, "extender") if fwd and fwd.get("extender") else None
    extra_cfg = list((fwd or {}).get("extra", []))
    named = (fwd or {}).get("modules", [])
    from . import modules as mods
    if not isinstance(named, list) or not all(isinstance(m, str) for m in named):
        raise ApplyError("firmware.modules: a list of module names")
    for m in named:
        if m == "extender":
            if not (fwd or {}).get("extender"):
                raise ApplyError("firmware.modules: 'extender' comes from its own repository -- give firmware.extender")
        elif m not in BOARD_MODULES:
            raise ApplyError(f"firmware.modules: '{m}' is " +
                             ("not built yet" if m in mods.BOARD_MODULES else
                              f"not one of Potluck's board modules ({', '.join(mods.BOARD_MODULES)})"))
    board_modules = []
    for m, spec in BOARD_MODULES.items():
        if m in named or all(k in extra_cfg for k in spec.kconfig):
            board_modules.append(m)
            extra_cfg += [k for k in spec.kconfig if k not in extra_cfg]
    if fwd and fwd.get("extender"):
        board_modules.append("extender")
    if fwd is not None and "present" in board_modules and not any(k.startswith(AMPDU_TX_KEY) for k in extra_cfg):
        # M13 (M0-LOG session 42): AMPDU transmit aggregation on a board that presents over its own station wedges its
        # radio -- ESP-NOW refused and the station's TCP stopped until a chip reset (a stall every 10-60 min; with it
        # off, 13 h 19 min clean). Every board runs one image, and any board may hold the presenting role, so a cell
        # that presents gets it off everywhere. A cluster file that names the key itself keeps its choice.
        extra_cfg.append(AMPDU_TX_OFF)
    for m in board_modules:
        spec = BOARD_MODULES.get(m)
        for r in spec.requires if spec else ():
            if r not in board_modules:
                raise ApplyError(f"firmware.modules: '{m}' needs '{r}' in the image too")
        if spec and spec.requires_any and not any(r in board_modules for r in spec.requires_any):
            raise ApplyError(f"firmware.modules: '{m}' needs one of {', '.join(spec.requires_any)} in the image too "
                             f"(a network connection)")
    present = doc.get("present")
    if present is not None:
        if not isinstance(present, dict) or not isinstance(present.get("broker"), str):
            raise ApplyError("present: {\"broker\": \"a.b.c.d[:port]\", \"keepalive_s\": 10, \"period_ms\": 2000}")
        if "present" not in board_modules:
            raise ApplyError("present: the cell presents itself only with the 'present' module in the firmware -- "
                             "add it to firmware.modules")
        extra_keys = set(present) - {"broker", "keepalive_s", "period_ms", "tls_ca", "hub", "cell"}
        if extra_keys:
            raise ApplyError(f"present: unknown key(s) {sorted(extra_keys)}")
        if "hub" in present and not isinstance(present["hub"], bool):
            raise ApplyError("present.hub: true (the PC's placement node presents while it is in the cell) or false")
        if present.get("tls_ca") is not None:
            # TLS to the broker: the file of the broker's CA certificate (PEM or DER), relative to this file. Its DER
            # goes into the manifest, so the owner's signature pins it; it is public, never a secret.
            ca_path = _rel(base, str(present["tls_ca"]))
            try:
                raw = Path(ca_path).read_bytes()
            except OSError as e:
                raise ApplyError(f"present.tls_ca: cannot read {ca_path}: {e}") from None
            import base64

            from . import deploy as dp

            text = raw.decode("ascii", "replace") if b"-----BEGIN" in raw else base64.b64encode(raw).decode()
            try:
                der = dp.broker_ca_der(text, "present.tls_ca")
            except dp.DeployError as e:
                raise ApplyError(str(e)) from None
            present = dict(present, tls_ca=base64.b64encode(der).decode())
    apps = [_source(a, f"apps[{i}]", base, f"app{i}") for i, a in enumerate(doc.get("apps", []))]
    if len({a.name for a in apps}) != len(apps):
        raise ApplyError("apps: two apps share a name")
    pkg = doc.get("package", {})
    manifests = []
    manifest_cells: dict[int, str] = {}
    for i, m in enumerate(pkg.get("manifests", [])):
        if isinstance(m, str):
            manifests.append((_rel(base, m), base))
        elif isinstance(m, dict) and isinstance(m.get("manifest"), str):
            manifests.append((_rel(base, m["manifest"]), _rel(base, m.get("root", "."))))
            if isinstance(m.get("cell"), str):  # M19: in a tree, the cell its portable actors live in
                manifest_cells[len(manifests) - 1] = m["cell"]
        else:
            raise ApplyError(f"package.manifests[{i}]: a path, or {{\"manifest\": ..., \"root\": ...}}")
    modules = [_rel(base, m) for m in pkg.get("modules", [])]
    ver = doc.get("verify", {})
    outside_raw = doc.get("outside", {})
    if not isinstance(outside_raw, dict):
        raise ApplyError("outside: an object of resource path -> \"read\" or \"read_write\"")
    outside = {}
    writers: dict[str, tuple[str, ...]] = {}
    for path, mode in outside_raw.items():
        # A path may leave out the cluster's prefix: "node-7368/hw/die_temp" is potluck://<system>/node-7368/...
        full = path if str(path).startswith("potluck://") else f"potluck://{system}/{str(path).lstrip('/')}"
        if isinstance(mode, dict):
            # M18: {"mode": "read_write", "writers": ["Home Assistant", ...]} -- which programs may write it through
            # the hub (by the name they logged in or were paired with).
            names = mode.get("writers", [])
            if not isinstance(names, list) or not all(isinstance(n, str) and n for n in names):
                raise ApplyError(f"outside['{path}'].writers: a list of program names")
            if names:
                writers[full] = tuple(names)
            mode = mode.get("mode")
        if mode not in ("read", "read_write"):
            raise ApplyError(f"outside['{path}']: \"read\" or \"read_write\", not {mode!r}")
        if full in writers and mode != "read_write":
            raise ApplyError(f"outside['{path}']: writers name who may write it, so its mode is \"read_write\"")
        outside[full] = mode
    cl = Cluster(p, system, doc.get("port"), int(doc.get("baud", 921600)), boards, keys, fwd, potluck, extender,
                 apps, extra_cfg, manifests, modules, _rel(base, pkg.get("guests_root", ".")),
                 float(ver.get("timeout_s", 240)), list(ver.get("read", [])), outside, tuple(board_modules),
                 dict(present) if present else None)
    cl.writers = writers
    cl.stubs = frozenset(stubs)
    cl.manifest_cells = manifest_cells
    cl.rooms = rooms
    cl.positions = positions
    cl.plan = check_plan(doc.get("plan"))
    cl.cells = cells
    cw, parents = {}, {}
    cells_doc = doc.get("cells") or {}
    if not isinstance(cells_doc, dict):
        raise ApplyError("cells: an object, cell name -> {\"window_ms\", \"parent\"}")
    static = {n for v in cells.values() for n in v}
    for name, spec in cells_doc.items():
        if not isinstance(spec, dict) or set(spec) - {"window_ms", "parent", "export", "period_ms"}:
            raise ApplyError(f"cells['{name}']: {{\"window_ms\": <ms>, \"parent\": \"<upper cell>\", \"export\": "
                             f"{{...}}, \"period_ms\": <ms>}}")
        if "export" in spec or "period_ms" in spec:
            if "parent" not in spec:
                raise ApplyError(f"cells['{name}']: exports go to a parent cell -- give it a \"parent\"")
            cl.exports[name] = check_exports(spec.get("export", {}), f"cells['{name}'].export")
            p = spec.get("period_ms", FOREMAN_PERIOD_MS)
            if not isinstance(p, int) or isinstance(p, bool) or not 500 <= p <= 60000:
                raise ApplyError(f"cells['{name}'].period_ms: 500..60000")
            cl.cell_periods[name] = p
        w = spec.get("window_ms", 0)
        if not isinstance(w, int) or isinstance(w, bool) or w % 100 or not 0 <= w <= 25500:
            raise ApplyError(f"cells['{name}'].window_ms: 0..25500, in steps of 100")
        if "parent" in spec:
            par = spec["parent"]
            if not isinstance(par, str) or not 1 <= len(par) <= 31 or not par.isprintable() or par == name:
                raise ApplyError(f"cells['{name}'].parent: another cell's name")
            parents[name] = par
        if w:
            cw[name] = w
    named = static | set(parents.values())
    for name, spec in cells_doc.items():
        if name not in named:
            raise ApplyError(f"cells['{name}']: no board is in this cell, and it is no cell's parent")
    cl.cell_windows = cw
    for name in parents:
        if name not in static:
            raise ApplyError(f"cells['{name}']: has a parent but no board -- a cell's foreman comes from its own boards")
        seen, at = {name}, parents[name]
        while at in parents:  # one tree, no loops
            if at in seen:
                raise ApplyError(f"cells: '{name}' is its own ancestor -- parents make a tree")
            seen.add(at)
            at = parents[at]
    cl.parents = parents
    for nid in foremen:  # Review of M19 #4 and #6: one cell of its own, which has a parent; the role adds the second
        mine = cells.get(nid, ())
        lab = dict(boards)[nid]
        if len(mine) != 1 or mine[0] not in parents:
            raise ApplyError(f"boards['{lab}'].foreman: a foreman is a board in exactly one cell, and that cell has a "
                             f"parent -- the role makes it a member of the parent too (a board holds two memberships "
                             f"at most)")
    cl.foremen = tuple(foremen)
    for name in parents:
        if not any(cells.get(n) == (name,) for n in foremen):
            raise ApplyError(f"cells['{name}']: has a parent but no board that may be its foreman -- mark one or more "
                             f"boards of it \"foreman\": true (on mains power)")
    if len(named) > 255:
        raise ApplyError("cells: at most 255 named cells in one tree")
    return cl


# ---- sources: git archive into a cache, never a live tree ---------------------------------------------

def default_cache() -> Path:
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        return Path(os.environ["LOCALAPPDATA"]) / "potluck" / "cache"
    return Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "potluck"


def _git(args: list[str], cwd: str | None = None, *, binary: bool = False):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True)
    if r.returncode != 0:
        raise ApplyError(f"git {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout if binary else r.stdout.decode().strip()


def resolve(src: Source, cache: Path) -> tuple[str, str]:
    """(repository to archive from, full commit sha). A URL is cloned (bare) into the cache once, then fetched."""
    repo = src.git
    if "://" in repo or repo.startswith("git@"):
        mirror = cache / "git" / hashlib.sha256(repo.encode()).hexdigest()[:16]
        if not mirror.exists():
            mirror.parent.mkdir(parents=True, exist_ok=True)
            _git(["clone", "--bare", "--quiet", repo, str(mirror)])
        else:
            _git(["fetch", "--quiet", "--tags", "origin", "+refs/heads/*:refs/heads/*"], cwd=str(mirror))
        repo = str(mirror)
    elif not Path(repo).exists():
        raise ApplyError(f"{src.name}: no repository at {repo}")
    sha = _git(["rev-parse", "--verify", f"{src.rev}^{{commit}}"], cwd=repo)
    return repo, sha


def export(src: Source, cache: Path) -> tuple[Path, str]:
    """The source tree at its commit, exported with `git archive` into the cache (once per commit)."""
    repo, sha = resolve(src, cache)
    dest = cache / "src" / f"{src.name}-{sha[:12]}"
    if not (dest / ".potluck-export").exists():
        tmp = dest.with_name(dest.name + ".tmp")
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True)
        data = _git(["archive", "--format=tar", sha], cwd=repo, binary=True)
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            t.extractall(tmp, filter="data")
        (tmp / ".potluck-export").write_text(sha + "\n", encoding="ascii")
        shutil.rmtree(dest, ignore_errors=True)
        tmp.rename(dest)
    return dest, sha


def dirty_warning(src: Source) -> str | None:
    if "://" in src.git or src.rev not in ("HEAD",):
        return None
    try:
        st = _git(["status", "--porcelain", "--untracked-files=no"], cwd=src.git)
    except ApplyError:
        return None
    return f"{src.name}: uncommitted changes in {src.git} are not built (apply builds the commit)" if st else None


# ---- firmware -----------------------------------------------------------------------------------------

@dataclass
class Firmware:
    image: Path
    elf: Path
    elf_sha: bytes
    version: str
    built: bool  # False: reused from the cache, or given
    #: M13: the build's per-archive size report (esp-idf-size json2), and the app components it carries.
    size_json: Path | None = None
    components: tuple[str, ...] = ()


def elf_sha256(path: Path) -> bytes:
    return hashlib.sha256(path.read_bytes()).digest()


UPDATE_SLOT_BYTES = 0x200000  # ota_0 / ota_1 in firmware/partitions.csv


def _fits(fw: Firmware) -> Firmware:
    n = fw.image.stat().st_size
    if n > UPDATE_SLOT_BYTES:
        raise ApplyError(f"firmware image is {n} B; an update slot holds {UPDATE_SLOT_BYTES} B "
                         f"(firmware/partitions.csv) -- leave something out of the build")
    return fw


def build_firmware(c: Cluster, cache: Path, log) -> Firmware | None:
    if c.firmware is None:
        return None
    if "image" in c.firmware:
        img = _rel(c.path.parent, c.firmware["image"])
        elf = _rel(c.path.parent, c.firmware.get("elf", str(img.with_suffix(".elf"))))
        if not img.exists() or not elf.exists():
            raise ApplyError(f"firmware: {img} or {elf} does not exist")
        return _fits(Firmware(img, elf, elf_sha256(elf), "given", False))
    pot_dir, pot_sha = export(c.potluck, cache)
    # Keyed by what the build reads -- the firmware tree (less its recorded size reports) and the build
    # script -- not by the commit: a commit that touches only the host tools or the documents must not send
    # new firmware to every board.
    repo, _ = resolve(c.potluck, cache)
    trees = {}
    for line in _git(["ls-tree", pot_sha, "firmware/", "tools/build_firmware.ps1"], cwd=repo).splitlines():
        meta, path = line.split("\t", 1)
        if path != "firmware/size":
            trees[path] = meta.split()[2]
    fw_commit = _git(["log", "-1", "--format=%h", "--abbrev=7", pot_sha, "--", *trees], cwd=repo)
    parts = {"potluck": trees, "extra": sorted(c.extra)}
    ext_dir = None
    if c.extender is not None:
        ext_dir, ext_sha = export(c.extender, cache)
        parts["extender"] = ext_sha
    app_dirs = []
    for a in c.apps:
        if not a.components:
            continue
        d, sha = export(a, cache)
        parts[f"app:{a.name}"] = sha
        app_dirs.append(d / a.components)
    key = hashlib.sha256(json.dumps(parts, sort_keys=True).encode()).hexdigest()[:12]
    variant = "extender-apply" if ext_dir is not None else "apply"
    out = cache / "fw" / key
    bdir = out / f"build-{variant}"
    img, elf = bdir / "potluck_m0.bin", bdir / "potluck_m0.elf"
    version = f"{fw_commit}-{key[:6]}"  # the last commit that changed the firmware, and the build's key
    size_json = out / "size" / f"esp32s3-{variant}" / "size-components.json"
    comps = tuple(sorted(p.name for d in app_dirs if d.is_dir() for p in d.iterdir() if (p / "CMakeLists.txt").exists()))
    if img.exists() and elf.exists():
        log(f"firmware: reusing the build of these sources ({version}, {out})")
        return _fits(Firmware(img, elf, elf_sha256(elf), version, False, size_json, comps))
    # PowerShell 7 where it is installed: Windows PowerShell 5.1 turns ESP-IDF's stderr into a terminating
    # error once the output is captured, and the build stops in export.ps1.
    shell = "pwsh" if shutil.which("pwsh") or os.name != "nt" else "powershell"
    cmd = [shell, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(pot_dir / "tools" / "build_firmware.ps1"),
           "-Variant", variant, "-OutDir", str(out), "-Version", version]
    if c.extra:
        cmd += ["-Extra", ",".join(c.extra)]
    if ext_dir is not None:
        cmd += ["-PmeHotspot", str(ext_dir / (c.extender.components or "firmware/components"))]
    if app_dirs:
        cmd += ["-AppComponents", ",".join(str(d) for d in app_dirs)]
    log(f"firmware: building {version} ({len(app_dirs)} app component dir(s){', extender' if ext_dir else ''}) "
        f"-- a few minutes the first time")
    t0 = time.monotonic()
    # Without Git Bash's MSYS variables: ESP-IDF's activation reads them, exports for that shell instead of
    # PowerShell, and idf.py is then not on PATH (found running apply from Git Bash, 2026-10-11).
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("MSYSTEM", "MINGW", "CHERE")) and k not in ("SHELL", "TERM", "SHLVL", "OSTYPE")}
    r = subprocess.run(cmd, cwd=str(pot_dir), capture_output=True, text=True, errors="replace", env=env)
    (out / "build.log").parent.mkdir(parents=True, exist_ok=True)
    (out / "build.log").write_text(r.stdout + r.stderr, encoding="utf-8")
    if r.returncode != 0 or not img.exists():
        tail = "\n".join((r.stdout + r.stderr).strip().splitlines()[-15:])
        raise ApplyError(f"firmware build failed (log: {out / 'build.log'}):\n{tail}")
    log(f"firmware: built in {time.monotonic() - t0:.0f} s")
    return _fits(Firmware(img, elf, elf_sha256(elf), version, True, size_json, comps))


#: Section 6's hard cap on Potluck's static internal RAM, and the archives counted against it
#: (tools/check_size_budget.py's CORE_ARCHIVES -- the build fails above it).
CORE_CAP_BYTES = 64 * 1024
CORE_ARCHIVES = ("libpot_frame.a", "libpot_link.a", "libpot_espnow.a", "libmain.a", "libpot_ns.a", "libpot_deploy.a",
                 "libpot_trust.a", "libpot_can.a", "libpot_reconcile.a", "libpot_actors.a")


def archive_sizes(size_json: Path) -> dict[str, dict[str, int]]:
    """Per archive: flash (code + read-only data), internal (static internal RAM, DIRAM) and psram (static
    external RAM), from esp-idf-size's json2 report. Empty if the report is missing."""
    try:
        doc = json.loads(size_json.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    out: dict[str, dict[str, int]] = {}
    for key, e in doc.items():
        if not isinstance(e, dict) or not isinstance(e.get("memory_types"), dict):
            continue
        name = e.get("abbrev_name") or key.replace("\\", "/").rsplit("/", 1)[-1]
        mt = e["memory_types"]

        def size(t: str) -> int:
            row = mt.get(t)
            return int(row.get("size", 0)) if isinstance(row, dict) else 0
        a = out.setdefault(name, {"flash": 0, "internal": 0, "psram": 0, "data": 0})
        a["flash"] += size("Flash Code") + size("Flash Data")
        a["internal"] += size("DIRAM") + size("DRAM") + size("IRAM")
        a["psram"] += size("External RAM")
        a["data"] += size("DIRAM") + size("DRAM")  # static data only: what the core gate counts
    return out


def module_costs(c: Cluster, fw: Firmware) -> list[str]:
    """M13: what each optional module costs the image, from the build's own size report -- one line each, then
    Potluck's core against section 6's cap. Static figures; what a module takes from the heap while it runs is
    measured on the boards."""
    if fw.size_json is None:
        return ["modules: no size report for a firmware given as an image"]
    sizes = archive_sizes(fw.size_json)
    if not sizes:
        return [f"modules: size report missing ({fw.size_json})"]
    rows: list[tuple[str, tuple[str, ...]]] = []
    for m in c.board_modules:
        rows.append((m, EXTENDER_ARCHIVES if m == "extender" else BOARD_MODULES[m].archives))
    rows += [(f"app component {comp}", (f"lib{comp}.a",)) for comp in fw.components]
    lines = []
    for name, archives in rows:
        tot = {"flash": 0, "internal": 0, "psram": 0, "data": 0}
        for a in archives:
            for k, v in sizes.get(a, {}).items():
                tot[k] += v
        lines.append(f"module {name}: {tot['flash']:,} B flash, {tot['internal']:,} B internal RAM, "
                     f"{tot['psram']:,} B PSRAM (static; its own archives -- what it pulls in from ESP-IDF is not "
                     f"counted here)")
    core = sum(sizes.get(a, {}).get("data", 0) for a in CORE_ARCHIVES)
    lines.append(f"Potluck core: {core:,} B of {CORE_CAP_BYTES:,} B static internal RAM "
                 f"({CORE_CAP_BYTES - core:,} B left)")
    return lines


# ---- the package --------------------------------------------------------------------------------------

def _rehome(obj: Any, old: str, new: str, ids: dict[int, int]) -> Any:
    """Every string under `obj` with potluck://<old>/ moved to potluck://<new>/ and node-<id> renumbered."""
    if isinstance(obj, str):
        s = obj.replace(f"potluck://{old}/", f"potluck://{new}/") if old != new else obj
        for a, b in ids.items():
            if a != b:
                s = s.replace(f"/node-{a:04x}/", f"/node-{b:04x}/")
        return s
    if isinstance(obj, list):
        return [_rehome(x, old, new, ids) for x in obj]
    if isinstance(obj, dict):
        return {k: _rehome(v, old, new, ids) for k, v in obj.items()}
    return obj


def near_sets(c: Cluster, k: int) -> dict[int, list[int]]:
    """M20 (passive-sensor's D20 rule): each board's K nearest boards. Candidates: the other boards in its room when it
    has one, else every other board; ranked by distance between positions, ties to the lower id; boards with no
    position after every placed one, by id; a board with no position takes the K lowest ids among its candidates;
    fewer than K candidates, all of them. Directional: A may list B while B does not list A."""
    ids = [n for n, _ in c.boards]
    out: dict[int, list[int]] = {}
    for n in ids:
        cands = [q for q in ids if q != n and (n not in c.rooms or c.rooms.get(q) == c.rooms[n])]
        me = c.positions.get(n)
        if me is None:
            ranked = sorted(cands)
        else:
            def key(q):
                p = c.positions.get(q)
                if p is None:
                    return (1, 0.0, q)
                return (0, (p[0] - me[0]) ** 2 + (p[1] - me[1]) ** 2, q)
            ranked = sorted(cands, key=key)
        out[n] = ranked[:k]
    return out


def room_slug(name: str) -> str:
    """M20: a room's name as a path-safe piece -- lowercase, [a-z0-9_], every other run of characters one '_':
    "Living room" -> "living_room". Empty after that (a name of signs only): "room"."""
    import re
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_") or "room"


def per_room(c: Cluster, actors: list[dict]) -> list[dict]:
    """M20: each actor of a module with "per_room" becomes one per room of the cluster file -- <name>_<slug>, placed
    only among that room's boards, `<room>` and its own act/<name>/ in string config values rewritten for it. With no
    rooms the actor stays as it is."""
    from . import modules as mods
    rooms = sorted(set(c.rooms.values()))
    if not rooms:
        return actors
    slugs: dict[str, str] = {}
    for r in rooms:  # two names with one slug ("Hall", "hall"): numbered
        s, i = room_slug(r), 2
        while s in slugs.values():
            s, i = f"{room_slug(r)}_{i}", i + 1
        slugs[r] = s
    out = []
    for a in actors:
        spec = mods.get(str(a.get("module", "")))
        if spec is None or not spec.per_room:
            out.append(a)
            continue
        base = a["name"]
        for r in rooms:
            name = f"{base}_{slugs[r]}"
            b = copy.deepcopy(a)
            b["name"] = name
            b["nodes"] = [n for n, _ in c.boards if c.rooms.get(n) == r]
            for k, v in list(b.get("config", {}).items()):
                if isinstance(v, str):
                    b["config"][k] = v.replace("<room>", slugs[r]).replace(f"/act/{base}/", f"/act/{name}/")
            out.append(b)
    return out


def cell_numbers(c: Cluster) -> dict[str, int]:
    """M19: the cluster file's cell names, sorted and numbered from 1 -- the numbers HELLO carries."""
    names = {n for v in c.cells.values() for n in v} | set(c.parents.values())
    return {name: i + 1 for i, name in enumerate(sorted(names))}


def check_exports(spec: Any, where: str) -> dict[str, dict[str, str]]:
    """M19: a cell's exports -- name -> {"max": "hw/die_temp", "unit": "celsius"} for an aggregate over every board's
    node-<id>/<suffix>, or {"copy": "node-0100/hw/die_temp", "type": "f32", "unit": "celsius"} for one resource."""
    from .value import Unit
    if not isinstance(spec, dict) or len(spec) > FOREMAN_MAX_EXPORTS:
        raise ApplyError(f"{where}: an object of up to {FOREMAN_MAX_EXPORTS} exports (besides 'alive')")
    out = {}
    for ex, d in spec.items():
        if not isinstance(ex, str) or not 1 <= len(ex) <= 24 or room_slug(ex) != ex or ex == "alive":
            raise ApplyError(f"{where}: '{ex}' -- an export's name is lowercase letters, digits and '_' (not 'alive', "
                             f"which every foreman publishes)")
        if not isinstance(d, dict) or len([k for k in d if k in EXPORT_KINDS]) != 1 or \
                set(d) - set(EXPORT_KINDS) - {"unit", "type"}:
            raise ApplyError(f"{where}.{ex}: one of {', '.join(EXPORT_KINDS)}, with an optional unit (and a type for copy)")
        kind = next(k for k in d if k in EXPORT_KINDS)
        arg = d[kind]
        if not isinstance(arg, str) or not 1 <= len(arg) <= 32 or not arg.isascii() or any(ch <= " " for ch in arg):
            raise ApplyError(f"{where}.{ex}.{kind}: a path under node-<id>/ (or for copy, one resource's path), 1..32 "
                             f"printable characters")
        unit = d.get("unit", "none")
        if not isinstance(unit, str) or unit.upper() not in Unit.__members__:
            raise ApplyError(f"{where}.{ex}.unit: one of {', '.join(u.lower() for u in Unit.__members__)}")
        unit = unit.lower()
        typ = d.get("type", "f32")
        if kind == "copy" and typ not in ("bool", "i32", "u32", "f32"):
            raise ApplyError(f"{where}.{ex}.type: bool, i32, u32 or f32")
        out[ex] = {"kind": kind, "of": arg, "unit": unit, "type": typ}
    return out


def foreman_cells(c: Cluster) -> dict[str, list[int]]:
    """M19: each federating cell's foreman-eligible boards, by id."""
    return {name: sorted(n for n in c.foremen if c.cells.get(n) == (name,)) for name in c.parents}


def peer_budget(c: Cluster, cabled: int | None = None) -> dict[int, dict[str, Any]]:
    """M19 (Review of M19 #6): every board's unicast peers at worst, against PEER_CAP. A board's peers are the
    boards of every cell it can be in -- its own, and the parent of its cell when it may hold the foreman role --
    where a cell's members are the boards that name it plus, from each child cell, CHILD_SEATS (its foreman). A board in no cell
    is admitted by every board, and admits every board. The PC (hub or host node, reached through the cabled board
    and its relay) takes one more on each board of the cabled board's cells; `cabled` None: on every board.
    Returns, per board: {"peers": total, "cells": {cell: count}, "host": 0|1}."""
    boards = [n for n, _ in c.boards]
    fc = foreman_cells(c)
    static: dict[str, set[int]] = {}
    for n in boards:
        for name in c.cells.get(n, ()):
            static.setdefault(name, set()).add(n)
    loose = {n for n in boards if not c.cells.get(n)}
    cabled_cells = set(c.cells.get(cabled, ())) if cabled is not None else None
    out: dict[int, dict[str, Any]] = {}
    for n in boards:
        own = tuple(c.cells.get(n, ()))
        mine = list(own) + ([c.parents[own[0]]] if n in c.foremen else [])
        if not mine:  # admitted by every board, and admits every board
            out[n] = {"peers": len(boards) - 1 + 1, "cells": {}, "host": 1}
            continue
        peers = set(loose)
        seats = 0
        per = {}
        for name in mine:
            peers |= static.get(name, set())
            kids = sum(min(len(e), CHILD_SEATS) for k, e in fc.items() if c.parents[k] == name and k not in own)
            seats += kids
            per[name] = len(static.get(name, set()) - {n}) + kids
        peers.discard(n)
        host = int(cabled_cells is None or n == cabled or bool(cabled_cells & set(own)))
        out[n] = {"peers": len(peers) + seats + host, "cells": per, "host": host}
    return out


def rollout_order(c: Cluster, need: list[int], cabled: int | None, inv: dict) -> list[int]:
    """The boards a firmware rollout visits, one at a time: the cabled board last, since everything goes through it.
    M19 (Review of M19 #3): in a tree, a cell's members first, then the boards that may hold its foreman role -- the
    standby before the one running it, which hands the role over when it restarts into the new image."""
    def runs_foreman(n: int) -> bool:
        st = inv.get(n)
        return st is not None and any(str(a).startswith("foreman_") for a in getattr(st, "actors", ()))
    return sorted(need, key=lambda n: (n == cabled, n in c.foremen, runs_foreman(n), need.index(n)))


def check_peers(c: Cluster, cabled: int | None = None) -> list[str]:
    """M19: one plan line per board of a tree (a cluster file with cells); ApplyError naming every board over
    PEER_CAP. A cluster file without cells is one cell, whose twenty-board limit is checked elsewhere."""
    if not c.cells:
        return []
    labels = dict(c.boards)
    budget = peer_budget(c, cabled)
    lines, over = [], []
    for n, u in budget.items():
        parts = [f"{k} {v}" for k, v in u["cells"].items()] + (["the PC 1"] if u["host"] else [])
        lines.append(f"plan: peers {labels[n]} (0x{n:04x}): {u['peers']} of {PEER_CAP} ({', '.join(parts)})")
        if u["peers"] > PEER_CAP:
            over.append((-u["peers"], n, f"{labels[n]} (0x{n:04x}) would need {u['peers']} unicast peers"))
    if c.parents and (c.present or c.extender):
        lines.append("plan: one channel for the whole tree (Review of M19 #7): a board in two cells carries every move "
                     "to the other, so every Wi-Fi exit in the tree must reach a router on the same channel")
    if over:
        over.sort()
        named = "; ".join(x for _, _, x in over[:3]) + (f"; and {len(over) - 3} more board(s)" if len(over) > 3 else "")
        raise ApplyError(f"{named} -- over the {PEER_CAP} a board holds: smaller cells, fewer foreman-eligible boards "
                         f"in a cell, or another upper cell")
    return lines



def room_numbers(c: Cluster) -> dict[str, int]:
    """M20: the rooms the cluster file names, sorted by name and numbered from 1 -- what node-<id>/sys/room holds."""
    return {name: i + 1 for i, name in enumerate(sorted(set(c.rooms.values())))}


def compose(c: Cluster, fragments: list[tuple[dict, Path, str]]) -> dict:
    """One manifest document from the fragments: the cluster's boards as its nodes (what each fragment says
    about a board merged in), every fragment's actors and bindings. `fragments` is (doc, root, where)."""
    nodes: dict[int, dict] = {}
    labels = {lab: nid for nid, lab in c.boards}
    for nid, lab in c.boards:
        nodes[nid] = {"node_id": nid, "label": lab}
    actors: list[dict] = []
    bindings: dict[str, str] = {}
    links: list[dict] = []
    names: dict[str, str] = {}
    for doc, root, where in fragments:
        if not isinstance(doc, dict):
            raise ApplyError(f"{where}: not a manifest")
        if doc.get("outside"):
            raise ApplyError(f"{where}: 'outside' -- what other systems may read or write -- is the owner's choice: "
                             f"it goes in the cluster file, not in a fragment")
        ids: dict[int, int] = {}
        pinned = {a.get("pin") for a in doc.get("actors", [])}
        ignored: set[int] = set()
        for n in doc.get("nodes", []):
            nid, lab = n.get("node_id"), n.get("label")
            if n.get("kind") == "host":
                ids[nid] = nid
                continue
            if nid in nodes:
                ids[nid] = nid
            elif lab in labels:
                ids[nid] = labels[lab]
            elif not c.boards:
                nodes[nid] = {"node_id": nid, "label": lab}
                ids[nid] = nid
            elif set(n) <= {"node_id", "label", "kind", "power"} and nid not in pinned and lab not in pinned:
                ignored.add(nid)  # a board of the author's own cell that nothing here needs: not this cell's
            else:
                raise ApplyError(f"{where}: board '{lab}' (0x{nid:04x}) is not one of the cluster's boards "
                                 f"({', '.join(lab for _, lab in c.boards)}), and the fragment needs it")
        doc = _rehome(doc, doc.get("system", "lab"), c.system, ids)
        for n in doc.get("nodes", []):
            if n.get("node_id") in ignored:
                continue
            nid = ids.get(n.get("node_id"), n.get("node_id"))
            tgt = nodes.setdefault(nid, {"node_id": nid, "label": n.get("label")})
            for k, v in n.items():
                if k in ("node_id", "label"):
                    continue
                if k == "owns":
                    have = {r["path"] for r in tgt.get("owns", [])}
                    tgt.setdefault("owns", []).extend(r for r in v if r["path"] not in have)
                elif k in tgt and tgt[k] != v:
                    raise ApplyError(f"{where}: board '{tgt['label']}': '{k}' is {v!r} here and {tgt[k]!r} in "
                                     f"another fragment")
                else:
                    tgt[k] = v
        for a in doc.get("actors", []):
            a = copy.deepcopy(a)
            if a.get("name") in names:
                raise ApplyError(f"{where}: actor '{a['name']}' is also in {names[a['name']]}")
            names[a["name"]] = where
            if isinstance(a.get("pin"), int):
                a["pin"] = ids.get(a["pin"], a["pin"])
            cfg = a.get("config", {})
            if str(a.get("module", "")).startswith("guest:") and isinstance(cfg.get("bundle"), str):
                cfg["bundle"] = str(_rel(root, cfg["bundle"]))  # each fragment's bundles from its own root
            actors.append(a)
        for k, v in doc.get("bindings", {}).items():
            if k in bindings and bindings[k] != v:
                raise ApplyError(f"{where}: binding '{k}' differs from another fragment's")
            bindings[k] = v
        links.extend(doc.get("links", []))
    actors = per_room(c, actors)  # M20: one per room where a module asks
    names = {a["name"]: names.get(a["name"], "per_room") for a in actors}
    if len(names) != len(actors):
        raise ApplyError("an actor made per room has the name of another actor")
    out = {"schema": 1, "system": c.system, "min_core_version": 1, "nodes": list(nodes.values()), "actors": actors}
    if bindings:
        out["bindings"] = bindings
    if links:
        out["links"] = links
    if c.outside:
        out["outside"] = dict(c.outside)  # M13: the owner's, from the cluster file
    if c.present:
        # M13: the presenting role, as one portable actor; the reconciler gives it to a connected board.
        if "present" in names:
            raise ApplyError(f"an actor named 'present' is in {names['present']}; the name belongs to the presenting role")
        pcfg = {k: v for k, v in c.present.items() if k != "cell"}
        pact = {"name": "present", "module": "builtin:present", "latency_class": 4, "config": pcfg}
        if c.cells:
            # M19 (Review of M19 #3, #7): in a tree the presenting role lives in one cell, named in the cluster file --
            # the cell whose boards reach the broker. Every Wi-Fi exit pins the whole tree to its router's channel, so
            # a tree's exits must all reach routers on one channel (the boards cannot check this; apply says so).
            cell = c.present.get("cell")
            inside = [n for n, _ in c.boards if cell in c.cells.get(n, ())]
            if not inside:
                raise ApplyError("present.cell: in a tree of cells the presenting role lives in one cell -- name it "
                                 "(a cell whose boards reach the broker)")
            pact["nodes"] = inside
        actors.append(pact)
    numbers = room_numbers(c)
    from . import modules as mods
    k = max((s.peers_nearest for s in mods.registered()), default=0)  # an app asks for nearest sets
    near = near_sets(c, k) if k else {}
    cnum = cell_numbers(c)
    for nid in sorted(set(c.positions) | set(c.rooms) | set(c.cells) | {n for n, ids in near.items() if ids}):
        # M20: each board's place on the floor plan and its room, held by the board itself (node-<id>/sys/pos/{x,y},
        # node-<id>/sys/room).
        name = f"pos_{nid:04x}"
        if name in names:
            raise ApplyError(f"an actor named '{name}' is in {names[name]}; the name belongs to board 0x{nid:04x}'s position")
        cfg: dict[str, Any] = {}
        if nid in c.positions:
            cfg["x"], cfg["y"] = c.positions[nid]
        if nid in c.rooms:
            cfg["room"] = numbers[c.rooms[nid]]
        if near.get(nid):
            cfg["near"] = ",".join(f"{q:04x}" for q in near[nid])
        if nid in c.cells:
            cfg["cells"] = ",".join(str(cnum[n]) for n in c.cells[nid])
            if any(c.cell_windows.get(n) for n in c.cells[nid]):
                cfg["cell_windows"] = ",".join(str(c.cell_windows.get(n, 0)) for n in c.cells[nid])
        actors.append({"name": name, "module": "builtin:position", "latency_class": 4, "pin": nid, "config": cfg})
    fc = foreman_cells(c)
    for cell in sorted(c.parents):
        # M19: the cell's foreman role, portable among its foreman-eligible boards (pot/foreman.hpp)
        slug = room_slug(cell)
        name = f"foreman_{slug}"
        if name in names:
            raise ApplyError(f"an actor named '{name}' is in {names[name]}; the name belongs to cell '{cell}''s foreman")
        members = sorted(n for n, _ in c.boards if cell in c.cells.get(n, ()))
        if len(members) > FOREMAN_MAX_MEMBERS:
            raise ApplyError(f"cell '{cell}': {len(members)} boards; a foreman names at most {FOREMAN_MAX_MEMBERS}")
        parent = c.parents[cell]
        cfg = {"cell": cnum[cell], "cell_name": slug, "parent": cnum[parent],
               "parent_window_ms": c.cell_windows.get(parent, 0), "members": ",".join(f"{n:04x}" for n in members),
               "period_ms": c.cell_periods.get(cell, FOREMAN_PERIOD_MS)}
        for ex, e in sorted(c.exports.get(cell, {}).items()):
            cfg[f"export:{ex}"] = "|".join((e["kind"], e["of"], e["unit"], e["type"]))
        actors.append({"name": name, "module": "builtin:foreman", "latency_class": 4, "nodes": fc[cell],
                       "config": cfg})
    for a in actors:
        # M19 (Review of M19 #3): an app's portable actors live in the cell its source names
        src = a.pop("_cell", None)
        if c.cells and src is not None and "pin" not in a:
            if src not in cnum:
                raise ApplyError(f"actor '{a['name']}': its app names cell '{src}', which the cluster file does not have")
            inside = [n for n, _ in c.boards if src in c.cells.get(n, ())]
            a["nodes"] = [n for n in (a.get("nodes") or inside) if n in inside]
            if not a["nodes"]:
                raise ApplyError(f"actor '{a['name']}': no board of cell '{src}' may run it")
    return out


@dataclass
class Package:
    manifest: Any
    doc: dict
    image: bytes  # the node image, not yet signed: its counter is chosen once the inventory is in


def plan_package(c: Cluster, cache: Path, log) -> Package | None:
    from . import deploy as dp
    from . import manifest as mf
    from . import modules as mods
    from .signing import read_key

    mods.clear()
    fragments: list[tuple[dict, Path, str]] = []
    for a in c.apps:
        if not a.components and not a.manifests:
            continue
        d, _ = export(a, cache)
        if a.components:
            for desc in sorted((d / a.components).glob("*/potluck-modules.json")):
                try:
                    mods.load(str(desc))
                except mods.ModuleError as exc:
                    raise ApplyError(f"app '{a.name}': {exc}") from None
                log(f"package: modules from app '{a.name}' ({desc.parent.name})")
        for m in (a.manifests if a.enabled else ()):  # M20: an app switched off adds nothing to the package
            mp = d / m
            frag = json.loads(mp.read_text(encoding="utf-8"))
            if a.cell is not None:  # M19: its portable actors live in that cell (compose places them there)
                for act in frag.get("actors", []):
                    act["_cell"] = a.cell
            fragments.append((frag, d, f"app '{a.name}' {m}"))
    for p in c.modules:
        try:
            mods.load(str(p))
        except (mods.ModuleError, OSError, ValueError) as exc:
            raise ApplyError(f"{p}: {exc}") from None
    # M13: a component that needs one of Potluck's board modules gets it only if the cluster file names it.
    for spec in mods.registered():
        missing = [r for r in spec.requires if r not in c.board_modules]
        if missing:
            raise ApplyError(f"component '{spec.component}' requires {', '.join(missing)} in the firmware: name it in "
                             f"the cluster file's firmware.modules")
    for k, (mp, root) in enumerate(c.manifests):
        try:
            frag = json.loads(mp.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ApplyError(f"{mp}: {exc}") from None
        if k in c.manifest_cells:  # M19: its portable actors live in that cell (compose places them there)
            for act in frag.get("actors", []):
                act["_cell"] = c.manifest_cells[k]
        fragments.append((frag, root, str(mp)))
    if not fragments:
        if not c.boards:
            raise ApplyError("no package and no boards: name the boards (\"boards\"), or give a package")
        return None  # firmware only (a minimal cell)
    doc = compose(c, fragments)
    try:
        m = mf.parse(doc)
        problems = mf.cross_check(m)
    except mf.ManifestErrors as exc:
        raise ApplyError(f"the composed manifest is not valid: {exc}") from None
    if problems:
        raise ApplyError("the composed manifest is not valid:\n  " + "\n  ".join(str(p) for p in problems))
    from . import locality
    across = locality.check_cells(m)  # M19 (Review of M19 #5): a binding across cells is L4
    if across:
        raise ApplyError("a binding crosses cells:\n  " + "\n  ".join(str(e) for e in across))
    from . import keystore
    for k in ("ca", "deploy_key", "deploy_cert", "deploy_bcert"):
        if not c.keys[k].exists() and keystore.lookup(c.keys[k]) is None:  # M20: a key may live only in the vault
            raise ApplyError(f"keys.{k}: {c.keys[k]} does not exist (GETTING-STARTED.md makes the keys)")
    ca = read_key(str(c.keys["ca"]))
    try:
        img = dp.compile_image(m, 0, str(c.guests_root))
        for name, author in dp.check_guest_authors(m, ca.public, str(c.guests_root)):
            log(f"package: guest '{name}' by {author}, signature verified under the cluster CA")
    except (dp.DeployError, OSError, ValueError) as exc:
        raise ApplyError(f"the package does not compile: {exc}") from None
    return Package(m, doc, img)


def sign_and_compile(c: Cluster, pkg: Package, counter: int, keep: Path | None = None) -> bytes:
    """The signed image for the cell. `keep`: also write the signed package there (what `potluck.hostnode
    --package` reads, so a placement host runs exactly what the boards run)."""
    from . import deploy as dp
    from .signing import read_key, sign_manifest, verify_package

    dk = read_key(str(c.keys["deploy_key"]))
    cert = json.loads(c.keys["deploy_cert"].read_text(encoding="utf-8"))
    signed = sign_manifest(pkg.manifest, dk, cert, counter)
    if keep is not None:
        keep.parent.mkdir(parents=True, exist_ok=True)
        keep.write_text(json.dumps(signed, indent=1), encoding="utf-8")
    v = verify_package(json.loads(json.dumps(signed)), read_key(str(c.keys["ca"])).public, min_counter=counter)
    img = dp.compile_image(v.manifest, counter, str(c.guests_root))
    bcert = bytes.fromhex(c.keys["deploy_bcert"].read_text(encoding="ascii").strip())
    return img + dp.image_trailer(img, dk.secret, bcert)


LEASE_MIN_BOARD_ID = 0x0100  # pot/lease.hpp's kLeaseMinBoardId: ids below it are hosts


def lease_counts(arbiter: int, boards) -> str | None:
    """Whether a deploy lease granted by `arbiter` excludes every other apply; None if it does, else why not.

    The cell's arbiter is the lowest live board the cabled board hears (pot/lease.hpp). In a partition each side has
    one, and each grants its own lease: two PCs, one on each side, would both deploy at the cell's floor + 1 -- one
    counter, two packages (tests/test_deploy_partition.cpp; M0-LOG session 43). Only one side can hold the cluster's
    lowest board, so only a lease granted there counts. It costs nothing apply did not already ask for: every board of
    the cluster file must answer the inventory before the lease is taken."""
    lowest = min((n for n in boards if n >= LEASE_MIN_BOARD_ID), default=None)  # a host never arbitrates
    if lowest is None or arbiter == lowest:
        return None
    if arbiter < lowest:
        return (f"board 0x{arbiter:04x} granted the cell's deploy lease and is not in the cluster file -- put it in, "
                f"or take it out of the cell")
    return (f"the cell's deploy lease was granted by board 0x{arbiter:04x}, not by the cluster's lowest board "
            f"0x{lowest:04x}: the board on the cable does not hear 0x{lowest:04x} -- a partition? Then a PC on the "
            f"other side could hold a lease of its own. Try again when the cell is whole")


def send_package(b, node: int, img: bytes, counter: int, digest8: bytes, log) -> tuple[int, int]:
    """BEGIN, CHUNKs, COMMIT through the cabled board, passed on to the cell. (peers_ok, peers_failed)."""
    from . import deploy as dp
    from . import frame as fr

    def step(op, payload, timeout):
        rep = dp.DeployReply.parse(b.deploy_step(op, payload, timeout=timeout, dst=node))
        if not rep.ok:
            raise ApplyError(f"0x{node:04x} refused the package: {rep.status_name}")
        return rep

    step(fr.Op.DEPLOY_BEGIN, dp.begin_payload(img, counter, digest8, distribute=True), 3.0)
    for off in range(0, len(img), dp.CHUNK_MAX):
        step(fr.Op.DEPLOY_CHUNK, dp.chunk_payload(off, img[off:off + dp.CHUNK_MAX]), 3.0)
    rep = step(fr.Op.DEPLOY_COMMIT, dp.commit_payload(img), 30.0)
    return rep.peers_ok, rep.peers_failed


# ---- what each board should run ------------------------------------------------------------------------

def _outputs(m, spec, a, nodes: list[int]) -> list[str]:
    """The paths a module's descriptor says an actor publishes ({node}, {actor} filled in)."""
    if spec is None or not spec.outputs:
        return []
    out = []
    for t in spec.outputs:
        if "{peer}" in t:
            continue  # one per link: counted by ns_budget, not read back (6 x N x (N-1) reads would swamp a verify)
        if t.startswith("?"):
            continue  # M20: published only sometimes -- counted by ns_budget, never waited for
        t = t.replace("{actor}", a.name)
        for n in (nodes if "{node}" in t else [None]):
            out.append(f"potluck://{m.system}/" + (t.replace("{node}", f"node-{n:04x}") if n is not None else t))
    return out


def expected(m, boards: list[int]) -> tuple[dict[int, int], int, list[str]]:
    """(pinned actors per board, portable actors in the cell, output paths to read back)."""
    from . import deploy as dp
    from . import guest as gs
    from . import modules as mods
    from . import reconcile as rc

    pinned = {n: 0 for n in boards}
    portable = 0
    reads: list[str] = []
    for a in m.actors:
        spec = mods.get(a.module)
        if a.module.startswith("guest:"):
            portable += 1
            for name, _t in gs.parse_outputs(a.config["outputs"], a.name):
                reads.append(dp.guest_output_path(m.system, a.name, name))
        elif a.module == "builtin:foreman":  # M19: portable; its summary, every export
            portable += 1
            reads += [dp.cell_path(m.system, a.config["cell_name"], name) for name, *_ in dp.foreman_exports(a)]
        elif a.module in ("builtin:ticker", "builtin:present") or (spec is not None and spec.placement == "portable"):
            # Every board's NODE_STATUS counts every portable actor of the package (portable_total), wherever it runs;
            # the ones a placement host may run are pc_runnable(), for the count of those running on the boards.
            portable += 1
            reads.append(rc.output_path(m, a))
            reads += _outputs(m, spec, a, [])
        elif spec is not None and spec.placement == "every":
            for n in boards:
                pinned[n] += 1
            reads += _outputs(m, spec, a, boards)
        else:
            node = m.placement_of(a)
            if node in pinned:
                pinned[node] += 1
            if a.module == "builtin:die_temp" and node is not None:
                reads.append(dp.die_temp_path(m.system, node))
            if a.module == "builtin:led_switch" and node is not None:
                reads.append(dp.led_switch_path(m.system, node))
            if a.module == "builtin:position" and node is not None:
                if "x" in a.config:
                    reads += list(dp.position_paths(m.system, node))
                if a.config.get("room"):
                    reads.append(dp.room_path(m.system, node))
                if a.config.get("near"):
                    reads.append(dp.near_path(m.system, node))
            reads += _outputs(m, spec, a, [node] if node is not None else [])
    if m.system != "lab":
        for n in boards:
            pinned[n] += 1  # the cluster-name actor
    for n in m.nodes:
        if n.kind == "host":
            continue  # the PC's own resources (a service, a placement host's uptime): served by programs, not boards
        for r in n.owns:
            reads.append(r.path)
    return pinned, portable, list(dict.fromkeys(reads))


#: pot/namespace.hpp's kNamespaceEntries: every board's table, own resources and replicas alike.
NS_CAP = 128
#: The six built-ins every board declares (pot_ns sys_resources.cpp) and M5's act/setpoint (m0_main.cpp).
NS_FIXED = ("sys/heap-free", "sys/heap-largest", "sys/uptime", "sys/boot-epoch", "sys/peers-alive", "sys/rssi")


def ns_budget(m, boards: list[int], cabled: int | None = None, presenters: tuple[int, ...] = (),
              outside: tuple[str, ...] = ()) -> tuple[dict[int, dict], list[str]]:
    """Section 15 question 2 (2026-10-15): how many namespace entries each board's table will hold, against NS_CAP.

    A table holds (from the firmware): its built-ins; every pinned actor's outputs in the package, declared on every
    board at load (actor_runtime.cpp); every portable actor's outputs, on every board (reconcile.cpp); its own outputs
    of each `every` module -- one per link for a `{peer}` path; and replicas added when something reads them, never
    removed: on the cabled board, every board's outputs (apply's verify and the hub read them all through it); on a
    board that may present, every `outside` path. Returns ({board: {"fixed", "package", "own", "replicas", "total"}},
    the modules that declare no outputs -- their entries are made at run time and not counted here; nor are the replicas
    an aggregator reads, which no descriptor states -- bench 2026-10-15: plan 15/16/16, boards 31/40/40, the difference
    exactly passive-sensor's undeclared outputs and rf_fusion's eight link replicas)."""
    from . import deploy as dp
    from . import guest as gs
    from . import modules as mods
    from . import reconcile as rc

    # M20: each board's position actor -- its entries are its own (others adopt them when they read them), and its
    # "near" set is whom a module with "peers": {"nearest": K} keeps per-link outputs for.
    pos_own: dict[int, set[str]] = {n: set() for n in boards}
    near: dict[int, list[int]] = {}
    for a in m.actors:
        node = m.placement_of(a) if a.module == "builtin:position" else None
        if node is None:
            continue
        if "x" in a.config:
            pos_own.setdefault(node, set()).update(dp.position_paths(m.system, node))
        if a.config.get("room"):
            pos_own.setdefault(node, set()).add(dp.room_path(m.system, node))
        if a.config.get("near"):
            near[node] = dp.near_ids(str(a.config["near"]))
            pos_own.setdefault(node, set()).add(dp.near_path(m.system, node))

    # M19 (Review of M19 #3): in a tree each board keeps only its cells' actors (deploy.hpp's scope rule), so it
    # counts only theirs -- a foreman both of its cells'. Without cells every board keeps every actor, as before.
    from . import locality
    cells = locality.node_cells(m)

    def actor_scope(a) -> set[int]:
        if a.module == "builtin:foreman":
            return {int(a.config["cell"]), int(a.config["parent"])}
        node = m.placement_of(a) if a.pin is not None or a.module == "builtin:position" else None
        if node is not None:
            return set(cells.get(node, ()))
        common = None
        for e in a.nodes:
            if e in cells:
                common = set(cells[e]) if common is None else common & cells[e]
        return common or set()

    def mine(n: int) -> set[int]:
        out = set(cells.get(n, ()))
        for a in m.actors:
            if a.module == "builtin:foreman" and n in a.nodes and int(a.config["cell"]) in out:
                out.add(int(a.config["parent"]))
        return out
    my_cells = {n: mine(n) for n in boards}

    def keeps(n: int, a) -> bool:
        if not cells or not my_cells.get(n):
            return True
        s = actor_scope(a)
        return not s or bool(s & my_cells[n]) or m.placement_of(a) == n

    def peers_of(n: int) -> list[int]:
        return [q for q in boards if q != n and (not cells or not my_cells.get(n) or not my_cells.get(q) or
                                                   my_cells[n] & set(cells.get(q, ())))]

    def expand(spec, a, n: int) -> list[str]:
        out = []
        for t in (spec.outputs if spec is not None else ()):
            t = t.lstrip("?").replace("{actor}", a.name).replace("{node}", f"node-{n:04x}")
            if "{peer}" in t:
                links = near.get(n, [])[:spec.peers_nearest] if spec.peers_nearest else peers_of(n)
            else:
                links = [None]
            for p in links:
                out.append(f"potluck://{m.system}/" + (t.replace("{peer}", f"node-{p:04x}") if p is not None else t))
        return out

    by_actor: list[tuple[Any, set[str]]] = []  # each actor's package entries, kept by the boards that keep it
    every: list[tuple] = []
    silent: list[str] = []
    for a in m.actors:
        spec = mods.get(a.module)
        package: set[str] = set()
        by_actor.append((a, package))
        if a.module.startswith("guest:"):
            package.update(dp.guest_output_path(m.system, a.name, name)
                           for name, _t in gs.parse_outputs(a.config["outputs"], a.name))
        elif a.module == "builtin:foreman":
            package.update(dp.cell_path(m.system, a.config["cell_name"], name) for name, *_ in dp.foreman_exports(a))
        elif a.module in ("builtin:ticker", "builtin:present") or (spec is not None and spec.placement == "portable"):
            package.add(rc.output_path(m, a))
            package.update(_outputs(m, spec, a, []))
            if spec is not None and not (spec.outputs or spec.outputs_declared):
                silent.append(a.module)
        elif spec is not None and spec.placement == "every":
            every.append((spec, a))
            if not (spec.outputs or spec.outputs_declared):
                silent.append(a.module)
        else:
            node = m.placement_of(a)
            if node is None:
                continue
            if a.module == "builtin:die_temp":
                package.add(dp.die_temp_path(m.system, node))
            if a.module == "builtin:led_switch":
                package.add(dp.led_switch_path(m.system, node))
            if a.module == "builtin:position":
                continue  # its board's own (pos_own); others adopt them when they read them
            package.update(expand(spec, a, node))
            if spec is not None and not (spec.outputs or spec.outputs_declared):
                silent.append(a.module)
    own = {n: {p for spec, a in every for p in expand(spec, a, n)} | pos_own.get(n, set()) for n in boards}
    # What the verify and the hub read through the cabled board: not the {peer} outputs, which neither reads (a reader
    # of those, such as a capture tool, adds them while it runs).
    own_read = {n: {p for spec, a in every for p in _outputs(m, spec, a, [n])} |
                {p for p in pos_own.get(n, set()) if not p.endswith("/sys/near")} for n in boards}
    out_paths = {(p if p.startswith("potluck://") else f"potluck://{m.system}/{p.lstrip('/')}") for p in outside}
    result: dict[int, dict] = {}
    for n in boards:
        fixed = {f"potluck://{m.system}/node-{n:04x}/{s}" for s in NS_FIXED} | {"act/setpoint"}
        package = {p for a, ps in by_actor if keeps(n, a) for p in ps}
        held = fixed | package | own[n]
        extra: set[str] = set()
        if n == cabled:
            extra |= {p for q in peers_of(n) for p in own_read[q]}
        if n in presenters:
            extra |= out_paths
        extra -= held
        result[n] = {"fixed": len(fixed), "package": len(package - fixed), "own": len(own[n] - fixed - package),
                     "replicas": len(extra), "total": len(held | extra)}
    return result, sorted(set(silent))


def tree_expectations(c: Cluster, pkg: Package, boards: list[int], reads: list[str]
                      ) -> tuple[dict[int, int], int, list[str]]:
    """M19: what the verify expects of a tree's real boards -- each board's portable total is the portable actors it
    keeps (a version-5 image's scope), the ones running on the real boards are those of the real boards' cells, and the
    reads are those the cabled board can resolve: not a stub's, not a cell's summary that no real board's cell keeps.
    Returns (portable total per board, portable actors of the real cells, the reads)."""
    import re

    from . import deploy as dp
    _, _, count, _, _, _, body_len = struct.unpack_from("<IBBHI8sI", pkg.image)
    body = pkg.image[dp.IMAGE_HEADER_LEN:dp.IMAGE_HEADER_LEN + body_len]
    scope = dp.scope_of(body, count) or [(0, 0)] * count
    decls = dp._decls(body, count)
    cells_n = dp.board_cells(body, count)

    def mine(n: int) -> set[int]:
        out = set(cells_n.get(n, ()))
        for (node, _t, cfg), (cell, also) in zip(decls, scope):
            if node == 0xFFFE and also and cell in out and n in dp._eligible_ids(cfg):
                out.add(also)
        return out
    per = {}
    for n in boards:
        m = mine(n)
        per[n] = sum(1 for (node, _t, _c), (cell, also) in zip(decls, scope)
                     if node == 0xFFFE and (not m or cell == 0 or cell in m or (also and also in m)))
    real = {x for n in boards for x in cells_n.get(n, ())}
    here = sum(1 for (node, _t, _c), (cell, _a) in zip(decls, scope) if node == 0xFFFE and (cell in real or cell == 0))
    slugs = {room_slug(name) for name, num in cell_numbers(c).items() if num in real}
    kept = []
    for p in reads:
        cm = re.search(r"/cell-([a-z0-9_]+)/", p)
        nm = re.search(r"/node-([0-9a-f]{4})/", p)
        if (cm and cm.group(1) not in slugs) or (nm and int(nm.group(1), 16) in c.stubs):
            continue
        kept.append(p)
    return per, here, kept


def pc_runnable(m) -> int:
    """How many of the package's portable actors a placement host may hold -- while the PC is in the cell they run
    there, not on a board (M14: the ticker drawn to the PC, the presenting role with present.hub)."""
    from . import reconcile as rc

    return sum(1 for a in m.actors if a.pin is None and not a.module.startswith("guest:")
               and any(m.node(n) is not None and m.node(n).kind == "host" for n, _ in rc.eligible(m, a)))


# ---- the command --------------------------------------------------------------------------------------

def cmd_inventory(b, boards: list[int]) -> int:
    for n in boards:
        st = node_status(b, n)
        if st is not None:
            print(st.line())
            continue
        f = fw_status(b, n)
        print(f"0x{n:04x}  " + (f"fw {f['version']} (counter {f['running']}, floor {f['floor']}, {f['state']}) -- "
                                 f"firmware from before M12: no inventory" if f else "UNREACHABLE"))
    return 0


def apply(c: Cluster, port: str, *, plan_only: bool, cache: Path) -> int:
    from . import fw as fwm
    from .bridge import Bridge
    from .value import Quality

    t_start = time.monotonic()
    times: dict[str, float] = {}

    def log(msg: str) -> None:
        print(f"[{time.monotonic() - t_start:7.1f} s] {msg}", flush=True)

    # 1. plan -- nothing on any board changes in this step.
    try:
        for s in [c.potluck, *c.apps] + ([c.extender] if c.extender else []):
            w = dirty_warning(s) if c.firmware is not None or s is not c.potluck else None
            if w:
                log(f"note: {w}")
        pkg = plan_package(c, cache, log)
        # M19: stub boards are in the tree and the package, never on the cable: potluck.stubs hosts them
        boards = [n for n, _ in c.boards if n not in c.stubs] or [n.node_id for n in pkg.manifest.nodes
                                                                 if n.kind != "host" and n.node_id not in c.stubs]
        if pkg is not None:
            pinned_want, portable_want, reads = expected(pkg.manifest, boards)
            pc_may_run = pc_runnable(pkg.manifest)
            portable_by, here_want = {n: portable_want for n in boards}, portable_want
            if c.cells:  # M19: each board keeps its cells' portable actors, and reads what its cells hold
                portable_by, here_want, reads = tree_expectations(c, pkg, boards, reads)
            log(f"package: '{c.system}', {len(pkg.manifest.actors)} actor(s) ({portable_want} portable), "
                f"image {len(pkg.image)} B, for {len(boards)} board(s)")
        else:
            pinned_want, portable_want, reads = {n: 0 for n in boards}, 0, []
            portable_by, here_want = {n: 0 for n in boards}, 0
            pc_may_run = 0
            log(f"package: none -- firmware only, for {len(boards)} board(s)")
        reads = list(dict.fromkeys(reads + c.verify_reads))
        fwb = build_firmware(c, cache, log)
        if fwb is not None:
            for line in module_costs(c, fwb):
                log(line)
            if AMPDU_TX_OFF in c.extra and "present" in c.board_modules:
                log("note: AMPDU transmit aggregation is off on every board, because this cell presents (a board "
                    "presenting over its own station with it on wedges its radio; M0-LOG session 42) -- the extender's "
                    "hotspot carries less as a result")
    except ApplyError as exc:
        print(f"REFUSED, nothing changed: {exc}")
        return 2
    times["plan (package, firmware build)"] = time.monotonic() - t_start

    with Bridge.open(port=port, baud=c.baud) as b, contextlib.ExitStack() as held:
        if b.hello(timeout=3.0) is None:
            print(f"REFUSED, nothing changed: no board answers on {port}")
            return 2
        cabled = b.peer_node_id
        t0 = time.monotonic()
        inv: dict[int, NodeStatus | None] = {n: node_status(b, n) for n in boards}
        fws: dict[int, dict | None] = {n: (None if inv[n] else fw_status(b, n)) for n in boards}
        for n in boards:
            st = inv[n]
            log(f"inventory: {st.line()}" if st else
                f"inventory: 0x{n:04x} " + (f"fw {fws[n]['version']} counter {fws[n]['running']} (before M12)"
                                            if fws[n] else "UNREACHABLE"))
        missing = [n for n in boards if inv[n] is None and fws[n] is None]
        if missing:
            print("REFUSED, nothing changed: unreachable: " + ", ".join(f"0x{n:04x}" for n in missing) +
                  " -- power them, or take them out of the cluster file")
            return 2
        if cabled not in boards:
            print(f"REFUSED, nothing changed: the board on the cable (0x{cabled:04x}) is not in the cluster file")
            return 2
        try:  # M19 (Review of M19 #6): the tree's peer budget, per board
            for line in check_peers(c, cabled):
                log(line)
        except ApplyError as exc:
            print(f"REFUSED, nothing changed: {exc}")
            return 2
        times["inventory"] = time.monotonic() - t0
        if fwb is not None:
            need = [n for n in boards if inv[n] is None or inv[n].elf_sha != fwb.elf_sha[:8]]
            log("plan: firmware " + (f"{fwb.version} to " + ", ".join(f"0x{n:04x}" for n in need) if need
                                     else "unchanged on every board"))
        if pkg is not None:
            now = sorted({x for st in inv.values() if st is not None for x in st.actors})
            log(f"plan: package '{c.system}' replaces what runs now ({', '.join(now) or 'nothing'}) with "
                f"{', '.join(a.name + ' (' + a.module + ')' for a in pkg.manifest.actors)}")
            # Section 15 question 2: every board's namespace table, counted before anything is sent.
            presenters = tuple(n for n in boards if c.present and inv[n] is not None and
                               any(mm == "present" and "no network" not in (r or "") for mm, _s, r in inv[n].modules))
            budget, silent = ns_budget(pkg.manifest, boards, cabled, presenters, tuple(c.outside))
            for n in boards:
                u = budget[n]
                log(f"plan: namespace 0x{n:04x}: {u['total']} of {NS_CAP} entries ({u['fixed']} built-in, "
                    f"{u['package']} the package's, {u['own']} its own"
                    + (f", {u['replicas']} replicas it reads" if u['replicas'] else "") + ")"
                    + ("  -- lookups slow past 7/8 full" if NS_CAP * 7 // 8 < u['total'] <= NS_CAP else ""))
            if silent:
                log(f"plan: namespace: {', '.join(silent)} declare(s) no outputs -- what they make at run time is not "
                    f"counted (a module lists its outputs in potluck-modules.json, `{{peer}}` for one per link)")
            over = [n for n in boards if budget[n]["total"] > NS_CAP]
            if over:
                print("REFUSED, nothing changed: " + ", ".join(f"0x{n:04x} would need {budget[n]['total']}"
                                                             for n in over)
                      + f" namespace entries, over the {NS_CAP} a board holds -- fewer outputs, fewer boards, or "
                        f"fewer per-link resources")
                return 2
            runs_guest = "guest" in now
            has_guest = any(a.module.startswith("guest:") for a in pkg.manifest.actors)
            if runs_guest != has_guest or (has_guest and not all(st is not None and st.pkg_counter for st in inv.values())):
                log("plan: the cell's guests change (a guest's code is never changed in place), so every board "
                    "reboots into the package, on trial")
        if plan_only:
            print("plan only: nothing changed")
            return 0
        # M14: the cell's deploy lease, held until this run ends -- a second apply, from this PC or another, is
        # refused here, before anything changes, naming the first.
        from .lease import KIND_DEPLOY, CellLease, LeaseRefused, LeaseUnavailable, holder_name

        try:
            lease = held.enter_context(CellLease.take(b, KIND_DEPLOY, holder_name("apply"), log=log))
        except (LeaseRefused, LeaseUnavailable) as exc:
            print(f"REFUSED, nothing changed: {exc}")
            return 2
        if lease.supported and lease.reply is not None:
            why = lease_counts(lease.reply.arbiter, boards)
            if why is not None:
                print(f"REFUSED, nothing changed: {why}")
                return 2
            log(f"lease: the cell's deploy lease is ours (board 0x{lease.reply.arbiter:04x} holds it)")
        uptime0 = {n: (inv[n].uptime_s if inv[n] else None) for n in boards}
        t_uptime0 = time.monotonic()
        reboots_expected: set[int] = set()

        # 2. firmware, where it differs.
        if fwb is not None:
            need = [n for n in boards if inv[n] is None or inv[n].elf_sha != fwb.elf_sha[:8]]
            if need:
                floors = [x for n in boards for x in ((inv[n].fw_floor, inv[n].fw_running) if inv[n] else
                                                      (fws[n]["floor"], fws[n]["running"]))]
                counter = max(floors) + 1
                order = rollout_order(c, need, cabled, inv)
                log(f"firmware: {fwb.version} to {', '.join(f'0x{n:04x}' for n in order)} at counter {counter}")
                from .signing import read_key

                dk = read_key(str(c.keys["deploy_key"]))
                bcert = bytes.fromhex(c.keys["deploy_bcert"].read_text(encoding="ascii").strip())
                img = fwb.image.read_bytes()
                trailer = fwm.fw_trailer(img, counter, dk.secret, bcert)
                t0 = time.monotonic()
                for n in order:
                    if lease.lost is not None:
                        print(f"FAILED: the cell's deploy lease is now {lease.lost.holder_line()} -- stopped before "
                              f"firmware to 0x{n:04x}")
                        return 1
                    t1 = time.monotonic()
                    r = fwm.send_image(b, n, img, counter, trailer)
                    if r["status"] != "ok":
                        print(f"FAILED: firmware to 0x{n:04x}: {r['status']} -- it keeps the image it runs; "
                              f"stopped before the package")
                        return 1
                    done = fwm.wait_confirmed(b, n, counter, 240.0)
                    if done is None or done["running"] != counter or done["state"] != "confirmed":
                        print(f"FAILED: 0x{n:04x} did not confirm firmware {counter}: {done} -- stopped")
                        return 1
                    reboots_expected.add(n)
                    log(f"firmware: 0x{n:04x} confirmed {done['version']} in {time.monotonic() - t1:.0f} s")
                times["firmware"] = time.monotonic() - t0
                b.hello(timeout=3.0)
                for n in boards:
                    inv[n] = node_status(b, n, timeout=4.0, tries=4)
                bad = [n for n in boards if inv[n] is None]
                if bad:
                    print("FAILED: no inventory after the firmware from " + ", ".join(f"0x{n:04x}" for n in bad))
                    return 1
                # What the package step does is measured from here: the firmware's reboots are reported above.
                uptime0 = {n: inv[n].uptime_s for n in boards}
                t_uptime0 = time.monotonic()
                reboots_expected.clear()
            else:
                log(f"firmware: every board already runs {fwb.version}")

        # 3. the package.
        if pkg is None:
            total = time.monotonic() - t_start
            print("")
            for k, v in times.items():
                print(f"  {k:<32} {v:7.1f} s")
            print(f"  {'command to verified':<32} {total:7.1f} s")
            want = fwb.version if fwb is not None else None
            off = [n for n in boards if want is not None and inv[n].fw_version != want]
            if off:
                print("NOT VERIFIED: " + ", ".join(f"0x{n:04x} runs {inv[n].fw_version}" for n in off))
                return 1
            print(f"VERIFIED: {len(boards)} board(s) run {want or 'their firmware'}; no package")
            return 0
        busy = [n for n in boards if inv[n] is not None and inv[n].pending]
        if busy:
            log("package: waiting for the trial on " + ", ".join(f"0x{n:04x}" for n in busy) + " to end")
            end = time.monotonic() + 300
            while busy and time.monotonic() < end:
                time.sleep(5)
                for n in list(busy):
                    st = node_status(b, n)
                    if st is not None and not st.pending:
                        inv[n] = st
                        busy.remove(n)
            if busy:
                print("FAILED: still on trial: " + ", ".join(f"0x{n:04x}" for n in busy))
                return 1
        counter = max(max(s.pkg_floor, s.pkg_counter) for s in inv.values() if s is not None) + 1
        keep = cache / "packages" / f"{c.system}-{counter}.pkg.json"
        try:
            signed = sign_and_compile(c, pkg, counter, keep=keep)
            log(f"package: signed at counter {counter}, kept as {keep}")
        except Exception as exc:  # noqa: BLE001 - a key problem the plan's checks did not catch
            print(f"FAILED before sending the package: {exc}")
            return 1
        digest8 = bytes.fromhex(pkg.manifest.digest())[:8]
        if lease.lost is not None:
            print(f"FAILED: the cell's deploy lease is now {lease.lost.holder_line()} -- stopped before the package")
            return 1
        t0 = time.monotonic()
        try:
            ok, failed = send_package(b, cabled, signed, counter, digest8, log)
        except Exception as exc:  # noqa: BLE001
            print(f"FAILED: {exc}")
            return 1
        log(f"package: counter {counter} committed on 0x{cabled:04x}, passed on to {ok} board(s)"
            + (f", {failed} failed (they will ask a peer for it)" if failed else ""))
        times["package"] = time.monotonic() - t0

        # 4. verify.
        t0 = time.monotonic()
        end = time.monotonic() + c.verify_timeout_s
        done: dict[int, NodeStatus] = {}
        seen_at: dict[int, float] = {}
        while time.monotonic() < end and len(done) < len(boards):
            time.sleep(4)
            for n in boards:
                if n in done:
                    continue
                st = node_status(b, n)
                if st is None:
                    b.hello(timeout=1.0)  # a board that rebooted forgets the host
                    continue
                if st.pkg_counter == counter and not st.pending and st.pinned_running >= pinned_want[n] \
                        and st.portable_total == portable_by[n]:
                    done[n] = st
                    seen_at[n] = time.monotonic()
                    log(f"verify: 0x{n:04x} confirmed package {counter}: {','.join(st.actors) or '-'}")
        problems = []
        for n in boards:
            if n not in done:
                st = node_status(b, n)
                problems.append(f"0x{n:04x} did not confirm package {counter}: " + (st.line() if st else "no answer"))
        # On the boards: every portable actor, less those the PC holds while it is in the cell (M14; found on the bench,
        # M0-LOG session 42 -- first "3 running, the package has 4" with the ticker on the PC, then, counted the other
        # way, no board ever confirmed a package with such an actor).
        here = sum(s.portable_here for s in done.values())
        lo = here_want - pc_may_run
        if len(done) == len(boards) and not lo <= here <= here_want:
            # The reconciler may still be settling a move; give it one more look.
            time.sleep(10)
            here = sum((node_status(b, n) or done[n]).portable_here for n in boards)
            if not lo <= here <= here_want:
                problems.append(f"{here} portable actor(s) running on the boards, the package has {here_want}"
                                + (f" ({pc_may_run} of them the PC may hold)" if pc_may_run else ""))
        for p in reads:
            # GOOD, not merely a value: the cabled board answers a read of another board's output from its
            # copy and refreshes the copy as it does, so the first answer can be an older, STALE one.
            # Until the verify deadline: a freshly started actor may publish nothing at first (one that learns
            # a baseline, say), and that is the app's behaviour, not a failed apply.
            r = None
            until = max(end, time.monotonic() + 15)
            while time.monotonic() < until:
                try:
                    r = b.read(p, timeout=2.0)
                    if r.quality == Quality.GOOD:
                        break
                except Exception:  # noqa: BLE001 - try again; reported below
                    r = None
                time.sleep(2.0)
            if r is None or r.quality != Quality.GOOD:
                problems.append(f"{p}: " + (r.format() if r is not None else "no answer"))
            else:
                log(f"verify: {p} = {r.format()}")
        times["verify"] = time.monotonic() - t0
        # Rebooted: up for less time than has passed since its uptime was read (a few seconds' slack).
        rebooted = [n for n in boards if n in done and uptime0[n] is not None and n not in reboots_expected
                    and done[n].uptime_s + 3 < uptime0[n] + (seen_at[n] - t_uptime0)]
        live = [n for n in boards if n in done and inv[n] is not None and done[n].live_applies > inv[n].live_applies]
    total = time.monotonic() - t_start
    print("")
    for k, v in times.items():
        print(f"  {k:<32} {v:7.1f} s")
    print(f"  {'command to verified':<32} {total:7.1f} s")
    print(f"  package {counter}: live on {len(live)} board(s); rebooted for it: "
          f"{', '.join(f'0x{n:04x}' for n in rebooted) or 'none'}")
    if problems:
        print("NOT VERIFIED:\n  " + "\n  ".join(problems))
        return 1
    print(f"VERIFIED: cluster '{c.system}' runs package {counter} on {len(boards)} board(s)")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="potluck.apply", description=__doc__.split("\n\n")[0])
    ap.add_argument("cluster", help="the cluster file")
    ap.add_argument("what", nargs="?", default="apply", choices=("apply", "inventory"))
    ap.add_argument("--port", default=None, help="the serial port of the board on the cable (else the file's)")
    ap.add_argument("--plan", action="store_true", help="plan, build and read the inventory; change nothing")
    ap.add_argument("--cache", default=None, help=f"where exports and builds are kept (default {default_cache()})")
    ap.add_argument("--passphrase-stdin", action="store_true",
                    help="M20: read the passphrase of the keys' vault from the first line of stdin (the hub's way)")
    a = ap.parse_args(argv)
    try:
        c = load_cluster(a.cluster)
    except ApplyError as exc:
        print(f"REFUSED, nothing changed: {exc}")
        return 2
    vault = c.keys["ca"].parent / "vault.json"
    if vault.exists() and (a.passphrase_stdin or not c.keys["deploy_key"].exists()):
        from . import keystore
        try:
            if a.passphrase_stdin:
                phrase = sys.stdin.readline().rstrip("\r\n")
            else:
                import getpass
                phrase = getpass.getpass(f"Passphrase for {vault}: ")
            keystore.unlock(vault, phrase)
        except (keystore.KeystoreError, OSError, ValueError) as exc:
            print(f"REFUSED, nothing changed: the keys' vault did not open ({exc})")
            return 2
    port = a.port or c.port
    if not port:
        print("REFUSED, nothing changed: no port (--port, or \"port\" in the cluster file)")
        return 2
    if a.what == "inventory":
        from .bridge import Bridge

        with Bridge.open(port=port, baud=c.baud) as b:
            if b.hello(timeout=3.0) is None:
                print(f"no board answers on {port}")
                return 1
            return cmd_inventory(b, [n for n, _ in c.boards] or [b.peer_node_id])
    return apply(c, port, plan_only=a.plan, cache=Path(a.cache) if a.cache else default_cache())


if __name__ == "__main__":
    sys.exit(main())
