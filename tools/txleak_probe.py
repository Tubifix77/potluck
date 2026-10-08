#!/usr/bin/env python3
"""Does a unicast that fails lose its transmit buffer? -- the M8 / M6.1 board-B stall, isolated.

    <IDF python> tools/txleak_probe.py [--victim-port COM5 --victim 33120 --hold 4]

Resets board B (COM4), lets it settle, then holds one peer (the victim) in reset for --hold seconds and
releases it. Before and after, it takes B's link record for the victim: frames handed to ESP-NOW
(`frames - enqueue_err`) against send completions (`cb_ok + cb_fail`). Every accepted frame must
complete -- a failed one as cb_fail. A shortfall is a frame the driver accepted and never finished:
on a build with a fixed pool of static TX buffers, one buffer gone for good.
"""

from __future__ import annotations

import argparse
import json
import time

import serial


def open_port(p: str) -> serial.Serial:
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = p, 115200, 0.05
    s.dtr = False
    s.rts = False
    s.open()
    return s


def next_link(s: serial.Serial, peer: int, timeout: float = 25.0) -> dict | None:
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        buf += s.read(max(1, s.in_waiting))
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode("utf-8", "replace").strip()
            if t.startswith('{"t":"link"') and f'"peer":{peer},' in t:
                return json.loads(t)
    return None


def shortfall(rec: dict) -> tuple[int, int, int]:
    tx = rec["tx"]
    accepted = tx["frames"] - tx["enqueue_err"]
    done = tx["cb_ok"] + tx["cb_fail"]
    return accepted, done, accepted - done


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--victim-port", default="COM5")
    ap.add_argument("--victim", type=int, default=33120)
    ap.add_argument("--hold", type=float, default=4.0)
    a = ap.parse_args()
    b = open_port("COM4")
    b.rts = True
    time.sleep(0.15)
    b.rts = False
    time.sleep(20)
    b.reset_input_buffer()
    before = next_link(b, a.victim)
    v = open_port(a.victim_port)
    v.rts = True
    time.sleep(a.hold)
    v.rts = False
    v.close()
    time.sleep(12)
    b.reset_input_buffer()
    after = next_link(b, a.victim)
    b.close()
    if before is None or after is None:
        print("no link record from B")
        return 1
    for name, rec in (("before", before), ("after", after)):
        acc, done, short = shortfall(rec)
        print(f"{name}: B->victim accepted {acc}, completed {done} (cb_ok {rec['tx']['cb_ok']}, "
              f"cb_fail {rec['tx']['cb_fail']}), never completed {short}, enqueue_err {rec['tx']['enqueue_err']}")
    print(f"frames lost across a {a.hold:.0f} s outage of the victim: "
          f"{shortfall(after)[2] - shortfall(before)[2]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
