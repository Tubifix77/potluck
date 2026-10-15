"""The owner's keys, encrypted at rest, and their backup -- M20 (Review of M20 #1 and #15; section 9.3 as amended).

    python -m potluck.keystore seal ../../keys                 # make keys/vault.json from the secret files there
    python -m potluck.keystore backup ../../keys --cluster ../../keys/desk-m14-hub.json --out potluck-backup.json
    python -m potluck.keystore restore potluck-backup.json --to <new keys folder>
    python -m potluck.keystore check ../../keys/vault.json     # ask the passphrase, say what the vault holds

WHAT IS SEALED

The secrets: the cluster CA's key (`ca.key`), the deploy key (`deploy.key`), any guest author's key on this PC
(`*.key`) and a placement host's identity (`*.id`). Sealed into one file, `vault.json`, under the owner's passphrase:
scrypt (RFC 7914) at the hub's own login setting -- N=2^17, r=8, p=1, a random 16-byte salt -- derives a 32-byte key,
and ChaCha20-Poly1305 encrypts and authenticates the whole set (a 12-byte random nonce; the vault's header is
authenticated too, so neither it nor the contents can be altered unnoticed). A wrong passphrase and a tampered file
fail the same way: nothing is returned.

A backup is the same sealed format holding everything a new PC needs to keep running the cell: the secrets, the
public halves and certificates beside them (`*.pub`, `*.cert`, `*.bcert`, `*.gcert`), the cell's node registry
(`nodes.json`), the cluster file, and the hub's logins if given.

Sealing never deletes the plaintext files: that is the owner's to do, once the vault is checked (`check`).

USING THE VAULT

`unlock(vault, passphrase)` opens it in this process's memory; `signing.read_key` then finds a key there when its file
is gone, and `lock()` forgets them. The hub unlocks for one owner action on 127.0.0.1 and locks after (M20); a command
on the console asks for the passphrase itself.
"""

from __future__ import annotations

import base64
import getpass
import hashlib
import json
import os
import sys
from pathlib import Path

VAULT_SCHEMA = "potluck-vault/1"
SCRYPT_N, SCRYPT_R, SCRYPT_P = 2 ** 17, 8, 1  # hubauth's setting (OWASP's first recommendation)
SCRYPT_MAXMEM = 256 * 1024 * 1024
MIN_PASSPHRASE = 10
SECRET_GLOBS = ("*.key", "*.id")
PUBLIC_GLOBS = ("*.pub", "*.cert", "*.bcert", "*.gcert", "nodes.json")


class KeystoreError(Exception):
    pass


def _kdf(passphrase: str, salt: bytes, n: int = SCRYPT_N, r: int = SCRYPT_R, p: int = SCRYPT_P) -> bytes:
    return hashlib.scrypt(passphrase.encode("utf-8"), salt=salt, n=n, r=r, p=p, maxmem=SCRYPT_MAXMEM, dklen=32)


def seal(files: dict[str, bytes], passphrase: str, kind: str = "vault", n: int = SCRYPT_N) -> dict:
    """Encrypt {name: content} under the passphrase. `n` is lowered only by tests."""
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

    if len(passphrase) < MIN_PASSPHRASE:
        raise KeystoreError(f"a passphrase of at least {MIN_PASSPHRASE} characters")
    salt, nonce = os.urandom(16), os.urandom(12)
    header = {"schema": VAULT_SCHEMA, "kind": kind, "kdf": {"scrypt": {"n": n, "r": SCRYPT_R, "p": SCRYPT_P},
                                                           "salt": salt.hex()}, "nonce": nonce.hex(),
              "names": sorted(files)}
    aad = json.dumps(header, sort_keys=True, separators=(",", ":")).encode()
    body = json.dumps({k: base64.b64encode(v).decode() for k, v in files.items()}, sort_keys=True).encode()
    ct = ChaCha20Poly1305(_kdf(passphrase, salt, n)).encrypt(nonce, body, aad)
    return dict(header, ct=base64.b64encode(ct).decode())


