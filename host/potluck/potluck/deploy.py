"""Section 7.4 deploy, the host's half -- M3.

A signed package (potluck.signing) is verified here, compiled into the node image format
(firmware/components/pot_deploy/include/pot/deploy.hpp), and sent with DEPLOY_BEGIN / CHUNK /
COMMIT. The two formats are kept in step by a shared golden image that both test suites check.

ADR-003 TIER 0. A deployable actor is a built-in native actor named `builtin:<type>` plus its
`config`. Any other module name cannot run on this firmware, and compiling such a package is an
error, not a silent omission: a deployment that quietly dropped an actor would report success for a
system that is not running.

ONE ARTIFACT PER SYSTEM. The host is cabled to one board. With `distribute`, that board commits the
image and passes it on to every peer it can reach, one hop each, before rebooting into it -- the
whole cell from one connection, without v2 frame forwarding.
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from dataclasses import dataclass
from typing import Any

from .manifest import Manifest
from .paths import path_hash

IMAGE_MAGIC = 0x44544F50  # "POTD"
IMAGE_VERSION = 1
IMAGE_VERSION_GUESTS = 2  # M7: version 1 plus a guest section
IMAGE_HEADER_LEN = 24
MAX_ACTORS = 16
MAX_IMAGE_LEN = 512
#: M7: deploy.hpp's kMaxImageLen. Only a firmware built with the sandbox (CONFIG_POT_WASM) accepts an
#: image this large; any other refuses it as TOO_LARGE, before writing anything.
MAX_IMAGE_LEN_GUESTS = 32 * 1024
MAX_GUESTS = 4
EVERY_NODE = 0xFFFF
# 220, not the node's 512: a CHUNK frame is 6 + data bytes, and the node caps a peer it has not pinned
# to ESP-NOW v2 -- the host, on the serial link -- at the 226-byte v1 payload (section 5.3). Images
# under 184 B never noticed; M5's 176-byte signature trailer made the first chunk 230 B and the node
# dropped it unanswered.
CHUNK_MAX = 220
FLAG_DISTRIBUTE = 0x01

ACTOR_LED = 1
ACTOR_FAULT = 2
ACTOR_TICKER = 3  # M6: portable; placed at run time by the reconciler (potluck.reconcile)
ACTOR_SVC_CLIENT = 4  # M8: calls a host's named service and publishes the answer
ACTOR_DIE_TEMP = 5  # M8.1: the chip's own temperature sensor, published to node-<id>/hw/die_temp
ACTOR_MC_LENDER = 6  # M9: lends its node's idle cores to the mc_pi pure function
ACTOR_MC_JOB = 7  # M9: a Monte Carlo job that borrows idle cores; publishes node-<id>/job/mc/pi
ACTOR_GUEST = 8  # M7: a sandboxed WebAssembly module from a guest author (potluck.guest); portable
#: M8.2: pot/actor.hpp's kMaxActorCfg -- the most config bytes a node keeps for one actor.
MAX_ACTOR_CFG = 48
BUILTINS = {"builtin:led": ACTOR_LED, "builtin:fault": ACTOR_FAULT, "builtin:ticker": ACTOR_TICKER,
            "builtin:svc_client": ACTOR_SVC_CLIENT, "builtin:die_temp": ACTOR_DIE_TEMP,
            "builtin:mc_lender": ACTOR_MC_LENDER, "builtin:mc_job": ACTOR_MC_JOB}


def mc_job_path(system: str, node_id: int) -> str:
    """M9. Where an mc_job publishes its estimate of pi when a run ends: under the node that ran it."""
    return f"potluck://{system}/node-{node_id:04x}/job/mc/pi"


def _mc_lender_config(a, where: str) -> bytes:
    extra = set(a.config) - {"slots"}
    if extra:
        raise DeployError(f"{where}: unknown mc_lender config key(s) {sorted(extra)}")
    if a.pin is None:
        raise DeployError(f"{where}: an mc_lender lends the cores of the node it is pinned to")
    slots = int(a.config.get("slots", 2))
    if not 1 <= slots <= 4:
        raise DeployError(f"{where}: mc_lender slots={slots} is outside 1..4")
    return struct.pack("<B", slots)


def _mc_job_config(m: Manifest, a, where: str) -> bytes:
    extra = set(a.config) - {"units", "samples", "seed", "lend", "start_after_ms"}
    if extra:
        raise DeployError(f"{where}: unknown mc_job config key(s) {sorted(extra)}")
    if a.pin is None:
        raise DeployError(f"{where}: an mc_job is pinned: it runs where it is placed, and lends from there")
    units = int(a.config.get("units", 48))
    samples = int(a.config.get("samples", 1000000))
    seed = int(a.config.get("seed", 1))
    lend = a.config.get("lend", True)
    start_after = int(a.config.get("start_after_ms", 0))
    if not 1 <= units <= 4096:
        raise DeployError(f"{where}: mc_job units={units} is outside 1..4096")
    if not 1000 <= samples <= 100_000_000:
        raise DeployError(f"{where}: mc_job samples={samples} is outside 1000..100000000")
    if not 0 <= seed <= 0xFFFFFFFF or not 0 <= start_after <= 0xFFFFFFFF:
        raise DeployError(f"{where}: mc_job seed and start_after_ms are u32")
    if not isinstance(lend, bool):
        raise DeployError(f"{where}: mc_job lend must be true or false")
    return struct.pack("<IHIIBI", path_hash(mc_job_path(m.system, a.pin)), units, samples, seed,
                       1 if lend else 0, start_after)


def die_temp_path(system: str, node_id: int) -> str:
    """M8.1. Where a die_temp actor publishes: under its own node, like sys/uptime, because the reading
    is that chip's and nobody else's."""
    return f"potluck://{system}/node-{node_id:04x}/hw/die_temp"


def _die_temp_config(m: Manifest, a, where: str) -> bytes:
    extra = set(a.config) - {"period_ms"}
    if extra:
        raise DeployError(f"{where}: unknown die_temp config key(s) {sorted(extra)}")
    if a.pin is None:
        raise DeployError(f"{where}: a die_temp reads the chip it runs on, so it is pinned")
    period = int(a.config.get("period_ms", 1000))
    if not 200 <= period <= 60000:
        raise DeployError(f"{where}: die_temp period_ms={period} is outside 200..60000")
    return struct.pack("<IH", path_hash(die_temp_path(m.system, a.pin)), period)
#: Section 8.3's menu, as the node encodes it for a service client. The stop and safe-state entries
#: concern actuators, which a service client never owns.
SVC_HOST_LOSS = {"continue": 0, "hold": 1}

#: pot::DeployStatus, in order.
STATUS_NAMES = ("OK", "TOO_LARGE", "DOWNGRADE", "NOT_STARTED", "BAD_OFFSET", "CRC_MISMATCH",
                "BAD_IMAGE", "TRIAL_IN_PROGRESS", "STORE_FAILED", "MALFORMED", "BUSY", "UNSIGNED",
                "BAD_SIGNATURE")

#: M5 step 6: what an enrolled node checks before it commits. The trailer follows the image on the
#: wire: a 112-byte deploy-key certificate (potluck.enrol, role 2) and the deploy key's Ed25519
#: signature over IMAGE_DOMAIN + SHA-512(image). firmware/components/pot_trust checks the same bytes.
IMAGE_DOMAIN = b"potluck-image-v1\0"
TRAILER_LEN = 176


def image_trailer(img: bytes, deploy_secret: bytes, deploy_cert: bytes) -> bytes:
    from . import ed25519_ref as ed

    if len(deploy_cert) != 112:
        raise DeployError("a deploy certificate is 112 bytes (python -m potluck.enrol --deploy-cert)")
    import hashlib

    return deploy_cert + ed.sign(deploy_secret, IMAGE_DOMAIN + hashlib.sha512(img).digest())

SLOT_NAMES = {0: "none", 1: "A", 2: "B"}


class DeployError(Exception):
    """The package cannot become a node image, or a node refused it."""


def _actor_config(module: str, cfg: dict[str, Any], where: str) -> bytes:
    if module == "builtin:led":
        allowed = {"r", "g", "b", "blink", "period_ms"}
        extra = set(cfg) - allowed
        if extra:
            raise DeployError(f"{where}: unknown led config key(s) {sorted(extra)}")
        r, g, b = (int(cfg.get(k, 0)) for k in ("r", "g", "b"))
        blink = 1 if cfg.get("blink", False) else 0
        period = int(cfg.get("period_ms", 1000))
        for name, v in (("r", r), ("g", g), ("b", b)):
            if not 0 <= v <= 255:
                raise DeployError(f"{where}: led {name}={v} is outside 0..255")
        if not 200 <= period <= 10000:
            raise DeployError(f"{where}: led period_ms={period} is outside 200..10000")
        return struct.pack("<BBBBH", r, g, b, blink, period)
    if module == "builtin:fault":
        extra = set(cfg) - {"panic_after_ms"}
        if extra:
            raise DeployError(f"{where}: unknown fault config key(s) {sorted(extra)}")
        ms = int(cfg.get("panic_after_ms", 0))
        if ms <= 0:
            raise DeployError(f"{where}: fault panic_after_ms must be > 0")
        return struct.pack("<I", ms)
    raise DeployError(f"{where}: module '{module}' is not a built-in actor this firmware has "
                      f"(ADR-003 Tier 0: one of {sorted(BUILTINS)})")


def _portable_header(m: Manifest, a, period: int, where: str) -> bytes:
    """M8.2 (PS-1): the standard portable-actor header every portable type's config starts with --
    out_hash u32, period_ms u16, count u8, count x (node u16, gravity u8). The firmware's
    portable_header() reads it."""
    from . import reconcile as rc

    if not 50 <= period <= 60000:
        raise DeployError(f"{where}: period_ms={period} is outside 50..60000")
    if a.pin is None:
        ok, why = rc.portability(m, a)
        if not ok:
            raise DeployError(f"{where}: a portable actor must be portable or pinned, and this one is neither: {why}")
    nodes = rc.eligible(m, a)
    if not 1 <= len(nodes) <= rc.MAX_ELIGIBLE:
        raise DeployError(f"{where}: {len(nodes)} eligible nodes; the node image holds 1..{rc.MAX_ELIGIBLE}")
    key = path_hash(rc.output_path(m, a))
    cfg = struct.pack("<IHB", key, period, len(nodes))
    for node_id, gravity in nodes:
        cfg += struct.pack("<HB", node_id, gravity)
    return cfg


def _ticker_config(m: Manifest, a, where: str) -> bytes:
    extra = set(a.config) - {"period_ms"}
    if extra:
        raise DeployError(f"{where}: unknown ticker config key(s) {sorted(extra)}")
    return _portable_header(m, a, int(a.config.get("period_ms", 100)), where)


def _external(m: Manifest, a, where: str) -> tuple[int, int, bytes]:
    """M8.2 (PS-1): (node, type, config) for a module an external component describes."""
    from . import modules as mods

    spec = mods.get(a.module)
    try:
        own = mods.encode_config(spec, a.config, m, where)
    except mods.ModuleError as e:
        raise DeployError(str(e)) from None
    if spec.placement == "portable":
        return 0xFFFE, spec.type, _portable_header(m, a, int(a.config.get("period_ms", 1000)), where) + own
    if spec.placement == "every":
        if a.pin is not None:
            raise DeployError(f"{where}: {a.module} runs on every node; it takes no pin")
        return 0xFFFF, spec.type, own
    node = m.placement_of(a)
    if node is None:
        raise DeployError(f"{where}: {a.module} is pinned: give it a pin")
    return node, spec.type, own


def guest_output_path(system: str, actor: str, name: str) -> str:
    """M7. Where a guest actor's output `name` lives: under its actor, node-free, like a ticker's."""
    return f"potluck://{system}/act/{actor}/{name}"


GUEST_CONFIG_KEYS = {"bundle", "sha256", "outputs", "fuel", "memory_kib", "period_ms"}


def _guest(m: Manifest, a, where: str, index: int, guest_root: str | None) -> tuple[bytes, bytes]:
    """M7: (config, blob) for a `guest:<name>` actor. The bundle is read from `config.bundle` (relative
    to `guest_root`, else the working directory) and must be the module `config.sha256` pins."""
    import os

    from . import guest as gs

    extra = set(a.config) - GUEST_CONFIG_KEYS
    if extra:
        raise DeployError(f"{where}: unknown guest config key(s) {sorted(extra)}")
    for k in ("bundle", "sha256", "outputs"):
        if not isinstance(a.config.get(k), str):
            raise DeployError(f"{where}: a guest needs config.{k}")
    path = a.config["bundle"]
    if guest_root and not os.path.isabs(path):
        path = os.path.join(guest_root, path)
    try:
        b = gs.load_bundle(path)
        outs = gs.parse_outputs(a.config["outputs"], where)
    except (gs.GuestError, OSError, ValueError) as exc:
        raise DeployError(f"{where}: {exc}") from None
    if a.module != f"guest:{b.name}":
        raise DeployError(f"{where}: module '{a.module}' but the bundle is guest:{b.name}")
    if b.sha256 != a.config["sha256"].lower():
        raise DeployError(f"{where}: the bundle's module (sha256 {b.sha256[:16]}...) is not the one the "
                          f"signed manifest pins ({a.config['sha256'][:16]}...)")
    fuel = int(a.config.get("fuel", 100000))
    kib = int(a.config.get("memory_kib", 64))
    if kib not in (0, 64):
        raise DeployError(f"{where}: memory_kib is 0 or 64 (one WebAssembly page)")
    period = int(a.config.get("period_ms", 1000))
    cfg = _portable_header(m, a, period, where) + bytes([index])
    inputs = [path_hash(m.bindings.get(p, p)) for p in a.needs]
    outputs = [(path_hash(guest_output_path(m.system, a.name, n)), t) for n, t in outs]
    try:
        blob = gs.encode_blob(b, inputs, outputs, fuel, kib // 64)
    except gs.GuestError as exc:
        raise DeployError(f"{where}: {exc}") from None
    return cfg, blob


def check_guest_authors(m: Manifest, ca_public: bytes, guest_root: str | None = None) -> list[tuple[str, int]]:
    """M7: [(actor, author)] for every guest, each author signature verified under the CA -- the check
    each node makes before it commits the image, made first on the host so a bad bundle never leaves."""
    import os

    from . import guest as gs

    out = []
    for a in m.actors:
        if not a.module.startswith("guest:"):
            continue
        path = str(a.config.get("bundle", ""))
        if guest_root and not os.path.isabs(path):
            path = os.path.join(guest_root, path)
        try:
            out.append((a.name, gs.verify_bundle(gs.load_bundle(path), ca_public)))
        except (gs.GuestError, OSError, ValueError) as exc:
            raise DeployError(f"actors[{a.name}]: {exc}") from None
    return out


def _svc_client_config(m: Manifest, a, where: str) -> bytes:
    """M8. The service is the actor's one binding; the provider is the node that owns it (a host)."""
    from . import reconcile as rc

    extra = set(a.config) - {"period_ms"}
    if extra:
        raise DeployError(f"{where}: unknown svc_client config key(s) {sorted(extra)}")
    if a.pin is None:
        raise DeployError(f"{where}: a svc_client is pinned to the node that consumes the service")
    if len(a.needs) != 1 or "/svc/" not in a.needs[0]:
        raise DeployError(f"{where}: a svc_client binds exactly one potluck://<cluster>/svc/<name> path")
    provider = m.owner_of(a.needs[0])
    if provider is None:
        raise DeployError(f"{where}: no node in the package owns {a.needs[0]}")
    period = int(a.config.get("period_ms", 1000))
    if not 100 <= period <= 60000:
        raise DeployError(f"{where}: svc_client period_ms={period} is outside 100..60000")
    loss = a.on_host_loss or "hold"  # section 8.3's default for what a host feeds
    if loss not in SVC_HOST_LOSS:
        raise DeployError(f"{where}: on_host_loss '{loss}' is for actuators; a svc_client takes "
                          f"{sorted(SVC_HOST_LOSS)}")
    svc = path_hash(m.bindings.get(a.needs[0], a.needs[0]))
    out = path_hash(rc.output_path(m, a))
    return struct.pack("<IHIHB", svc, provider.node_id, out, period, SVC_HOST_LOSS[loss])


