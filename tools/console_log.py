#!/usr/bin/env python3
"""Record one board's console, every line with a host timestamp, without resetting it.

    <IDF python> tools/console_log.py COM4 captures/<name>.jsonl <seconds>

For boards with no credentials on them only: it keeps every line, and the Wi-Fi driver names networks.
"""
import json
import sys
import time

import serial

port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.05
s.dtr = False
s.rts = False
s.open()
end = time.time() + secs
buf = b""
n = 0
with open(out, "w", encoding="utf-8") as f:
    while time.time() < end:
        buf += s.read(max(1, s.in_waiting))
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            f.write(json.dumps({"host_ts": time.time(), "port": port, "line": line.decode("utf-8", "replace").strip()}) + "\n")
            n += 1
s.close()
print(f"{n} lines")
