"""Node enrolment -- ARCHITECTURE.md section 9.3, M5.

    python -m potluck.enrol --port COM4 --ca-key keys/ca.key          # enrol the board on COM4
    python -m potluck.enrol --port COM4 --show                        # just print what it reports
    python -m potluck.enrol --check-cert <hex> --ca keys/ca.pub       # verify a certificate offline

WHAT ENROLMENT IS

Section 9.3: "Per-node keypair generated on-device at provisioning; the private key never leaves.
Node certificate signed by the cluster CA binds node_id <-> public key." And "Provisioning is
physical. A node joins by being connected over USB and enrolled." So this talks to a board over its
own USB console -- not the frame link, not the radio -- asks for the public key it generated, signs a
certificate binding that key to the node id the board derived from its own MAC, and hands the
certificate back together with the CA's public key. The board refuses a certificate that names
another node, another key, or that the CA it was given did not sign.

THE NODE CERTIFICATE, v1 (112 bytes, little-endian; firmware/components/pot_trust parses the same)

     off  len  field
       0    4  magic        "PNC1"
       4    1  version      1
       5    1  role         1 = node
       6    2  node_id
       8    4  ca_fp        first 4 bytes of SHA-512(CA public key): which cluster
      12    4  issued       Unix seconds when signed; informational, nothing expires in v1 (9.5)
      16   32  node_pub     the node's Ed25519 public key
      48   64  ca_sig       Ed25519 by the CA over DOMAIN || bytes[0:48]

It is binary and fixed-size because it rides inside HELLO, which must fit one ESP-NOW frame. The
domain string keeps a node-certificate signature from ever verifying as any other CA signature (the
deploy-key certificates in signing.py sign JSON under a different preimage).

THE CONSOLE PROTOCOL

One line each way. The PC sends lines starting "POT! "; the board answers with one JSON line of
type "enrol", among its ordinary log output.

    POT! id                          -> {"t":"enrol","node":N,"pub":"<hex>","enrolled":0|1,"ca_fp":"<hex>"}
    POT! cert <ca_pub hex> <cert hex> -> {"t":"enrol","result":"ok"} or {"t":"enrol","result":"<error>"}

THE CA KEY

Section 9.3 wants it offline: "Not on the workstation that runs potctl. Not in the repo." On this
bench it lives in keys/ (gitignored) on the workstation, which is the bench's stated compromise, not
the design. This tool reads it, signs, and keeps nothing.
"""

from __future__ import annotations

import hashlib
import json
import struct
import sys
import time
from dataclasses import dataclass

from . import ed25519_ref as ed
from .signing import KeyPair, SigningError, read_key

MAGIC = b"PNC1"
VERSION = 1
ROLE_NODE = 1
ROLE_DEPLOY = 2  # the same format certifies a deploy key (M5 step 6); node_id is 0
CERT_LEN = 112
SIGNED_LEN = 48
DOMAIN = b"potluck-node-cert-v1\0"


class EnrolError(Exception):
    pass


def ca_fingerprint(ca_public: bytes) -> bytes:
    return hashlib.sha512(ca_public).digest()[:4]


@dataclass(frozen=True)
class NodeCert:
    node_id: int
    ca_fp: bytes
    issued: int
    node_pub: bytes
    ca_sig: bytes


def build_cert(ca: KeyPair, node_id: int, node_pub: bytes, issued: int | None = None,
               role: int = ROLE_NODE) -> bytes:
    if ca.secret is None:
        raise EnrolError("the CA key given is a public key; signing needs the .key file")
    if ca.role != "ca":
        raise EnrolError(f"that key's role is '{ca.role}', not 'ca'")
    if len(node_pub) != 32:
        raise EnrolError("a node public key is 32 bytes")
    if not 0 <= node_id <= 0xFFFF:
        raise EnrolError("node id out of range")
    if issued is None:
        issued = int(time.time())
    body = (MAGIC + struct.pack("<BBH", VERSION, role, node_id) + ca_fingerprint(ca.public) +
            struct.pack("<I", issued & 0xFFFFFFFF) + node_pub)
    assert len(body) == SIGNED_LEN
    return body + ed.sign(ca.secret, DOMAIN + body)


def parse_cert(raw: bytes, role: int = ROLE_NODE) -> NodeCert:
    if len(raw) != CERT_LEN:
        raise EnrolError(f"a node certificate is {CERT_LEN} bytes, this is {len(raw)}")
    if raw[:4] != MAGIC:
        raise EnrolError("not a node certificate (magic)")
    version, cert_role, node_id = struct.unpack_from("<BBH", raw, 4)
    if version != VERSION:
        raise EnrolError(f"certificate version {version}, this tool knows {VERSION}")
    if cert_role != role:
        raise EnrolError(f"certificate role {cert_role}, expected {role}")
    (issued,) = struct.unpack_from("<I", raw, 12)
    return NodeCert(node_id=node_id, ca_fp=raw[8:12], issued=issued, node_pub=raw[16:48], ca_sig=raw[48:112])


