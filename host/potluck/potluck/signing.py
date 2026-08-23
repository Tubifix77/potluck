"""Signing a deploy package -- ARCHITECTURE.md section 9.3, and the host half of M5.

    python -m potluck.signing keygen  --role ca     --label lab-ca  --out keys/lab-ca
    python -m potluck.signing keygen  --role deploy --label lab-dep --out keys/lab-deploy
    python -m potluck.signing certify --ca keys/lab-ca.key --key keys/lab-deploy.pub \
                                      --out keys/lab-deploy.cert
    python -m potluck.signing sign    --key keys/lab-deploy.key --cert keys/lab-deploy.cert \
                                      --counter 7 manifests/home.json --out build/home.potpkg.json
    python -m potluck.signing verify  --ca keys/lab-ca.pub --min-counter 7 build/home.potpkg.json

WHAT SECTION 9.3 ASKS FOR, AND WHICH PARTS ARE HERE

  * **A cluster CA, separate from the deploy key.** "Compromise means bad code, not a forged cluster
    identity -- a deliberate separation so the two failures are recoverable independently." So the CA
    signs a short certificate authorising a deploy key, and the deploy key signs manifests. A
    verifier needs only the CA's *public* key: the certificate travels inside the package.
  * **Anti-rollback.** "Monotonic counter in NVS; manifests below it are refused." The counter is
    inside the signed preimage rather than beside it, so downgrading it invalidates the signature
    instead of merely being noticed.
  * **The firmware's verification path, enrolment, and the NVS counter itself** are M5 proper and wait
    for boards. This is the tooling that produces something for them to verify.

AND ONE THING THIS DELIBERATELY DOES NOT DECIDE

Section 13-M5 has an open **[MEASURE]**: "Ed25519 vs P-256 decided on measured verify cost". That is a
bench question about the node, not about the laptop. So `alg` is a field in every structure here, the
verifier dispatches on it, and adding P-256 later needs no schema change. The host tooling uses
Ed25519 today because it can be implemented in one dependency-free file (`ed25519_ref.py`, checked
against RFC 8032's own test vectors) -- convenience on this side, not an answer for that side.

ON KEY MATERIAL

A private key file is a private key file. This writes them with owner-only permissions where the OS
supports it and says so plainly where it does not, never prints a secret, and never puts one in a
package. Everything past that -- where the file lives, who can read it, whether it should be in an
HSM -- is outside a build tool's business and is not pretended otherwise.
"""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import stat
import sys
import time
from dataclasses import dataclass
from typing import Any

from . import ed25519_ref as ed
from .manifest import Manifest, ManifestErrors, load as load_manifest, parse as parse_manifest

SCHEMA = 1
PACKAGE_TYPE = "potluck-package-v1"
CERT_TYPE = "potluck-key-cert-v1"

#: The only algorithm implemented here. A field rather than an assumption -- see the module docstring.
ALG_ED25519 = "ed25519"
ALGORITHMS = (ALG_ED25519,)

ROLES = ("ca", "deploy")


class SigningError(Exception):
    """Anything that makes a package unusable, with the reason a person needs."""


# ---------------------------------------------------------------------------------------------
# Keys
# ---------------------------------------------------------------------------------------------


def key_id(alg: str, public: bytes) -> str:
    """A short, stable name for a key: the first 8 bytes of SHA-256 over its algorithm and bytes.

    The algorithm is inside the hash, so the same 32 bytes under a different algorithm is a
    different key id rather than the same one -- which matters the day a second algorithm exists.
    """
    return hashlib.sha256(alg.encode("ascii") + b"\0" + public).hexdigest()[:16]


