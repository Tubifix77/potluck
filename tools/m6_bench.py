#!/usr/bin/env python3
"""M6 on the bench: kill, return, partition and heal, around a portable actor.

    <IDF python> tools\\m6_bench.py failover --cycles 3 --out captures\\m6-failover
    <IDF python> tools\\m6_bench.py partition --out captures\\m6-partition

Needs pyserial (the ESP-IDF Python environment has it) and the bench of WHEN-THE-BOARDS-ARRIVE.md:
A, B, C on their USB consoles (COM3, COM4, COM5), the CP2102 frame link to A on COM6, all three
running a build with the reconciler and the m6-ticker package deployed.

WHAT IT DOES. It records every console line from the three boards with a host timestamp, and reads
`potluck://lab/act/ticker/out` through A's cable ten times a second -- which is section 13-M6's
"serves reads", measured from outside the boards.

  failover   finds the board running the ticker (the high 16 bits of its value), holds that board in
             reset by its console's RTS line -- the same line esptool uses, so no wiring changes --
             then releases it. "Kill" is the chip held in reset: it sends nothing, answers nothing,
             and boots in a new incarnation when released. Never board A, whose cable is the reader.
  partition  isolates the runner with the `POT! deaf` console instrument on all three boards (both
             directions), waits, then clears it: a real two-sided partition of the cell, and a heal.

WHAT IT REPORTS (and writes next to the capture):
  * kill -> first good read served by another node: the 2 s line;
  * no read after the kill carried the dead node's value as Good;
  * double-running: each board's actor_started / actor_stopped events, put on the host clock by the
    stats lines' up_ms, and every pair of running intervals checked for overlap;
  * fencing: every board's actor_owner events, whose term must never go down while an owner is set.

Board clocks are mapped to the host clock by the smallest (host_ts - up_ms) any stats line showed,
which is the line with the least delay; the remaining error is a few milliseconds of USB latency and
is printed with the result rather than hidden in it.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "host", "potluck"))

import serial  # noqa: E402  (pyserial: the IDF Python environment)

from potluck.bridge import Bridge, RequestTimeout  # noqa: E402

PATH = "potluck://lab/act/ticker/out"
BOARDS = {0x6300: "COM3", 0x7368: "COM4", 0x8160: "COM5"}
MAC_TAIL = {0x6300: "6300", 0x7368: "7368", 0x8160: "8160"}
CONSOLE_BAUD = 921600


class Console:
    """One board's console: every line, timestamped; and its reset line."""

    def __init__(self, node: int, port: str, out) -> None:
        self.node = node
        self.port = port
        self.out = out
        self.lock = out_lock
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, CONSOLE_BAUD, 0.2
        s.dtr = False
        s.rts = False
        s.open()
        self.s = s
        self.lines: list[tuple[float, dict]] = []
        self.stop = False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self) -> None:
        buf = b""
        while not self.stop:
            try:
                chunk = self.s.read(4096)
            except serial.SerialException:
                time.sleep(0.05)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                ts = time.time()
                line = raw.decode("utf-8", "replace").strip()
                with self.lock:
                    self.out.write(json.dumps({"host_ts": ts, "port": self.port, "node": self.node, "line": line}) + "\n")
                if line.startswith("{"):
                    try:
                        self.lines.append((ts, json.loads(line)))
                    except json.JSONDecodeError:
                        pass

    def send(self, text: str) -> None:
        self.s.write((text + "\n").encode())

    def hold_reset(self, on: bool) -> None:
        # esptool's classic reset: RTS asserted with DTR deasserted pulls EN low.
        self.s.dtr = False
        self.s.rts = on

    def close(self) -> None:
        self.stop = True
        self.t.join(timeout=1)
        self.s.close()


out_lock = threading.Lock()


