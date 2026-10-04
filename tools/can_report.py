"""Summarise a two-board CAN capture (lines "HH:MM:SS <board> <json>"): per board, the window between
its first and last {"t":"can"} line. Usage: python tools/can_report.py captures/m4-can-....log"""
import json, sys

rows = {}
for line in open(sys.argv[1], encoding="utf-8"):
    parts = line.split(" ", 2)
    if len(parts) < 3 or '"t":"can"' not in parts[2]:
        continue
    try:
        rows.setdefault(parts[1], []).append(json.loads(parts[2]))
    except json.JSONDecodeError:
        pass

for name, r in sorted(rows.items()):
    a, b = r[0], r[-1]
    secs = (b["up_ms"] - a["up_ms"]) / 1000
    d = {k: b[k] - a[k] for k in b if isinstance(b[k], int) and k not in ("node", "up_ms")
         and not k.startswith("ss_last") and not k.startswith("ss_m")}
    print(f"{name} node {a['node']:#06x}: {len(r)} samples, {secs:.0f} s window, up {b['up_ms']/1000:.0f} s")
    print("  deltas:", {k: v for k, v in d.items() if v})
    print(f"  tx frames/s {d['tx_frames']/secs:.0f}, rx frames/s {d['rx_frames']/secs:.0f}")
    if b["ss_sent"]:
        lasts = [x["ss_last_us"] for x in r]
        print(f"  ss_last samples: min {min(lasts)} max {max(lasts)} us; "
              f"cumulative since boot min {b['ss_min_us']} max {b['ss_max_us']} us")
    print("  end:", {k: b[k] for k in ("ss_sent", "ss_rx", "ss_rx_gaps", "arb_lost", "bit_err",
                                         "form_err", "stuff_err", "rx_overflow", "rx_timeouts",
                                         "rx_out_of_order", "tx_fail") if k in b})