def compile_image(m: Manifest, counter: int, guest_root: str | None = None) -> bytes:
    """The node image for a manifest at a rollback counter. Deterministic: same input, same bytes.
    M7: with guest actors it is a version-2 image, their blobs in its guest section."""
    body = b""
    count = 0
    guests: list[bytes] = []
    from . import modules as mods

    for a in m.actors:
        where = f"actors[{a.name}]"
        if a.module.startswith("guest:"):
            if len(guests) >= MAX_GUESTS:
                raise DeployError(f"{where}: an image carries at most {MAX_GUESTS} guests")
            cfg, blob = _guest(m, a, where, len(guests), guest_root)
            guests.append(blob)
            body += struct.pack("<HBB", 0xFFFE, ACTOR_GUEST, len(cfg)) + cfg
            count += 1
            continue
        if mods.get(a.module) is not None:
            node, typ, cfg = _external(m, a, where)
            if len(cfg) > MAX_ACTOR_CFG:
                raise DeployError(f"{where}: {len(cfg)} bytes of config; a node keeps at most {MAX_ACTOR_CFG}")
            body += struct.pack("<HBB", node, typ, len(cfg)) + cfg
            count += 1
            continue
        if a.module not in BUILTINS:
            raise DeployError(f"{where}: module '{a.module}' is not a built-in actor this firmware "
                              f"has (ADR-003 Tier 0: one of {sorted(BUILTINS)}), and no --modules "
                              f"descriptor describes it")
        if a.module == "builtin:ticker":
            # Section 7.7: every node gets it; the reconciler decides where it runs. A pinned ticker
            # is a portable one with a single eligible node.
            node = 0xFFFE
            cfg = _ticker_config(m, a, where)
            body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
            count += 1
            continue
        node = m.placement_of(a)
        if node is None:
            raise DeployError(f"{where}: not placed -- pin it, or resolve placement first")
        spec = m.node(node)
        if spec is not None and spec.kind == "host":
            raise DeployError(f"{where}: placed on '{spec.label}', a host: built-in actors run on firmware")
        if a.module == "builtin:die_temp":
            cfg = _die_temp_config(m, a, where)
            body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
            count += 1
            continue
        if a.module in ("builtin:mc_lender", "builtin:mc_job"):
            cfg = _mc_lender_config(a, where) if a.module == "builtin:mc_lender" else _mc_job_config(m, a, where)
            body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
            count += 1
            continue
        if a.module == "builtin:svc_client":
            cfg = _svc_client_config(m, a, where)
            body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
            count += 1
            continue
        cfg = _actor_config(a.module, a.config, where)
        body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
        count += 1
    if count > MAX_ACTORS:
        raise DeployError(f"{count} actors; the node image holds at most {MAX_ACTORS}")
    digest = bytes.fromhex(m.digest())[:8]
    version = IMAGE_VERSION_GUESTS if guests else IMAGE_VERSION
    header = struct.pack("<IBBHI8sI", IMAGE_MAGIC, version, count, 0, counter, digest, len(body))
    img = header + body
    if guests:
        img += bytes([len(guests)])
        for g in guests:
            img += struct.pack("<I", len(g)) + g
    limit = MAX_IMAGE_LEN_GUESTS if guests else MAX_IMAGE_LEN
    if len(img) > limit:
        raise DeployError(f"image is {len(img)} bytes; the node accepts at most {limit}")
    return img


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def begin_payload(img: bytes, counter: int, digest8: bytes, *, distribute: bool) -> bytes:
    return struct.pack("<III8sB3x", len(img), crc32(img), counter, digest8[:8].ljust(8, b"\0"),
                       FLAG_DISTRIBUTE if distribute else 0)


