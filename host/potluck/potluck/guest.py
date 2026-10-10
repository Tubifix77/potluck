"""Guest actors, the host's half -- M7.

A guest is a WebAssembly module from a party the cluster's owner does not trust with a node, run on the
boards in a sandbox (firmware/components/pot_wasm/include/pot/guest.hpp). Three parties, three steps:

    # the owner, once per author: certify the author's key under the cluster CA (role 3)
    python -m potluck.signing keygen --role guest --label acme --out keys/acme
    python -m potluck.enrol --guest-cert keys/acme.pub --author 7 --ca-key keys/ca.key --out keys/acme.gcert

    # the author, per module: sign it into a bundle (the owner never needs the author's key)
    python -m potluck.guest sign guests/build/overheat_alarm.wasm --key keys/acme.key --cert keys/acme.gcert \\
                                 --out guests/build/overheat_alarm.guest.json
    python -m potluck.guest verify guests/build/overheat_alarm.guest.json --ca keys/ca.pub

    # the owner: name it in a manifest, pinned by its hash, and deploy as usual (potluck.deploy)
    {"name": "alarm", "module": "guest:overheat_alarm", "latency_class": 3,
     "needs": ["potluck://lab/node-7368/hw/die_temp"],
     "config": {"bundle": "guests/build/overheat_alarm.guest.json", "sha256": "<the module's>",
                "outputs": "out:f32 alarm:i32 count:i32", "fuel": 100000, "memory_kib": 64,
                "period_ms": 1000}}

The guest reads its inputs by index in `needs` order and publishes its outputs by index in `outputs`
order, each to potluck://<system>/act/<actor>/<name>. The owner's manifest decides all of it; the module
only computes. `sha256` makes the owner's package signature cover the exact module: a bundle that
changes after the manifest was signed is refused here, and the owner's image signature covers the bytes
the nodes receive. Each node checks the author's signature again before the guest runs.

THE BUNDLE (JSON): type "potluck-guest-v1", name, module (base64), sha256 (of the module), cert (the
author's 112-byte role-3 certificate, hex), sig (the author's Ed25519 over GUEST_DOMAIN || SHA-512(module),
hex).

THE BLOB (what the node image carries per guest actor) is documented in pot/guest.hpp; encode_blob() is
its only writer, and tests/test_guest.cpp parses the golden one test_guest.py makes.
"""

from __future__ import annotations

import base64
import hashlib
import json
import struct
import sys
from dataclasses import dataclass

from . import ed25519_ref as ed
from . import enrol as en

BUNDLE_TYPE = "potluck-guest-v1"
GUEST_DOMAIN = b"potluck-guest-v1\0"
BLOB_MAGIC = 0x54534750  # "PGST"
BLOB_VERSION = 1
MAX_INPUTS = 8
MAX_OUTPUTS = 8
MAX_PAGES = 1  # 64 KiB
FUEL_MIN = 1000
FUEL_MAX = 50_000_000
#: pot::ValueType
TYPES = {"i32": 2, "f32": 4}
WASM_HEADER = b"\0asm\x01\0\0\0"


class GuestError(Exception):
    """A bundle or a guest declaration that cannot become a guest."""


@dataclass(frozen=True)
class Bundle:
    name: str
    module: bytes
    cert: bytes
    sig: bytes

    @property
    def sha256(self) -> str:
        return hashlib.sha256(self.module).hexdigest()


def signed_message(module: bytes) -> bytes:
    return GUEST_DOMAIN + hashlib.sha512(module).digest()


def make_bundle(name: str, module: bytes, author_secret: bytes, cert: bytes) -> Bundle:
    if module[:8] != WASM_HEADER:
        raise GuestError("not a WebAssembly 1.0 binary")
    en.parse_cert(cert, en.ROLE_GUEST)  # a role-3 certificate, or refuse
    if ed.public_key(author_secret) != cert[16:48]:
        raise GuestError("the key is not the one the certificate names")
    return Bundle(name, module, cert, ed.sign(author_secret, signed_message(module)))


def bundle_to_dict(b: Bundle) -> dict:
    return {"type": BUNDLE_TYPE, "name": b.name, "module": base64.b64encode(b.module).decode("ascii"),
            "sha256": b.sha256, "cert": b.cert.hex(), "sig": b.sig.hex()}


def bundle_from_dict(d: dict, where: str = "bundle") -> Bundle:
    if not isinstance(d, dict) or d.get("type") != BUNDLE_TYPE:
        raise GuestError(f"{where}: not a {BUNDLE_TYPE} bundle")
    try:
        b = Bundle(str(d["name"]), base64.b64decode(d["module"], validate=True), bytes.fromhex(d["cert"]),
                   bytes.fromhex(d["sig"]))
    except (KeyError, ValueError) as exc:
        raise GuestError(f"{where}: malformed ({exc})") from None
    if b.sha256 != d.get("sha256"):
        raise GuestError(f"{where}: the module does not match the bundle's own sha256")
    if len(b.cert) != en.CERT_LEN or len(b.sig) != 64:
        raise GuestError(f"{where}: a certificate is {en.CERT_LEN} bytes and a signature 64")
    return b


