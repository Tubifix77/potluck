"""The hub on the house network, set up from the PC with nothing typed: its certificate and the tray's --listen.

Asked for by the Potluck Desk (2026-10-20, HOME-ASSISTANT.md part B steps 1-2): a newcomer made the hub's certificate
with openssl and edited tray.json by hand, then restarted potluck.exe from Task Manager. POST /api/setup/listen (on
127.0.0.1 only) does it: a certificate for the PC's address -- kept when the one there already names it, so Home
Assistant's pairing and a phone that trusts it stay valid -- the tray's hub_args, and a restart through the tray.

The certificate is its own CA (basicConstraints CA:TRUE) naming the PC's address and 127.0.0.1, so a phone can install it
as a trusted certificate (the login page links it) and Home Assistant pins its SHA-256.
"""

from __future__ import annotations

import datetime
import hashlib
import ipaddress
import json
import os
from pathlib import Path

#: How long a certificate this makes is valid -- a budget, not a measured client limit.
VALID_DAYS = 825
LISTEN_PORT = 8443


def potluck_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/.cache")
    return Path(base) / "potluck"


def cert_paths() -> tuple[Path, Path]:
    d = potluck_dir() / "hub-tls"
    return d / "cert.pem", d / "key.pem"


def covers(cert_pem: bytes, ip: str) -> bool:
    """Whether a PEM certificate names `ip` among its subject alternative names."""
    from cryptography import x509
    try:
        cert = x509.load_pem_x509_certificate(cert_pem)
        san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
    except (ValueError, x509.ExtensionNotFound):
        return False
    return ipaddress.ip_address(ip) in san.get_values_for_type(x509.IPAddress)


def fingerprint(cert_pem: bytes) -> str:
    import ssl
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert_pem.decode("ascii"))).hexdigest()


def make(ip: str, cert_path: Path, key_path: Path) -> None:
    """A new key and certificate for `ip` and 127.0.0.1, written to the two paths (the key readable by this user)."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "potluck-hub")])
    now = datetime.datetime.now(datetime.timezone.utc)
    ips = sorted({ipaddress.ip_address(ip), ipaddress.ip_address("127.0.0.1")}, key=str)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(days=VALID_DAYS))
            .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
            .add_extension(x509.KeyUsage(digital_signature=True, key_cert_sign=True, crl_sign=False,
                                         content_commitment=False, key_encipherment=False, data_encipherment=False,
                                         key_agreement=False, encipher_only=False, decipher_only=False), critical=True)
            .add_extension(x509.SubjectAlternativeName([x509.IPAddress(a) for a in ips]
                                                       + [x509.DNSName("localhost")]), critical=False)
            .sign(key, hashes.SHA256()))
    cert_path.parent.mkdir(parents=True, exist_ok=True)
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))


def _strip_listen(args: list) -> list[str]:
    out, skip = [], False
    for x in args:
        if skip:
            skip = False
            continue
        if x in ("--listen", "--tls-cert", "--tls-key"):
            skip = True
            continue
        out.append(str(x))
    return out


def set_listen(on: bool, address: str | None = None, port: int = LISTEN_PORT) -> dict:
    """Write the tray's hub_args for the house network (on) or the PC only (off), making or keeping the certificate.
    Returns {"listen", "address", "certificate": "kept" | "made" | None, "fp", "tray_json"}. ValueError on a bad
    address."""
    tray = potluck_dir() / "tray.json"
    try:
        cfg = json.loads(tray.read_text(encoding="utf-8-sig"))
        if not isinstance(cfg, dict):
            cfg = {}
    except FileNotFoundError:
        cfg = {}
    args = _strip_listen(cfg.get("hub_args") or [])
    out = {"listen": None, "address": None, "certificate": None, "fp": None, "tray_json": str(tray)}
    if on:
        if address is None:
            from .hub_mdns import lan_ip
            address = lan_ip()
        ip = ipaddress.ip_address(address)  # ValueError for anything but an address
        if ip.version != 4 or ip.is_loopback or ip.is_unspecified:
            raise ValueError(f"{address}: the PC's address on the house network (IPv4), not loopback or 0.0.0.0")
        cert, key = cert_paths()
        if cert.exists() and key.exists() and covers(cert.read_bytes(), str(ip)):
            out["certificate"] = "kept"
        else:
            make(str(ip), cert, key)
            out["certificate"] = "made"
        args += ["--listen", f"0.0.0.0:{port}", "--tls-cert", str(cert), "--tls-key", str(key)]
        out.update(listen=f"0.0.0.0:{port}", address=str(ip), fp=fingerprint(cert.read_bytes()))
    cfg["hub_args"] = args
    tray.parent.mkdir(parents=True, exist_ok=True)
    tray.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    return out


def runs_under_tray(cluster_path: str | Path) -> bool:
    """Whether the tray starts this hub (tray.json names its cluster file): then a hub that stops is started again."""
    try:
        cfg = json.loads((potluck_dir() / "tray.json").read_text(encoding="utf-8-sig"))
        named = cfg.get("cluster") if isinstance(cfg, dict) else None
        return bool(named) and Path(named).resolve() == Path(cluster_path).resolve()
    except (OSError, ValueError):
        return False
