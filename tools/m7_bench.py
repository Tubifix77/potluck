"""M7's experiment in a running cell: does a guest in the sandbox cost a board its duties?

    python tools/m7_bench.py duty --target COM5 --samples 2000000 --runs 6 --out captures/m7-duty
    python tools/m7_bench.py report captures/m7-duty

`duty` records every board's console (no reset) for a quiet minute, then sends `POT! wasm <samples>
<runs>` to the target -- M9's Monte Carlo kernel interpreted on a task of its own at the lowest priority
(firmware/main/m0_main.cpp, wasm_rt) -- records until it is done, then another quiet minute. `report`
compares the phases on what the cell sees of the target: its probe turnaround as its peers measure it
(remote_turnaround_us in THEIR link records), heartbeats lost, deaths. A guest that slows the node shows
there first; M9's native lender raised it by +0.3 ms until its kernel moved to IRAM.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

PORTS = {"COM3": 0x6300, "COM4": 0x7368, "COM5": 0x8160}


def tap(port: str, out: str, stop: threading.Event, send: dict) -> None:
    import serial

    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.2, False, False
    s.open()
    buf = b""
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        while not stop.is_set():
            line = send.pop(port, None)
            if line is not None:
                s.write((line + "\r\n").encode("ascii"))
                f.write(f"{time.time():.3f} >>> {line}\n")
            buf += s.read(4096)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("ascii", "replace").rstrip()
                if text.startswith("{"):  # JSON only: the extender's ESP-IDF log can name networks
                    f.write(f"{time.time():.3f} {text}\n")
            f.flush()
    s.close()


def duty(a) -> int:
    os.makedirs(a.out, exist_ok=True)
    stop = threading.Event()
    send: dict = {}
    ts = [threading.Thread(target=tap, args=(p, os.path.join(a.out, f"{p}.log"), stop, send), daemon=True)
          for p in PORTS]
    for t in ts:
        t.start()
    marks = {"start": time.time()}
    time.sleep(a.quiet)
    marks["load_on"] = time.time()
    send[a.target] = f"POT! wasm {a.samples} {a.runs}"
    log = os.path.join(a.out, f"{a.target}.log")
    deadline = time.time() + a.max_load
    while time.time() < deadline:
        time.sleep(2)
        with open(log, encoding="utf-8") as f:
            if '"phase":"done"' in f.read():
                break
    marks["load_off"] = time.time()
    time.sleep(a.quiet)
    marks["end"] = time.time()
    stop.set()
    for t in ts:
        t.join()
    with open(os.path.join(a.out, "marks.json"), "w", encoding="utf-8") as f:
        json.dump({"target": a.target, "target_node": PORTS[a.target], **marks}, f)
    return report_dir(a.out)


def report_dir(d: str) -> int:
    with open(os.path.join(d, "marks.json"), encoding="utf-8") as f:
        m = json.load(f)
    tgt = m["target_node"]
    phases = [("before", m["start"], m["load_on"]), ("guest running", m["load_on"], m["load_off"]),
              ("after", m["load_off"], m["end"])]
    rows = {p[0]: {"turn_max": 0, "turn": [], "hb_lost": 0, "deaths": 0, "rtt_p99_hi": 0} for p in phases}
    first_hb: dict = {}
    for port, node in PORTS.items():
        with open(os.path.join(d, f"{port}.log"), encoding="utf-8") as f:
            for line in f:
                t_s, _, js = line.partition(" ")
                if not js.startswith("{"):
                    continue
                try:
                    o = json.loads(js)
                    t = float(t_s)
                except ValueError:
                    continue
                ph = next((p[0] for p in phases if p[1] <= t < p[2]), None)
                if ph is None:
                    continue
                r = rows[ph]
                if o.get("t") == "event" and o.get("kind") == "peer_dead" and tgt in (o.get("peer"), o.get("node")):
                    r["deaths"] += 1
                if o.get("t") == "link" and o.get("peer") == tgt and node != tgt:
                    rt = o.get("rtt", {})
                    r["turn"].append(rt.get("remote_turnaround_us", 0))
                    r["turn_max"] = max(r["turn_max"], rt.get("remote_turnaround_max_us", 0))
                    r["rtt_p99_hi"] = max(r["rtt_p99_hi"], (rt.get("p99_us") or [0, 0])[1])
                    hb = o.get("rx", {}).get("hb_lost", 0)
                    key = (node, ph)
                    first_hb.setdefault(key, hb)
                    r["hb_lost"] = max(r["hb_lost"], hb - first_hb[key])
    print(f"target 0x{tgt:04x}; seen from its peers' link records")
    print(f"{'phase':14s} {'turnaround last (us)':>24s} {'turnaround max':>15s} {'p99 RTT hi':>11s} {'hb lost':>8s} {'deaths':>7s}")
    for name, _, _ in phases:
        r = rows[name]
        tl = r["turn"]
        rng = f"{min(tl)}-{max(tl)}" if tl else "-"
        print(f"{name:14s} {rng:>24s} {r['turn_max']:>15d} {r['rtt_p99_hi']:>11d} {r['hb_lost']:>8d} {r['deaths']:>7d}")
    runs = []
    with open(os.path.join(d, f"{m['target']}.log"), encoding="utf-8") as f:
        for line in f:
            _, _, js = line.partition(" ")
            if '"wasm_run"' in js:
                runs.append(json.loads(js))
    for o in runs:
        print("  ", json.dumps(o))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("duty")
    d.add_argument("--target", default="COM5")
    d.add_argument("--samples", type=int, default=2000000)
    d.add_argument("--runs", type=int, default=6)
    d.add_argument("--quiet", type=float, default=60.0)
    d.add_argument("--max-load", type=float, default=900.0)
    d.add_argument("--out", required=True)
    r = sub.add_parser("report")
    r.add_argument("dir")
    a = ap.parse_args()
    return duty(a) if a.cmd == "duty" else report_dir(a.dir)


if __name__ == "__main__":
    sys.exit(main())
