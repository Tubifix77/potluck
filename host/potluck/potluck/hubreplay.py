"""M16: the hub's recording, and a recorded session replayed through the dashboard's own tabs.

    python -m potluck.hub ... --record <dir>          (every pull period's model and the new events, hourly files)
    python -m potluck.hub --replay <file.jsonl.gz> [--api 127.0.0.1:8470] [--speed 1]

RECORDING. Each pull period the hub appends its model (the same JSON GET /api/model serves) and any new events to
`<dir>/hub-<YYYYmmdd-HH>.jsonl.gz`, one file an hour, and deletes files older than `--record-hours` (default 24) --
the retention, which the model states (`hub.retention`), so how much history is kept is decided and shown (M16's
acceptance, BEYOND-M12.md question 4). About half a megabyte an hour for the bench's three boards at a 2 s pull (measured, M0-LOG session 42).
The recording is the model, not the radio's frames: it replays what the dashboard showed, on the hub's clock; the
frames themselves are section 7.6's capture (potluck-capture), which replays the cell byte for byte.

REPLAY. The recording is served through the same API as a live hub (model, events, stream), with `hub.replay` in the
model saying it is one -- the file, its first and last moment, where playback is, its speed, paused or not -- so every
tab works on it unchanged. POST /api/replay {"seek_at": <ms>, "speed": <x>, "paused": <bool>} moves it. Nothing can be
written: there is no cell behind it.
"""

from __future__ import annotations

import gzip
import json
import os
import threading
import time
from collections import deque
from pathlib import Path


class Recorder:
    def __init__(self, directory: str, hours: int = 24) -> None:
        self.dir = Path(directory)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.hours = hours
        self._name = None
        self._f = None
        self.bytes = 0
        self.started_at = int(time.time() * 1000)
        self.last_seq = 0

    def _open(self, now: float) -> None:
        name = time.strftime("hub-%Y%m%d-%H.jsonl.gz", time.localtime(now))
        if name == self._name:
            return
        if self._f is not None:
            self._f.close()
        self._name = name
        self._f = gzip.open(self.dir / name, "at", encoding="utf-8")
        cutoff = now - self.hours * 3600
        for p in self.dir.glob("hub-*.jsonl.gz"):
            try:
                if p.stat().st_mtime < cutoff:
                    p.unlink()
            except OSError:
                pass

    def write(self, snapshot: dict, events: list[dict]) -> None:
        now = time.time()
        self._open(now)
        new = [e for e in events if e["seq"] > self.last_seq]
        if new:
            self.last_seq = new[-1]["seq"]
            self._f.write(json.dumps({"t": "events", "events": new}) + "\n")
        line = json.dumps({"t": "model", "m": snapshot}) + "\n"
        self._f.write(line)
        self._f.flush()
        self.bytes = sum(p.stat().st_size for p in self.dir.glob("hub-*.jsonl.gz"))

    def retention(self) -> dict:
        files = sorted(self.dir.glob("hub-*.jsonl.gz"))
        return {"recording": str(self.dir), "hours": self.hours, "files": len(files), "bytes": self.bytes,
                "since_file": files[0].name if files else None}

    def close(self) -> None:
        if self._f is not None:
            self._f.close()


def load(path: str) -> tuple[list[tuple[int, str]], list[dict]]:
    """A recording's models as (at, raw JSON) in order, and its events. A file being written may end mid-line."""
    models: list[tuple[int, str]] = []
    events: list[dict] = []
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as f:
        try:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if r.get("t") == "model":
                    models.append((int(r["m"]["at"]), json.dumps(r["m"])))
                elif r.get("t") == "events":
                    events.extend(r["events"])
        except (EOFError, OSError):
            pass  # a recording still being written ends without gzip's trailer
    return models, events


class ReplayModel:
    """What serve_api needs from a Model, played back from a recording."""

    cluster = None  # nothing is writable: there is no cell behind a replay

    def __init__(self, path: str, speed: float = 1.0) -> None:
        self.path = path
        self.models, self.all_events = load(path)
        if not self.models:
            raise ValueError(f"{path}: no model in it")
        self.lock = threading.Lock()
        self.pull_period_ms = 2000
        self.speed = speed
        self.paused = False
        self.first_at, self.last_at = self.models[0][0], self.models[-1][0]
        self._pos_at = self.first_at
        self._wall = time.monotonic()
        self.events: deque[dict] = deque(maxlen=5000)
        self.seq = 0
        self._ev_i = 0
        self._sync()

    def position_at(self) -> int:
        if self.paused:
            return self._pos_at
        at = self._pos_at + int((time.monotonic() - self._wall) * 1000 * self.speed)
        return min(at, self.last_at)

    def control(self, seek_at=None, speed=None, paused=None) -> None:
        with self.lock:
            self._pos_at = self.position_at()
            self._wall = time.monotonic()
            if seek_at is not None:
                self._pos_at = max(self.first_at, min(int(seek_at), self.last_at))
                self.events.clear()
                self._ev_i = 0
            if speed is not None:
                self.speed = max(0.1, min(float(speed), 120.0))
            if paused is not None:
                self.paused = bool(paused)
        self._sync()

    def _sync(self) -> None:
        """Events up to the playback position into the deque the API serves."""
        at = self.position_at()
        with self.lock:
            while self._ev_i < len(self.all_events) and self.all_events[self._ev_i]["at"] <= at:
                e = self.all_events[self._ev_i]
                self.events.append(e)
                self.seq = max(self.seq, e["seq"])
                self._ev_i += 1

    def snapshot(self) -> dict:
        self._sync()
        at = self.position_at()
        lo, hi = 0, len(self.models) - 1
        while lo < hi:  # the last model at or before the position
            mid = (lo + hi + 1) // 2
            if self.models[mid][0] <= at:
                lo = mid
            else:
                hi = mid - 1
        m = json.loads(self.models[lo][1])
        m.setdefault("hub", {})["replay"] = {
            "file": os.path.basename(self.path), "from_at": self.first_at, "to_at": self.last_at, "position_at": at,
            "speed": self.speed, "paused": self.paused or at >= self.last_at, "models": len(self.models)}
        self.pull_period_ms = m["hub"].get("pull_period_ms", 2000)
        return m

    def record(self, kind: str, board, detail: dict) -> None:
        pass  # a replay changes nothing


def serve_replay(path: str, api: str, speed: float, listen=None, sessions=None, tls=None) -> int:
    from .hub import serve_api

    model = ReplayModel(path, speed)
    serve_api(model, api)
    if listen:
        serve_api(model, listen, sessions=sessions, tls=tls)
    print(f"[hub] replaying {path} ({len(model.models)} models, "
          f"{(model.last_at - model.first_at) / 60000:.1f} min) at http://{api}/", flush=True)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        return 0
