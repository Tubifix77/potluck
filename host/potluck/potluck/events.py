"""M24: key and pointer events from the PC into the cell.

A program gives the hub events for an app (POST /api/input); the hub numbers them and delivers them in order to the
app's input, act/<actor>/in, as CALLs of batches (pot/input.hpp) through the cabled board -- one batch at a time, the next only after the board
answered the last with the number it took up to. A batch not answered in RESEND_S is sent again; the board drops what it
already took. Which events an app takes is its module's "takes" (a descriptor key; the key-echo test app takes both).
"""

from __future__ import annotations

import struct
import threading
import time
from collections import deque

KINDS = {"key": 1, "pointer": 2}  # pot/input.hpp's kInputKey, kInputPointer
VERSION = 1
BATCH_MAX = 24  # pot/input.hpp's kInputBatchMax
QUEUE_MAX = 512  # events waiting per app; beyond it a POST is refused (429), never silently dropped
RESEND_S = 0.5  # 2.5 x the host link's own p99 (200 ms: the cabled board's probe of the PC's node, 2026-10-25)
#: built-in modules that take events, as an external module says it with "takes" in its descriptor
BUILTIN_TAKES = {"builtin:key_echo": ("key", "pointer")}


class InputError(ValueError):
    pass


def check_events(events, takes: tuple[str, ...]) -> list[tuple[int, int, int, int]]:
    """A POST's events as (kind, flags, a, b), or InputError naming the first bad one. key: {"kind": "key", "code":
    0..65535 (USB HID usage, page 7), "down": bool}; pointer: {"kind": "pointer", "dx", "dy": -32768..32767,
    "buttons": 0..7}."""
    if not isinstance(events, list) or not 1 <= len(events) <= QUEUE_MAX:
        raise InputError(f"events is a list of 1..{QUEUE_MAX}")
    out = []
    for i, e in enumerate(events):
        kind = e.get("kind") if isinstance(e, dict) else None
        if kind not in KINDS:
            raise InputError(f"events[{i}]: kind is one of {sorted(KINDS)}")
        if kind not in takes:
            raise InputError(f"events[{i}]: this app does not take {kind} events (it takes {list(takes)})")

        def num(name, lo, hi, default=None):
            v = e.get(name, default)
            if isinstance(v, bool) or not isinstance(v, int) or not lo <= v <= hi:
                raise InputError(f"events[{i}]: {name} is a whole number {lo}..{hi}")
            return v
        if kind == "key":
            down = e.get("down", True)
            if not isinstance(down, bool):
                raise InputError(f"events[{i}]: down is true or false")
            out.append((KINDS[kind], 1 if down else 0, num("code", 0, 0xFFFF), 0))
        else:
            out.append((KINDS[kind], num("buttons", 0, 7, 0), num("dx", -0x8000, 0x7FFF, 0),
                        num("dy", -0x8000, 0x7FFF, 0)))
    return out


def encode_batch(batch: list[tuple]) -> bytes:
    """The CALL's arguments for (seq, kind, flags, a, b, ...) events, as pot/input.hpp parses them."""
    if not 1 <= len(batch) <= BATCH_MAX:
        raise InputError(f"a batch is 1..{BATCH_MAX} events")
    out = bytes([VERSION, len(batch)])
    for seq, kind, flags, a, b, *_ in batch:
        out += struct.pack("<HBBhh", seq & 0xFFFF, kind, flags, a if a < 0x8000 else a - 0x10000, b)
    return out


def _behind_or_at(seq: int, last: int) -> bool:
    """seq is at or before last, modulo 2^16 (as pot/input.hpp's InputOrder counts)."""
    return ((last - seq) & 0xFFFF) < 0x8000