@dataclass(frozen=True)
class KeyPair:
    alg: str
    role: str
    label: str
    public: bytes
    secret: bytes | None  # absent in a .pub file

    @property
    def id(self) -> str:
        return key_id(self.alg, self.public)

    def public_dict(self) -> dict[str, Any]:
        return {
            "schema": SCHEMA,
            "alg": self.alg,
            "role": self.role,
            "label": self.label,
            "key_id": self.id,
            "public": self.public.hex(),
        }

    def private_dict(self) -> dict[str, Any]:
        if self.secret is None:
            raise SigningError("this is a public key; it has no secret to write")
        d = self.public_dict()
        d["secret"] = self.secret.hex()
        return d


def generate(role: str, label: str, alg: str = ALG_ED25519) -> KeyPair:
    if role not in ROLES:
        raise SigningError(f"role must be one of {', '.join(ROLES)}")
    if alg not in ALGORITHMS:
        raise SigningError(f"unknown algorithm '{alg}'")
    # secrets, not random: this is the one place in the project where the difference is the point.
    sk = secrets.token_bytes(ed.KEY_BYTES)
    return KeyPair(alg=alg, role=role, label=label, public=ed.public_key(sk), secret=sk)


def _key_from_dict(d: Any, where: str) -> KeyPair:
    if not isinstance(d, dict):
        raise SigningError(f"{where}: not a key file")
    for field in ("alg", "role", "label", "public"):
        if field not in d:
            raise SigningError(f"{where}: missing '{field}'")
    if d["alg"] not in ALGORITHMS:
        raise SigningError(f"{where}: unknown algorithm '{d['alg']}'")
    if d["role"] not in ROLES:
        raise SigningError(f"{where}: unknown role '{d['role']}'")
    try:
        public = bytes.fromhex(d["public"])
        secret = bytes.fromhex(d["secret"]) if "secret" in d else None
    except ValueError as exc:
        raise SigningError(f"{where}: key bytes are not hex: {exc}") from exc
    if len(public) != ed.KEY_BYTES:
        raise SigningError(f"{where}: a public key is {ed.KEY_BYTES} bytes, not {len(public)}")
    if secret is not None and len(secret) != ed.KEY_BYTES:
        raise SigningError(f"{where}: a secret key is {ed.KEY_BYTES} bytes, not {len(secret)}")
    kp = KeyPair(alg=d["alg"], role=d["role"], label=d["label"], public=public, secret=secret)
    if "key_id" in d and d["key_id"] != kp.id:
        # A key id that does not match its own bytes means the file was edited, and every trust
        # decision downstream is made on the id.
        raise SigningError(f"{where}: key_id {d['key_id']} does not match the public key ({kp.id})")
    if secret is not None and ed.public_key(secret) != public:
        raise SigningError(f"{where}: the secret and public halves are not a pair")
    return kp


def write_keypair(kp: KeyPair, out_base: str) -> tuple[str, str]:
    """Write `<base>.key` and `<base>.pub`. Returns both paths."""
    priv_path = out_base + ".key"
    pub_path = out_base + ".pub"
    parent = os.path.dirname(os.path.abspath(priv_path))
    os.makedirs(parent, exist_ok=True)

    # Created with owner-only permissions from the outset rather than chmod-ed afterwards: between
    # the two there is a window, and a private key is exactly the wrong thing to leave in one.
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    fd = os.open(priv_path, flags, stat.S_IRUSR | stat.S_IWUSR)
    with os.fdopen(fd, "w", encoding="ascii") as f:
        json.dump(kp.private_dict(), f, indent=2, sort_keys=True)
        f.write("\n")

    with open(pub_path, "w", encoding="ascii") as f:
        json.dump(kp.public_dict(), f, indent=2, sort_keys=True)
        f.write("\n")
    return priv_path, pub_path


def read_key(path: str) -> KeyPair:
    with open(path, "r", encoding="utf-8") as f:
        try:
            d = json.load(f)
        except json.JSONDecodeError as exc:
            raise SigningError(f"{path}: {exc.msg} at line {exc.lineno}") from exc
    return _key_from_dict(d, path)


# ---------------------------------------------------------------------------------------------
# Certificates: the CA saying "this deploy key may sign for this cluster"
# ---------------------------------------------------------------------------------------------


