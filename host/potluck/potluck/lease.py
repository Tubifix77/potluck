"""M14: the cell's leases -- one resident hub per cell, one `potluck apply` at a time (firmware: pot/lease.hpp).

Two PCs on cables to two different boards must not both deploy (they would race for one rollback counter) and must
not both run a resident hub. The lowest-id live board holds the leases; the board on our cable finds it (target 0)
and passes the request on. A lease is held by a random token for a time to live, renewed at a third of it; a
request for one someone else holds is refused with the holder's name.

    with CellLease.take(bridge, KIND_DEPLOY, "apply on desk-pc") as lease:
        ...                       # lease.lost turns True if another holder took it after the arbiter changed

A cell that does not answer LEASE at all (firmware before M14) gets no lease: take() returns a CellLease with
`supported` False and says so, rather than refusing -- the firmware that adds leases has to be deployable.
"""

from __future__ import annotations

import secrets
import socket
import struct
import threading
import time
from dataclasses import dataclass

from . import frame as fr

KIND_DEPLOY = 1
KIND_HUB = 2
KIND_NAMES = {KIND_DEPLOY: "deploy", KIND_HUB: "hub"}
OP_TAKE, OP_RELEASE, OP_QUERY = 1, 2, 3
STATUS_OK, STATUS_HELD, STATUS_MALFORMED, STATUS_UNREACHABLE = 0, 1, 2, 3
STATUS_NAMES = {0: "ok", 1: "held", 2: "malformed", 3: "unreachable"}
NAME_MAX = 31
DEFAULT_TTL_S = 60


def holder_name(tool: str) -> str:
    """What a refused claimant is told: the tool and the PC, e.g. "apply on DESKTOP-7" (31 characters at most)."""
    host = socket.gethostname() or "a PC"
    name = "".join(c if 0x20 <= ord(c) <= 0x7E and c not in '"\\' else "?" for c in f"{tool} on {host}")
    return name[:NAME_MAX]


def encode_request(kind: int, op: int, token: int, *, ttl_s: int = 0, name: str = "", target: int = 0) -> bytes:
    raw = name.encode("ascii", "replace")[:NAME_MAX]
    return struct.pack("<HBBHIB", target, kind, op, ttl_s, token, len(raw)) + raw


@dataclass
class LeaseReply:
    status: int
    kind: int
    arbiter: int
    remaining_ms: int
    token: int
    name: str

    @classmethod
    def parse(cls, raw: bytes) -> "LeaseReply":
        if len(raw) < 13:
            raise ValueError(f"a lease reply is at least 13 bytes, got {len(raw)}")
        status, kind, arbiter, remaining, token, n = struct.unpack_from("<BBHIIB", raw, 0)
        if len(raw) < 13 + n:
            raise ValueError("lease reply's name is cut short")
        return cls(status, kind, arbiter, remaining, token, raw[13:13 + n].decode("ascii", "replace"))

    def holder_line(self) -> str:
        return (f"held by \"{self.name}\" (on board 0x{self.arbiter:04x}, {self.remaining_ms / 1000:.0f} s left "
                f"unless renewed)")


class LeaseRefused(Exception):
    """Someone else holds the lease; the message names them."""

    def __init__(self, kind: int, reply: LeaseReply):
        self.reply = reply
        super().__init__(f"the cell's {KIND_NAMES.get(kind, kind)} lease is {reply.holder_line()}")


class LeaseUnavailable(Exception):
    """The cell answers LEASE but its arbiter does not (stalled, rebooting, or busy passing another request on)."""


def request(bridge, kind: int, op: int, token: int, *, ttl_s: int = 0, name: str = "",
            timeout: float = 3.0) -> LeaseReply:
    raw = bridge.deploy_step(fr.Op.LEASE, encode_request(kind, op, token, ttl_s=ttl_s, name=name), timeout=timeout)
    return LeaseReply.parse(raw)