def chunk_payload(offset: int, data: bytes) -> bytes:
    if len(data) > CHUNK_MAX:
        raise DeployError(f"chunk of {len(data)} bytes exceeds {CHUNK_MAX}")
    return struct.pack("<IH", offset, len(data)) + data


def commit_payload(img: bytes) -> bytes:
    return struct.pack("<I", crc32(img))


@dataclass(frozen=True)
class DeployReply:
    status: int
    slot: int
    pending: int
    received: int
    counter: int
    peers_ok: int
    peers_failed: int

    @property
    def ok(self) -> bool:
        return self.status == 0

    @property
    def status_name(self) -> str:
        return STATUS_NAMES[self.status] if self.status < len(STATUS_NAMES) else f"status {self.status}"

    @classmethod
    def parse(cls, raw: bytes) -> "DeployReply":
        if len(raw) < 16:
            raise DeployError(f"deploy reply is {len(raw)} bytes, expected 16")
        status, slot, pending, received, counter, ok, failed = struct.unpack_from("<HBBIIBB", raw)
        return cls(status, slot, pending, received, counter, ok, failed)


def package_digest8(m: Manifest) -> bytes:
    return bytes.fromhex(m.digest())[:8]


def manifest_digest(m: Manifest) -> str:
    return hashlib.sha256(m.canonical_bytes()).hexdigest()
