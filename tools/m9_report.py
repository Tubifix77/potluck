#!/usr/bin/env python3
"""M9's report: the job's runs, and whether lending changed the lenders' own duties.

    python tools/m9_report.py captures/m9-<name>-COM3.jsonl captures/m9-<name>-COM4.jsonl [...COM5]

The first file is A's console from tools/m9_bench.py (its bench commands and mc_done lines mark the
windows); the others are lenders' consoles from tools/json_capture.py, on the same PC clock. For each
lender it compares the windows -- idle, A running alone, lending -- by what the lender must keep doing:
its heartbeat delivery and round trips to each peer (from its link records' cumulative counters and
histogram, differenced across the window, as tools/soak_report.py does), its own actors' output
(die_temp reads per second), and what it lent (mc_lend). A histogram percentile is a bucket interval,
never an interpolated number.
"""

from __future__ import annotations

import json
import sys

sys.path.insert(0, __file__.rsplit("tools", 1)[0] + "tools")
from soak_report import percentile_bucket  # noqa: E402


def load(path: str) -> list[dict]:
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return out


def windows(a: list[dict]) -> list[tuple[str, float, float, dict | None]]:
    """(kind, start, end, mc_done) from A's capture: idle before the first run and between runs."""
    w: list[tuple[str, float, float, dict | None]] = []
    t_first = next((r["host_ts"] for r in a if r["rec"] == "bench"), None)
    start = a[0]["host_ts"] if a else 0.0
    if t_first is not None:
        w.append(("idle", start, t_first, None))
    sent: tuple[str, float] | None = None
    last_end = None
    for r in a:
        if r["rec"] == "bench" and r["cmd"].startswith("POT! mc run "):
            if last_end is not None:
                w.append(("idle", last_end, r["host_ts"], None))
            sent = ("lend" if r["cmd"].endswith("1") else "alone", r["host_ts"])
        elif r["rec"] == "mc_done" and sent is not None:
            w.append((sent[0], sent[1], r["host_ts"], r))
            last_end = r["host_ts"]
            sent = None
    if last_end is not None and a:
        w.append(("idle", last_end, a[-1]["host_ts"], None))
    return w


def lender_window(recs: list[dict], t0: float, t1: float) -> dict:
    inside = [r for r in recs if t0 <= r["host_ts"] <= t1]
    out: dict = {"links": {}}
    by_peer: dict[int, list[dict]] = {}
    for r in inside:
        if r["rec"] == "link" and r.get("state") == "alive" and r.get("peer", 0) < 0xFFFE:
            by_peer.setdefault(r["peer"], []).append(r)
    for peer, rs in by_peer.items():
        if len(rs) < 2 or rs[0].get("epoch") != rs[-1].get("epoch"):
            continue
        f, l = rs[0], rs[-1]
        hist = [b - a for a, b in zip(f["rtt"]["hist"], l["rtt"]["hist"])]
        got = l["rx"]["bcast_frames"] - f["rx"]["bcast_frames"]
        lost = l["rx"]["hb_lost"] - f["rx"]["hb_lost"]
        out["links"][peer] = {
            "rtt_n": sum(hist),
            "p50": percentile_bucket(hist, 50),
            "p99": percentile_bucket(hist, 99),
            "hb_delivery": (got / (got + lost)) if got + lost > 0 else None,
        }
    die = [r for r in inside if r["rec"] == "die_temp"]
    if len(die) >= 2 and die[-1]["up_ms"] > die[0]["up_ms"]:
        out["die_reads_per_s"] = (die[-1]["reads"] - die[0]["reads"]) * 1000.0 / (die[-1]["up_ms"] - die[0]["up_ms"])
        out["die_errors"] = die[-1]["errors"] - die[0]["errors"]
    lend = [r for r in inside if r["rec"] == "mc_lend"]
    if lend:
        out["served"] = lend[-1]["served"] - lend[0]["served"]
        out["unit_ms"] = lend[-1].get("unit_ms")
    return out


def main(argv: list[str]) -> int:
    a = load(argv[0])
    print("== runs (board A)")
    for kind, t0, t1, done in windows(a):
        if done is not None:
            print(f"  {kind:5}  {done['elapsed_ms']:6} ms  local {done['local']:4} lent {done['lent']:4}  "
                  f"refused {done['refused']} lost {done['lost']}  hits {done['hits']}  pi {done['pi']:.7f}")
    runs = [d for _, _, _, d in windows(a) if d is not None]
    alone = sorted(d["elapsed_ms"] for d in runs if d["lend"] == 0)
    lent = sorted(d["elapsed_ms"] for d in runs if d["lend"] == 1)
    if alone and lent:
        print(f"  median alone {alone[len(alone) // 2]} ms, lending {lent[len(lent) // 2]} ms: "
              f"speedup {alone[len(alone) // 2] / lent[len(lent) // 2]:.2f}x; "
              f"results identical: {len({d['hits'] for d in runs}) == 1}")
    for path in argv[1:]:
        recs = load(path)
        node = next((r["node"] for r in recs if "node" in r), None)
        print(f"== lender {node:#06x} ({path})" if node is not None else f"== {path}")
        for kind, t0, t1, _ in windows(a):
            w = lender_window(recs, t0, t1)
            links = "  ".join(
                f"{p:#06x}: hb {v['hb_delivery'] * 100:.2f}% rtt p50 {v['p50']} p99 {v['p99']} (n {v['rtt_n']})"
                if v["hb_delivery"] is not None else f"{p:#06x}: -"
                for p, v in sorted(w["links"].items()))
            extra = ""
            if "die_reads_per_s" in w:
                extra += f"  die_temp {w['die_reads_per_s']:.2f}/s err {w['die_errors']}"
            if "served" in w:
                extra += f"  lent {w['served']} (unit {w['unit_ms']} ms)"
            print(f"  {kind:5} {t1 - t0:6.0f} s  {links}{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
