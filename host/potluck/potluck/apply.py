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
MODULE_STATES = {1: "active", 2: "idle", 3: "failed"}
MODULE_REASONS = {0: "", 1: "no credentials", 2: "no network", 3: "standby", 4: "nothing here uses it",
                  5: "out of memory", 6: "refused", 7: "error"}


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
    #: section 13's class: "a security liability" or "filler" -- printed by --plan, never a reason to refuse.
    why_optional: str = "filler"


#: Built so far. The extender is not here: it comes from its own repository (`firmware.extender`), and is
#: named "extender" in the inventory and in a descriptor's `requires`.
BOARD_MODULES = {
    "sandbox": BoardModule(("CONFIG_POT_WASM=y",), ("libpot_wasm.a",), why_optional="filler"),
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


def _rel(base: Path, p: str) -> Path:
    q = Path(os.path.expandvars(os.path.expanduser(p)))
    return q if q.is_absolute() else (base / q).resolve()


def _source(d: Any, where: str, base: Path, default_name: str) -> Source:
    if not isinstance(d, dict):
        raise ApplyError(f"{where}: must be an object")
    extra = set(d) - {"name", "git", "rev", "components", "manifests"}
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
    return Source(str(d.get("name", default_name)), git, rev, d.get("components"), tuple(mans))


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
               "comment", "outside"}
    extra = set(doc) - allowed
    if extra:
        raise ApplyError(f"{path}: unknown key(s) {sorted(extra)}")
    system = doc.get("system", "lab")
    if not isinstance(system, str) or not 1 <= len(system) <= 31 or any(c in system for c in " /") or \
            not system.isascii() or not system.isprintable():
        raise ApplyError("system: a cluster name is 1..31 printable ASCII characters, without spaces or '/'")
    boards = []
    for i, bd in enumerate(doc.get("boards", [])):
        if not isinstance(bd, dict) or "node" not in bd:
            raise ApplyError(f"boards[{i}]: {{\"node\": \"<hex id>\", \"label\": \"...\"}}")
        nid = int(bd["node"], 16) if isinstance(bd["node"], str) else int(bd["node"])
        boards.append((nid, str(bd.get("label", f"node-{nid:04x}"))))
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
    for m in board_modules:
        for r in BOARD_MODULES[m].requires if m in BOARD_MODULES else ():
            if r not in board_modules:
                raise ApplyError(f"firmware.modules: '{m}' needs '{r}' in the image too")
    apps = [_source(a, f"apps[{i}]", base, f"app{i}") for i, a in enumerate(doc.get("apps", []))]
    if len({a.name for a in apps}) != len(apps):
        raise ApplyError("apps: two apps share a name")
    pkg = doc.get("package", {})
    manifests = []
    for i, m in enumerate(pkg.get("manifests", [])):
        if isinstance(m, str):
            manifests.append((_rel(base, m), base))
        elif isinstance(m, dict) and isinstance(m.get("manifest"), str):
            manifests.append((_rel(base, m["manifest"]), _rel(base, m.get("root", "."))))
        else:
            raise ApplyError(f"package.manifests[{i}]: a path, or {{\"manifest\": ..., \"root\": ...}}")
    modules = [_rel(base, m) for m in pkg.get("modules", [])]
    ver = doc.get("verify", {})
    outside_raw = doc.get("outside", {})
    if not isinstance(outside_raw, dict):
        raise ApplyError("outside: an object of resource path -> \"read\" or \"read_write\"")
    outside = {}
    for path, mode in outside_raw.items():
        # A path may leave out the cluster's prefix: "node-7368/hw/die_temp" is potluck://<system>/node-7368/...
        full = path if str(path).startswith("potluck://") else f"potluck://{system}/{str(path).lstrip('/')}"
        if mode not in ("read", "read_write"):
            raise ApplyError(f"outside['{path}']: \"read\" or \"read_write\", not {mode!r}")
        outside[full] = mode
    return Cluster(p, system, doc.get("port"), int(doc.get("baud", 921600)), boards, keys, fwd, potluck, extender,
                   apps, extra_cfg, manifests, modules, _rel(base, pkg.get("guests_root", ".")),
                   float(ver.get("timeout_s", 240)), list(ver.get("read", [])), outside, tuple(board_modules))


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
                     f"{tot['psram']:,} B PSRAM (static)")
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
    out = {"schema": 1, "system": c.system, "min_core_version": 1, "nodes": list(nodes.values()), "actors": actors}
    if bindings:
        out["bindings"] = bindings
    if links:
        out["links"] = links
    if c.outside:
        out["outside"] = dict(c.outside)  # M13: the owner's, from the cluster file
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
        for m in a.manifests:
            mp = d / m
            fragments.append((json.loads(mp.read_text(encoding="utf-8")), d, f"app '{a.name}' {m}"))
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
    for mp, root in c.manifests:
        try:
            fragments.append((json.loads(mp.read_text(encoding="utf-8")), root, str(mp)))
        except (OSError, ValueError) as exc:
            raise ApplyError(f"{mp}: {exc}") from None
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
    for k in ("ca", "deploy_key", "deploy_cert", "deploy_bcert"):
        if not c.keys[k].exists():
            raise ApplyError(f"keys.{k}: {c.keys[k]} does not exist (GETTING-STARTED.md makes the keys)")
    ca = read_key(str(c.keys["ca"]))
    try:
        img = dp.compile_image(m, 0, str(c.guests_root))
        for name, author in dp.check_guest_authors(m, ca.public, str(c.guests_root)):
            log(f"package: guest '{name}' by {author}, signature verified under the cluster CA")
    except (dp.DeployError, OSError, ValueError) as exc:
        raise ApplyError(f"the package does not compile: {exc}") from None
    return Package(m, doc, img)