def cert_preimage(alg: str, subject_public: bytes, role: str, label: str, not_after: int) -> bytes:
    """Exactly what a certificate signature covers.

    Written out as a function, and domain-separated by the type string, so the two sides can never
    disagree about it and a certificate can never be replayed as a package signature.
    """
    return b"\n".join([
        CERT_TYPE.encode("ascii"),
        alg.encode("ascii"),
        subject_public.hex().encode("ascii"),
        role.encode("ascii"),
        label.encode("ascii"),
        str(int(not_after)).encode("ascii"),
    ]) + b"\n"


def certify(ca: KeyPair, subject: KeyPair, not_after: int = 0) -> dict[str, Any]:
    if ca.secret is None:
        raise SigningError("the CA key given has no secret half; a .pub cannot sign")
    if ca.role != "ca":
        raise SigningError(f"the signing key is role '{ca.role}', not 'ca'")
    if subject.role != "deploy":
        raise SigningError(f"the subject key is role '{subject.role}', not 'deploy'")
    pre = cert_preimage(subject.alg, subject.public, subject.role, subject.label, not_after)
    sig = ed.sign(ca.secret, pre)
    return {
        "schema": SCHEMA,
        "type": CERT_TYPE,
        "alg": subject.alg,
        "subject": {
            "key_id": subject.id,
            "public": subject.public.hex(),
            "role": subject.role,
            "label": subject.label,
        },
        "not_after": int(not_after),
        "issuer": {"key_id": ca.id, "label": ca.label},
        "signature": sig.hex(),
    }


def verify_cert(cert: Any, ca_public: bytes, *, now: int | None = None) -> KeyPair:
    """Check a certificate against a CA public key and return the subject it authorises."""
    if not isinstance(cert, dict) or cert.get("type") != CERT_TYPE:
        raise SigningError(f"not a {CERT_TYPE}")
    if cert.get("alg") not in ALGORITHMS:
        raise SigningError(f"certificate uses an algorithm this tool cannot check: {cert.get('alg')}")
    subject = cert.get("subject")
    if not isinstance(subject, dict):
        raise SigningError("certificate has no subject")
    try:
        subject_public = bytes.fromhex(subject.get("public", ""))
        sig = bytes.fromhex(cert.get("signature", ""))
    except ValueError as exc:
        raise SigningError(f"certificate hex is malformed: {exc}") from exc

    alg = cert["alg"]
    role = subject.get("role", "")
    label = subject.get("label", "")
    not_after = int(cert.get("not_after", 0))
    pre = cert_preimage(alg, subject_public, role, label, not_after)
    if not ed.verify(ca_public, pre, sig):
        raise SigningError("the certificate is not signed by this CA")
    if role != "deploy":
        raise SigningError(f"the certificate authorises role '{role}', not 'deploy'")

    kp = KeyPair(alg=alg, role=role, label=label, public=subject_public, secret=None)
    if subject.get("key_id") not in (None, kp.id):
        raise SigningError("the certificate's key_id does not match its own subject key")
    if not_after != 0:
        current = int(time.time()) if now is None else now
        if current > not_after:
            raise SigningError(f"the certificate expired at {not_after} (now {current})")
    return kp


# ---------------------------------------------------------------------------------------------
# Packages: a manifest, a counter, and a signature over both
# ---------------------------------------------------------------------------------------------


def package_preimage(manifest: Manifest, rollback_counter: int) -> bytes:
    """What a package signature covers: the counter *and* the manifest.

    The counter is inside the signature rather than beside it, so a downgrade attempt produces an
    invalid signature rather than a valid signature with a suspicious number next to it. §9.3's
    anti-rollback then has something to stand on even before the node consults its NVS counter.
    """
    return (PACKAGE_TYPE.encode("ascii") + b"\n" +
            str(int(rollback_counter)).encode("ascii") + b"\n" +
            manifest.canonical_bytes())