def open_sealed(doc: dict, passphrase: str) -> dict[str, bytes]:
    from cryptography.exceptions import InvalidTag
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

    if doc.get("schema") != VAULT_SCHEMA:
        raise KeystoreError("not a Potluck vault or backup")
    header = {k: doc[k] for k in ("schema", "kind", "kdf", "nonce", "names")}
    aad = json.dumps(header, sort_keys=True, separators=(",", ":")).encode()
    s = doc["kdf"]["scrypt"]
    key = _kdf(passphrase, bytes.fromhex(doc["kdf"]["salt"]), int(s["n"]), int(s["r"]), int(s["p"]))
    try:
        body = ChaCha20Poly1305(key).decrypt(bytes.fromhex(doc["nonce"]), base64.b64decode(doc["ct"]), aad)
    except InvalidTag:
        raise KeystoreError("wrong passphrase, or the file was changed") from None
    return {k: base64.b64decode(v) for k, v in json.loads(body).items()}


def collect(folder: Path, globs: tuple[str, ...]) -> dict[str, bytes]:
    out: dict[str, bytes] = {}
    for g in globs:
        for p in sorted(folder.glob(g)):
            if p.is_file():
                out[p.name] = p.read_bytes()
    return out


# ---- in this process: an unlocked vault ------------------------------------------------------------------------

_unlocked: dict[str, dict[str, bytes]] = {}  # folder -> {name: content}


def unlock(vault: str | Path, passphrase: str) -> list[str]:
    p = Path(vault).resolve()
    files = open_sealed(json.loads(p.read_text(encoding="utf-8")), passphrase)
    _unlocked[str(p.parent)] = files
    return sorted(files)


def lock() -> None:
    _unlocked.clear()


def lookup(path: str | Path) -> bytes | None:
    """A key file's content from an unlocked vault in its folder, or None."""
    p = Path(path).resolve()
    return _unlocked.get(str(p.parent), {}).get(p.name)


# ---- the command line ------------------------------------------------------------------------------------------

def _ask(prompt: str, twice: bool = False) -> str:
    pw = os.environ.get("POTLUCK_PASSPHRASE")  # for scripted tests only; never set it in a shell you keep history of
    if pw is not None:
        return pw
    pw = getpass.getpass(prompt)
    if twice and getpass.getpass("Again: ") != pw:
        raise KeystoreError("the two passphrases differ")
    return pw


def _flag(argv: list[str], name: str) -> str | None:
    return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else None


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if not argv:
            print(__doc__.split("\n\n")[1])
            return 2
        cmd = argv[0]
        if cmd == "seal":
            folder = Path(argv[1])
            files = collect(folder, SECRET_GLOBS)
            if not files:
                raise KeystoreError(f"{folder}: no *.key or *.id files to seal")
            doc = seal(files, _ask("Passphrase for the keys: ", twice=True))
            (folder / "vault.json").write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
            print(f"sealed {len(files)} file(s) into {folder / 'vault.json'}: {', '.join(sorted(files))}")
            print("the plaintext files are still there: delete them yourself once `check` opens the vault")
            return 0
        if cmd == "check":
            names = open_sealed(json.loads(Path(argv[1]).read_text(encoding="utf-8")), _ask("Passphrase: "))
            print(f"opens: {', '.join(sorted(names))}")
            return 0
        if cmd == "backup":
            folder = Path(argv[1])
            files = collect(folder, SECRET_GLOBS + PUBLIC_GLOBS)
            vault = folder / "vault.json"
            if vault.exists():
                files["vault.json"] = vault.read_bytes()
            for flag in ("--cluster", "--hub-users"):
                v = _flag(argv, flag)
                if v:
                    files[Path(v).name] = Path(v).read_bytes()
            out = _flag(argv, "--out") or "potluck-backup.json"
            doc = seal(files, _ask("Passphrase for the backup: ", twice=True), kind="backup")
            Path(out).write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
            print(f"backup of {len(files)} file(s) -> {out}: {', '.join(sorted(files))}")
            return 0
        if cmd == "restore":
            files = open_sealed(json.loads(Path(argv[1]).read_text(encoding="utf-8")), _ask("Passphrase: "))
            to = Path(_flag(argv, "--to") or ".")
            to.mkdir(parents=True, exist_ok=True)
            clash = [n for n in files if (to / n).exists()]
            if clash:
                raise KeystoreError(f"{to} already has {', '.join(clash)}: restore into an empty folder")
            for n, b in files.items():
                (to / n).write_bytes(b)
            print(f"restored {len(files)} file(s) into {to}")
            return 0
        raise KeystoreError(f"unknown command {cmd!r}: seal, check, backup or restore")
    except (KeystoreError, OSError, ValueError, KeyError) as exc:
        print(f"keystore: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
