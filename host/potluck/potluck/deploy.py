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

from .manifest import Manifest, outside_rules
from .paths import path_hash

IMAGE_MAGIC = 0x44544F50  # "POTD"
IMAGE_VERSION = 1
IMAGE_VERSION_GUESTS = 2  # M7: version 1 plus a guest section
IMAGE_VERSION_OUTSIDE = 3  # M13: version 2 plus the outside section
IMAGE_VERSION_BROKER_CA = 4  # M13: version 3 plus the broker's CA, for a presenter with TLS
IMAGE_VERSION_CELLS = 5  # M19: version 4 (CA possibly empty) plus the scope section -- a tree's package, per cell
MAX_BROKER_CA = 1024  # deploy.hpp's kMaxBrokerCa (DER)
IMAGE_HEADER_LEN = 24
#: deploy.hpp's kMaxActors on a board with PSRAM (every board `apply` builds): the declarations a board keeps. 16 on
#: one without. M19: a version-5 image lists up to MAX_TREE_ACTORS, and each board keeps its own cells' (scope_of).
MAX_ACTORS = 64
MAX_TREE_ACTORS = 255
MAX_IMAGE_LEN = 512
#: M7: deploy.hpp's kMaxImageLen. Only a firmware built with the sandbox (CONFIG_POT_WASM) accepts an
#: image this large; any other refuses it as TOO_LARGE, before writing anything.
MAX_IMAGE_LEN_GUESTS = 32 * 1024
MAX_GUESTS = 4
#: M13: a firmware with the presenting module receives a signed stream of up to 2048 B (m0_main.cpp's kImageBufLen),
#: room for the outside section's names and a broker CA; the image is that less its signature trailer.
MAX_IMAGE_LEN_PRESENT = 2048 - 176
#: M19/M20: a board with PSRAM receives a signed stream of up to kMaxImageLen (32 KB, the buffer is in PSRAM) -- a
#: cell of twenty boards with their positions, or a tree's package. A board without PSRAM refuses more than 512 B
#: (2048 with the presenting module) at BEGIN, before it writes anything.
MAX_IMAGE_LEN_PSRAM = 32 * 1024 - 176
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
ACTOR_CLUSTER = 9  # M12: the cluster's name, every node; added by compile_image when `system` is not "lab"
ACTOR_PRESENT = 10  # M13: the presenting role -- portable, needs a connected board (pot/present.hpp)
PRESENT_TLS = 0x01  # M13: deploy.hpp's kPresentTls -- the presenter connects with TLS, against the image's CA
ACTOR_LED_SWITCH = 11  # M13: the status LED as a writable resource, node-<id>/led/on (pot/led_switch.hpp)
ACTOR_POSITION = 12  # M20: where a board sits on the cell's floor plan, node-<id>/sys/pos/{x,y} (pot/position.hpp)
ACTOR_FOREMAN = 13  # M19: a cell's foreman role -- portable among its foreman-eligible boards (pot/foreman.hpp)
CLUSTER_NAME_MAX = 31
OUTSIDE_NAME_MAX = 40  # M13: deploy.hpp's kMaxOutsideName
#: M8.2: pot/actor.hpp's kMaxActorCfg -- the most config bytes a node keeps for one actor.
MAX_ACTOR_CFG = 48
BUILTINS = {"builtin:led": ACTOR_LED, "builtin:fault": ACTOR_FAULT, "builtin:ticker": ACTOR_TICKER,
            "builtin:svc_client": ACTOR_SVC_CLIENT, "builtin:die_temp": ACTOR_DIE_TEMP,
            "builtin:mc_lender": ACTOR_MC_LENDER, "builtin:mc_job": ACTOR_MC_JOB,
            "builtin:present": ACTOR_PRESENT, "builtin:led_switch": ACTOR_LED_SWITCH,
            "builtin:position": ACTOR_POSITION}


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


def led_switch_path(system: str, node_id: int) -> str:
    """M13. The writable resource a led_switch actor declares on its board."""
    return f"potluck://{system}/node-{node_id:04x}/led/on"


#: M20: the floor plan's extent a position may take, metres from its top-left corner (x right, y down).
POSITION_MAX_M = 1000.0


def position_paths(system: str, node_id: int) -> tuple[str, str]:
    """M20. Where a board's position on the floor plan lives: its own two entries, x and y in metres from the
    plan's top-left corner, x to the right and y down -- an app reads them through the cell, and they come with
    the signed package, so a cell with no PC keeps them."""
    base = f"potluck://{system}/node-{node_id:04x}/sys/pos"
    return f"{base}/x", f"{base}/y"