class Reader:
    """The ticker, read through A's cable at 10 Hz."""

    def __init__(self, out) -> None:
        self.out = out
        self.reads: list[tuple[float, int | None, str, int]] = []  # (ts, value, quality, age)
        self.bridge = Bridge.open(port="COM6", tcp=None, baud=115200, capture=None, heartbeat=True,
                                  on_log=lambda m: None, on_frame=None)
        self.bridge.start()
        if self.bridge.hello(timeout=3.0) is None:
            raise SystemExit("no HELLO_ACK from A over COM6")
        self.stop = False
        self.t = threading.Thread(target=self.run, daemon=True)
        self.t.start()

    def run(self) -> None:
        while not self.stop:
            t0 = time.time()
            try:
                r = self.bridge.read(PATH, timeout=0.5, dst=0x6300)
                v, q = r.number()
                rec = (t0, v if isinstance(v, int) else None, str(q), r.age_ms)
            except RequestTimeout:
                rec = (t0, None, "TIMEOUT", -1)
            self.reads.append(rec)
            with out_lock:
                self.out.write(json.dumps({"host_ts": rec[0], "read": PATH, "value": rec[1], "quality": rec[2],
                                           "age_ms": rec[3]}) + "\n")
            time.sleep(max(0.0, 0.1 - (time.time() - t0)))

    def runner(self) -> int | None:
        for ts, v, q, _ in reversed(self.reads[-20:]):
            if v is not None and "GOOD" in q.upper():
                return v >> 16
        return None

    def close(self) -> None:
        self.stop = True
        self.t.join(timeout=2)
        try:
            self.bridge.bye()
        finally:
            self.bridge.close()


def offsets(cons: dict[int, Console]) -> dict[int, float]:
    """Board clock -> host clock: min(host_ts - up_ms/1000) over the board's stats lines (current boot)."""
    out = {}
    for n, c in cons.items():
        best = None
        last_up = None
        for ts, j in c.lines:
            up = j.get("up_ms")
            if not isinstance(up, int):
                continue
            if last_up is not None and up < last_up:
                best = None  # rebooted: only the latest incarnation's clock is wanted
            last_up = up
            off = ts - up / 1000.0
            best = off if best is None or off < best else best
        out[n] = best
    return out


def events(cons: dict[int, Console], kinds: tuple[str, ...]) -> list[tuple[float, int, dict]]:
    """Every event of these kinds, on the host clock. Each board's boots are mapped separately."""
    res = []
    for n, c in cons.items():
        # Split the board's lines at reboots (up_ms going backwards), mapping each boot by its own lines.
        boots: list[list[tuple[float, dict]]] = [[]]
        last = None
        for ts, j in c.lines:
            up = j.get("up_ms")
            if isinstance(up, int):
                if last is not None and up < last:
                    boots.append([])
                last = up
            boots[-1].append((ts, j))
        for lines in boots:
            off = None
            for ts, j in lines:
                up = j.get("up_ms")
                if isinstance(up, int):
                    o = ts - up / 1000.0
                    off = o if off is None or o < off else off
            if off is None:
                continue
            for ts, j in lines:
                if j.get("t") == "event" and j.get("kind") in kinds:
                    at = j.get("at_ms")
                    if isinstance(at, int):
                        res.append((off + at / 1000.0, n, j))
    res.sort(key=lambda r: r[0])
    return res


def running_intervals(evs, t_end: float) -> dict[int, list[tuple[float, float]]]:
    iv: dict[int, list[tuple[float, float]]] = {}
    open_at: dict[int, float] = {}
    for t, n, j in evs:
        if j["kind"] == "actor_started":
            open_at[n] = t
        elif j["kind"] == "actor_stopped" and n in open_at:
            iv.setdefault(n, []).append((open_at.pop(n), t))
    for n, t in open_at.items():
        iv.setdefault(n, []).append((t, t_end))
    return iv


def overlaps(iv) -> list[tuple[int, int, float]]:
    bad = []
    nodes = list(iv)
    for i, a in enumerate(nodes):
        for b in nodes[i + 1:]:
            for s1, e1 in iv[a]:
                for s2, e2 in iv[b]:
                    o = min(e1, e2) - max(s1, s2)
                    if o > 0:
                        bad.append((a, b, o))
    return bad