def sign_manifest(m: Manifest, deploy: KeyPair, cert: dict[str, Any],
                  rollback_counter: int) -> dict[str, Any]:
    if deploy.secret is None:
        raise SigningError("the deploy key given has no secret half; a .pub cannot sign")
    if deploy.role != "deploy":
        raise SigningError(f"the signing key is role '{deploy.role}', not 'deploy'")
    if rollback_counter < 0:
        raise SigningError("a rollback counter is monotonic and cannot be negative")
    subject = cert.get("subject", {}) if isinstance(cert, dict) else {}
    if subject.get("public") != deploy.public.hex():
        raise SigningError("the certificate does not authorise this key: "
                           f"cert subject {subject.get('key_id')}, signing key {deploy.id}")

    pre = package_preimage(m, rollback_counter)
    return {
        "schema": SCHEMA,
        "type": PACKAGE_TYPE,
        "rollback_counter": int(rollback_counter),
        "manifest": m.to_dict(),
        "cert": cert,
        "signature": {
            "alg": deploy.alg,
            "key_id": deploy.id,
            "sig": ed.sign(deploy.secret, pre).hex(),
        },
        # Convenience only, and it is never trusted: verify recomputes it. Here so a person reading
        # the file can compare it against `potluck.manifest digest` without a tool.
        "manifest_digest": m.digest(),
    }


@dataclass(frozen=True)
class Verified:
    manifest: Manifest
    rollback_counter: int
    signer: KeyPair


def verify_package(doc: Any, ca_public: bytes, *, min_counter: int = 0,
                   now: int | None = None) -> Verified:
    """Check everything, in the order that fails cheapest first, and return what was proven."""
    if not isinstance(doc, dict) or doc.get("type") != PACKAGE_TYPE:
        raise SigningError(f"not a {PACKAGE_TYPE}")
    if doc.get("schema") != SCHEMA:
        raise SigningError(f"this tool understands schema {SCHEMA}, not {doc.get('schema')}")

    counter = doc.get("rollback_counter")
    if not isinstance(counter, int) or isinstance(counter, bool) or counter < 0:
        raise SigningError(f"rollback_counter must be a non-negative integer; got {counter!r}")
    if counter < min_counter:
        # §9.3: "manifests below it are refused". Checked before the signature, because a downgrade
        # is refused whether or not it is correctly signed -- and a correctly signed downgrade is
        # exactly the attack the counter exists for.
        raise SigningError(f"rollback: this package is counter {counter}, "
                           f"and {min_counter} has already been accepted")

    # The manifest is fully re-validated, not trusted because it is signed. A signature says who
    # wrote it, never that it is well-formed.
    try:
        m = parse_manifest(doc.get("manifest"))
    except ManifestErrors as exc:
        raise SigningError(f"the signed manifest is not valid:\n{exc.report()}") from exc

    signer = verify_cert(doc.get("cert"), ca_public, now=now)

    sig_block = doc.get("signature")
    if not isinstance(sig_block, dict):
        raise SigningError("no signature block")
    if sig_block.get("alg") not in ALGORITHMS:
        raise SigningError(f"signature uses an algorithm this tool cannot check: "
                           f"{sig_block.get('alg')}")
    if sig_block.get("key_id") != signer.id:
        raise SigningError(f"the signature claims key {sig_block.get('key_id')} but the certificate "
                           f"authorises {signer.id}")
    try:
        sig = bytes.fromhex(sig_block.get("sig", ""))
    except ValueError as exc:
        raise SigningError(f"signature is not hex: {exc}") from exc

    if not ed.verify(signer.public, package_preimage(m, counter), sig):
        raise SigningError("the package signature does not check out")
    return Verified(manifest=m, rollback_counter=counter, signer=signer)


def load_package(path: str) -> Any:
    with open(path, "r", encoding="utf-8") as f:
        try:
            return json.load(f)
        except json.JSONDecodeError as exc:
            raise SigningError(f"{path}: {exc.msg} at line {exc.lineno}") from exc


# ---------------------------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------------------------


