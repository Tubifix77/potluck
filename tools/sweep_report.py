#!/usr/bin/env python3
"""Split a distance-sweep capture into one block per spot, and measure each.

    python tools\\sweep_report.py --walker 0x7368 captures\\sweep-<label>-COM3.jsonl [...COM5.jsonl]

One board walks the house on a mains charger; the others stay on the PC and are captured. Every time
the walker is plugged in it boots with a new epoch, and the fixed boards report that epoch in each
link record, so the capture splits itself: one spot = one walker epoch. Nobody has to note times for
the split to work. Notes are still needed for what the numbers mean -- where the spot was, how far,
through how many walls.

PER SPOT, FROM EACH FIXED BOARD'S VIEW OF THE WALKER

  heartbeat   the walker's broadcast beacons this board received, against those it lost (hb_seq
              gaps). Measured, from rx.bcast_frames -- this needs firmware a652cab or later, because
              the walker reboots at every spot and an uptime reconstruction would be wrong.
  outbound    this board's unicast frames to the walker, by MAC ACK: did the WALKER receive them.
  inbound     the walker's unicast frames to this board, by sequence gaps.
  RTT         the round-trip histogram for the spot, as bucket intervals, never interpolated.
  RSSI        min / median / max over the spot's samples.
  deaths      times this board declared the walker dead while it was at the spot. At a spot past the
              edge the link flickers between alive and dead, and this is where that shows.
  timeline    one character per 10 s sample of heartbeat delivery:
                #  >= 99 %    +  >= 90 %    -  >= 50 %    .  < 50 %    x  dead
              Read it for oscillation: "##.##x#" is the cliff, a steady "-----" is a weak link.

A CONTROL COLUMN, FROM THE SAME BOARD

  The fixed boards also hear each other, and they did not move. If their link degrades during a spot
  as well, the cause was the house -- a microwave, a video call -- and not the walker's distance.

WHAT A SPOT'S END MEANS

  A spot ends when the walker is unplugged. The fixed board then declares it dead and keeps reporting
  it dead until the next boot. Those trailing dead samples are the walk to the next spot, not loss,
  and are excluded -- reported only as a count. If a spot was past the edge and the link never came
  back before unplugging, that looks the same from here, so the timeline and the notes have to say
  which it was.
"""

import argparse
import collections
import json
import statistics
import sys
from datetime import datetime

RTT_EDGE_US = [1000, 2000, 3000, 4000, 6000, 8000, 11000, 16000, 22000,
               30000, 42000, 60000, 85000, 110000, 200000, None]


def bucket_label(i):
    lo = "0" if i == 0 else f"{RTT_EDGE_US[i-1]/1000:g}"
    if RTT_EDGE_US[i] is None:
        return f">{RTT_EDGE_US[i-1]/1000:g} ms"
    return f"{lo}-{RTT_EDGE_US[i]/1000:g} ms"


def percentile_bucket(hist, pct):
    total = sum(hist)
    if total == 0:
        return "unmeasured"
    want = total * pct / 100.0
    run = 0
    for i, n in enumerate(hist):
        run += n
        if run >= want:
            return bucket_label(i)
    return bucket_label(len(hist) - 1)


def ratio(num, den):
    return "unmeasured" if den <= 0 else f"{100.0 * num / den:.2f}%"


def clock(ts):
    return datetime.fromtimestamp(ts).strftime("%H:%M:%S")


def mark(alive, rx, lost):
    if not alive:
        return "x"
    if rx + lost == 0:
        return "?"
    d = rx / (rx + lost)
    return "#" if d >= 0.99 else "+" if d >= 0.90 else "-" if d >= 0.50 else "."


def load(path):
    links = collections.defaultdict(list)
    events = []
    me = None
    for line in open(path, encoding="utf-8", errors="replace"):
        try:
            r = json.loads(line)
        except ValueError:
            continue
        if r.get("rec") == "link":
            links[r["peer"]].append(r)
            me = r["node"]
        elif r.get("rec") == "event":
            events.append(r)
    return me, links, events


def delta(a, b, *keys):
    """b[keys] - a[keys], walking nested dicts."""
    va, vb = a, b
    for k in keys:
        va, vb = va.get(k, 0), vb.get(k, 0)
    return vb - va


def window_delivery(base, last):
    rx = delta(base, last, "rx", "bcast_frames")
    lost = delta(base, last, "rx", "hb_lost")
    return rx, lost


def segments(samples):
    """Split one peer's link samples into runs of the same peer epoch."""
    segs = []
    for i, r in enumerate(samples):
        if not segs or segs[-1]["epoch"] != r["epoch"]:
            segs.append(dict(epoch=r["epoch"], idx=[]))
        segs[-1]["idx"].append(i)
    return segs


def at_time(samples, ts):
    """The last sample at or before host time ts, or the first one."""
    best = samples[0]
    for r in samples:
        if r["host_ts"] <= ts:
            best = r
        else:
            break
    return best


