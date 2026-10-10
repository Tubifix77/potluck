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
    ts = [threading.Thread(target=tap, args=(p, os.path.join(a.out, f"board-{p}.log"), stop, send), daemon=True)
          for p in PORTS]
    for t in ts:
        t.start()
    marks = {"start": time.time()}
    time.sleep(a.quiet)
    marks["load_on"] = time.time()
    send[a.target] = f"POT! wasm {a.samples} {a.runs}"
    log = os.path.join(a.out, f"board-{a.target}.log")
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
    phases = [("before", m["start"], m["load_on"]), ("guest", m["load_on"], m["load_off"]),
              ("after", m["load_off"], m["end"])]
    # Per direction (who hears whom) and phase: heartbeats lost per MINUTE -- the phases differ in length --
    # and the probe turnaround the far end reported. A link that does not touch the target is the control:
    # what moves on it moved for the cell, not for the guest.
    links: dict = {}
    deaths = {p[0]: 0 for p in phases}
    for port, node in PORTS.items():
        with open(os.path.join(d, f"board-{port}.log"), encoding="utf-8") as f:
            for line in f:
                t_s, _, js = line.partition(" ")
                if not js.startswith("{"):
                    continue
                try:
                    o = json.loads(js)
                    t = float(t_s)
                except ValueError:
                    continue  # a line two tasks printed into at once
                ph = next((p[0] for p in phases if p[1] <= t < p[2]), None)
                if ph is None:
                    continue
                if o.get("t") == "event" and o.get("kind") == "peer_dead":
                    deaths[ph] += 1
                if o.get("t") == "link" and o.get("peer") in PORTS.values():
                    rt, rx = o.get("rtt", {}), o.get("rx", {})
                    links.setdefault((node, o["peer"], ph), []).append(
                        (t, rx.get("hb_lost", 0), rt.get("remote_turnaround_us", 0)))
    print(f"target 0x{tgt:04x} ran the guest; deaths before/guest/after: "
          f"{deaths['before']}/{deaths['guest']}/{deaths['after']}")
    for node in PORTS.values():
        for peer in PORTS.values():
            if peer == node:
                continue
            cells = []
            for name, _, _ in phases:
                xs = links.get((node, peer, name), [])
                if len(xs) < 2:
                    cells.append(f"{name} -")
                    continue
                mins = (xs[-1][0] - xs[0][0]) / 60
                turn = [x[2] for x in xs]
                cells.append(f"{name} {(xs[-1][1] - xs[0][1]) / mins:5.1f}/min turn {min(turn)}-{max(turn)} us")
            role = "  (control)" if tgt not in (node, peer) else ""
            print(f"0x{node:04x} hears 0x{peer:04x}: " + " | ".join(cells) + role)
    runs = []
    with open(os.path.join(d, f"board-{m['target']}.log"), encoding="utf-8") as f:
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
