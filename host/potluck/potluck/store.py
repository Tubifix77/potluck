"""The store (M20): sandboxed apps from GitHub -- off until the owner turns it on, and every install the owner's.

An app is a guest (potluck.guest: a WebAssembly module run in the boards' sandbox, which can write nothing but its
own outputs) published in a GitHub repository with potluck-app.json at its root:

    {"type": "potluck-app-v1", "name": "overheat_alarm", "description": "...",
     "author_key": "<the author's Ed25519 public key, hex>",
     "module": "<the .wasm's path in the repository>", "sha256": "<the module's>",
     "sig": "<the author's Ed25519 over GUEST_DOMAIN || SHA-512(module), hex>",
     "inputs": [{"name": "temperature", "type": "f32", "unit": "celsius"}],
     "outputs": "out:f32 alarm:i32 count:i32", "fuel": 100000, "period_ms": 1000}

No certificate and no paths (ARCHITECTURE section 13, M20 review item 7): on the first install the cell pins the key
to the repository, gives the author its next author number and certifies the key under its own CA (role 3); the owner
binds each input to one of the cell's resources. A repository whose key changes is a new author, never certified
automatically -- refused, naming both keys. The install writes the guest's bundle and a one-actor manifest fragment
under <cell>/store/, adds the fragment to the cluster file's package, and the next `potluck apply` sends it -- like
every other screen, a cluster file underneath. Nothing here runs the module: the boards do, in the sandbox, after
checking the author's signature again.
"""

from __future__ import annotations

import hashlib
import json
import re
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from . import ed25519_ref as ed
from . import guest as gu

APP_FILE = "potluck-app.json"
APP_TYPE = "potluck-app-v1"
REPO = re.compile(r"^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$")
REF = re.compile(r"^[A-Za-z0-9_./-]{1,100}$")
NAME = re.compile(r"^[a-z][a-z0-9_]{0,30}$")
MODULE_MAX = 256 * 1024
REGISTRY = "store.json"
FIRST_AUTHOR = 100  # store authors from 100 up; numbers below are the owner's to hand out by hand


class StoreError(Exception):
    pass


@dataclass(frozen=True)
class App:
    name: str
    description: str
    author_key: bytes
    module_path: str
    sha256: str
    sig: bytes
    inputs: tuple[dict, ...]
    outputs: str
    fuel: int
    period_ms: int


def parse_app(doc: object, where: str = APP_FILE) -> App:
    if not isinstance(doc, dict) or doc.get("type") != APP_TYPE:
        raise StoreError(f"{where}: not a Potluck app (\"type\": \"{APP_TYPE}\")")
    name = doc.get("name")
    if not isinstance(name, str) or not NAME.match(name):
        raise StoreError(f"{where}: 'name' is lowercase letters, digits and '_', starting with a letter, at most 31")
    try:
        key = bytes.fromhex(str(doc.get("author_key", "")))
        sig = bytes.fromhex(str(doc.get("sig", "")))
    except ValueError:
        raise StoreError(f"{where}: 'author_key' and 'sig' are hex") from None
    if len(key) != 32 or len(sig) != 64:
        raise StoreError(f"{where}: 'author_key' is 32 bytes and 'sig' 64, in hex")
    sha = str(doc.get("sha256", ""))
    if not re.fullmatch(r"[0-9a-f]{64}", sha):
        raise StoreError(f"{where}: 'sha256' is the module's SHA-256, 64 lowercase hex digits")
    mod = doc.get("module")
    if not isinstance(mod, str) or not mod.endswith(".wasm") or ".." in mod.split("/") or mod.startswith("/"):
        raise StoreError(f"{where}: 'module' is the path of a .wasm inside the repository")
    inputs = doc.get("inputs", [])
    if not isinstance(inputs, list) or len(inputs) > gu.MAX_INPUTS or not all(
            isinstance(i, dict) and isinstance(i.get("name"), str) and i.get("type") in gu.TYPES for i in inputs):
        raise StoreError(f"{where}: 'inputs' is at most {gu.MAX_INPUTS} of {{\"name\", \"type\": "
                         f"{' | '.join(gu.TYPES)}, \"unit\"?}}")
    outs = doc.get("outputs", "")
    if not isinstance(outs, str):
        raise StoreError(f"{where}: 'outputs' is \"name:type ...\"")
    try:
        gu.parse_outputs(outs, where)
    except gu.GuestError as exc:
        raise StoreError(str(exc)) from None
    fuel, period = doc.get("fuel", 100_000), doc.get("period_ms", 1000)
    if not isinstance(fuel, int) or not gu.FUEL_MIN <= fuel <= gu.FUEL_MAX:
        raise StoreError(f"{where}: 'fuel' is {gu.FUEL_MIN}..{gu.FUEL_MAX}")
    if not isinstance(period, int) or not 100 <= period <= 3_600_000:
        raise StoreError(f"{where}: 'period_ms' is 100..3600000")
    desc = doc.get("description", "")
    return App(name, desc[:200] if isinstance(desc, str) else "", key, mod, sha, sig,
               tuple({"name": i["name"][:40], "type": i["type"], "unit": str(i.get("unit", ""))[:20]} for i in inputs),
               outs, fuel, period)