def report(path, walker):
    me, links, events = load(path)
    if me is None:
        print(f"{path}: no link records")
        return
    if walker not in links:
        print(f"{path}: node 0x{me:04x} never heard the walker 0x{walker:04x}")
        return
    ws = links[walker]
    if "bcast_frames" not in ws[0]["rx"]:
        print(f"{path}: no rx.bcast_frames -- firmware older than a652cab. The walker reboots at every")
        print("spot, so heartbeat delivery cannot be reconstructed from uptime. Reflash, then sweep.")
        return
    controls = {p: s for p, s in links.items() if p != walker}

    print("=" * 96)
    print(f"0x{me:04x}'s view of the walker 0x{walker:04x}   ({path})")
    print("=" * 96)
    for n, seg in enumerate(segments(ws), 1):
        idx = seg["idx"]
        alive_idx = [i for i in idx if ws[i]["state"] == "alive"]
        if not alive_idx:
            print(f"\n spot {n}: walker epoch {seg['epoch']} -- never alive in a sample; skipped")
            continue
        # The spot runs to the last alive sample; dead samples after it are the walk to the next spot.
        last_alive = alive_idx[-1]
        body = [i for i in idx if i <= last_alive]
        trailing = len(idx) - len(body)
        first = ws[body[0]]
        last = ws[last_alive]
        # Baseline is the spot's own first sample, not the last one before it. The sample before
        # it straddles the unplug, and using it charged each spot with the previous spot's ending:
        # the death, the un-ACKed frame and the probe timeout of the walker going away (seen in the
        # 2026-10-03 desk test, where every reset showed up as one death in the *next* spot). The
        # cost is up to one 10 s sample per spot, against spots of two minutes or more.
        base = first
        t0, t1 = first["host_ts"], last["host_ts"]
        minutes = (t1 - t0) / 60.0

        rx, lost = window_delivery(base, last)
        ok = delta(base, last, "tx", "cb_ok")
        fail = delta(base, last, "tx", "cb_fail")
        rfr = delta(base, last, "rx", "frames")
        gaps = delta(base, last, "rx", "lost_seqgap")
        hist = [b - a for a, b in zip(base["rtt"]["hist"], last["rtt"]["hist"])]
        tmo = delta(base, last, "rtt", "timeouts")
        rssi = [ws[i]["rssi"] for i in body if ws[i]["state"] == "alive" and ws[i].get("rssi") is not None]

        up0, up1 = base["up_ms"], last["up_ms"]
        deaths = sum(1 for e in events if e.get("peer") == walker and e.get("kind") == "peer_dead"
                     and up0 < e.get("at_ms", -1) <= up1)
        dead_samples = sum(1 for i in body if ws[i]["state"] != "alive")

        line = ""
        prev = base
        for i in body[1:]:
            r = ws[i]
            a, l = window_delivery(prev, r)
            line += mark(r["state"] == "alive", a, l)
            prev = r

        print(f"\n spot {n}   {clock(t0)} - {clock(t1)}   {minutes:4.1f} min   walker epoch {seg['epoch']}")
        print(f"   heartbeat  {rx + lost:>7,} sent, {lost:>6,} lost   delivery {ratio(rx, rx + lost)}")
        print(f"   outbound   {ok + fail:>7,} unicast, {fail:>5,} un-ACKed  PDR {ratio(ok, ok + fail)}"
              f"   (did the walker receive)")
        print(f"   inbound    {rfr + gaps:>7,} unicast, {gaps:>5,} missing  PDR {ratio(rfr, rfr + gaps)}")
        print(f"   RTT        p50 {percentile_bucket(hist, 50)}   p99 {percentile_bucket(hist, 99)}"
              f"   {sum(hist):,} samples, {tmo} timeouts")
        if rssi:
            print(f"   RSSI       {min(rssi)} / {statistics.median(rssi):g} / {max(rssi)} dBm (min / median / max)")
        print(f"   deaths     {deaths} at this spot   ({dead_samples} of {len(body)} samples dead)")
        print(f"   timeline   {line}")
        if trailing:
            print(f"   then       {trailing} dead sample(s) until the next power-up -- the walk, or a lost link")

        for peer, cs in sorted(controls.items()):
            cb = at_time(cs, t0 - 0.001)
            ce = at_time(cs, t1)
            if "bcast_frames" not in cb["rx"]:
                continue
            crx, clost = window_delivery(cb, ce)
            print(f"   control    0x{me:04x} <- 0x{peer:04x} (unmoved) heartbeat delivery "
                  f"{ratio(crx, crx + clost)}")
    print()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--walker", required=True, help="node id of the moving board, e.g. 0x7368")
    ap.add_argument("captures", nargs="+")
    args = ap.parse_args(argv)
    walker = int(args.walker, 0)
    for p in args.captures:
        report(p, walker)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
