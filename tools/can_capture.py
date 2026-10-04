"""Read B (COM4) and C (COM5) consoles without resetting them; log everything, print CAN lines."""
import sys, threading, time, serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
out = sys.argv[2] if len(sys.argv) > 2 else None
ports = {"B": "COM4", "C": "COM5"}
lock = threading.Lock()
logf = open(out, "a", encoding="utf-8") if out else None


def reader(name, port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = False
    s.rts = False
    s.open()
    end = time.time() + secs
    buf = b""
    while time.time() < end:
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode("utf-8", "replace").rstrip()
            with lock:
                if logf:
                    logf.write(f"{time.strftime('%H:%M:%S')} {name} {t}\n")
                if '"t":"can"' in t or "pot.can" in t or "E (" in t or "admit" in t.lower():
                    print(f"{time.strftime('%H:%M:%S')} {name} {t}", flush=True)
    s.close()


th = [threading.Thread(target=reader, args=kv) for kv in ports.items()]
for t in th:
    t.start()
for t in th:
    t.join()
