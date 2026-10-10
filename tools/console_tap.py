"""Record a board's console without resetting it: every line, stamped with the host's clock.

    python tools/console_tap.py COM5 600 captures/m11-c.log

Opens the port with DTR and RTS released, so the board keeps running (ESP32-S3 dev boards reset
on an RTS edge). Needs pyserial (the IDF Python has it). Lines are written as
`<unix seconds> <line>`, so captures of several boards share one clock.
"""

import sys
import time

import serial


def main() -> int:
    port, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    s = serial.Serial()
    s.port = port
    s.baudrate = int(sys.argv[4]) if len(sys.argv) > 4 else 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    end = time.time() + secs
    buf = b""
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        while time.time() < end:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                f.write(f"{time.time():.3f} {line.decode('ascii', 'replace').rstrip()}\n")
            f.flush()
    s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
