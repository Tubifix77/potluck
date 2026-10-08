#!/usr/bin/env python3
"""Capture a board's statistics in potluck's capture format, keeping ONLY its JSON lines.

    <IDF python> tools/json_capture.py COM4 captures/<name>.jsonl <seconds>

For the board that holds Wi-Fi credentials (M6.1's extender). `python -m potluck --capture` keeps every
console line, and that board's ESP-IDF log names the networks it uses at start-up. This keeps the
node's `{"t":...}` records only -- which never carry a credential -- written as `python -m potluck`
writes them (the record type under `rec`, plus `host_ts`), so `tools/soak_report.py` reads the file
unchanged. Every other line is counted and dropped unread. Stops early if <out>.stop appears.
"""

from __future__ import annotations

import json
import os
import sys
import time

import serial

port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])
stop = out + ".stop"
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.05
s.dtr = False
s.rts = False
s.open()
kept = dropped = 0
buf = b""
end = time.time() + secs
with open(out, "a", encoding="utf-8") as f:
    while time.time() < end and not os.path.exists(stop):
        buf += s.read(max(1, s.in_waiting))
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            line = raw.decode("utf-8", "replace").strip()
            if not line.startswith("{"):
                dropped += 1
                continue
            try:
                j = json.loads(line)
            except json.JSONDecodeError:
                dropped += 1
                continue
            rec = j.pop("t", None)
            if rec is None:
                dropped += 1
                continue
            j["rec"] = rec
            j["host_ts"] = time.time()
            f.write(json.dumps(j) + "\n")
            f.flush()
            kept += 1
s.close()
print(f"kept {kept} records, dropped {dropped} other lines")
