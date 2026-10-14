#!/usr/bin/env python3
"""M14's last line on the bench: the presentation handed between the cell's role and the hub, seen at the broker.

    python tools/m14_present_report.py captures/m14p-broker.jsonl [<cell id>]

Reads a subscriber's log of potluck/<cell>/# (tools/mqtt_tool.py sub --out: wall time, topic, payload) and prints
every change of who presents -- each presenter's availability going online or offline, with its node -- and, for each
handover, how long the cell's states paused: the longest gap between two state publications of any one entity
around it, and whether two presenters published states at once (never twice is the acceptance).
"""

from __future__ import annotations

import json
import sys
from collections import defaultdict


def main(argv: list[str]) -> int:
    path = argv[0]
    rows = [json.loads(line) for line in open(path, encoding="utf-8") if line.startswith("{")]
    pres = []  # (wall, node, online)
    states: dict[str, list[float]] = defaultdict(list)
    last_online: dict[str, bool] = {}
    for r in rows:
        t, topic = r["wall"], r["topic"]
        if "/presenter/" in topic:
            node = topic.rsplit("/", 1)[1]
            try:
                on = json.loads(r["payload"]).get("online") == 1
            except ValueError:
                continue
            if last_online.get(node) != on:
                pres.append((t, node, on))
                last_online[node] = on
        elif topic.endswith("/state"):
            states[topic].append(t)
    t0 = rows[0]["wall"] if rows else 0
    print(f"{len(rows)} messages over {(rows[-1]['wall'] - t0) / 60:.1f} min, {len(states)} entities" if rows else "empty")
    for t, node, on in pres:
        print(f"  {t - t0:9.2f} s  presenter {node} {'ONLINE' if on else 'offline'}")
    # Every gap over 3 s (states are every 2 s) in any entity's states: where the presentation paused.
    gaps = []
    for topic, ts in states.items():
        for a, b in zip(ts, ts[1:]):
            if b - a > 3.0:
                gaps.append((a - t0, b - a, topic.split("/")[2]))
    gaps.sort()
    print("state gaps over 3 s (start, length, entity):")
    for start, length, ent in gaps:
        print(f"  {start:9.2f} s  {length:5.2f} s  {ent}")
    if not gaps:
        print("  none")
    # Two presenters at once: both availability topics online in the same stretch.
    online: set[str] = set()
    worst = 0.0
    since = None
    for t, node, on in pres:
        (online.add if on else online.discard)(node)
        if len(online) > 1 and since is None:
            since = t
        elif len(online) <= 1 and since is not None:
            worst = max(worst, t - since)
            since = None
    print(f"longest stretch with two presenters online: {worst:.2f} s"
          + (" (still two at the end)" if since is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
