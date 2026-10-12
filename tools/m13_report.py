#!/usr/bin/env python3
"""M13: what the cell presented, from the bench broker's log (tools/mqtt_bench_broker.py) -- one clock, the broker's.

    python tools/m13_report.py <broker.jsonl> [--since <t seconds>] [--until <t>]

Prints: each presenter's sessions (connect, will, clean disconnect); the discovery configs per entity, checked
against section 13's rules (the cell id in the unique id, nothing retained, availability one topic per presenter
with availability_mode any); state messages per entity with their rate and the quality and age they carried; every
change in a presenter's live-board list and "down" list, with its time -- what Home Assistant's availability
follows; and the outside writes announced on the event topic.
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--since", type=float, default=0.0)
    ap.add_argument("--until", type=float, default=1e18)
    a = ap.parse_args()
    rows = []
    with open(a.log, encoding="utf-8") as f:
        for line in f:
            try:
                d = json.loads(line)
            except ValueError:
                continue
            if a.since <= d.get("t", 0) <= a.until:
                rows.append(d)

    print("== sessions")
    for d in rows:
        if d["ev"] in ("connect", "will") and str(d.get("client", "")).startswith("potluck-"):
            print(f"  {d['t']:9.3f}  {d['ev']:<10} {d['client']}" + (f"  keepalive {d.get('keepalive')} s" if d["ev"] == "connect" else
                                                                      f"  -> {d.get('topic')}"))
        elif d["ev"] == "disconnect" and str(d.get("client", "")).startswith("potluck-"):
            print(f"  {d['t']:9.3f}  disconnect {d['client']}  clean={d.get('clean')}  ({d.get('why')})")

    pubs = [d for d in rows if d["ev"] == "publish" and d.get("payload") is not None]
    configs: dict[str, dict] = {}
    problems: list[str] = []
    for d in pubs:
        t = d["topic"]
        if t.endswith("/config") and t.startswith("homeassistant/"):
            try:
                c = json.loads(d["payload"])
            except ValueError:
                problems.append(f"{t}: config is not JSON")
                continue
            configs[c.get("unique_id", t)] = c
            if d.get("retain"):
                problems.append(f"{t}: retained")
            if c.get("availability_mode") != "any":
                problems.append(f"{t}: availability_mode {c.get('availability_mode')}")
            cell = t.split("/")[2].removeprefix("potluck_")
            if cell not in c.get("unique_id", ""):
                problems.append(f"{t}: unique id without the cell id")
    print(f"\n== discovery: {len(configs)} entit(ies)")
    for uid, c in sorted(configs.items()):
        av = c.get("availability", [])
        print(f"  {uid}  '{c.get('name')}'  unit={c.get('unit_of_measurement', '-')}  class={c.get('device_class', '-')}"
              f"  expire_after={c.get('expire_after', '-')}  presenters={len(av)}  cmd={'yes' if 'command_topic' in c else 'no'}")
    for p in problems:
        print(f"  PROBLEM {p}")

    states = defaultdict(list)
    for d in pubs:
        if d["topic"].endswith("/state") and d["topic"].startswith("potluck/"):
            try:
                states[d["topic"].split("/")[2]].append((d["t"], json.loads(d["payload"])))
            except ValueError:
                pass
    print("\n== states")
    for h, xs in sorted(states.items()):
        ts = [t for t, _ in xs]
        span = ts[-1] - ts[0] if len(ts) > 1 else 0
        gaps = [b - a for a, b in zip(ts, ts[1:])]
        q = defaultdict(int)
        for _, s in xs:
            q[s.get("quality")] += 1
        ages = [s.get("age_ms", 0) for _, s in xs]
        print(f"  {h}  n={len(xs)}  every {span / max(1, len(xs) - 1):.2f} s  max gap {max(gaps, default=0):.2f} s"
              f"  quality {dict(q)}  age ms median {sorted(ages)[len(ages) // 2]} max {max(ages)}")

    print("\n== availability (each presenter's live boards and resources down)")
    last: dict[str, tuple] = {}
    for d in pubs:
        if "/presenter/" not in d["topic"]:
            continue
        who = d["topic"].rsplit("/", 1)[1]
        try:
            p = json.loads(d["payload"])
        except ValueError:
            continue
        key = (p.get("online"), tuple(p.get("nodes", [])), tuple(p.get("down", [])))
        if last.get(who) != key:
            src = " (WILL)" if "(will)" in str(d.get("src")) else ""
            print(f"  {d['t']:9.3f}  {who}: online={key[0]} nodes={list(key[1])} down={list(key[2])}{src}")
            last[who] = key

    print("\n== outside writes")
    for d in pubs:
        if d["topic"].endswith("/set"):
            print(f"  {d['t']:9.3f}  command {d['topic']} = {d['payload']}  (from {d.get('src')})")
        elif d["topic"].endswith("/event"):
            print(f"  {d['t']:9.3f}  event   {d['payload']}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
