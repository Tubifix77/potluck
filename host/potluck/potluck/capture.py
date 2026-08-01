"""Capture writer -- ARCHITECTURE.md section 7.6's format.

section 7.6: "potluck-bridge tees every frame -- in both directions, with a host
timestamp -- to a rotating capture file." That bridge is M2. This is the same
content at M0's scale, written by the tool that is reading the serial port.

The host timestamp is the point. The node's own clock is in the record too, but
only the host's clock is one the host can vouch for, and section 7.6's value comes
from being able to line up two nodes' streams afterwards -- which needs a common
clock, and the host is the only one there is. Both are kept and both are labelled,
because the difference between them is exactly the unsynchronised-clock problem
that link_stats.hpp refuses to paper over.

One JSON object per line: greppable, tail-able, streamable, and readable by
`potctl replay` when M2 arrives without a format conversion.
"""

from __future__ import annotations

import gzip
import json
import time
from pathlib import Path
from typing import Any, TextIO

#: Rotate at this size by default. 64 MiB of these lines is roughly a day of a
#: two-node soak with the frame tee on, so a soak produces a handful of files
#: rather than one unopenable one.
DEFAULT_ROTATE_BYTES = 64 * 1024 * 1024


class CaptureWriter:
    """Writes capture lines, rotating by size.

    Every line is flushed. A soak that ends in a power cut and leaves a buffered,
    empty capture file has produced nothing, and buffering saves nothing worth
    having at forty lines a second.
    """

    def __init__(
        self,
        path: str | Path,
        *,
        rotate_bytes: int = DEFAULT_ROTATE_BYTES,
        compress: bool = False,
    ) -> None:
        self.base = Path(path)
        self.rotate_bytes = rotate_bytes
        self.compress = compress
        self.serial = 0
        self.written_lines = 0
        self.written_bytes = 0
        self._fh: TextIO | None = None
        self._current: Path | None = None
        self._open_next()

    # -- lifecycle ----------------------------------------------------------
    def _current_path(self) -> Path:
        stem = self.base.stem
        suffix = self.base.suffix or ".jsonl"
        name = f"{stem}{suffix}" if self.serial == 0 else f"{stem}.{self.serial:03d}{suffix}"
        if self.compress:
            name += ".gz"
        return self.base.parent / name

    def _open_next(self) -> None:
        self.close()
        self.base.parent.mkdir(parents=True, exist_ok=True)
        self._current = self._current_path()
        if self.compress:
            self._fh = gzip.open(self._current, "at", encoding="utf-8", newline="\n")
        else:
            self._fh = open(self._current, "a", encoding="utf-8", newline="\n")
        self.written_bytes = 0
        self.serial += 1

    def close(self) -> None:
        if self._fh is not None:
            self._fh.flush()
            self._fh.close()
            self._fh = None

    def __enter__(self) -> CaptureWriter:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    @property
    def path(self) -> Path | None:
        return self._current

    # -- writing ------------------------------------------------------------
    def _write(self, obj: dict[str, Any]) -> None:
        assert self._fh is not None
        # separators drops the spaces json.dumps adds by default; over a day of
        # capture that is real disk for no information.
        line = json.dumps(obj, separators=(",", ":")) + "\n"
        self._fh.write(line)
        self._fh.flush()
        self.written_lines += 1
        self.written_bytes += len(line)
        if self.written_bytes >= self.rotate_bytes:
            self._open_next()

    def write_frame(
        self,
        *,
        node_id: int | None,
        direction: str,
        raw: bytes,
        peer_mac: str = "",
        rssi: int = 0,
        node_ts_us: int | None = None,
        host_ts: float | None = None,
    ) -> None:
        """Record one frame in section 7.6's format: host timestamp, direction, raw frame."""
        self._write(
            {
                "host_ts": host_ts if host_ts is not None else time.time(),
                "rec": "frame",
                "node": node_id,
                "dir": direction,
                "peer_mac": peer_mac,
                "rssi": rssi,
                "node_ts_us": node_ts_us,
                "raw": raw.hex(),
            }
        )

    def write_record(self, kind: str, data: dict[str, Any], *, host_ts: float | None = None) -> None:
        """Record a statistics line. Not section 7.6 content, but it belongs in
        the same file: reconstructing what a node believed at a given moment
        needs its counters next to its frames, not in a separate file with a
        separate clock.

        THE ENVELOPE KEY IS `rec`, NOT `kind`, AND THAT IS NOT COSMETIC.

        It was `kind` once, and the payload was splatted in after it — so any record carrying its own
        field called `kind` silently overwrote the envelope and became unidentifiable. Two record types
        do exactly that: `ns` carries the resource kind (`sampled` / `event`) and `event` carries the
        event kind (`peer_dead` …). Every such line written to a capture was corrupt, and nothing said
        so; the loss only surfaced when a replay of a capture full of `ns` records produced an empty
        namespace.

        Note the console stream never had this problem — there the envelope key is `t` — so the bug was
        introduced purely by translating `t` to a name a payload might also use. Hence `rec`, which is
        not a field any record emits, and the envelope written *after* the payload so that even a future
        collision cannot win.
        """
        self._write(
            {
                **data,
                "host_ts": host_ts if host_ts is not None else time.time(),
                "rec": kind,
            }
        )

    def write_log(self, text: str, *, host_ts: float | None = None) -> None:
        """Record a node log line, so a panic or a warning lands in the capture
        beside the traffic that preceded it."""
        self._write(
            {
                "host_ts": host_ts if host_ts is not None else time.time(),
                "rec": "log",
                "text": text,
            }
        )


def read_capture(path: str | Path):
    """Iterate a capture file, transparently handling .gz.

    Yields dicts. A corrupt line is skipped rather than raising: a capture
    truncated by a power cut is still worth everything before the cut.
    """
    p = Path(path)
    opener = gzip.open if p.suffix == ".gz" else open
    with opener(p, "rt", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue
