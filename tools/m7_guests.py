"""M7's bench: guest actors in the cell, read from the host on A's cable, summarised against the consoles.

    <IDF python> tools/m7_guests.py watch --port COM6 --seconds 900 --out captures/m7-guests-watch.jsonl
    python tools/m7_guests.py report captures/m7-guests       # reads <prefix>-watch.jsonl, <prefix>-COM*.jsonl
    python tools/m7_guests.py duty captures/m7-duty --target 0x8160   # the cell around a guest burning its fuel

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


def duty(a) -> int:
    """What the cell saw of the target while a guest burned its budget there, against before and after:
    the target's probe turnaround as its peers measured it (remote_turnaround_us in THEIR link records),
    heartbeats lost on every link to and from it, deaths. The burn is read off the target's own console:
    from its first guest_fault minus one period's burn (the first tick starts burning before it faults)
    from the target starting the guest (its actor_started event) to the quarantine."""
    recs = []
    for path in sorted(glob.glob(a.prefix + "-COM*.jsonl")):
        recs += load(path)
    recs.sort(key=lambda r: r["host_ts"])
    faults = [r for r in recs if r.get("rec") == "guest_fault" and r.get("node") == a.target]
    if not faults:
        print("no guest_fault on the target: nothing burned there")
        return 1
    q = next((r for r in faults if r.get("quarantined")), faults[-1])
    gap = (faults[-1]["host_ts"] - faults[0]["host_ts"]) / max(1, len(faults) - 1)
    # The burn starts when the target started that guest (its actor_started event for the same key).
    starts = [r["host_ts"] for r in recs if r.get("rec") == "event" and r.get("kind") == "actor_started"
              and r.get("node") == a.target and r.get("a") == faults[0].get("key")
              and r["host_ts"] <= faults[0]["host_ts"]]
    burn = (starts[-1] if starts else faults[0]["host_ts"] - gap, q["host_ts"])
    t_first = recs[0]["host_ts"]
    print(f"target 0x{a.target:04x}: {len(faults)} faults, burning ~{burn[1] - burn[0]:.0f} s "
          f"(@{burn[0] - t_first:.0f}..{burn[1] - t_first:.0f} s), one tick every ~{gap:.1f} s")
    # The settle window after the deploy's reboot is excluded from "before": boot noise is not a guest.
    boot = min((r["host_ts"] for r in recs if r.get("rec") == "link" and r.get("up_ms", 1e9) < 15000
                and r["host_ts"] > t_first + 60), default=burn[0])
    phases = {"before": (t_first, min(boot, burn[0]) - 1), "burn": burn, "after": (burn[1] + 15, recs[-1]["host_ts"])}
    last = {}
    stats = {k: {"turn": [], "hb_lost": 0, "hb_rx": 0, "minutes": (v[1] - v[0]) / 60.0} for k, v in phases.items()}
    for r in recs:
        if r.get("rec") != "link":
            continue
        key = (r["node"], r["peer"])
        rx = r.get("rx", {})
        prev = last.get(key)
        last[key] = r
        if a.target not in key or prev is None or r.get("up_ms", 0) < prev.get("up_ms", 0):
            continue
        for k, (t0, t1) in phases.items():
            if t0 <= r["host_ts"] <= t1:
                s = stats[k]
                s["hb_lost"] += max(0, rx.get("hb_lost", 0) - prev.get("rx", {}).get("hb_lost", 0))
                s["hb_rx"] += max(0, rx.get("frames", 0) - prev.get("rx", {}).get("frames", 0))
                if r["peer"] == a.target and r.get("rtt", {}).get("remote_turnaround_us"):
                    s["turn"].append(r["rtt"]["remote_turnaround_us"])
    for k, s in stats.items():
        t = sorted(s["turn"])
        mid = t[len(t) // 2] if t else None
        print(f"  {k:6s} {s['minutes']:5.1f} min: target's turnaround as peers see it median {mid} us, "
              f"max {t[-1] if t else None} us ({len(t)} samples); heartbeats lost on its links "
              f"{s['hb_lost']} ({s['hb_lost'] / max(s['minutes'], 1e-9):.1f}/min), frames {s['hb_rx']}")
    dead = [r for r in recs if r.get("rec") == "event" and r.get("kind") == "peer_dead"
            and burn[0] <= r["host_ts"] <= burn[1]]
    print(f"  peer_dead during the burn: {len(dead)}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("duty")
    s.add_argument("prefix")
    s.add_argument("--target", type=lambda v: int(v, 0), default=0x8160)
    s = sub.add_parser("watch")
    s.add_argument("--port", default="COM6")
    s.add_argument("--seconds", type=float, default=900)
    s.add_argument("--out", required=True)
    s = sub.add_parser("report")
    s.add_argument("prefix")
    a = p.parse_args()
    return {"watch": watch, "report": report, "duty": duty}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
