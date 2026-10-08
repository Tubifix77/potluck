#!/usr/bin/env python3
"""Record the poor-mans-extender's status lines from a board's console -- M6.1 step 0.

    <IDF python> tools/hs_capture.py COM4 captures/m61-step0-hs.jsonl [max_seconds]

Opens the port without resetting the board (DTR/RTS held low) and keeps ONLY the `{"t":"hs",...}`
lines, each with a host timestamp. Everything else the console prints is counted and dropped: the
Wi-Fi driver logs the network name on every association, and CR-6's rule is that no capture kept on
Potluck's side contains a credential. Stops after max_seconds, or when a file named <out>.stop appears.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time

import serial

port, out = sys.argv[1], sys.argv[2]
limit = float(sys.argv[3]) if len(sys.argv) > 3 else 3600.0
stop_file = out + ".stop"

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.2
s.dtr = False
s.rts = False
s.open()

kept = dropped = 0
end = time.time() + limit
buf = b""
with open(out, "a", encoding="utf-8") as f:
    while time.time() < end and not os.path.exists(stop_file):
        buf += s.read(max(1, s.in_waiting))
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").strip()
            # pme_hotspot's "router lost (reason N); retrying in M s" names no network: the reason
            # number alone is kept, which is what tells a wrong password from a router not found.
            m = re.search(r"router lost \(reason (\d+)\); retrying in (\d+) s", line)
            if m:
                f.write(json.dumps({"t": "sta_lost", "reason": int(m.group(1)), "retry_s": int(m.group(2)),
                                    "host_ts": time.time()}) + "\n")
                f.flush()
                kept += 1
                continue
            i = line.find('{"t":"hs"')
            if i < 0:
                dropped += 1
                continue
            try:
                rec = json.loads(line[i:])
            except json.JSONDecodeError:
                dropped += 1
                continue
            rec["host_ts"] = time.time()
            f.write(json.dumps(rec) + "\n")
            f.flush()
            kept += 1
s.close()
print(f"kept {kept} status lines, dropped {dropped} other lines")