def _flag(args: list[str], name: str, default: str | None = None) -> str | None:
    if name in args:
        i = args.index(name)
        if i + 1 < len(args):
            return args[i + 1]
        raise SigningError(f"{name} needs a value")
    return default


def _positional(args: list[str]) -> list[str]:
    out: list[str] = []
    skip = False
    for i, a in enumerate(args):
        if skip:
            skip = False
            continue
        if a.startswith("--"):
            skip = True
            continue
        out.append(a)
    return out


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, rest = args[0], args[1:]

    try:
        if cmd == "keygen":
            role = _flag(rest, "--role", "deploy") or "deploy"
            label = _flag(rest, "--label", role) or role
            out = _flag(rest, "--out")
            if out is None:
                raise SigningError("keygen needs --out <path without extension>")
            kp = generate(role, label)
            priv, pub = write_keypair(kp, out)
            print(f"{role} key '{label}'  id {kp.id}")
            print(f"  secret -> {priv}")
            print(f"  public -> {pub}")
            if os.name == "nt":
                # Said out loud rather than silently skipped: the file was created with the mode
                # asked for, and on Windows that mode does not mean what it means elsewhere.
                print("  note: on Windows the owner-only file mode is advisory. Keep the .key out "
                      "of version control and off shared drives yourself.")
            return 0

        if cmd == "certify":
            ca_path = _flag(rest, "--ca")
            key_path = _flag(rest, "--key")
            out = _flag(rest, "--out")
            days = int(_flag(rest, "--days", "0") or "0")
            if not ca_path or not key_path or not out:
                raise SigningError("certify needs --ca <ca.key> --key <deploy.pub> --out <path>")
            ca = read_key(ca_path)
            subject = read_key(key_path)
            not_after = int(time.time()) + days * 86400 if days > 0 else 0
            cert = certify(ca, subject, not_after)
            with open(out, "w", encoding="ascii") as f:
                json.dump(cert, f, indent=2, sort_keys=True)
                f.write("\n")
            print(f"certified deploy key {subject.id} ('{subject.label}') "
                  f"under CA {ca.id} -> {out}")
            print("  no expiry" if not_after == 0 else f"  expires at {not_after}")
            return 0

        if cmd == "sign":
            key_path = _flag(rest, "--key")
            cert_path = _flag(rest, "--cert")
            out = _flag(rest, "--out")
            counter = int(_flag(rest, "--counter", "1") or "1")
            files = _positional(rest)
            if not key_path or not cert_path or not out or not files:
                raise SigningError("sign needs --key --cert --out and a manifest file")
            deploy = read_key(key_path)
            with open(cert_path, "r", encoding="utf-8") as f:
                cert = json.load(f)
            m = load_manifest(files[0], require_placement=True)
            pkg = sign_manifest(m, deploy, cert, counter)
            with open(out, "w", encoding="ascii") as f:
                json.dump(pkg, f, indent=2, sort_keys=True)
                f.write("\n")
            print(f"signed '{m.system}' at counter {counter} with {deploy.id} -> {out}")
            print(f"  manifest digest {m.digest()}")
            return 0

        if cmd == "verify":
            ca_path = _flag(rest, "--ca")
            min_counter = int(_flag(rest, "--min-counter", "0") or "0")
            files = _positional(rest)
            if not ca_path or not files:
                raise SigningError("verify needs --ca <ca.pub> and a package file")
            ca = read_key(ca_path)
            v = verify_package(load_package(files[0]), ca.public, min_counter=min_counter)
            print(f"{files[0]}: signature checks out")
            print(f"  system    {v.manifest.system}, {len(v.manifest.actors)} actor(s)")
            print(f"  signer    {v.signer.id} ('{v.signer.label}'), authorised by CA {ca.id}")
            print(f"  counter   {v.rollback_counter} (minimum accepted {min_counter})")
            print(f"  digest    {v.manifest.digest()}")
            return 0

    except SigningError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print("usage: python -m potluck.signing {keygen|certify|sign|verify} ...", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