def owner_regressions(evs) -> list[str]:
    bad = []
    floor: dict[int, int] = {}
    for t, n, j in evs:
        if j["kind"] != "actor_owner" or j.get("peer", 0) == 0:
            continue
        term = j.get("b", 0)
        if term < floor.get(n, 0):
            bad.append(f"0x{n:04x} accepted term {term} after {floor[n]} at {t:.3f}")
        floor[n] = max(floor.get(n, 0), term)
    return bad


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("failover", "partition"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--hold-s", type=float, default=10.0)
    ap.add_argument("--settle-s", type=float, default=25.0)
    a = ap.parse_args()

    raw = open(a.out + ".jsonl", "w", encoding="utf-8")
    report = []

    def say(s: str) -> None:
        print(s, flush=True)
        report.append(s)

    cons = {n: Console(n, p, raw) for n, p in BOARDS.items()}
    reader = Reader(raw)
    marks: list[tuple[float, str]] = []

    def mark(what: str) -> None:
        marks.append((time.time(), what))
        with out_lock:
            raw.write(json.dumps({"host_ts": marks[-1][0], "mark": what}) + "\n")
        say(f"[{time.strftime('%H:%M:%S')}] {what}")

    try:
        time.sleep(a.settle_s)
        cycles = a.cycles if a.mode == "failover" else 1
        for k in range(cycles):
            victim = reader.runner()
            if victim is None or victim not in BOARDS:
                say(f"cycle {k}: no runner visible through A (last reads {reader.reads[-3:]}); stopping")
                break
            if victim == 0x6300:
                say(f"cycle {k}: the ticker runs on A, the cabled board; cannot kill the reader. Stopping.")
                break
            if a.mode == "failover":
                mark(f"kill 0x{victim:04x} (reset held)")
                t_kill = marks[-1][0]
                cons[victim].hold_reset(True)
                time.sleep(a.hold_s)
                mark(f"release 0x{victim:04x}")
                cons[victim].hold_reset(False)
            else:
                others = [n for n in BOARDS if n != victim]
                mark(f"partition: isolate 0x{victim:04x}")
                t_kill = marks[-1][0]
                for n in others:
                    cons[n].send(f"POT! deaf {MAC_TAIL[victim]}")
                    cons[victim].send(f"POT! deaf {MAC_TAIL[n]}")
                time.sleep(a.hold_s)
                mark("heal")
                for n in BOARDS:
                    cons[n].send("POT! deaf 0")
            # The 2 s line, from outside: first Good read served by a node other than the victim.
            served = next((ts for ts, v, q, _ in reader.reads
                           if ts > t_kill and v is not None and "GOOD" in q.upper() and (v >> 16) != victim), None)
            stale_good = [r for r in reader.reads
                          if r[0] > t_kill + 1.0 and r[1] is not None and (r[1] >> 16) == victim
                          and "GOOD" in r[2].upper() and r[0] < marks[-1][0]]
            say(f"cycle {k}: served by another node {served - t_kill:.3f} s after the {a.mode} start"
                if served else f"cycle {k}: NOT served by another node before the {a.mode} ended")
            say(f"cycle {k}: Good reads carrying the {('dead' if a.mode == 'failover' else 'isolated')} "
                f"node's value, 1 s after the start and before the end: {len(stale_good)}")
            time.sleep(a.settle_s)
        time.sleep(11)  # one more stats period, so every event has been printed
    finally:
        reader.close()
        for c in cons.values():
            c.hold_reset(False)
            c.close()

    t_end = time.time()
    evs = events(cons, ("actor_started", "actor_stopped", "actor_owner"))
    for t, n, j in evs:
        say(f"  {t:.3f} 0x{n:04x} {j['kind']} node 0x{j.get('peer', 0):04x} a={j.get('a')} b={j.get('b')}")
    iv = running_intervals(evs, t_end)
    ov = overlaps(iv)
    if a.mode == "failover":
        say(f"double-running intervals (board clocks mapped to host): {len(ov)}"
            + "".join(f"\n  0x{x:04x}/0x{y:04x} overlap {o * 1000:.1f} ms" for x, y, o in ov))
    else:
        say("running intervals per board: " + "; ".join(
            f"0x{n:04x} " + ", ".join(f"{s - marks[0][0]:+.2f}..{e - marks[0][0]:+.2f}" for s, e in v)
            for n, v in iv.items()))
    reg = owner_regressions(evs)
    say(f"consumers that accepted a lower term than one they had accepted: {len(reg)}"
        + "".join(f"\n  {r}" for r in reg))
    good = sum(1 for r in reader.reads if "GOOD" in r[2].upper())
    say(f"reads: {len(reader.reads)}, Good {good}, timeouts {sum(1 for r in reader.reads if r[2] == 'TIMEOUT')}")
    raw.close()
    with open(a.out + "-report.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
