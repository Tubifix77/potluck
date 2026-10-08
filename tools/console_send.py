#!/usr/bin/env python3
"""Send one line to a board's console without resetting it, and print the JSON lines it answers.

    python tools\\console_send.py COM4 "POT! die_temp fail 1" [seconds]

For test instruments (`POT! ...`). It opens the port with DTR and RTS held low, as
tools/json_capture.py does, so the board's auto-reset circuit is not triggered. Only lines that
start with "{" are printed: on the extender build the ESP-IDF log can name networks, and this tool
never shows it. Not for credentials -- those are typed by the owner (tools/pme_set_wifi.py).
"""

import sys
import time

import serial  # pyserial, from the ESP-IDF Python environment

port, text = sys.argv[1], sys.argv[2]
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 0.2
s.open()
s.reset_input_buffer()
s.write((text + "\r\n").encode("ascii"))
s.flush()
end = time.time() + secs
buf = b""
while time.time() < end:
    buf += s.read(4096)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        t = line.decode("utf-8", "replace").strip()
        if t.startswith("{") and '"t":"test"' in t.replace(" ", ""):
            print(t)
s.close()
