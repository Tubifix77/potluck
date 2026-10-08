#!/usr/bin/env python3
"""M8 on the bench: a node calling a host service, and degrading correctly when the host is off.

    <IDF python> tools\\m8_bench.py --out captures\\m8-svc

Needs the bench of WHEN-THE-BOARDS-ARRIVE.md with the `m8-svc` package deployed: board A (console
COM3) runs the `clock` svc_client, which calls `potluck://lab/svc/time` on the host every second; the
host reaches A over the CP2102 frame link on COM6.

The script IS the host: it runs potluck-agent in-process, and takes it away in the two ways a host goes
away -- abruptly (the link closed with no goodbye: a cable pulled, a laptop lid shut, a crash) and
politely (a BYE first) -- then brings it back each time. Throughout, it records A's console: the
client's `{"t":"svc"}` line (state, counters, and its output exactly as a reader gets it, quality and
age included), A's membership events, and A's clock lines for putting events on the host's clock.

Section 13-M8's line is "an MCU actor calling one and degrading correctly when the host is off". What
"correctly" is checked as, per phase:
  * host present  -> state serving, output GOOD and young, answers counted;
  * host gone     -> state degraded, nothing new published, output STALE with a growing age (never a
                     host's old answer presented as fresh), and A's own membership unaffected: no other
                     peer dies;
  * host back     -> serving again, output GOOD, without anything restarted by hand.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "host", "potluck"))

import serial  # noqa: E402

from potluck.agent import Agent  # noqa: E402
from potluck.bridge import Bridge  # noqa: E402

PHASES = (("host present", "agent", 60), ("host pulled (no BYE)", "pull", 40), ("host back", "agent", 40),
          ("host stopped (BYE)", "bye", 30), ("host back", "agent", 30))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    raw = open(a.out + ".jsonl", "w", encoding="utf-8")
    lock = threading.Lock()
    lines: list[tuple[float, dict]] = []
    stop = threading.Event()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = "COM3", 115200, 0.05
    s.dtr = False
    s.rts = False
    s.open()

    def reader() -> None:
        buf = b""
        while not stop.is_set():
            buf += s.read(max(1, s.in_waiting))
            while b"\n" in buf:
                one, buf = buf.split(b"\n", 1)
                ts = time.time()
                t = one.decode("utf-8", "replace").strip()
                with lock:
                    raw.write(json.dumps({"host_ts": ts, "port": "COM3", "line": t}) + "\n")
                if t.startswith("{"):
                    try:
                        lines.append((ts, json.loads(t)))
                    except json.JSONDecodeError:
                        pass

    th = threading.Thread(target=reader, daemon=True)
    th.start()
    marks: list[tuple[float, str]] = []

    def mark(what: str) -> None:
        marks.append((time.time(), what))
        with lock:
            raw.write(json.dumps({"host_ts": marks[-1][0], "mark": what}) + "\n")
        print(f"[{time.strftime('%H:%M:%S')}] {what}", flush=True)

    bridge = None
    agent = None
    served_total = 0
    try:
        for label, action, secs in PHASES:
            if action == "agent":
                holder = {}
                bridge = Bridge.open(port="COM6", tcp=None, baud=921600, on_log=lambda m: None,
                                     on_request=lambda f: holder["a"].handle(f))
                agent = Agent(bridge, cluster="lab", services=["time"])
                holder["a"] = agent
                bridge.start()
                ok = bridge.hello(timeout=3.0) is not None
                mark(f"{label}: agent up (hello {'answered' if ok else 'NOT answered'})")
            elif action == "pull":
                served_total += agent.stats.served
                bridge.close()  # no BYE: the host simply stops
                bridge = None
                mark(f"{label}")
            elif action == "bye":
                served_total += agent.stats.served
                bridge.bye()
                bridge.close()
                bridge = None
                mark(f"{label}")
            time.sleep(secs)
        if bridge is not None:
            served_total += agent.stats.served
            bridge.bye()
            bridge.close()
        time.sleep(11)
    finally:
        stop.set()
        th.join(timeout=1)
        s.close()
        raw.close()

    # ---- the verdict, per phase -------------------------------------------------------------------
    out = []

    def say(t: str) -> None:
        print(t, flush=True)
        out.append(t)

    bounds = [m[0] for m in marks] + [time.time()]
    for k, (t0, what) in enumerate(marks):
        t1 = bounds[k + 1]
        svc = [j for ts, j in lines if j.get("t") == "svc" and t0 + 2 < ts <= t1]
        peers = [j for ts, j in lines if j.get("t") == "peers" and t0 + 2 < ts <= t1]
        say(f"== {what} ({t1 - t0:.0f} s): {len(svc)} svc lines")
        for j in svc:
            say(f"   state={j['state']:<9} out={j['out_quality']:<12} age={j['out_age_ms']:>6} ms  "
                f"calls={j['calls']} answered={j['answered']} lost={j['lost']} timed_out={j['timed_out']} "
                f"not_sent={j['not_sent']} degr={j['degradations']} recov={j['recoveries']}")
        others_dead = sum(1 for j in peers for p in j.get("list", []) if p.get("id") != 254 and p.get("state") != "alive")
        say(f"   A's other peers not alive in this phase's lines: {others_dead}")
    evs = [(ts, j) for ts, j in lines if j.get("t") == "event" and j.get("peer") == 254]
    for ts, j in evs:
        say(f"   event {j['kind']} (host 0x00fe) at A's {j['at_ms']} ms")
    say(f"host: {served_total} calls served by the agent across its three lives")
    with open(a.out + "-report.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
