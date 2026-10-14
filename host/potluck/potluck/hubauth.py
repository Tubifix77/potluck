"""M16: the hub's login -- who may look, who may write, and that nobody deploys through it.

    python -m potluck.hubauth add <name> --right look|write     (asks for the password twice; prints neither)
    python -m potluck.hubauth list
    python -m potluck.hubauth remove <name>

The hub's API on 127.0.0.1 is the PC's own user and needs no login. On any other address (`potluck.hub --listen
0.0.0.0:8443`, a house's or a vehicle's network) it is served only over TLS, and every request but the login page
itself needs a session from a login in this file. Three rights, kept apart (ARCHITECTURE section 13, M16):

  look    every GET: the model, events, the stream
  write   look, and POST /api/write -- only what the package lets the outside write, checked again by the board
  deploy  not here at all: `potluck apply` runs on the PC that holds the owner's keys, never through the hub

Passwords are stored as scrypt (RFC 7914) at OWASP's first recommended setting, N=2^17, r=8, p=1, with a 16-byte
random salt each (the ledger, 2026-10-14). Sessions are random 256-bit tokens in an HttpOnly, Secure, SameSite=Strict
cookie, held in the hub's memory for 12 hours: a restarted hub asks everyone to log in again. Five failed logins from
one address in five minutes refuse that address for a minute.
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import hmac
import json
import os
import secrets
import sys
import threading
import time
from pathlib import Path

RIGHTS = ("look", "write")
SCRYPT_N, SCRYPT_R, SCRYPT_P = 2 ** 17, 8, 1
SCRYPT_MAXMEM = 256 * 1024 * 1024  # N=2^17, r=8 needs 128 MiB; OpenSSL's default cap is 32 MiB
SESSION_S = 12 * 3600
FAIL_WINDOW_S, FAIL_LIMIT, LOCKOUT_S = 300, 5, 60
COOKIE = "potluck_session"


def users_file() -> Path:
    if os.environ.get("POTLUCK_HUB_USERS"):  # a test's own file, never the owner's
        return Path(os.environ["POTLUCK_HUB_USERS"])
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/.cache")
    return Path(base) / "potluck" / "hub-users.json"


def hash_password(password: str, salt: bytes | None = None, n: int = SCRYPT_N) -> dict:
    salt = salt if salt is not None else os.urandom(16)
    h = hashlib.scrypt(password.encode("utf-8"), salt=salt, n=n, r=SCRYPT_R, p=SCRYPT_P, maxmem=SCRYPT_MAXMEM,
                       dklen=32)
    return {"scrypt": {"n": n, "r": SCRYPT_R, "p": SCRYPT_P}, "salt": salt.hex(), "hash": h.hex()}


def check_password(rec: dict, password: str) -> bool:
    s = rec.get("scrypt") or {}
    try:
        if len(password) > 1024:  # Python's docs: limit a password to a sensible length
            return False
        h = hashlib.scrypt(password.encode("utf-8"), salt=bytes.fromhex(rec["salt"]), n=int(s["n"]),
                           r=int(s["r"]), p=int(s["p"]), maxmem=SCRYPT_MAXMEM, dklen=32)
    except (KeyError, ValueError):
        return False
    return hmac.compare_digest(h.hex(), rec.get("hash", ""))


class Users:
    def __init__(self, path: Path | None = None) -> None:
        self.path = path or users_file()
        try:
            self.users: dict[str, dict] = json.loads(self.path.read_text(encoding="utf-8")).get("users", {})
        except (OSError, ValueError):
            self.users = {}

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"users": self.users}, indent=1), encoding="utf-8")
        os.replace(tmp, self.path)

    def add(self, name: str, right: str, password: str, n: int = SCRYPT_N) -> None:
        if right not in RIGHTS:
            raise ValueError(f"right must be one of {', '.join(RIGHTS)} (deploy is never through the hub)")
        if not name or len(name) > 64 or any(c in name for c in ' "\\'):
            raise ValueError("a name of 1-64 characters, no spaces or quotes")
        if not 10 <= len(password) <= 1024:
            raise ValueError("a password of 10-1024 characters")
        self.users[name] = {"right": right, **hash_password(password, n=n)}

    def check(self, name: str, password: str) -> str | None:
        """The right of `name` if the password is its own, else None (the same work either way)."""
        rec = self.users.get(name)
        ok = check_password(rec if rec is not None else _DUMMY, password)
        return rec["right"] if rec is not None and ok else None


_DUMMY = {"scrypt": {"n": 2 ** 4, "r": SCRYPT_R, "p": 1}, "salt": "00" * 16, "hash": "00"}


class Sessions:
    """The hub's sessions and its failed-login counts, in memory."""

    def __init__(self, users: Users) -> None:
        self.users = users
        self.lock = threading.Lock()
        self.sessions: dict[str, tuple[str, str, float]] = {}  # token -> (name, right, expires)
        self.fails: dict[str, list[float]] = {}

    def login(self, name: str, password: str, addr: str) -> tuple[str | None, str]:
        now = time.monotonic()
        with self.lock:
            recent = [t for t in self.fails.get(addr, []) if now - t < FAIL_WINDOW_S]
            self.fails[addr] = recent
            if len(recent) >= FAIL_LIMIT and now - recent[-1] < LOCKOUT_S:
                return None, "too many failed logins from this address: wait a minute"
        right = self.users.check(name, password)
        with self.lock:
            if right is None:
                self.fails.setdefault(addr, []).append(now)
                return None, "wrong name or password"
            self.fails.pop(addr, None)
            token = secrets.token_urlsafe(32)
            self.sessions[token] = (name, right, now + SESSION_S)
            return token, right

    def get(self, token: str | None) -> tuple[str, str] | None:
        if not token:
            return None
        with self.lock:
            s = self.sessions.get(token)
            if s is None:
                return None
            if time.monotonic() > s[2]:
                del self.sessions[token]
                return None
            return s[0], s[1]

    def logout(self, token: str | None) -> None:
        with self.lock:
            self.sessions.pop(token or "", None)


def cookie_token(header: str | None) -> str | None:
    for part in (header or "").split(";"):
        k, _, v = part.strip().partition("=")
        if k == COOKIE:
            return v
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="potluck.hubauth")
    sub = ap.add_subparsers(dest="cmd", required=True)
    a_add = sub.add_parser("add")
    a_add.add_argument("name")
    a_add.add_argument("--right", choices=RIGHTS, required=True)
    sub.add_parser("list")
    a_rm = sub.add_parser("remove")
    a_rm.add_argument("name")
    a = ap.parse_args(argv)
    u = Users()
    if a.cmd == "list":
        for name, rec in sorted(u.users.items()):
            print(f"{name}: {rec['right']}")
        if not u.users:
            print(f"no logins ({u.path})")
        return 0
    if a.cmd == "remove":
        if u.users.pop(a.name, None) is None:
            print(f"no login named {a.name}")
            return 1
        u.save()
        print(f"removed {a.name}")
        return 0
    pw = getpass.getpass(f"password for {a.name}: ")
    if pw != getpass.getpass("again: "):
        print("the two did not match; nothing saved")
        return 1
    try:
        u.add(a.name, a.right, pw)
    except ValueError as exc:
        print(f"{exc}; nothing saved")
        return 1
    u.save()
    print(f"saved {a.name} ({a.right}) in {u.path}; a running hub reads it at its next start")
    return 0


if __name__ == "__main__":
    sys.exit(main())
