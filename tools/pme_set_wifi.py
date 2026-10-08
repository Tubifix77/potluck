#!/usr/bin/env python3
"""Give a poor-mans-extender board the house Wi-Fi from a file -- run by the OWNER, not by Claude.

    <IDF python> tools/pme_set_wifi.py COM4 <path to your file>

The file holds two lines and nothing else:

    line 1: the house Wi-Fi name
    line 2: the house Wi-Fi password

Why this exists: a long name and a complex password are easy to mistype at the `pme>` prompt, and a
mistyped password shows up only as the board failing to join (reason 15, a 4-way handshake timeout).
This script sends exactly what is in the file, so the only copy you type is the one in the file.

What it does, and does not do:
  * reads the file, checks the lengths (name 1-32, password 8-64), and sends
    `sta "<name>" "<password>"` then `restart` to the board's console, quoting and escaping both so
    spaces, quotes and backslashes arrive intact;
  * never prints the name or the password, and never prints the board's echo of the line (the
    console repeats what it receives) -- it prints only whether the board said "saved";
  * writes nothing to disk. Delete the file yourself when the test is over.

Close any monitor on the port first: only one program can hold it.
"""

from __future__ import annotations

import sys
import time

import serial


def quote(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2])
        return 2
    port, path = sys.argv[1], sys.argv[2]
    with open(path, encoding="utf-8-sig") as f:
        lines = [l.rstrip("\r\n") for l in f.readlines()]
    lines = [l for l in lines if l != ""]
    if len(lines) != 2:
        print(f"the file must hold exactly two non-empty lines (name, password); it holds {len(lines)}")
        return 2
    name, password = lines
    if not 1 <= len(name.encode()) <= 32:
        print(f"the name is {len(name.encode())} bytes; Wi-Fi allows 1-32")
        return 2
    if not 8 <= len(password) <= 64:
        print(f"the password is {len(password)} characters; WPA2 allows 8-64")
        return 2
    line = "sta " + quote(name) + " " + quote(password)
    if len(line) >= 256:
        print("name and password together are too long for the board's 256-byte command line")
        return 2

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.05
    s.dtr = False
    s.rts = False
    s.open()

    # The board's prompt is ESP-IDF's linenoise. Started under a terminal (the IDF monitor) it runs in
    # its full mode, which asks the terminal where the cursor is (ESC[6n) before every line and eats
    # input until it gets an answer -- a script on the port then has its lines swallowed, and a
    # prompt left waiting by an earlier session stays wedged (all found on the bench, M0-LOG
    # session 27). Started with nothing answering, it falls back to a plain mode with no questions.
    # So: restart the board (RTS, as esptool does; its saved settings survive), let it boot with
    # nothing answering, then send the line. CR LF, as the monitor sends: a bare CR or LF was ignored.
    def pump(secs: float, stop: tuple = ()) -> bytes:
        r = b""
        end = time.time() + secs
        while time.time() < end:
            r += s.read(1024)
            if any(x in r for x in stop):
                break
        return r

    try:
        s.rts = True
        time.sleep(0.15)
        s.rts = False
        if b"pme>" not in pump(6.0, (b"pme>",)):
            print("board: no prompt after a restart; is a monitor still open on the port?")
            return 1
        pump(1.0)
        s.write(line.encode("utf-8") + b"\r\n")
        reply = pump(5.0, (b"saved", b"failed", b"must be", b"usage", b"Unrecognized"))
        if b"saved" in reply:
            print("board: house Wi-Fi saved")
        elif b"must be" in reply or b"usage" in reply:
            print("board: refused the values (length or format); nothing saved")
            return 1
        else:
            print("board: no 'saved' reply within 5 s; nothing confirmed"
                  + (" (the command was not understood)" if b"Unrecognized" in reply else ""))
            return 1
        pump(0.5)
        s.write(b"restart\r\n")
        pump(1.0)
        print("board: restarting to use it")
    finally:
        reply = b""  # drop the echo: it contains the line just sent
        s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
