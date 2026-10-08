#!/usr/bin/env python3
"""Turn a soak capture into the figures §13-M0's acceptance table asks for.

    python tools\\soak_report.py captures\\soak-2026-10-02-COM*.jsonl

One capture file per board. Every board reports its own view of every link, so N boards give N
independent views of the same cell and A's opinion of B can be checked against B's opinion of A --
the report prints both halves of each pair next to each other for exactly that reason.

WHAT THIS DOES NOT DO

It does not interpolate inside a histogram bucket, and it does not print a PDR for a link with an
empty denominator. M0-RUNBOOK  section 5 is explicit on both: a percentile is reported as the bucket
interval containing it because that is all a histogram knows, and an unmeasured ratio prints
"unmeasured", never 0 and never 100. A number this tool cannot support is a number it does not
print.

THE BROADCAST DENOMINATOR: MEASURED WHERE THE FIRMWARE GIVES IT, RECONSTRUCTED WHERE IT DOES NOT

The firmware counts heartbeat loss in rx.hb_lost (a gap in the 32-bit hb_seq, so a real loss).
Before a652cab it did not emit its denominator, rx_bcast_frames, in the stats line. The unicast PDR the firmware does
print covers the unicast class only -- so the traffic M0 is actually about, the 100 ms heartbeat, has
its losses counted and its delivery ratio never stated.

Firmware from a652cab emits it as rx.bcast_frames, and the tool uses it whenever both ends of a
window carry it. For older captures it reconstructs the denominator from elapsed uptime: a peer's
heartbeat is a fixed-period timer, so over an interval it must have sent interval / period of them.
That is sound only while the peer did not reboot, which is checked (reboots_seen and a constant
epoch) and reported. Every figure derived this way is labelled RECONSTRUCTED. It is evidence, not
acceptance.

THE UNICAST PDR OF A LATE-JOINING BOARD, BEFORE c70756f

Firmware before c70756f seeded a new peer's unicast seq baseline from the broadcast HELLO that
created it, so a board that booted into a running cell charged each peer with one phantom gap the
size of the distance between that peer's two counters (M0-LOG session 30: ~12,500, 47 % "PDR" with
zero gaps in the window). The inbound unicast PDR the firmware prints is cumulative since boot, so in
such captures read the window's seq gaps instead. Heartbeat delivery comes from hb_seq and was never
affected.
"""

import collections
import glob
import json
import sys

# Upper edge of each bucket in microseconds, from link_stats.hpp. A sample lands in the first bucket
# whose edge it does not exceed, so bucket i covers (edge[i-1], edge[i]].
RTT_EDGE_US = [1000, 2000, 3000, 4000, 6000, 8000, 11000, 16000, 22000,
               30000, 42000, 60000, 85000, 110000, 200000, None]

HB_PERIOD_MS = 100  # CONFIG_POT_HB_PERIOD_MS


def bucket_label(i):
    lo = "0" if i == 0 else f"{RTT_EDGE_US[i-1]/1000:g}"
    if RTT_EDGE_US[i] is None:
        return f">{RTT_EDGE_US[i-1]/1000:g} ms"
    return f"{lo}-{RTT_EDGE_US[i]/1000:g} ms"


def percentile_bucket(hist, pct):
    """The bucket interval containing the pct-th percentile, as a label. None if no samples."""
    total = sum(hist)
    if total == 0:
        return None
    want = total * pct / 100.0
    run = 0
    for i, n in enumerate(hist):
        run += n
        if run >= want:
            return bucket_label(i)
    return bucket_label(len(hist) - 1)


def ratio(num, den):
    """A delivery ratio as a string, or 'unmeasured' when the denominator is empty."""
    if den <= 0:
        return "unmeasured"
    return f"{100.0 * num / den:.4f}%"


def load(path):
    recs = []
    bad = 0
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line:
            continue
        try:
            recs.append(json.loads(line))
        except ValueError:
            bad += 1
    return recs, bad


