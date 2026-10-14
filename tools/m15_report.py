#!/usr/bin/env python3
"""M15's bench [MEASURE]: does the hub's telemetry pull cost the cell its M0 figures?

    python tools/m15_report.py captures/m15      (reads <name>-{a,b,c}.jsonl and <name>-marks.txt)

The marks file names the phases ("<phase>-start <unix s>" lines, "phase3-end" last). For every phase and every
board-to-board link it prints heartbeat delivery (from the link records' hb_lost / bcast_frames deltas) and the
RTT p50/p99 of the probes answered within the phase (from the histogram's bucket deltas), so a phase is compared
on what happened in it, not on totals since boot.
"""

from __future__ import annotations

import gzip
import json
import os
import sys

# link_stats.hpp's kRttBucketEdgeUs: upper edges in microseconds; the last bucket is everything above.
BOUNDS = [1000, 2000, 3000, 4000, 6000, 8000, 11000, 16000, 22000, 30000, 42000, 60000, 85000, 110000, 200000, None]


def pct(hist: list[int], p: float) -> str:
    n = sum(hist)
    if n == 0:
        return "-"
    want = n * p
    acc = 0
    for i, c in enumerate(hist):
        acc += c
        if acc >= want:
            lo = 0 if i == 0 else BOUNDS[i - 1]
            hi = BOUNDS[i]
            return f"{lo / 1000:g}-{hi / 1000:g} ms" if hi else f">{lo / 1000:g} ms"
    return "-"


def lines(path: str):
    if not os.path.exists(path) and os.path.exists(path + ".gz"):  # committed captures are compressed
        return gzip.open(path + ".gz", "rt", encoding="utf-8")
    return open(path, encoding="utf-8")


def main(argv: list[str]) -> int:
    base = argv[0]
    marks = []
    for line in open(f"{base}-marks.txt", encoding="utf-8"):
        name, t = line.split()
        marks.append((name.replace("-start", ""), float(t)))
    phases = [(marks[i][0], marks[i][1], marks[i + 1][1]) for i in range(len(marks) - 1)]
    names = {"a": 0x6300, "b": 0x7368, "c": 0x8160}
    for phase, t0, t1 in phases:
        print(f"== {phase}  ({(t1 - t0) / 60:.1f} min)")
        tot_lost = tot_bc = 0
        for b, node in names.items():
            first: dict[int, dict] = {}
            last: dict[int, dict] = {}
            for line in lines(f"{base}-{b}.jsonl"):
                if '"link"' not in line:
                    continue
                r = json.loads(line)
                if r.get("rec") != "link" or not (t0 <= r["host_ts"] <= t1) or r.get("peer") == 254:
                    continue
                first.setdefault(r["peer"], r)
                last[r["peer"]] = r
            for peer, f in sorted(first.items()):
                l = last[peer]
                if l["epoch"] != f["epoch"]:
                    print(f"  {node:04x} <- {peer:04x}: the peer rebooted in the phase, skipped")
                    continue
                lost = l["rx"]["hb_lost"] - f["rx"]["hb_lost"]
                bc = l["rx"]["bcast_frames"] - f["rx"]["bcast_frames"]
                hist = [x - y for x, y in zip(l["rtt"]["hist"], f["rtt"]["hist"])]
                tot_lost += lost
                tot_bc += bc
                print(f"  {node:04x} <- {peer:04x}: heartbeats {bc + lost:6d}, lost {lost:4d} "
                      f"({100 * lost / max(1, bc + lost):.2f} %)   RTT n={sum(hist):4d} p50 {pct(hist, 0.5):>9} "
                      f"p99 {pct(hist, 0.99):>10}")
        print(f"  all links: heartbeat loss {100 * tot_lost / max(1, tot_lost + tot_bc):.2f} % "
              f"({tot_lost} of {tot_lost + tot_bc})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
