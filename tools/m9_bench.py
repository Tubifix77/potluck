#!/usr/bin/env python3
"""M9 on the bench: run the Monte Carlo job on board A alone and with lenders, from A's console.

    <IDF python> tools/m9_bench.py COM3 captures/m9-<name>.jsonl [--runs 3] [--idle 30]
                                    [--reset-port COM4 --reset-after 4]

Holds A's console for the whole session, keeping only its {"t":...} records (in the capture format
tools/json_capture.py writes, so tools/soak_report.py reads the file). It sits idle for --idle
seconds -- the lenders' baseline -- then alternates "POT! mc run 0" (A alone) and "POT! mc run 1"
(B and C may lend), --runs of each, waiting for each run's mc_done line and an idle gap after it.
With --reset-port, every lending run also resets that board (esptool's hard reset, ~--reset-after
seconds in), which is M9's "a lender held in reset mid-unit loses no result".

No host joins the cluster: the console is a board's UART log, not a cluster member, and potctl's
bridge is not running. Prints one summary line per run and a speedup at the end.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("out")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--idle", type=float, default=30.0)
    ap.add_argument("--gap", type=float, default=15.0)
    ap.add_argument("--timeout", type=float, default=600.0)
    ap.add_argument("--reset-port")
    ap.add_argument("--reset-after", type=float, default=4.0)
    a = ap.parse_args()

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0.05
    s.dtr = False
    s.rts = False
    s.open()
    f = open(a.out, "a", encoding="utf-8")
    buf = b""
    done: list[dict] = []

    def pump(seconds: float, until_done: int | None = None) -> None:
        nonlocal buf
        end = time.time() + seconds
        while time.time() < end:
            buf += s.read(max(1, s.in_waiting))
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("{"):
                    continue
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                t = d.pop("t", None)
                if t is None:
                    continue
                d["rec"] = t
                d["host_ts"] = time.time()
                f.write(json.dumps(d) + "\n")
                if t == "mc_done":
                    done.append(d)
            if until_done is not None and len(done) >= until_done:
                return

    def send(text: str) -> None:
        s.write((text + "\r\n").encode("ascii"))
        s.flush()
        f.write(json.dumps({"rec": "bench", "cmd": text, "host_ts": time.time()}) + "\n")

    pump(a.idle)  # the baseline: lenders idle
    for k in range(a.runs):
        for lend in (0, 1):
            want = len(done) + 1
            send(f"POT! mc run {lend}")
            if lend and a.reset_port:
                pump(a.reset_after, want)
                f.write(json.dumps({"rec": "bench", "cmd": f"reset {a.reset_port}", "host_ts": time.time()}) + "\n")
                subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", a.reset_port, "read-mac"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            pump(a.timeout, want)
            if len(done) < want:
                print(f"run {k} lend {lend}: no mc_done within {a.timeout} s")
                return 1
            r = done[-1]
            print(f"run {k} lend {lend}: {r['elapsed_ms']} ms, hits {r['hits']}, pi {r['pi']:.7f}, "
                  f"local {r['local']} lent {r['lent']} refused {r['refused']} lost {r['lost']}")
            pump(a.gap)
    f.close()
    s.close()
    alone = [r["elapsed_ms"] for r in done if r["lend"] == 0]
    lent = [r["elapsed_ms"] for r in done if r["lend"] == 1]
    hits = {r["hits"] for r in done}
    if alone and lent:
        print(f"median alone {sorted(alone)[len(alone) // 2]} ms, median lending {sorted(lent)[len(lent) // 2]} ms, "
              f"speedup {sorted(alone)[len(alone) // 2] / sorted(lent)[len(lent) // 2]:.2f}x; "
              f"hits identical across runs: {len(hits) == 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