def main(argv):
    paths = []
    for a in argv:
        paths.extend(sorted(glob.glob(a)))
    if not paths:
        print("usage: soak_report.py <capture.jsonl> [...]  (globs allowed)")
        return 2

    boards = {}   # node id -> dict of parsed state
    for p in paths:
        recs, bad = load(p)
        node = [r for r in recs if r.get("rec") == "node"]
        link = [r for r in recs if r.get("rec") == "link"]
        ev = [r for r in recs if r.get("rec") == "event"]
        if not node:
            print(f"{p}: no node records -- wrong baud, or the board never spoke")
            continue
        me = node[0]["node"]
        boards[me] = dict(path=p, node=node, link=link, ev=ev, bad=bad)

    if not boards:
        return 1

    print("=" * 86)
    print("POTLUCK SOAK REPORT -- figures for the 13-M0 acceptance table")
    print("=" * 86)
    print()

    # ---- window and integrity ---------------------------------------------------------------
    print("-- window and integrity " + "-" * 62)
    worst_h = None
    for me, b in sorted(boards.items()):
        first, last = b["node"][0], b["node"][-1]
        hours = (last["up_ms"] - first["up_ms"]) / 3.6e6
        worst_h = hours if worst_h is None else min(worst_h, hours)
        m = last["membership"]
        m0 = first["membership"]
        epochs = set(r["epoch"] for r in b["node"])
        flags = []
        # The counters run from the board's boot, which may predate the capture (a soak started on a
        # cell that has been up for hours). What happened inside the window is the difference; the
        # lifetime figure is shown beside it when they differ, so neither can be mistaken for the other.
        for key in ("deaths", "revivals", "reboots_seen"):
            if m[key] != m0[key]:
                flags.append(f"{key}_in_window={m[key] - m0[key]}")
        if m["deaths"]:
            flags.append(f"deaths_since_boot={m['deaths']}")
        if m["revivals"]:
            flags.append(f"revivals={m['revivals']}")
        if m["reboots_seen"]:
            flags.append(f"reboots_seen_since_boot={m['reboots_seen']}")
        if m["table_full"]:
            flags.append(f"table_full={m['table_full']}")
        if m["events_dropped"]:
            flags.append(f"events_dropped={m['events_dropped']}")
        if len(epochs) != 1:
            flags.append(f"epoch CHANGED {sorted(epochs)} -- this node rebooted")
        if b["bad"]:
            flags.append(f"{b['bad']} unparseable lines")
        verdict = "clean" if not flags else "  ".join(flags)
        print(f"  0x{me:04x}  {hours:6.2f} h observed, {len(b['node']):5d} samples   {verdict}")
        rx = last["rx"]
        noisy = {k: v for k, v in rx.items() if k != "total" and v}
        if noisy:
            print(f"           node rx anomalies: {noisy}")
        tx = last["tx"]
        noisy = {k: v for k, v in tx.items() if k != "total" and v}
        if noisy:
            print(f"           node tx anomalies: {noisy}")
    print(f"\n  shortest observed window: {worst_h:.2f} h"
          f"   {'-- 24 h NOT reached' if worst_h < 24 else '-- 24 h reached'}")
    print()

    # ---- memory ------------------------------------------------------------------------------
    print("-- static allocation holds (section 6: no growth, no fragmentation) " + "-" * 25)
    for me, b in sorted(boards.items()):
        d = [r["free_dram"] for r in b["node"]]
        lb = sorted(set(r["largest_block"] for r in b["node"]))
        hours = (b["node"][-1]["up_ms"] - b["node"][0]["up_ms"]) / 3.6e6
        drift = d[-1] - d[0]
        frag = "constant" if len(lb) == 1 else f"VARIED {lb[0]:,}..{lb[-1]:,}"
        print(f"  0x{me:04x}  free_dram {d[0]:,} -> {d[-1]:,}   drift {drift:+,} B over {hours:.2f} h"
              f"   (min {min(d):,})")
        print(f"           largest_block {frag}"
              + (f" ({lb[0]:,} B)" if len(lb) == 1 else ""))
        if hours > 0:
            print(f"           extrapolated {drift / hours * 24:+,.0f} B/day")
    print()

    # ---- per link ----------------------------------------------------------------------------
    print("-- per link " + "-" * 74)
    agg = [0] * 16
    pairs = collections.defaultdict(dict)
    for me, b in sorted(boards.items()):
        bypeer = collections.defaultdict(list)
        for r in b["link"]:
            bypeer[r["peer"]].append(r)
        for peer, rs in sorted(bypeer.items()):
            f, l = rs[0], rs[-1]
            hours = (l["up_ms"] - f["up_ms"]) / 3.6e6
            d_tx = l["tx"]["frames"] - f["tx"]["frames"]
            d_rx = l["rx"]["frames"] - f["rx"]["frames"]
            d_gap = l["rx"]["lost_seqgap"] - f["rx"]["lost_seqgap"]
            d_hb = l["rx"]["hb_lost"] - f["rx"]["hb_lost"]
            d_dup = l["rx"]["reorder_dup"] - f["rx"]["reorder_dup"]
            # Firmware from a652cab on emits bcast_frames, hb_lost's real denominator. Use it when
            # both ends of the window carry it; reconstruct from uptime only for older captures.
            if "bcast_frames" in f["rx"] and "bcast_frames" in l["rx"]:
                d_bcast = l["rx"]["bcast_frames"] - f["rx"]["bcast_frames"]
                hb_expected = d_bcast + d_hb
                hb_source = "measured"
            else:
                hb_expected = int(hours * 3.6e6 / HB_PERIOD_MS)
                hb_source = "RECONSTRUCTED from uptime"
            # The firmware's histogram is cumulative since BOOT, not since the capture began -- a
            # board that was not reset carries every earlier run in it. Difference first and last
            # sample so the histogram describes this window and nothing else. min/max cannot be
            # differenced, so they are reported as since-boot and labelled that way.
            rtt = dict(l["rtt"])
            rtt["hist"] = [b - a for a, b in zip(f["rtt"]["hist"], l["rtt"]["hist"])]
            rtt["samples"] = l["rtt"]["samples"] - f["rtt"]["samples"]
            rtt["timeouts"] = l["rtt"]["timeouts"] - f["rtt"]["timeouts"]
            for i, n in enumerate(rtt["hist"]):
                agg[i] += n
            pairs[frozenset((me, peer))][(me, peer)] = dict(
                hours=hours, rssi=l.get("rssi"), state=l["state"],
                tx_pdr=l["tx"]["pdr_ppm"], rx_pdr=l["rx"]["pdr_ppm"],
                d_tx=d_tx, d_rx=d_rx, d_gap=d_gap, d_hb=d_hb, d_dup=d_dup,
                dup_total=l["rx"]["reorder_dup"], hb_total=l["rx"]["hb_lost"],
                hb_expected=hb_expected, hb_source=hb_source, rtt=rtt,
                cb_fail=l["tx"]["cb_fail"], enq=l["tx"]["enqueue_err"],
                bad=l["rx"]["dropped_bad"], misses=l["misses"], mtu=l["mtu"],
                ver=l["espnow_ver"])

    for key in sorted(pairs, key=lambda s: sorted(s)):
        halves = pairs[key]
        a, bb = sorted(set(x for pair in halves for x in pair))
        print(f"  0x{a:04x} <-> 0x{bb:04x}")
        for (src, dst), v in sorted(halves.items()):
            print(f"    0x{src:04x} -> 0x{dst:04x}   {v['state']}, ESP-NOW v{v['ver']}, "
                  f"MTU {v['mtu']}, RSSI {v['rssi']} dBm, misses {v['misses']}")
            print(f"      outbound  {v['d_tx']:>9,} frames  cb_fail {v['cb_fail']}  "
                  f"enqueue_err {v['enq']}   unicast PDR "
                  + ("unmeasured (no delivery signal on this link)" if v['tx_pdr'] is None else f"{v['tx_pdr']/1e4:.4f}%"))
            print(f"      inbound   {v['d_rx']:>9,} frames  seq gaps {v['d_gap']}  "
                  f"bad {v['bad']}   unicast PDR {v['rx_pdr']/1e4:.4f}%")
            print(f"      heartbeat {v['hb_expected']:>9,} expected ({v['hb_source']})  "
                  f"lost {v['d_hb']}   delivery {ratio(v['hb_expected'] - v['d_hb'], v['hb_expected'])}")
            print(f"      duplicates {v['dup_total']} total, {v['d_dup']} inside this window")
            r = v["rtt"]
            print(f"      RTT  {r['samples']:,} samples, {r['timeouts']} timeouts in this window   "
                  f"(min {r['min_us']/1000:.2f} / max {r['max_us']/1000:.2f} ms since boot)")
            print(f"           p50 {percentile_bucket(r['hist'], 50)}   "
                  f"p99 {percentile_bucket(r['hist'], 99)}")
            print(f"           local txq max {r['txq_max_us']/1000:.2f} ms   "
                  f"peer turnaround {r['remote_turnaround_us']} us "
                  f"(max {r['remote_turnaround_max_us']} us)")
        print()

    # ---- aggregate histogram -----------------------------------------------------------------
    print("-- delay histogram, every link pooled " + "-" * 48)
    total = sum(agg)
    if total == 0:
        print("  no RTT samples")
    else:
        run = 0
        for i, n in enumerate(agg):
            run += n
            bar = "#" * int(60.0 * n / max(agg)) if max(agg) else ""
            print(f"  {bucket_label(i):>12}  {n:>8,}  {100.0*n/total:6.2f}%  "
                  f"{100.0*run/total:6.2f}% cum  {bar}")
        print(f"\n  {total:,} samples   p50 {percentile_bucket(agg, 50)}   "
              f"p99 {percentile_bucket(agg, 99)}   p99.9 {percentile_bucket(agg, 99.9)}")
        # bimodality: the kill criterion asks whether the tail is bimodal, not merely long
        peak = max(range(16), key=lambda i: agg[i])
        tail = [i for i in range(peak + 2, 16) if agg[i] > 0]
        gaps = [i for i in range(peak + 1, (max(tail) if tail else peak) + 1) if agg[i] == 0]
        print(f"  mode is {bucket_label(peak)}; "
              + (f"empty buckets inside the tail at {[bucket_label(i) for i in gaps]} "
                 "-- LOOK, this is what bimodal looks like"
                 if gaps else "tail decays without a gap -- long, not bimodal"))
    print()

    print("-- what still needs a human " + "-" * 58)
    print("  * A heartbeat denominator marked RECONSTRUCTED comes from uptime, not the firmware:")
    print("    accept it only with the reboot check in the integrity section clean.")
    print("  * A PDR figure without a geometry is not a measurement (M0-RUNBOOK section 8).")
    print("    Record the physical arrangement next to these numbers.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
