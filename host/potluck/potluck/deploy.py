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
IMAGE_HEADER_LEN = 24
MAX_ACTORS = 16
MAX_IMAGE_LEN = 512
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
BUILTINS = {"builtin:led": ACTOR_LED, "builtin:fault": ACTOR_FAULT, "builtin:ticker": ACTOR_TICKER}

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


def _ticker_config(m: Manifest, a, where: str) -> bytes:
    from . import reconcile as rc

    extra = set(a.config) - {"period_ms"}
    if extra:
        raise DeployError(f"{where}: unknown ticker config key(s) {sorted(extra)}")
    period = int(a.config.get("period_ms", 100))
    if not 50 <= period <= 60000:
        raise DeployError(f"{where}: ticker period_ms={period} is outside 50..60000")
    if a.pin is None:
        ok, why = rc.portability(m, a)
        if not ok:
            raise DeployError(f"{where}: a ticker must be portable or pinned, and this one is neither: {why}")
    nodes = rc.eligible(m, a)
    if not 1 <= len(nodes) <= rc.MAX_ELIGIBLE:
        raise DeployError(f"{where}: {len(nodes)} eligible nodes; the node image holds 1..{rc.MAX_ELIGIBLE}")
    key = path_hash(rc.output_path(m, a))
    cfg = struct.pack("<IHB", key, period, len(nodes))
    for node_id, gravity in nodes:
        cfg += struct.pack("<HB", node_id, gravity)
    return cfg


def compile_image(m: Manifest, counter: int) -> bytes:
    """The node image for a manifest at a rollback counter. Deterministic: same input, same bytes."""
    body = b""
    count = 0
    for a in m.actors:
        where = f"actors[{a.name}]"
        if a.module not in BUILTINS:
            raise DeployError(f"{where}: module '{a.module}' is not a built-in actor this firmware "
                              f"has (ADR-003 Tier 0: one of {sorted(BUILTINS)})")
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
        cfg = _actor_config(a.module, a.config, where)
        body += struct.pack("<HBB", node, BUILTINS[a.module], len(cfg)) + cfg
        count += 1
    if count > MAX_ACTORS:
        raise DeployError(f"{count} actors; the node image holds at most {MAX_ACTORS}")
    digest = bytes.fromhex(m.digest())[:8]
    header = struct.pack("<IBBHI8sI", IMAGE_MAGIC, IMAGE_VERSION, count, 0, counter, digest, len(body))
    img = header + body
    if len(img) > MAX_IMAGE_LEN:
        raise DeployError(f"image is {len(img)} bytes; the node accepts at most {MAX_IMAGE_LEN}")
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
