"""M7's bench: guest actors in the cell, read from the host on A's cable, summarised against the consoles.

    <IDF python> tools/m7_guests.py watch --port COM6 --seconds 900 --out captures/m7-guests-watch.jsonl
    python tools/m7_guests.py report captures/m7-guests       # reads <prefix>-watch.jsonl, <prefix>-COM*.jsonl

`watch` reads every guest output of manifests/m7-guests.json through A once a second -- value, quality and
age, section 4's tuple, as any consumer gets it -- and writes one JSON line per read. Consoles are recorded
alongside with tools/json_capture.py (JSON lines only: board B holds the owner's Wi-Fi credentials).

`report` says, per guest: where it ran and when it moved (the actor_owner events), each node's last
{"t":"guest"} line (runs, ok, failed, quarantined, fuel), every guest_fault, and -- from the host's reads --
whether the alarm's count ever went backwards (a cold start) or stalled, and how long the outputs read
anything but GOOD. And for the cell: every peer_dead, by whom and of whom.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "host", "potluck"))

PATHS = [
    "potluck://lab/act/alarm/out",
    "potluck://lab/act/alarm/alarm",
    "potluck://lab/act/alarm/count",
    "potluck://lab/act/spin/out",
    "potluck://lab/node-7368/hw/die_temp",
]
QUALITY = {0: "GOOD", 1: "STALE", 2: "UNAVAILABLE", 3: "NO_DATA", 4: "FAULTY"}


def watch(a) -> int:
    from potluck.bridge import Bridge, RequestTimeout

    bridge = Bridge.open(port=a.port, tcp=None, baud=921600, capture=None, heartbeat=True)
    bridge.start()
    if bridge.hello(timeout=5.0) is None:
        print("no HELLO from the cabled board", file=sys.stderr)
        return 2
    t_end = time.time() + a.seconds
    with open(a.out, "a", encoding="utf-8") as f:
        while time.time() < t_end and not os.path.exists(a.out + ".stop"):
            t0 = time.time()
            for p in PATHS:
                rec = {"host_ts": round(time.time(), 3), "path": p}
                try:
                    r = bridge.read(p, timeout=1.5)
                    num, q = r.number()
                    rec.update(value=num, quality=QUALITY.get(q, q), age_ms=r.age_ms)
                except RequestTimeout:
                    rec["timeout"] = True
                f.write(json.dumps(rec) + "\n")
            f.flush()
            time.sleep(max(0.0, 1.0 - (time.time() - t0)))
    bridge.close()
    return 0


def load(path: str) -> list[dict]:
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def report(a) -> int:
    prefix = a.prefix
    t0 = None
    print(f"M7 guest bench report: {prefix}")
    deaths = []
    for path in sorted(glob.glob(prefix + "-COM*.jsonl")):
        recs = load(path)
        if not recs:
            continue
        port = path.rsplit("-", 1)[-1].split(".")[0]
        t0 = min(t0 or recs[0]["host_ts"], recs[0]["host_ts"])
        guests = {}
        faults = []
        owners = []
        for r in recs:
            t = r.get("rec") or r.get("t")
            if t == "guest":
                guests[r.get("guest")] = r
            elif t == "guest_fault":
                faults.append(r)
            elif t == "event" and r.get("kind") in ("peer_dead",):
                deaths.append((port, r))
            elif t == "event" and r.get("kind") in ("actor_started", "actor_stopped", "actor_owner"):
                owners.append(r)
        span = (recs[-1]["host_ts"] - recs[0]["host_ts"]) / 60.0
        print(f"\n{port}: {len(recs)} records over {span:.1f} min")
        for g, r in sorted(guests.items(), key=lambda kv: str(kv[0])):
            print(f"  guest {g} (key {r.get('key')}): runs {r.get('runs')} ok {r.get('ok')} failed {r.get('failed')} "
                  f"overruns {r.get('overruns')} fresh {r.get('fresh')} reads {r.get('reads')} ck {r.get('ck_saved')} "
                  f"fuel {r.get('fuel')} strikes {r.get('strikes')} quarantined {r.get('quarantined')} "
                  f"error '{r.get('error')}'")
        for r in faults:
            print(f"  fault @{r['host_ts'] - t0:7.1f} s: guest {r.get('guest')} strikes {r.get('strikes')} "
                  f"quarantined {r.get('quarantined')}: {r.get('error')}")
        for r in owners:
            print(f"  {r.get('kind')} @{r['host_ts'] - t0:7.1f} s: {json.dumps({k: v for k, v in r.items() if k not in ('rec', 'host_ts', 't')})}")
    print(f"\npeer_dead events: {len(deaths)}")
    for port, r in deaths:
        print(f"  {port} @{r['host_ts'] - (t0 or r['host_ts']):7.1f} s: {json.dumps({k: v for k, v in r.items() if k not in ('rec', 'host_ts', 't')})}")
    wpath = prefix + "-watch.jsonl"
    if os.path.exists(wpath):
        w = load(wpath)
        print(f"\nhost reads through A: {len(w)}")
        for p in PATHS:
            rs = [r for r in w if r["path"] == p]
            if not rs:
                continue
            q = {}
            for r in rs:
                k = "TIMEOUT" if r.get("timeout") else r.get("quality")
                q[k] = q.get(k, 0) + 1
            print(f"  {p}: {q}")
            if p.endswith("/count"):
                vals = [(r["host_ts"], r["value"]) for r in rs if r.get("quality") == "GOOD"]
                back = [(t, a_, b_) for (t, a_), (_, b_) in zip(vals, vals[1:]) if b_ < a_]
                print(f"    GOOD counts: first {vals[0][1] if vals else None}, last {vals[-1][1] if vals else None}, "
                      f"went backwards {len(back)} time(s){': ' + str(back[:3]) if back else ''}")
                gaps = []
                bad_since = None
                for r in rs:
                    good = r.get("quality") == "GOOD"
                    if not good and bad_since is None:
                        bad_since = r["host_ts"]
                    if good and bad_since is not None:
                        gaps.append(r["host_ts"] - bad_since)
                        bad_since = None
                print(f"    spells not GOOD: {len(gaps)}, longest {max(gaps):.1f} s" if gaps else "    never not GOOD")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("watch")
    s.add_argument("--port", default="COM6")
    s.add_argument("--seconds", type=float, default=900)
    s.add_argument("--out", required=True)
    s = sub.add_parser("report")
    s.add_argument("prefix")
    a = p.parse_args()
    return watch(a) if a.cmd == "watch" else report(a)


if __name__ == "__main__":
    sys.exit(main())
