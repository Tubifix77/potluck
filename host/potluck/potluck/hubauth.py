"""M16: the hub's login -- who may look, who may write, and that nobody deploys through it.

    python -m potluck.hubauth add <name> --right look|write     (asks for the password twice; prints neither)
    python -m potluck.hubauth list
    python -m potluck.hubauth remove <name>
    python -m potluck.hubauth tokens                  (M17: programs paired with the hub, such as Home Assistant)
    python -m potluck.hubauth revoke <name>

The hub's API on 127.0.0.1 is the PC's own user and needs no login. On any other address (`potluck.hub --listen
0.0.0.0:8443`, a house's or a vehicle's network) it is served only over TLS, and every request but the login page
itself needs a session from a login in this file. Three rights, kept apart (ARCHITECTURE section 13, M16):

  look    every GET: the model, events, the stream
  write   look, and POST /api/write -- only what the package lets the outside write, checked again by the board
  deploy  not here at all: `potluck apply` runs on the PC that holds the owner's keys, never through the hub

Passwords are stored as scrypt (RFC 7914) at OWASP's first recommended setting, N=2^17, r=8, p=1, with a 16-byte
random salt each (the ledger, 2026-10-14). Sessions are random 256-bit tokens in an HttpOnly, Secure, SameSite=Strict
cookie, valid 30 days and kept across a restart of the hub in hub-sessions.json beside the logins -- the tokens'
SHA-256 only, never a token (2026-10-19: every reinstall and PC restart logged the owner's phone out). Five failed logins from
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
SESSION_S = 30 * 24 * 3600
FAIL_WINDOW_S, FAIL_LIMIT, LOCKOUT_S = 300, 5, 60
COOKIE = "potluck_session"


def users_file() -> Path:
    if os.environ.get("POTLUCK_HUB_USERS"):  # a test's own file, never the owner's
        return Path(os.environ["POTLUCK_HUB_USERS"])
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/.cache")
    return Path(base) / "potluck" / "hub-users.json"


def sessions_file() -> Path:
    """Where the hub keeps its sessions across a restart: beside the logins (users_file)."""
    return users_file().with_name("hub-sessions.json")


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
            doc = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            doc = {}
        self.users: dict[str, dict] = doc.get("users", {})
        # M17: paired programs' tokens -- 256 random bits each, kept as SHA-256 (no slow hash needed for those).
        self.tokens: dict[str, dict] = doc.get("tokens", {})

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"users": self.users, "tokens": self.tokens}, indent=1), encoding="utf-8")
        os.replace(tmp, self.path)

    def add_token(self, name: str, right: str) -> str:
        if right not in RIGHTS:
            raise ValueError(f"right must be one of {', '.join(RIGHTS)}")
        token = secrets.token_urlsafe(32)
        base, k = name, 2
        while name in self.tokens:  # a second Home Assistant gets "Home Assistant 2", never replaces the first
            name, k = f"{base} {k}", k + 1
        self.tokens[name] = {"right": right, "sha256": hashlib.sha256(token.encode()).hexdigest(),
                             "at": int(time.time())}
        return token

    def check_token(self, token: str) -> tuple[str, str] | None:
        try:  # a revoke from the command line counts at once: re-read the file when it changed
            mtime = self.path.stat().st_mtime
            if mtime != getattr(self, "_mtime", None):
                self._mtime = mtime
                self.tokens = json.loads(self.path.read_text(encoding="utf-8")).get("tokens", {})
        except (OSError, ValueError):
            pass
        h = hashlib.sha256(token.encode()).hexdigest()
        for name, rec in self.tokens.items():
            if hmac.compare_digest(rec.get("sha256", ""), h):
                return name, rec["right"]
        return None

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

    def __init__(self, users: Users, path: Path | None = None) -> None:
        self.users = users
        self.lock = threading.Lock()
        # SHA-256 of the token -> (name, right, expires as wall-clock seconds); kept in `path` when given
        self.sessions: dict[str, tuple[str, str, float]] = {}
        self.fails: dict[str, list[float]] = {}
        self.path = path
        if path is not None:
            try:
                doc = json.loads(path.read_text(encoding="utf-8"))
                now = time.time()
                for h, s in (doc.get("sessions") or {}).items():
                    if isinstance(s, list) and len(s) == 3 and float(s[2]) > now and s[1] in RIGHTS:
                        self.sessions[str(h)] = (str(s[0]), str(s[1]), float(s[2]))
            except (OSError, ValueError, TypeError, AttributeError):
                pass  # none yet, or unreadable: everyone logs in again, nothing worse

    @staticmethod
    def _key(token: str) -> str:
        return hashlib.sha256(token.encode("utf-8")).hexdigest()

    def _save_locked(self) -> None:
        if self.path is None:
            return
        now = time.time()
        live = {h: list(s) for h, s in self.sessions.items() if s[2] > now}
        try:
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps({"sessions": live}), encoding="utf-8")
            os.replace(tmp, self.path)
        except OSError:
            pass  # kept in memory regardless: only a restart would forget it

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
            self.sessions[self._key(token)] = (name, right, time.time() + SESSION_S)
            self._save_locked()
            return token, right

    def get(self, token: str | None) -> tuple[str, str] | None:
        if not token:
            return None
        key = self._key(token)
        with self.lock:
            s = self.sessions.get(key)
            if s is None:
                return None
            if time.time() > s[2]:
                del self.sessions[key]
                self._save_locked()
                return None
            if self.users.users.get(s[0]) is None:  # a login removed since: its sessions go with it
                del self.sessions[key]
                self._save_locked()
                return None
            return s[0], s[1]

    def logout(self, token: str | None) -> None:
        with self.lock:
            self.sessions.pop(self._key(token or ""), None)
            self._save_locked()


class Pairing:
    """M17: a program asks for a token; the owner approves on the PC (HUB-API.md, "Announced on the network, and
    paired"). Requests live 300 s; at most 4 wait at once; an approved token is handed over once."""

    TTL_S, MAX_PENDING = 300, 4

    def __init__(self, users: Users) -> None:
        self.users = users
        self.lock = threading.Lock()
        self.reqs: dict[str, dict] = {}

    def _expire(self) -> None:
        now = time.monotonic()
        for k in [k for k, r in self.reqs.items() if now - r["t"] > self.TTL_S]:
            del self.reqs[k]

    def request(self, name: str, right: str, addr: str) -> str | None:
        name = (name or "a program")[:64]
        right = right if right in RIGHTS else "look"
        with self.lock:
            self._expire()
            if sum(1 for r in self.reqs.values() if r["state"] == "pending") >= self.MAX_PENDING:
                return None
            pid = secrets.token_urlsafe(12)
            self.reqs[pid] = {"name": name, "right": right, "from": addr, "at": int(time.time() * 1000),
                              "t": time.monotonic(), "state": "pending", "token": None}
            return pid

    def pending(self) -> list[dict]:
        with self.lock:
            self._expire()
            return [{"pair_id": k, "name": r["name"], "right": r["right"], "from": r["from"], "at": r["at"]}
                    for k, r in self.reqs.items() if r["state"] == "pending"]

    def answer(self, pid: str, approve: bool) -> bool:
        with self.lock:
            self._expire()
            r = self.reqs.get(pid)
            if r is None or r["state"] != "pending":
                return False
            if approve:
                r["token"] = self.users.add_token(r["name"], r["right"])
                self.users.save()
                r["state"] = "approved"
            else:
                r["state"] = "denied"
            return True

    def fetch(self, pid: str) -> dict | None:
        with self.lock:
            self._expire()
            r = self.reqs.get(pid)
            if r is None:
                return None
            out = {"state": r["state"]}
            if r["state"] == "approved":
                out.update(name=r["name"], right=r["right"], token=r["token"])
                del self.reqs[pid]  # handed over once
            elif r["state"] == "denied":
                del self.reqs[pid]
            return out


def bearer_token(header: str | None) -> str | None:
    if header and header.startswith("Bearer "):
        return header[7:].strip() or None
    return None


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
    sub.add_parser("tokens")
    a_rv = sub.add_parser("revoke")
    a_rv.add_argument("name")
    a = ap.parse_args(argv)
    u = Users()
    if a.cmd == "tokens":
        for name, rec in sorted(u.tokens.items()):
            print(f"{name}: {rec['right']} (paired {time.strftime('%Y-%m-%d %H:%M', time.localtime(rec.get('at', 0)))})")
        if not u.tokens:
            print(f"no paired programs ({u.path})")
        return 0
    if a.cmd == "revoke":
        if u.tokens.pop(a.name, None) is None:
            print(f"no paired program named {a.name}")
            return 1
        u.save()
        print(f"revoked {a.name}; a running hub refuses it from its next request")
        return 0
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