def room_path(system: str, node_id: int) -> str:
    """M20: a board's room as a number -- the rooms the cluster file names, sorted by name and numbered from 1 -- so an
    app can tell which boards share a room (passive-sensor's same-room links). Declared only when the board has one."""
    return f"potluck://{system}/node-{node_id:04x}/sys/room"


def near_path(system: str, node_id: int) -> str:
    """M20: the boards nearest this one in its room (Bytes: up to four u16 ids, nearest first), as `apply` works them
    out when an app asks ("peers": {"nearest": K}) -- the board's own entry, read locally by its apps."""
    return f"potluck://{system}/node-{node_id:04x}/sys/near"


def near_ids(text: str) -> list[int]:
    """The "near" config value: hex node ids, comma-separated ("7368,8160")."""
    return [int(t, 16) for t in text.split(",") if t]


def _position_config(m: Manifest, a, where: str) -> bytes:
    """x_hash, y_hash, room_hash u32; x, y f32 (-1 = not placed); room u32 (0 = none); near_hash u32, near u16[4]
    (0-padded) -- pot/position.hpp."""
    extra = set(a.config) - {"x", "y", "room", "near", "cells", "cell_windows"}
    if extra:
        raise DeployError(f"{where}: unknown position config key(s) {sorted(extra)}")
    if a.pin is None:
        raise DeployError(f"{where}: a position is a board's own, so it is pinned")
    placed = "x" in a.config or "y" in a.config
    try:
        x, y = (float(a.config["x"]), float(a.config["y"])) if placed else (-1.0, -1.0)
        room = int(a.config.get("room", 0))
    except (KeyError, TypeError, ValueError):
        raise DeployError(f"{where}: a position needs numbers x and y (metres), a room a number") from None
    if placed and not (0.0 <= x <= POSITION_MAX_M and 0.0 <= y <= POSITION_MAX_M):
        raise DeployError(f"{where}: position ({x}, {y}) is outside 0..{POSITION_MAX_M:g} m")
    try:
        near = near_ids(str(a.config.get("near", "")))
    except ValueError:
        raise DeployError(f"{where}: near is hex node ids, comma-separated") from None
    if len(near) > 4 or any(not 0x0001 <= n <= 0xFFFE or n == a.pin for n in near) or len(set(near)) != len(near):
        raise DeployError(f"{where}: near names up to four other boards, each once")
    try:
        cells = [int(t) for t in str(a.config.get("cells", "")).split(",") if t]
    except ValueError:
        raise DeployError(f"{where}: cells is cell numbers, comma-separated") from None
    if len(cells) > 2 or any(not 1 <= c <= 255 for c in cells) or len(set(cells)) != len(cells):
        raise DeployError(f"{where}: a board belongs to at most two cells, numbered 1..255")
    try:
        windows = [int(t) for t in str(a.config.get("cell_windows", "")).split(",") if t]
    except ValueError:
        raise DeployError(f"{where}: cell_windows is milliseconds, comma-separated") from None
    windows += [0] * (len(cells) - len(windows))
    if len(windows) != len(cells) or any(w % 100 or not 0 <= w <= 25500 for w in windows):
        raise DeployError(f"{where}: a cell's window is 0..25500 ms in 100 ms steps, one per cell")
    if not 0 <= room <= 0xFFFF or (not placed and room == 0 and not near and not cells):
        raise DeployError(f"{where}: a position actor needs a position, a room (1..65535), its nearest boards or a cell")
    hx, hy = (path_hash(p) for p in position_paths(m.system, a.pin))
    return struct.pack("<IIIffII4H4B", hx, hy, path_hash(room_path(m.system, a.pin)), x, y, room,
                       path_hash(near_path(m.system, a.pin)), *(near + [0] * (4 - len(near))),
                       *(cells + [0] * (2 - len(cells))), *([w // 100 for w in windows] + [0] * (2 - len(windows))))


def _led_switch_config(m: Manifest, a, where: str) -> bytes:
    extra = set(a.config) - {"r", "g", "b", "blink_ms"}
    if extra:
        raise DeployError(f"{where}: unknown led_switch config key(s) {sorted(extra)}")
    if a.pin is None:
        raise DeployError(f"{where}: a led_switch lights the board it runs on, so it is pinned")
    rgb = [int(a.config.get(k, d)) for k, d in (("r", 255), ("g", 160), ("b", 0))]
    blink = int(a.config.get("blink_ms", 0))
    if any(not 0 <= v <= 255 for v in rgb) or not (blink == 0 or 200 <= blink <= 25500):
        raise DeployError(f"{where}: r, g, b are 0..255 and blink_ms 0 or 200..25500")
    return struct.pack("<IBBBB", path_hash(led_switch_path(m.system, a.pin)), *rgb, blink // 100)


def broker_ca_der(value: str, where: str) -> bytes:
    """M13: the broker's CA certificate as DER, from a PEM certificate or its base64 body (what `potluck apply`
    puts in the manifest from the cluster file's `tls_ca` file). Checked for shape only; the board's TLS library
    judges the certificate itself."""
    import base64
    import binascii

    text = str(value)
    if "-----BEGIN" in text:
        lines = text.strip().splitlines()
        try:
            a = next(i for i, ln in enumerate(lines) if ln.startswith("-----BEGIN CERTIFICATE-----"))
            b = next(i for i, ln in enumerate(lines) if i > a and ln.startswith("-----END CERTIFICATE-----"))
        except StopIteration:
            raise DeployError(f"{where}: tls_ca is PEM but holds no CERTIFICATE block") from None
        text = "".join(lines[a + 1:b])
    try:
        der = base64.b64decode("".join(text.split()), validate=True)
    except (binascii.Error, ValueError):
        raise DeployError(f"{where}: tls_ca is neither a PEM certificate nor base64 DER") from None
    if not der or der[0] != 0x30 or len(der) > MAX_BROKER_CA:
        raise DeployError(f"{where}: tls_ca must be one DER certificate of 1..{MAX_BROKER_CA} bytes "
                          f"(got {len(der)} bytes{'' if der[:1] == b'0' else ', not a DER SEQUENCE'})")
    return der


def _present_config(m: Manifest, a, where: str) -> bytes:
    """M13: the portable header (every board eligible; the reconciler places it only on a connected one), then
    broker a.b.c.d, port, keep-alive, flags. With `tls_ca` (base64 DER, or PEM) the presenter connects with TLS and
    verifies the broker against that CA, which the image carries (version 4)."""
    extra = set(a.config) - {"broker", "keepalive_s", "period_ms", "tls_ca", "hub"}  # hub: who is eligible only
    if extra:
        raise DeployError(f"{where}: unknown present config key(s) {sorted(extra)}")
    broker = str(a.config.get("broker", ""))
    host, _, port = broker.partition(":")
    parts = host.split(".")
    try:
        ip = [int(p) for p in parts]
        port_n = int(port or 1883)
    except ValueError:
        ip, port_n = [], 0
    if len(ip) != 4 or any(not 0 <= v <= 255 for v in ip) or not ip[0] or not 1 <= port_n <= 65535:
        raise DeployError(f"{where}: broker must be an IPv4 address and an optional port, \"192.168.0.77:1883\" "
                          f"(a name needs DNS on the board, which the presenter does not do yet)")
    keepalive = int(a.config.get("keepalive_s", 10))
    if not 5 <= keepalive <= 120:
        raise DeployError(f"{where}: keepalive_s={keepalive} is outside 5..120")
    if a.config.get("hub"):
        from . import reconcile as rc

        if not any(m.node(n) is not None and m.node(n).kind == "host" for n, _ in rc.eligible(m, a)):
            raise DeployError(f"{where}: hub: true needs a placement host in the cell (a manifest node with kind "
                              f"'host' and placement: true) -- the PC's node is what presents")
    hdr = _portable_header(m, a, int(a.config.get("period_ms", 2000)), where)
    flags = PRESENT_TLS if a.config.get("tls_ca") else 0
    own = struct.pack("<4BHBB", *ip, port_n, keepalive, flags)
    if flags:
        # The CA's crc32 rides in the declaration: a package that changes only the CA then changes the presenter too,
        # and a live change restarts it with the new one (deploy.hpp).
        own += struct.pack("<I", crc32(broker_ca_der(a.config["tls_ca"], where)))
    return hdr + own


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
    M7: with guest actors it is a version-2 image, their blobs in its guest section. M13: with outside rules
    (an `outside` map, a safety-relevant resource) a version-3 image, the guest section (maybe empty) and then
    the outside section."""
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
        if a.module in ("builtin:ticker", "builtin:present"):
            # Section 7.7: every node gets it; the reconciler decides where it runs. A pinned ticker
            # is a portable one with a single eligible node.
            node = 0xFFFE
            cfg = _ticker_config(m, a, where) if a.module == "builtin:ticker" else _present_config(m, a, where)
            body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
            count += 1
            continue
        node = m.placement_of(a)
        if node is None:
            raise DeployError(f"{where}: not placed -- pin it, or resolve placement first")
        spec = m.node(node)
        if spec is not None and spec.kind == "host":
            raise DeployError(f"{where}: placed on '{spec.label}', a host: built-in actors run on firmware")
        if a.module == "builtin:led_switch":
            cfg = _led_switch_config(m, a, where)
            body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
            count += 1
            continue
        if a.module == "builtin:position":
            cfg = _position_config(m, a, where)
            body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
            count += 1
            continue
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
    if m.system != "lab":
        # M12: the boards name their built-in resources under the cluster (potluck://<system>/node-...).
        # A package for "lab" carries nothing extra, so it is byte for byte what it was before M12.
        name = m.system.encode("ascii", "replace")
        if not 1 <= len(name) <= CLUSTER_NAME_MAX or any(c < 0x21 or c > 0x7E or c == 0x2F for c in name):
            raise DeployError(f"system '{m.system}': a cluster name is 1..{CLUSTER_NAME_MAX} printable ASCII "
                              f"characters, without spaces or '/'")
        body += struct.pack("<HBB", 0xFFFF, ACTOR_CLUSTER, len(name)) + name
        count += 1
    scope = scope_of(body, count)
    if count > (MAX_TREE_ACTORS if scope is not None else MAX_ACTORS):
        raise DeployError(f"{count} actors; the node image holds at most "
                          f"{MAX_TREE_ACTORS if scope is not None else MAX_ACTORS}")
    if scope is not None:
        for nid, kept in kept_per_board(body, count, scope).items():
            if kept > MAX_ACTORS:
                raise DeployError(f"node 0x{nid:04x} would keep {kept} actors of its cells; a board keeps at most "
                                  f"{MAX_ACTORS}")
    digest = bytes.fromhex(m.digest())[:8]
    rules = outside_rules(m)
    cas = {broker_ca_der(a.config["tls_ca"], f"actors[{a.name}]") for a in m.actors
           if a.module == "builtin:present" and a.config.get("tls_ca")}
    if len(cas) > 1:
        raise DeployError("two presenters pin different broker CAs; an image carries one")
    ca = cas.pop() if cas else b""
    version = (IMAGE_VERSION_CELLS if scope is not None else IMAGE_VERSION_BROKER_CA if ca else
               IMAGE_VERSION_OUTSIDE if rules else IMAGE_VERSION_GUESTS if guests else IMAGE_VERSION)
    header = struct.pack("<IBBHI8sI", IMAGE_MAGIC, version, count, 0, counter, digest, len(body))
    img = header + body
    if guests or rules or ca or scope is not None:
        img += bytes([len(guests)])
        for g in guests:
            img += struct.pack("<I", len(g)) + g
    if rules or ca or scope is not None:
        img += bytes([len(rules)])
        prefix = f"potluck://{m.system}/"
        for path, flags in rules:
            name = path[len(prefix):] if path.startswith(prefix) else path
            raw = name.encode("ascii", "replace")
            if not 1 <= len(raw) <= OUTSIDE_NAME_MAX or any(c < 0x21 or c > 0x7E or c in b'"\\' for c in raw):
                raise DeployError(f"outside: '{name}' -- the name a board calls a resource outside the cell is the path "
                                  f"after {prefix}, 1..{OUTSIDE_NAME_MAX} printable ASCII characters, no quotes")
            img += struct.pack("<IBB", path_hash(path), flags, len(raw)) + raw
    if ca or scope is not None:
        img += struct.pack("<H", len(ca)) + ca
    if scope is not None:
        img += bytes([count]) + b"".join(bytes(p) for p in scope)
    limit = MAX_IMAGE_LEN_GUESTS if guests else MAX_IMAGE_LEN_PSRAM
    if len(img) > limit:
        raise DeployError(f"image is {len(img)} bytes; the node accepts at most {limit}")
    return img


def _decls(body: bytes, count: int) -> list[tuple[int, int, bytes]]:
    out, at = [], 0
    for _ in range(count):
        node, typ, ln = struct.unpack_from("<HBB", body, at)
        out.append((node, typ, body[at + 4:at + 4 + ln]))
        at += 4 + ln
    return out


def _eligible_ids(cfg: bytes) -> list[int]:
    return [struct.unpack_from("<H", cfg, 7 + 3 * i)[0] for i in range(cfg[6])]


def board_cells(body: bytes, count: int) -> dict[int, tuple[int, ...]]:
    """M19: each board's cells, as its own position actor names them (config bytes 36 and 37)."""
    out = {}
    for node, typ, cfg in _decls(body, count):
        if typ == ACTOR_POSITION and len(cfg) == 40 and cfg[36]:
            out[node] = tuple(c for c in cfg[36:38] if c)
    return out


def scope_of(body: bytes, count: int) -> list[tuple[int, int]] | None:
    """M19 (Review of M19 #3): each actor's (cell, also) -- the scope section of a version-5 image -- or None for a
    package with no cells. Pinned: its board's cells. Every node: 0 (every cell). Portable: the one cell all its
    eligible boards share; a foreman role's second is its cell's parent, where its summary is read."""
    cells = board_cells(body, count)
    if not cells:
        return None
    out = []
    for k, (node, typ, cfg) in enumerate(_decls(body, count)):
        if node == EVERY_NODE:
            out.append((0, 0))
        elif node == 0xFFFE:
            common = None
            for e in _eligible_ids(cfg):
                if e in cells:
                    common = set(cells[e]) if common is None else common & set(cells[e])
            if common is not None and not common:
                raise DeployError(f"actor #{k} (type {typ}) is portable among boards of different cells -- a portable "
                                  f"actor lives in one cell (Review of M19 #3): name its cell")
            also = cfg[7 + 3 * cfg[6] + 1] if typ == ACTOR_FOREMAN else 0
            out.append((min(common) if common else 0, also))
        else:
            c = cells.get(node, ())
            out.append((c[0] if c else 0, c[1] if len(c) > 1 else 0))
    return out


def kept_per_board(body: bytes, count: int, scope: list[tuple[int, int]]) -> dict[int, int]:
    """M19: how many declarations each board keeps of a version-5 image -- parse_image's rule, on the host."""
    decls = _decls(body, count)
    cells = board_cells(body, count)
    out = {}
    for me, own in cells.items():
        mine = set(own)
        for (node, typ, cfg), (cell, also) in zip(decls, scope):
            if node == 0xFFFE and also and cell in mine and me in _eligible_ids(cfg):
                mine.add(also)
        out[me] = sum(1 for (node, _t, _c), (cell, also) in zip(decls, scope)
                      if node == me or cell == 0 or cell in mine or (also and also in mine))
    return out


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


def declarations(img: bytes) -> list[bytes]:
    """M12: each actor's declaration in a node image -- node u16 (little-endian), type u8, length u8, config --
    the bytes a board's registration row is handed, and what a test kit rig takes (Rig::declare_bytes)."""
    _, _, count, _, _, _, body_len = struct.unpack_from("<IBBHI8sI", img)
    out, at = [], IMAGE_HEADER_LEN
    for _ in range(count):
        n = 4 + img[at + 3]
        out.append(img[at:at + n])
        at += n
    return out


def main(argv: list[str] | None = None) -> int:
    """python -m potluck.deploy decl <manifest> [--modules <descriptor>]... [--actor <name>] [--guests <dir>]

    Prints each actor's declaration bytes (hex, and as a C++ array) -- the bytes to test a row with."""
    import argparse
    import sys

    from . import modules as mods
    from .manifest import load

    ap = argparse.ArgumentParser(prog="potluck.deploy")
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decl", help="each actor's declaration bytes, for a test")
    d.add_argument("manifest")
    d.add_argument("--modules", action="append", default=[])
    d.add_argument("--actor", default=None)
    d.add_argument("--guests", default=None)
    a = ap.parse_args(argv)
    try:
        for path in a.modules:
            mods.load(path)
        m = load(a.manifest)
        decls = declarations(compile_image(m, 1, a.guests))
    except (DeployError, mods.ModuleError, OSError, ValueError) as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2
    names = [x.name for x in m.actors]
    if m.system != "lab":
        names.append("(cluster name)")
    for name, b in zip(names, decls):
        if a.actor and name != a.actor:
            continue
        node, typ, n = struct.unpack_from("<HBB", b)
        print(f"{name}: node 0x{node:04x}, type 0x{typ:02x}, {n} config bytes")
        print(f"  {b.hex()}")
        print("  const uint8_t kDecl[] = {" + ", ".join(f"0x{x:02x}" for x in b) + "};")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
