"""M25: the bulk channel, the hub's side -- a file the PC offers, pushed to the board's app that asked for it.

An app asks the PC's `file` service for a file by name (potluck.services); the hub answers with its size and pushes it
here: CALLs of the app's input, act/<actor>/in, each a chunk at an offset (pot/bulk.hpp), through the board on the PC's
cable (the hub's _call_app). WINDOW chunks are in flight at a time; the app answers every chunk with the offset it
expects next (a cumulative acknowledgement), and when those answers stop moving for RESEND_S the hub sends again from
there (go-back-N). A transfer with no progress for GIVE_UP_S has failed.
"""

from __future__ import annotations

import secrets
import struct
import threading
import time

VERSION = 1  # pot/bulk.hpp's kBulkVersion
CHUNK = 192  # pot/bulk.hpp's kBulkChunkMax
WINDOW = 3  # in flight at a time: the cabled board passes on four CALLs at most, its proxy table shared with writes
RESEND_S = 0.15  # as an input batch's (potluck.events): ~5 x the median answer through the cabled board
GIVE_UP_S = 15.0
#: Chunks a second, all transfers together: the channel's share for files. 2026-10-25 on the bench, unpaced (~78/s, 64 KB
#: in 4.5 s) cost every radio link 1.2-2.0 points of heartbeat loss -- M25's line is one. At 30/s 64 KB takes ~12 s.
RATE = 30.0


def chunk_args(xfer: int, offset: int, data: bytes) -> bytes:
    """A chunk's CALL arguments, as pot/bulk.hpp's parse_bulk_chunk reads them."""
    return struct.pack("<BHI", VERSION, xfer & 0xFFFF, offset) + data


class Transfer:
    def __init__(self, board: int, path_hash: int, name: str, data: bytes, now: float):
        self.board, self.path_hash, self.name, self.data = board, path_hash, name, data
        self.xfer = secrets.randbelow(0xFFFF) + 1
        self.acked = 0  # what the app has said it holds
        self.sent = 0  # how far the chunks out reach
        self.started = self.progress_at = now
        self.done_at: float | None = None
        self.failed = False
        self.resent = 0
        self.chunks = 0

    @property
    def state(self) -> str:
        return "done" if self.done_at is not None else "failed" if self.failed else "reading"

    def snapshot(self) -> dict:
        end = self.done_at if self.done_at is not None else self.progress_at
        secs = max(end - self.started, 1e-6)
        return {"board": f"{self.board:04x}", "file": self.name, "bytes": self.acked, "size": len(self.data),
                "kbps": round(self.acked * 8 / 1000 / secs, 1), "state": self.state, "chunks": self.chunks,
                "resent": self.resent, "ms": int(secs * 1000)}


class Bulk:
    """Every transfer, and the thread that sends their chunks through `send(board, path_hash, args) -> bool` (False: no
    way to reach the board now -- tried again)."""

    def __init__(self, send, clock=time.time, rate: float | None = RATE):
        self.send, self.clock = send, clock
        self.gap = 1.0 / rate if rate else 0.0  # None: unpaced (tests)
        self.next_at = 0.0  # when the next chunk may go
        self.transfers: list[Transfer] = []
        self.lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def start(self, board: int, path_hash: int, name: str, data: bytes) -> Transfer:
        """A new transfer to (board, path); one there already is replaced -- the app asked again."""
        t = Transfer(board, path_hash, name, data, self.clock())
        with self.lock:
            self.transfers = [x for x in self.transfers if (x.board, x.path_hash) != (board, path_hash)][-19:] + [t]
            if self._thread is None:
                self._thread = threading.Thread(target=self._run, name="potluck-bulk", daemon=True)
                self._thread.start()
        return t

    def by_path(self, path_hash: int) -> Transfer | None:
        with self.lock:
            return next((t for t in reversed(self.transfers) if t.path_hash == path_hash and t.state == "reading"), None)

    def ack(self, t: Transfer, expects: int) -> None:
        with self.lock:
            if expects > t.acked:
                t.acked = min(expects, len(t.data))
                t.progress_at = self.clock()
            if t.acked >= len(t.data) and t.done_at is None:
                t.done_at = self.clock()

    def fail(self, t: Transfer) -> None:
        with self.lock:
            t.failed = True

    def pump(self) -> None:
        now = self.clock()
        with self.lock:
            active = [t for t in self.transfers if t.state == "reading"]
        for t in active:
            with self.lock:
                if now - t.progress_at >= GIVE_UP_S:
                    t.failed = True
                    continue
                if t.sent > t.acked and now - t.progress_at >= RESEND_S:
                    t.sent = t.acked  # go back to what the app holds
                    t.resent += 1
                    t.progress_at = now  # one resend per quiet spell
                due = []
                while (t.sent < len(t.data) and t.sent - t.acked < WINDOW * CHUNK and
                       now >= self.next_at - 1e-9):
                    piece = t.data[t.sent:t.sent + CHUNK]
                    due.append((t.sent, piece))
                    t.sent += len(piece)
                    self.next_at = max(self.next_at, now - self.gap) + self.gap  # paced: RATE a second at most
            for offset, piece in due:
                if not self.send(t.board, t.path_hash, chunk_args(t.xfer, offset, piece)):
                    with self.lock:
                        t.sent = min(t.sent, offset)  # not sent: from here on the next pass
                    break
                with self.lock:
                    t.chunks += 1

    def _run(self) -> None:
        while not self._stop.wait(0.005):
            try:
                self.pump()
            except Exception:  # a send that raised: the next pass tries again
                pass

    def stop(self) -> None:
        self._stop.set()

    def snapshot(self) -> list[dict]:
        with self.lock:
            return [t.snapshot() for t in self.transfers]