class InputQueue:
    """One app's events on their way: numbered, batched, sent one batch at a time, answered or sent again."""

    def __init__(self, app: str, owner: int, path_hash: int, takes: tuple[str, ...]):
        self.app, self.owner, self.path_hash, self.takes = app, owner, path_hash, takes
        self.lock = threading.Lock()
        self.waiting: deque = deque()  # (seq, kind, flags, a, b, enqueued_at)
        self.in_flight: list = []
        self.sent_at = 0.0
        self.again = False  # the batch out goes again on the next pass (not sent, or partly answered): no resend
        self.next_seq = 1
        self.queued = self.batches = self.acked = self.resent = self.refused = 0
        self.last_ack_at: float | None = None
        self.latencies: deque = deque(maxlen=2000)  # seconds from POST to the board's answer, per event

    def put(self, events: list[tuple[int, int, int, int]], now: float) -> int:
        """Queue events; the first one's number. InputError if they would not fit."""
        with self.lock:
            if len(self.waiting) + len(self.in_flight) + len(events) > QUEUE_MAX:
                raise InputError(f"{QUEUE_MAX} events are already waiting for this app")
            first = self.next_seq
            for ev in events:
                self.waiting.append((self.next_seq, *ev, now))
                self.next_seq = (self.next_seq + 1) & 0xFFFF
            self.queued += len(events)
            return first

    def due(self, now: float) -> bytes | None:
        """The batch to send now, if any: a new one when nothing is out, the same one again when its answer is late."""
        with self.lock:
            if self.in_flight:
                if self.again:
                    self.again = False
                elif now - self.sent_at < RESEND_S:
                    return None
                else:
                    self.resent += 1
            else:
                if not self.waiting:
                    return None
                while self.waiting and len(self.in_flight) < BATCH_MAX:
                    self.in_flight.append(self.waiting.popleft())
                self.batches += 1
            self.sent_at = now
            return encode_batch(self.in_flight)

    def ack(self, last: int, now: float) -> int:
        """The board took everything up to `last`: those leave; the count that did."""
        with self.lock:
            done = [e for e in self.in_flight if _behind_or_at(e[0], last)]
            self.in_flight = [e for e in self.in_flight if not _behind_or_at(e[0], last)]
            for e in done:
                self.latencies.append(now - e[5])
            self.acked += len(done)
            self.last_ack_at = now
            if done and self.in_flight:
                self.again = True  # the rest of a part-answered batch goes again at once
            return len(done)

    def refuse(self) -> None:
        """The board refused the batch (no such app there, or not a batch it reads): it is dropped, and counted."""
        with self.lock:
            self.refused += len(self.in_flight)
            self.in_flight = []

    def snapshot(self) -> dict:
        with self.lock:
            lat = sorted(self.latencies)

            def pct(p):
                return round(lat[min(len(lat) - 1, int(p * len(lat)))] * 1000, 1) if lat else None
            return {"app": self.app, "board": f"{self.owner:04x}", "takes": list(self.takes), "queued": self.queued,
                    "waiting": len(self.waiting) + len(self.in_flight), "batches": self.batches, "taken": self.acked,
                    "resent": self.resent, "refused": self.refused, "p50_ms": pct(0.50), "p99_ms": pct(0.99),
                    "last_answer_at": int(self.last_ack_at * 1000) if self.last_ack_at else None}


class Inputs:
    """Every app's queue, and the thread that sends their batches through `send(owner, path_hash, args) -> bool`
    (False: no way to reach the board now -- kept, tried again)."""

    def __init__(self, send, clock=time.time):
        self.send, self.clock = send, clock
        self.queues: dict[str, InputQueue] = {}
        self.lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def queue(self, app: str, owner: int, path_hash: int, takes: tuple[str, ...]) -> InputQueue:
        with self.lock:
            q = self.queues.get(app)
            if q is None or (q.owner, q.path_hash) != (owner, path_hash):
                q = self.queues[app] = InputQueue(app, owner, path_hash, takes)
            if self._thread is None:
                self.start()  # the sender runs once there is something to send
            return q

    def by_path(self, path_hash: int) -> InputQueue | None:
        """The queue whose app's input this is (an input path names its actor, so it is one queue's)."""
        with self.lock:
            return next((q for q in self.queues.values() if q.path_hash == path_hash), None)

    def pump(self) -> None:
        now = self.clock()
        with self.lock:
            qs = list(self.queues.values())
        for q in qs:
            data = q.due(now)
            if data is not None and not self.send(q.owner, q.path_hash, data):
                with q.lock:
                    q.again = True  # not sent: tried again on the next pass

    def start(self) -> None:
        def run():
            while not self._stop.wait(0.01):
                try:
                    self.pump()
                except Exception:  # a send that raised: the next pass tries again
                    pass
        self._thread = threading.Thread(target=run, name="potluck-input", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def snapshot(self) -> list[dict]:
        with self.lock:
            return [q.snapshot() for q in self.queues.values()]
