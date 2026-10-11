#!/usr/bin/env python3
"""Give a Potluck board with the `wifi` module the house Wi-Fi from a file -- run by the OWNER, not by Claude.

    <IDF python> tools/wifi_set.py COM5 <path to your file>
    <IDF python> tools/wifi_set.py COM5 --forget

The file holds two lines: the house Wi-Fi name, then its password (8-64 characters, WPA2).

M13: Potluck's own station (firmware.modules: ["wifi"]). It sends `POT! wifi sta "<name>" "<password>"` to the
board's console, quoting and escaping both, waits for the board's result line, and restarts the board once so the
station comes up with it. It never prints the name or the password, nor the board's output around them -- only
whether the board said "saved". It writes nothing to disk; delete the file yourself when you are done.

Close any monitor on the port first: only one program can hold it.
"""

from __future__ import annotations

import sys
import time

import serial


def quote(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def send(port: str, line: str, saved: str) -> int:
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.05
    s.dtr = False
    s.rts = False
    s.open()
    reply = b""
    try:
        s.reset_input_buffer()
        s.write(b"POT! wifi " + line.encode("utf-8") + b"\n")
        end = time.time() + 5
        while time.time() < end and b'"t":"wifi_cfg"' not in reply:
            reply += s.read(1024)
        if f'"result":"{saved}"'.encode() in reply:
            print(f"board: {saved.replace('_', ' ')}")
        elif b'"result":"refused_length"' in reply:
            print("board: refused (length); nothing saved")
            return 1
        elif b'"t":"wifi_cfg"' in reply:
            print("board: did not save it")
            return 1
        else:
            print("board: no answer within 5 s -- does its firmware carry the wifi module, and is a monitor open?")
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
    if not 1 <= len(name.encode()) <= 32:
        print(f"the name is {len(name.encode())} bytes; Wi-Fi allows 1-32")
        return 2
    if not 8 <= len(password) <= 64:
        print(f"the password is {len(password)} characters; WPA2 allows 8-64")
        return 2
    line = "sta " + quote(name) + " " + quote(password)
    if len(line) >= 240:
        print("name and password together are too long for the board's command line")
        return 2
    return send(port, line, "sta_saved")


if __name__ == "__main__":
    sys.exit(main())
