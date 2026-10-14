"""Namespace census: how many entries each board's table holds, and what they are.

Reads JSON-line captures (tools/json_capture.py, tools/reset_capture.py) whose per-entry statistics lines
({"t":"ns"} on the console, {"rec":"ns"} once captured; firmware/main/m0_main.cpp) list every occupied slot a
board can read, and names each hash against the paths a cell of the given boards can declare. Written for
ARCHITECTURE section 15, question 2 (2026-10-15). Event entries are not printed by the firmware (a READ of
one is refused), so they are not counted.

    python tools/ns_census.py captures/led-check-COM3.jsonl captures/led-check-COM4.jsonl ... \
        [--boards 6300,7368,8160] [--system lab] [--actors ticker,clock,present,rf_fusion,alarm]
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict

FNV_OFFSET, FNV_PRIME = 2166136261, 16777619
SYS = ["sys/heap-free", "sys/heap-largest", "sys/uptime", "sys/boot-epoch", "sys/peers-alive", "sys/rssi"]
LINK = ["seq", "count", "mean", "var", "min", "max"]


def fnv(s: str) -> int:
    h = FNV_OFFSET
    for b in s.encode():
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFF
    return h


def candidates(system: str, boards: list[int], actor_outs: list[str]) -> dict[int, tuple[str, str]]:
    """hash -> (path, category)."""
    p = f"potluck://{system}/"
    out: dict[int, tuple[str, str]] = {}
    nodes = boards + [0x00FE]
    for n in nodes:
        for s in SYS:
            out[fnv(f"{p}node-{n:04x}/{s}")] = (f"node-{n:04x}/{s}", "sys")
        out[fnv(f"{p}node-{n:04x}/rf/channel")] = (f"node-{n:04x}/rf/channel", "rf_link")
        out[fnv(f"{p}node-{n:04x}/hw/die_temp")] = (f"node-{n:04x}/hw/die_temp", "pinned")
        out[fnv(f"{p}node-{n:04x}/led/on")] = (f"node-{n:04x}/led/on", "pinned")
        for q in nodes:
            if q != n:
                for f in LINK:
                    out[fnv(f"{p}node-{n:04x}/rf/link-{q:04x}/{f}")] = (f"node-{n:04x}/rf/link-{q:04x}/{f}", "rf_link")
    for a in actor_outs:
        out[fnv(f"{p}act/{a}")] = (f"act/{a}", "actor")
    out[fnv("act/setpoint")] = ("act/setpoint", "builtin")
    out[fnv(f"{p}svc/time")] = ("svc/time", "host")
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("captures", nargs="+")
    ap.add_argument("--boards", default="6300,7368,8160")
    ap.add_argument("--system", default="lab")
    ap.add_argument("--actors", default="ticker/out,clock/out,present/out,rf_fusion/out,rf_fusion/confidence,"
                    "rf_fusion/motion,alarm/out,alarm/alarm,alarm/count")
    ap.add_argument("--list", action="store_true", help="print every entry")
    a = ap.parse_args(argv)
    boards = [int(x, 16) for x in a.boards.split(",")]
    names = candidates(a.system, boards, a.actors.split(","))
    # The last statistics period per board: the firmware prints the whole table each period, in slot order,
    # so a period's lines are the ones since that board's last clk record.
    tables: dict[int, dict[int, dict]] = defaultdict(dict)
    for path in a.captures:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                kind = d.get("rec", d.get("t"))
                node = int(d.get("node", 0))
                if kind == "clk":
                    tables[node] = {}
                elif kind == "ns":
                    tables[node][int(d["hash"])] = d
    for node, t in sorted(tables.items()):
        by = defaultdict(lambda: [0, 0])  # category -> [own, replica]
        for h, d in t.items():
            cat = names.get(h, ("?", "unnamed"))[1]
            by[cat][0 if int(d.get("owner", 0)) == node else 1] += 1
        parts = ", ".join(f"{c} {o} own + {r} replica" for c, (o, r) in sorted(by.items()))
        print(f"0x{node:04x}: {len(t)} of 128 entries -- {parts}")
        if a.list:
            for h, d in sorted(t.items(), key=lambda kv: names.get(kv[0], ("~", ""))[0]):
                print(f"    {names.get(h, (f'0x{h:08x}', ''))[0]:40s} owner {int(d.get('owner', 0)):04x} {d.get('quality')}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
