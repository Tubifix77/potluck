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
    for suffix, who in names.items():
        recs = load(f"{base}-{suffix}.jsonl")
        if not recs:
            continue
        if suffix == "host":
            offset = min((r["host_ts"] - r["up_ms"] / 1000.0 for r in recs if r["rec"] == "host"), default=None)
        else:
            offset = min((r["host_ts"] - r["up_ms"] / 1000.0 for r in recs if r["rec"] == "clk"), default=None)
        if offset is None:
            continue
        for r in recs:
            if r.get("rec") == "event" and r.get("kind") in ("actor_started", "actor_stopped"):
                starts.append((offset + r["at_ms"] / 1000.0, who, r["kind"], r.get("b", 0)))
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
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