def load_bundle(path: str) -> Bundle:
    with open(path, "r", encoding="utf-8") as f:
        return bundle_from_dict(json.load(f), path)


def verify_bundle(b: Bundle, ca_public: bytes) -> int:
    """The author's number, if the certificate chains to the CA and the signature covers the module."""
    try:
        c = en.verify_cert(b.cert, ca_public, en.ROLE_GUEST)
    except en.EnrolError as exc:
        raise GuestError(f"author certificate: {exc}") from None
    if not ed.verify(c.node_pub, signed_message(b.module), b.sig):
        raise GuestError("the author's signature does not cover this module")
    return c.node_id


def parse_outputs(spec: str, where: str) -> list[tuple[str, int]]:
    """'out:f32 alarm:i32' -> [('out', 4), ('alarm', 2)]."""
    out: list[tuple[str, int]] = []
    for item in spec.split():
        name, _, typ = item.partition(":")
        if not name or typ not in TYPES:
            raise GuestError(f"{where}: output '{item}' is not <name>:i32 or <name>:f32")
        if not all(ch.isalnum() or ch in "_-" for ch in name):
            raise GuestError(f"{where}: output name '{name}' must be letters, digits, '_' or '-'")
        if any(name == n for n, _ in out):
            raise GuestError(f"{where}: two outputs named '{name}'")
        out.append((name, TYPES[typ]))
    if not 1 <= len(out) <= MAX_OUTPUTS:
        raise GuestError(f"{where}: a guest has 1..{MAX_OUTPUTS} outputs")
    return out


def encode_blob(b: Bundle, inputs: list[int], outputs: list[tuple[int, int]], fuel: int, pages: int) -> bytes:
    """The node's guest blob (pot/guest.hpp): the owner's wiring and limits, the author's cert and
    signature, the module."""
    if len(inputs) > MAX_INPUTS:
        raise GuestError(f"a guest reads at most {MAX_INPUTS} inputs")
    if not 1 <= len(outputs) <= MAX_OUTPUTS:
        raise GuestError(f"a guest has 1..{MAX_OUTPUTS} outputs")
    if not FUEL_MIN <= fuel <= FUEL_MAX:
        raise GuestError(f"fuel {fuel} is outside {FUEL_MIN}..{FUEL_MAX}")
    if not 0 <= pages <= MAX_PAGES:
        raise GuestError(f"memory: 0 or {MAX_PAGES * 64} KiB")
    blob = struct.pack("<IBBBBI", BLOB_MAGIC, BLOB_VERSION, len(inputs), len(outputs), pages, fuel)
    for h in inputs:
        blob += struct.pack("<I", h)
    for h, t in outputs:
        blob += struct.pack("<IB", h, t)
    return blob + b.cert + b.sig + b.module


def _flag(args: list[str], name: str) -> str | None:
    if name in args:
        i = args.index(name)
        if i + 1 < len(args):
            return args[i + 1]
    return None


def main(argv: list[str] | None = None) -> int:
    from .signing import SigningError, read_key

    args = list(sys.argv[1:] if argv is None else argv)
    try:
        if len(args) >= 2 and args[0] == "sign":
            path = args[1]
            key = read_key(_flag(args, "--key") or "")
            if key.role != "guest" or key.secret is None:
                raise GuestError("--key: a guest author's private key (signing keygen --role guest)")
            with open(_flag(args, "--cert") or "", "r", encoding="ascii") as f:
                cert = bytes.fromhex(f.read().strip())
            with open(path, "rb") as f:
                module = f.read()
            name = path.replace("\\", "/").rsplit("/", 1)[-1].rsplit(".", 1)[0]
            b = make_bundle(name, module, key.secret, cert)
            out = _flag(args, "--out") or ""
            with open(out, "w", encoding="ascii", newline="\n") as f:
                json.dump(bundle_to_dict(b), f, indent=2)
                f.write("\n")
            print(f"{name}: {len(module)} B, sha256 {b.sha256}, signed by author {en.parse_cert(cert, en.ROLE_GUEST).node_id}"
                  f" -> {out}")
            return 0
        if len(args) >= 2 and args[0] == "verify":
            b = load_bundle(args[1])
            ca = read_key(_flag(args, "--ca") or "")
            author = verify_bundle(b, ca.public)
            print(f"ok: {b.name}, {len(b.module)} B, sha256 {b.sha256}, author {author}")
            return 0
        print(__doc__.strip().splitlines()[0])
        print("usage: python -m potluck.guest sign <module.wasm> --key <author.key> --cert <author cert> --out <bundle>")
        print("       python -m potluck.guest verify <bundle> --ca <ca.pub>")
        return 2
    except (GuestError, en.EnrolError, SigningError, OSError, ValueError) as exc:
        print(f"guest: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