class CellLease:
    """A lease this program holds, renewed on a thread until released (or the `with` block ends)."""

    def __init__(self, bridge, kind: int, name: str, ttl_s: int, token: int, supported: bool,
                 reply: LeaseReply | None):
        self.bridge, self.kind, self.name, self.ttl_s, self.token = bridge, kind, name, ttl_s, token
        self.supported = supported
        self.reply = reply
        self.lost: LeaseReply | None = None  # set if a renewal found another holder
        self.renewals = 0
        self.renew_failures = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    @classmethod
    def take(cls, bridge, kind: int, name: str, *, ttl_s: int = DEFAULT_TTL_S, tries: int = 3,
             strict: bool = True, backoff_s: float = 0.5, log=print) -> "CellLease":
        """Take the lease. Someone else holding it: LeaseRefused. No reply at all, every try: a cell before M14 --
        noted, and a CellLease with `supported` False. A reply saying the arbiter is unreachable (found by review: a
        stalled lowest board, or two requests meeting at the cabled board's one proxy slot) is retried with back-off,
        then `strict` raises LeaseUnavailable and otherwise the renewal thread keeps trying -- never mistaken for old
        firmware, so exclusion is not dropped silently."""
        token = secrets.randbits(32) or 1
        last: Exception | None = None
        unreachable: LeaseReply | None = None
        for i in range(tries + 2):
            if i >= tries and unreachable is None:
                break  # the extra tries are for an arbiter that answered Unreachable
            try:
                rep = request(bridge, kind, OP_TAKE, token, ttl_s=ttl_s, name=name)
            except Exception as exc:  # a timeout: the board ignored LEASE (before M14), or the link hiccupped
                last = exc
                continue
            if rep.status == STATUS_HELD:
                raise LeaseRefused(kind, rep)
            if rep.status == STATUS_OK:
                lease = cls(bridge, kind, name, ttl_s, token, True, rep)
                lease._start()
                return lease
            if rep.status == STATUS_UNREACHABLE:
                unreachable = rep
                time.sleep(backoff_s * (2 ** min(i, 3)))
                continue
            last = RuntimeError(f"lease {STATUS_NAMES.get(rep.status, rep.status)} from board 0x{rep.arbiter:04x}")
        if unreachable is not None:
            what = (f"the cell's {KIND_NAMES.get(kind, kind)} lease is kept by board 0x{unreachable.arbiter:04x}, "
                    f"which does not answer")
            if strict:
                raise LeaseUnavailable(what + " -- try again when it does (or take it out of the cell)")
            log(f"warning: {what}; going on and asking again every {ttl_s / 3:.0f} s")
            lease = cls(bridge, kind, name, ttl_s, token, True, None)
            lease._start()
            return lease
        log(f"note: the cell gives no {KIND_NAMES.get(kind, kind)} lease ({last}) -- firmware before M14? "
            f"Going on without one: nothing stops a second one at the same time")
        return cls(bridge, kind, name, ttl_s, token, False, None)

    def _start(self) -> None:
        self._thread = threading.Thread(target=self._renew_loop, name=f"lease-{self.kind}", daemon=True)
        self._thread.start()

    def _renew_loop(self) -> None:
        while not self._stop.wait(self.ttl_s / 3):
            try:
                rep = request(self.bridge, self.kind, OP_TAKE, self.token, ttl_s=self.ttl_s, name=self.name)
            except Exception:
                self.renew_failures += 1  # the arbiter rebooting (a firmware rollout does that): try again later
                continue
            if rep.status == STATUS_HELD:
                self.lost = rep  # the arbiter changed and someone else took it there first
                return
            if rep.status == STATUS_OK:
                self.reply = rep
                self.renewals += 1
            else:
                self.renew_failures += 1

    def release(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5.0)
        if self.supported and self.lost is None:
            try:
                request(self.bridge, self.kind, OP_RELEASE, self.token, timeout=2.0)
            except Exception:
                pass  # it lapses at the end of its time to live
        self.supported = False

    def __enter__(self) -> "CellLease":
        return self

    def __exit__(self, *exc) -> None:
        self.release()