def sign_and_compile(c: Cluster, pkg: Package, counter: int) -> bytes:
    from . import deploy as dp
    from .signing import read_key, sign_manifest, verify_package

    dk = read_key(str(c.keys["deploy_key"]))
    cert = json.loads(c.keys["deploy_cert"].read_text(encoding="utf-8"))
    signed = sign_manifest(pkg.manifest, dk, cert, counter)
    v = verify_package(json.loads(json.dumps(signed)), read_key(str(c.keys["ca"])).public, min_counter=counter)
    img = dp.compile_image(v.manifest, counter, str(c.guests_root))
    bcert = bytes.fromhex(c.keys["deploy_bcert"].read_text(encoding="ascii").strip())
    return img + dp.image_trailer(img, dk.secret, bcert)


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
        elif a.module == "builtin:ticker" or (spec is not None and spec.placement == "portable"):
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
            reads += _outputs(m, spec, a, [node] if node is not None else [])
    if m.system != "lab":
        for n in boards:
            pinned[n] += 1  # the cluster-name actor
    for n in m.nodes:
        for r in n.owns:
            reads.append(r.path)
    return pinned, portable, list(dict.fromkeys(reads))


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
        boards = [n for n, _ in c.boards] or [n.node_id for n in pkg.manifest.nodes if n.kind != "host"]
        if pkg is not None:
            pinned_want, portable_want, reads = expected(pkg.manifest, boards)
            log(f"package: '{c.system}', {len(pkg.manifest.actors)} actor(s) ({portable_want} portable), "
                f"image {len(pkg.image)} B, for {len(boards)} board(s)")
        else:
            pinned_want, portable_want, reads = {n: 0 for n in boards}, 0, []
            log(f"package: none -- firmware only, for {len(boards)} board(s)")
        reads = list(dict.fromkeys(reads + c.verify_reads))
        fwb = build_firmware(c, cache, log)
        if fwb is not None:
            for line in module_costs(c, fwb):
                log(line)
    except ApplyError as exc:
        print(f"REFUSED, nothing changed: {exc}")
        return 2
    times["plan (package, firmware build)"] = time.monotonic() - t_start

    with Bridge.open(port=port, baud=c.baud) as b:
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
        times["inventory"] = time.monotonic() - t0
        if fwb is not None:
            need = [n for n in boards if inv[n] is None or inv[n].elf_sha != fwb.elf_sha[:8]]
            log("plan: firmware " + (f"{fwb.version} to " + ", ".join(f"0x{n:04x}" for n in need) if need
                                     else "unchanged on every board"))
        if pkg is not None:
            now = sorted({x for st in inv.values() if st is not None for x in st.actors})
            log(f"plan: package '{c.system}' replaces what runs now ({', '.join(now) or 'nothing'}) with "
                f"{', '.join(a.name + ' (' + a.module + ')' for a in pkg.manifest.actors)}")
            runs_guest = "guest" in now
            has_guest = any(a.module.startswith("guest:") for a in pkg.manifest.actors)
            if runs_guest != has_guest or (has_guest and not all(st is not None and st.pkg_counter for st in inv.values())):
                log("plan: the cell's guests change (a guest's code is never changed in place), so every board "
                    "reboots into the package, on trial")
        if plan_only:
            print("plan only: nothing changed")
            return 0
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
                order = [n for n in need if n != cabled] + ([cabled] if cabled in need else [])
                log(f"firmware: {fwb.version} to {', '.join(f'0x{n:04x}' for n in order)} at counter {counter}")
                from .signing import read_key

                dk = read_key(str(c.keys["deploy_key"]))
                bcert = bytes.fromhex(c.keys["deploy_bcert"].read_text(encoding="ascii").strip())
                img = fwb.image.read_bytes()
                trailer = fwm.fw_trailer(img, counter, dk.secret, bcert)
                t0 = time.monotonic()
                for n in order:
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
        try:
            signed = sign_and_compile(c, pkg, counter)
        except Exception as exc:  # noqa: BLE001 - a key problem the plan's checks did not catch
            print(f"FAILED before sending the package: {exc}")
            return 1
        digest8 = bytes.fromhex(pkg.manifest.digest())[:8]
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
                        and st.portable_total == portable_want:
                    done[n] = st
                    seen_at[n] = time.monotonic()
                    log(f"verify: 0x{n:04x} confirmed package {counter}: {','.join(st.actors) or '-'}")
        problems = []
        for n in boards:
            if n not in done:
                st = node_status(b, n)
                problems.append(f"0x{n:04x} did not confirm package {counter}: " + (st.line() if st else "no answer"))
        here = sum(s.portable_here for s in done.values())
        if len(done) == len(boards) and here != portable_want:
            # The reconciler may still be settling a move; give it one more look.
            time.sleep(10)
            here = sum((node_status(b, n) or done[n]).portable_here for n in boards)
            if here != portable_want:
                problems.append(f"{here} portable actor(s) running in the cell, the package has {portable_want}")
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
    a = ap.parse_args(argv)
    try:
        c = load_cluster(a.cluster)
    except ApplyError as exc:
        print(f"REFUSED, nothing changed: {exc}")
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