def verify_module(app: App, module: bytes) -> None:
    """The bytes are the module the app names, and its author signed them -- before the cell certifies anyone."""
    if len(module) > MODULE_MAX:
        raise StoreError(f"{app.name}: the module is {len(module)} B, more than {MODULE_MAX}")
    if not module.startswith(gu.WASM_HEADER):
        raise StoreError(f"{app.name}: the module is not WebAssembly")
    if hashlib.sha256(module).hexdigest() != app.sha256:
        raise StoreError(f"{app.name}: the module's SHA-256 is not the one potluck-app.json names")
    if not ed.verify(app.author_key, gu.signed_message(module), app.sig):
        raise StoreError(f"{app.name}: the author's signature over the module does not verify")


def github_fetcher(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "potluck-store"})
    with urllib.request.urlopen(req, timeout=15) as r:  # noqa: S310 -- https only, built from checked parts
        return r.read(MODULE_MAX + 1)


def fetch(repo: str, ref: str = "HEAD", fetcher: Callable[[str], bytes] = github_fetcher) -> tuple[App, bytes]:
    """potluck-app.json and its module from a GitHub repository, checked. The one place the hub reaches outside."""
    if not REPO.match(repo) or not REF.match(ref) or ".." in ref:
        raise StoreError("a repository is owner/name, and a ref a branch, tag or commit")
    base = f"https://raw.githubusercontent.com/{repo}/{ref}/"
    try:
        doc = json.loads(fetcher(base + APP_FILE))
    except (OSError, ValueError) as exc:
        raise StoreError(f"{repo}: no {APP_FILE} at {ref} ({exc})") from None
    app = parse_app(doc, f"{repo}/{APP_FILE}")
    try:
        module = fetcher(base + app.module_path)
    except OSError as exc:
        raise StoreError(f"{repo}: {app.module_path} could not be fetched ({exc})") from None
    verify_module(app, module)
    return app, module


# ---- the cell's side ---------------------------------------------------------------------------------------------

def load_registry(cell: Path) -> dict:
    """<cell>/store.json: {"on": the owner switched the store on, "authors": {repo: {key, id}}, "apps": {name: ...}}."""
    p = cell / REGISTRY
    if not p.exists():
        return {"on": False, "authors": {}, "apps": {}}
    doc = json.loads(p.read_text(encoding="utf-8"))
    doc.setdefault("on", False)
    doc.setdefault("authors", {})
    doc.setdefault("apps", {})
    return doc


def set_on(cell: Path, on: bool) -> None:
    """The store is off until the owner turns it on (M20: optional, and a security liability -- it fetches code)."""
    reg = load_registry(cell)
    reg["on"] = bool(on)
    (cell / REGISTRY).write_text(json.dumps(reg, indent=2) + "\n", encoding="utf-8")


def candidates(app: App, resources: list[dict]) -> list[list[str]]:
    """Per input, the cell's resources of the same value type it could be bound to. The app's unit is shown to the
    owner beside the choice, not matched: units are spelled differently on each side ("celsius", "C")."""
    return [sorted(r["path"] for r in resources if r.get("type") == i["type"]) for i in app.inputs]


def pin_author(reg: dict, repo: str, key: bytes) -> tuple[int, bool]:
    """(author number, new): the repository's pinned author, or the next number for a repository seen first now."""
    have = reg["authors"].get(repo)
    if have is not None:
        if have["key"] != key.hex():
            raise StoreError(f"{repo}: its author key changed ({have['key'][:16]}... pinned, {key.hex()[:16]}... now) -- "
                             "a new author, never certified automatically; remove the app and install it again if you "
                             "trust the new key")
        return int(have["id"]), False
    used = [int(a["id"]) for a in reg["authors"].values()]
    n = max([FIRST_AUTHOR - 1] + used) + 1
    reg["authors"][repo] = {"key": key.hex(), "id": n}
    return n, True


