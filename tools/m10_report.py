#!/usr/bin/env python3
"""M10's report: who ran the portable actor when, on one clock, and whether it ever ran twice.

    python tools/m10_report.py captures/<name>   (reads <name>-host.jsonl and <name>-COM3/4/5.jsonl)

A board prints its events only when its statistics period comes round (every 10 s), so an event's
host_ts is when the line was read, not when it happened. Each board's events are put on the PC's clock
from their at_ms, by that board's own offset: its "clk" lines are printed first in a period, so
host_ts - up_ms is that board's clock offset plus the serial delay of one short line (taken as the
minimum over the run). The PC's own events come with host_ts within a few ms. Prints the ownership
timeline -- every actor_started / actor_stopped -- and the longest time two nodes ran the actor at once.
"""

from __future__ import annotations

import json
import sys


def load(path: str) -> list[dict]:
    out = []
    try:
        for line in open(path, encoding="utf-8", errors="replace"):
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    except FileNotFoundError:
        pass
    return out


def main(argv: list[str]) -> int:
    base = argv[0]
    names = {"host": "PC", "COM3": "A", "COM4": "B", "COM5": "C"}
    starts: list[tuple[float, str, str, int]] = []  # (t, who, kind, detail)
    failovers: list[tuple[str, int, int | None]] = []  # (board, ms since the PC's last frame, ms to start)
    # A PC that is killed or unplugged logs no actor_stopped: it stopped at its last frame.
    pc_gone: list[float] = []
    for suffix, who in names.items():
        recs = load(f"{base}-{suffix}.jsonl")
        if not recs:
            continue
        # The PC prints each event as it happens, so its host_ts is the time (and every run of the
        # PC process restarts its clock); a board's events need its offset.
        offset = None if suffix == "host" else min(
            (r["host_ts"] - r["up_ms"] / 1000.0 for r in recs if r["rec"] == "clk"), default=None)
        if suffix != "host" and offset is None:
            continue
        deaths = []
        for r in recs:
            if r.get("rec") != "event":
                continue
            t = r["host_ts"] if offset is None else offset + r["at_ms"] / 1000.0
            if r.get("kind") in ("actor_started", "actor_stopped"):
                starts.append((t, who, r["kind"], r.get("b", 0)))
            if r.get("kind") == "peer_dead" and r.get("peer") == 254 and offset is not None:
                deaths.append((r["at_ms"], r.get("b", 0)))
                pc_gone.append(offset + (r["at_ms"] - r.get("b", 0)) / 1000.0)  # its last frame, as this board heard it
        for at, since_rx in deaths:
            took = next((e["at_ms"] - at for e in recs if e.get("rec") == "event" and e.get("kind") == "actor_started"
                         and 0 <= e["at_ms"] - at <= 3000), None)
            failovers.append((who, since_rx, took))
    for t in sorted(pc_gone):
        if not any(abs(t - x[0]) < 2.0 and x[1] == "PC" and x[2] == "actor_stopped" for x in starts):
            starts.append((t, "PC", "actor_stopped", "gone"))
    starts.sort()
    running: dict[str, float] = {}
    overlap_max = 0.0
    overlap_at = None
    t0 = starts[0][0] if starts else 0.0
    for t, who, kind, detail in starts:
        if kind == "actor_started":
            running[who] = t
        else:
            running.pop(who, None)
        if len(running) > 1:
            began = max(running.values())
            # how long until one of them stops
            nxt = next((tt for tt, w, k, _ in starts if tt > t and k == "actor_stopped" and w in running), None)
            if nxt is not None and nxt - began > overlap_max:
                overlap_max, overlap_at = nxt - began, began
        print(f"  {t - t0:8.3f} s  {who:2}  {kind:13}  {'term' if kind == 'actor_started' else 'why'} {detail}")
    print(f"longest overlap of two runners: {overlap_max * 1000:.0f} ms" + (f" at {overlap_at - t0:.3f} s" if overlap_at else ""))
    for who, since_rx, took in failovers:
        if took is not None:
            print(f"  failover: {who} declared the PC dead {since_rx} ms after its last frame and started it "
                  f"{took} ms later: {since_rx + took} ms")
        else:
            print(f"  {who} declared the PC dead {since_rx} ms after its last frame (it started nothing: not its turn)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
