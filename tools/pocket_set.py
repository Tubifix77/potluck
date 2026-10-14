#!/usr/bin/env python3
"""Give a Potluck board with the `pocket` module its own access point from a file -- run by the OWNER, not by Claude.

    <IDF python> tools/pocket_set.py COM3 <path to your file>
    <IDF python> tools/pocket_set.py COM3 --forget

The file holds two lines: the access point's name (1-32 bytes, no spaces), then its password (8-63 characters, WPA2).
Make both up: this is the board's own network for a phone, not the house Wi-Fi.

M13.1: the pocket page (firmware.modules: ["pocket"]). It sends `POT! pocket ap <name> <password>` to the board's
console, waits for the board's result line, and restarts the board once so the access point comes up. It never prints
the name or the password, nor the board's output around them -- only whether the board said "saved". It writes nothing
to disk; delete the file yourself when you are done. On an extender whose hotspot is up the module stays idle (the
extender owns the access point), so give it to a board that is not the extender.

Close any monitor on the port first: only one program can hold it.
"""

from __future__ import annotations

import sys
import time

import serial


def send(port: str, line: str, saved: str) -> int:
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.05
    s.dtr = False
    s.rts = False
    s.open()
    reply = b""
    try:
        s.reset_input_buffer()
        s.write(b"POT! pocket " + line.encode("utf-8") + b"\n")
        end = time.time() + 5
        while time.time() < end and b'"t":"pocket_cfg"' not in reply:
            reply += s.read(1024)
        if f'"result":"{saved}"'.encode() in reply:
            print(f"board: {saved.replace('_', ' ')}")
        elif b'"result":"refused_length"' in reply:
            print("board: refused (length); nothing saved")
            return 1
        elif b'"t":"pocket_cfg"' in reply:
            print("board: did not save it")
            return 1
        else:
            print("board: no answer within 5 s -- does its firmware carry the pocket module, and is a monitor open?")
            return 1
        s.rts = True
        time.sleep(0.15)
        s.rts = False
        print("board: restarting to use it")
        return 0
    finally:
        reply = b""
        s.close()


def main() -> int:
    args = sys.argv[1:]
    if len(args) != 2:
        print(__doc__.strip().splitlines()[2])
        return 2
    port, what = args
    if what == "--forget":
        return send(port, "forget", "forgotten")
    with open(what, encoding="utf-8-sig") as f:
        lines = [l.rstrip("\r\n") for l in f.readlines() if l.strip("\r\n") != ""]
    if len(lines) != 2:
        print(f"the file must hold two non-empty lines (name, password); it holds {len(lines)}")
        return 2
    name, password = lines
    if not 1 <= len(name.encode()) <= 32 or " " in name:
        print(f"the name is {len(name.encode())} bytes{' with a space' if ' ' in name else ''}; it must be 1-32 bytes, "
              f"no spaces")
        return 2
    if not 8 <= len(password) <= 63:
        print(f"the password is {len(password)} characters; WPA2 allows 8-63")
        return 2
    line = "ap " + name + " " + password
    if len(line) >= 160:
        print("name and password together are too long for the board's command line")
        return 2
    return send(port, line, "ap_saved")


if __name__ == "__main__":
    sys.exit(main())