def verify_cert(raw: bytes, ca_public: bytes, role: int = ROLE_NODE) -> NodeCert:
    c = parse_cert(raw, role)
    if c.ca_fp != ca_fingerprint(ca_public):
        raise EnrolError("certificate is from a different CA (fingerprint)")
    if not ed.verify(ca_public, DOMAIN + raw[:SIGNED_LEN], c.ca_sig):
        raise EnrolError("CA signature does not verify")
    return c


# ---------------------------------------------------------------------------------------------
# Talking to a board
# ---------------------------------------------------------------------------------------------


def _open(port: str):
    import serial  # pyserial; only needed when a board is involved

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False  # never reset the board: enrolment must not reboot it
    s.rts = False
    s.open()
    return s


def _ask(s, line: str, timeout: float = 5.0) -> dict:
    s.reset_input_buffer()
    s.write((line + "\n").encode("ascii"))
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += s.read(512)
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode("utf-8", "replace").strip()
            if text.startswith('{"t":"enrol"'):
                try:
                    return json.loads(text)
                except json.JSONDecodeError:
                    pass
    raise EnrolError(f"no enrol reply to '{line.split(' ')[1]}' within {timeout:.0f} s "
                     "(is the board running M5 firmware, and is this its console port?)")


def query(port: str) -> dict:
    s = _open(port)
    try:
        return _ask(s, "POT! id")
    finally:
        s.close()


def enrol(port: str, ca: KeyPair, issued: int | None = None) -> tuple[dict, bytes]:
    s = _open(port)
    try:
        info = _ask(s, "POT! id")
        node_id = int(info["node"])
        node_pub = bytes.fromhex(info["pub"])
        cert = build_cert(ca, node_id, node_pub, issued)
        verify_cert(cert, ca.public)  # never send what we would not accept ourselves
        reply = _ask(s, f"POT! cert {ca.public.hex()} {cert.hex()}", timeout=10.0)
        if reply.get("result") != "ok":
            raise EnrolError(f"the board refused the certificate: {reply.get('result')}")
        after = _ask(s, "POT! id")
        if not after.get("enrolled") or after.get("ca_fp") != ca_fingerprint(ca.public).hex():
            raise EnrolError(f"the board accepted, but now reports {after}")
        return after, cert
    finally:
        s.close()


def _flag(argv: list[str], name: str) -> str | None:
    if name in argv:
        i = argv.index(name)
        if i + 1 < len(argv):
            return argv[i + 1]
    return None


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if "--check-cert" in argv:
            ca = read_key(_flag(argv, "--ca") or "")
            c = verify_cert(bytes.fromhex(_flag(argv, "--check-cert") or ""), ca.public)
            print(f"ok: node 0x{c.node_id:04x}, key {c.node_pub.hex()}, issued {c.issued}")
            return 0
        if "--host-cert" in argv:
            # M10: enrol a host node (pot_hostnode) as a board is enrolled -- its own key, certified by
            # the cluster CA for its node id -- so the other members verify its HELLO like any board's.
            # The identity file is pot_hostnode's ("seed <hex>"); the CA's key and the certificate are
            # appended to it. Its public key is derived from the seed by the node itself (--keygen
            # prints it), so it is given here rather than computed twice.
            ident = _flag(argv, "--host-cert") or ""
            pub = bytes.fromhex(_flag(argv, "--pub") or "")
            node = int(_flag(argv, "--node") or "0x00fe", 0)
            ca = read_key(_flag(argv, "--ca-key") or "")
            cert = build_cert(ca, node, pub)
            verify_cert(cert, ca.public)
            with open(ident, "a", encoding="ascii") as f:
                f.write(f"ca {ca.public.hex()}\ncert {cert.hex()}\n")
            print(f"host node 0x{node:04x} certified by CA {ca.id} -> {ident}")
            return 0
        if "--deploy-cert" in argv:
            # Once, with the CA key: certify the deploy key for node-side image checks (M5 step 6).
            deploy = read_key(_flag(argv, "--deploy-cert") or "")
            ca = read_key(_flag(argv, "--ca-key") or "")
            cert = build_cert(ca, 0, deploy.public, role=ROLE_DEPLOY)
            verify_cert(cert, ca.public, ROLE_DEPLOY)
            out = _flag(argv, "--out") or ""
            with open(out, "w", encoding="ascii") as f:
                f.write(cert.hex() + "\n")
            print(f"deploy key {deploy.id} certified by CA {ca.id} -> {out}")
            return 0
        port = _flag(argv, "--port")
        if not port:
            print(__doc__.split("\n\n")[1])
            return 2
        if "--show" in argv:
            print(json.dumps(query(port)))
            return 0
        ca = read_key(_flag(argv, "--ca-key") or "")
        info, cert = enrol(port, ca)
        print(f"enrolled node 0x{int(info['node']):04x} on {port} under CA {ca.id}")
        print(f"certificate {cert.hex()}")
        return 0
    except (EnrolError, SigningError, OSError, KeyError, ValueError) as exc:
        print(f"enrol: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
