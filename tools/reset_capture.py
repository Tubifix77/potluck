#!/usr/bin/env python3
"""Usage: <IDF python> tools/reset_capture.py <out.log> <seconds before> <seconds after> [COM3 COM4 COM5]

Record every board's JSON console lines, reset all of them together part-way, keep recording.

JSON lines only ({"t":...}): board B's text log can name Wi-Fi networks."""
import sys
import threading
import time

import serial

out_path, before, after = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
ports = sys.argv[4:] or ["COM3", "COM4", "COM5"]
ss = {}
for p in ports:
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = p, 115200, 0.2, False, False
    s.open()
    ss[p] = s
lines = []
lock = threading.Lock()
stop = time.time() + before + after


def reader(p, s):
    buf = b""
    while time.time() < stop:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode("utf-8", "replace").strip()
            if t.startswith('{"t":'):
                with lock:
                    lines.append(f"{time.time():.3f} {p} {t}")


ths = [threading.Thread(target=reader, args=(p, s)) for p, s in ss.items()]
for t in ths:
    t.start()
time.sleep(before)
for s in ss.values():
    s.rts = True
time.sleep(0.2)
for s in ss.values():
    s.rts = False
with lock:
    lines.append(f"{time.time():.3f} ALL reset (EN held 200 ms)")
for t in ths:
    t.join()
with open(out_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print(f"{len(lines)} lines")