def install(cluster_file: str | Path, repo: str, ref: str, app: App, module: bytes, bindings: list[str],
            certify: Callable[[int, bytes], bytes]) -> dict:
    """Write the app into the cell: its bundle and one-actor fragment under <cell>/store/, the fragment in the cluster
    file's package. `bindings`: one resource path per input, in order. `certify(author_id, author_key)` returns the
    role-3 certificate (the CA's signature -- the vault, unlocked for this one action). Nothing is sent: apply does."""
    from . import apply as ap
    verify_module(app, module)
    if len(bindings) != len(app.inputs) or not all(isinstance(b, str) and b.startswith("potluck://") for b in bindings):
        raise StoreError(f"{app.name}: bind each of its {len(app.inputs)} input(s) to one of the cell's resources")
    cf = Path(cluster_file)
    c = ap.load_cluster(str(cf))
    cell = cf.parent
    reg = load_registry(cell)
    if app.name in reg["apps"] and reg["apps"][app.name]["repo"] != repo:
        raise StoreError(f"{app.name}: another app of that name is installed, from {reg['apps'][app.name]['repo']}")
    author, _ = pin_author(reg, repo, app.author_key)
    cert = certify(author, app.author_key)
    bundle = gu.Bundle(app.name, module, cert, app.sig)
    store = cell / "store"
    store.mkdir(exist_ok=True)
    (store / f"{app.name}.guest.json").write_text(json.dumps(gu.bundle_to_dict(bundle), indent=1) + "\n",
                                                  encoding="utf-8")
    frag = {"schema": 1, "system": c.system, "min_core_version": 1, "comment": f"store app {app.name} from {repo}",
            "actors": [{"name": app.name, "module": f"guest:{app.name}", "latency_class": 4, "needs": bindings,
                        "config": {"bundle": f"store/{app.name}.guest.json", "sha256": app.sha256,
                                   "outputs": app.outputs, "fuel": app.fuel, "memory_kib": 64,
                                   "period_ms": app.period_ms}}]}
    (store / f"{app.name}.json").write_text(json.dumps(frag, indent=2) + "\n", encoding="utf-8")
    entry = f"store/{app.name}.json"
    doc = json.loads(cf.read_text(encoding="utf-8"))
    pkg = doc.setdefault("package", {})
    mans = pkg.setdefault("manifests", [])
    if not any((m.get("manifest") if isinstance(m, dict) else m) == entry for m in mans):
        mans.append({"manifest": entry, "root": "."})
    _write_cluster(cf, doc)
    reg["apps"][app.name] = {"repo": repo, "ref": ref, "sha256": app.sha256, "author": author}
    (cell / REGISTRY).write_text(json.dumps(reg, indent=2) + "\n", encoding="utf-8")
    return {"name": app.name, "author": author, "fragment": entry}


def remove(cluster_file: str | Path, name: str) -> None:
    """Take a store app out of the cluster file's package (its files stay; the author stays pinned). Apply sends it."""
    cf = Path(cluster_file)
    reg = load_registry(cf.parent)
    if name not in reg["apps"]:
        raise StoreError(f"{name}: not a store app of this cell")
    doc = json.loads(cf.read_text(encoding="utf-8"))
    mans = doc.get("package", {}).get("manifests", [])
    entry = f"store/{name}.json"
    doc.setdefault("package", {})["manifests"] = [m for m in mans
                                                  if (m.get("manifest") if isinstance(m, dict) else m) != entry]
    _write_cluster(cf, doc)
    del reg["apps"][name]
    (cf.parent / REGISTRY).write_text(json.dumps(reg, indent=2) + "\n", encoding="utf-8")


def _write_cluster(cf: Path, doc: dict) -> None:
    """The cluster file, checked by apply's own loader before it replaces the old one (kept as <file>.bak)."""
    from . import apply as ap
    tmp = cf.with_suffix(cf.suffix + ".new")
    tmp.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    try:
        ap.load_cluster(str(tmp))
    except ap.ApplyError as exc:
        tmp.unlink()
        raise StoreError(f"the cluster file would not load: {exc}") from None
    bak = cf.with_suffix(cf.suffix + ".bak")
    bak.write_bytes(cf.read_bytes())
    tmp.replace(cf)
