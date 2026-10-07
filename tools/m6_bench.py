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
from potluck.value import Quality  # noqa: E402

PATH = "potluck://lab/act/ticker/out"
BOARDS = {0x6300: "COM3", 0x7368: "COM4", 0x8160: "COM5"}
MAC_TAIL = {0x6300: "6300", 0x7368: "7368", 0x8160: "8160"}
CONSOLE_BAUD = 115200


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
        self.bridge = Bridge.open(port="COM6", tcp=None, baud=921600, capture=None, heartbeat=True,
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
                rec = (t0, v if isinstance(v, int) else None, Quality.name_of(int(q)), r.age_ms)
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


def split_boots(lines: list[tuple[float, dict]]) -> list[list[tuple[float, dict]]]:
    """A board's lines, one list per boot. A boot record starts a new one."""
    boots: list[list[tuple[float, dict]]] = [[]]
    for ts, j in lines:
        if j.get("t") == "boot" and boots[-1]:
            boots.append([])
        boots[-1].append((ts, j))
    return boots


def boot_offset(lines: list[tuple[float, dict]]) -> tuple[float | None, float]:
    """(offset, spread) mapping this boot's clock to the host's. From the clk lines, which each board
    prints first in a stats period while its console is idle; min(host_ts - up_ms) is the least
    delayed one. `spread` is max - min over them: an honest bound on how wrong one sample can be."""
    offs = [ts - j["up_ms"] / 1000.0 for ts, j in lines if j.get("t") == "clk" and isinstance(j.get("up_ms"), int)]
    if not offs:
        return None, 0.0
    return min(offs), max(offs) - min(offs)


def events(by_node: dict[int, list[tuple[float, dict]]], kinds: tuple[str, ...], t_start: float):
    """Every event of these kinds that happened after t_start, on the host clock, with its boot index.
    Returns (events, worst offset spread)."""
    res = []
    worst = 0.0
    for n, lines in by_node.items():
        for boot, bl in enumerate(split_boots(lines)):
            off, spread = boot_offset(bl)
            if off is None:
                continue
            worst = max(worst, spread)
            for ts, j in bl:
                if j.get("t") == "event" and j.get("kind") in kinds and isinstance(j.get("at_ms"), int):
                    t = off + j["at_ms"] / 1000.0
                    if t >= t_start:
                        res.append((t, n, j, boot))
    res.sort(key=lambda r: r[0])
    return res, worst


def running_intervals(evs, t_end: float, kills: list[tuple[float, int]]) -> dict[int, list[tuple[float, float]]]:
    """A board held in reset runs nothing: its open interval closes at the kill, not at its next event."""
    iv: dict[int, list[tuple[float, float]]] = {}
    open_at: dict[int, float] = {}
    stream = [(t, n, j) for t, n, j, _ in evs] + [(t, n, {"kind": "_kill"}) for t, n in kills]
    stream.sort(key=lambda r: r[0])
    for t, n, j in stream:
        if j["kind"] == "_kill":
            if n in open_at:
                iv.setdefault(n, []).append((open_at.pop(n), t))
        elif j["kind"] == "actor_started":
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


def causal_starts(evs) -> tuple[int, list[str]]:
    """Each start, judged on the starter's own clock, where no mapping error can reach: the last owner
    it had accepted just before starting must be nobody (0) -- it held no live claim from anyone else.
    A handover shows as owner X -> 0 -> start: X's claim withdrawn, which X sends only after it has
    stopped. Returns (starts, violations)."""
    last_owner: dict[tuple[int, int], int] = {}
    starts = 0
    bad = []
    for t, n, j, boot in evs:
        k = (n, boot)
        if j["kind"] == "actor_owner":
            last_owner[k] = j.get("peer", 0)
        elif j["kind"] == "actor_started":
            starts += 1
            prev = last_owner.get(k, 0)
            if prev not in (0, n):
                bad.append(f"0x{n:04x} started while it accepted 0x{prev:04x} as owner")
    return starts, bad


def owner_regressions(evs) -> list[str]:
    bad = []
    # Per board and per boot: a rebooted board remembers nothing, and starts its floor again.
    floor: dict[tuple[int, int], int] = {}
    for t, n, j, boot in evs:
        if j["kind"] != "actor_owner" or j.get("peer", 0) == 0:
            continue
        term = j.get("b", 0)
        k = (n, boot)
        if term < floor.get(k, 0):
            bad.append(f"0x{n:04x} accepted term {term} after {floor[k]} at {t:.3f}")
        floor[k] = max(floor.get(k, 0), term)
    return bad


def analyze(by_node, marks, reads, t_start: float, t_end: float, mode: str, say) -> None:
    t0 = marks[0][0] if marks else t_start
    starts = [m for m in marks if m[1].startswith(("kill ", "partition"))]
    ends = [m for m in marks if m[1].startswith(("release ", "heal"))]
    for k, (t_kill, what) in enumerate(starts):
        victim = int(what.split("0x")[1].split()[0], 16)
        t_stop = ends[k][0] if k < len(ends) else t_end
        served = next((ts for ts, v, q, _ in reads
                       if ts > t_kill and v is not None and q == "GOOD" and (v >> 16) != victim), None)
        stale = [r for r in reads if t_kill + 1.0 < r[0] < t_stop and r[1] is not None
                 and (r[1] >> 16) == victim and r[2] == "GOOD"]
        say(f"cycle {k}: {what}: served by another node "
            + (f"{served - t_kill:.3f} s later" if served else "NEVER, before the end"))
        say(f"cycle {k}: Good reads carrying 0x{victim:04x}'s value from 1 s after the start to the end: {len(stale)}")
    evs, spread = events(by_node, ("actor_started", "actor_stopped", "actor_owner"), t_start)
    for t, n, j, _ in evs:
        say(f"  {t - t0:+8.3f} 0x{n:04x} {j['kind']:<14} node 0x{j.get('peer', 0):04x} b={j.get('b')}")
    kills = [(t, int(w.split("0x")[1].split()[0], 16)) for t, w in marks if w.startswith("kill ")]
    iv = running_intervals(evs, t_end, kills)
    if mode == "failover":
        ov = overlaps(iv)
        say(f"running intervals overlapping, across board clocks mapped to the host's: {len(ov)}"
            + "".join(f"\n  0x{x:04x}/0x{y:04x} {o * 1000:.1f} ms" for x, y, o in ov)
            + f"\n  (clock mapping: worst spread of a board's clk samples {spread * 1000:.1f} ms)")
    else:
        say("running intervals per board, s from the partition: " + "; ".join(
            f"0x{n:04x} " + ", ".join(f"{s - t0:+.2f}..{e - t0:+.2f}" for s, e in v) for n, v in iv.items()))
    n_starts, bad = causal_starts(evs)
    say(f"starts: {n_starts}; started while accepting another node's live claim: {len(bad)}"
        + "".join(f"\n  {b}" for b in bad))
    reg = owner_regressions(evs)
    say(f"consumers that accepted a lower term than one they had accepted: {len(reg)}"
        + "".join(f"\n  {r}" for r in reg))
    say(f"reads: {len(reads)}, Good {sum(1 for r in reads if r[2] == 'GOOD')}, "
        f"timeouts {sum(1 for r in reads if r[2] == 'TIMEOUT')}")


def load(path: str):
    by_node: dict[int, list[tuple[float, dict]]] = {}
    marks, reads = [], []
    first = None
    for l in open(path, encoding="utf-8"):
        r = json.loads(l)
        first = r["host_ts"] if first is None else first
        if "mark" in r:
            marks.append((r["host_ts"], r["mark"]))
        elif "read" in r:
            reads.append((r["host_ts"], r["value"], r["quality"], r["age_ms"]))
        elif r.get("line", "").startswith("{"):
            try:
                by_node.setdefault(r["node"], []).append((r["host_ts"], json.loads(r["line"])))
            except json.JSONDecodeError:
                pass
    return by_node, marks, reads, first


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("failover", "partition", "analyze"))
    ap.add_argument("--out", required=True, help="capture prefix; for analyze, the .jsonl to read")
    ap.add_argument("--as", dest="as_mode", choices=("failover", "partition"), default="failover")
    ap.add_argument("--cycles", type=int, default=3)
    ap.add_argument("--hold-s", type=float, default=10.0)
    ap.add_argument("--settle-s", type=float, default=25.0)
    a = ap.parse_args()

    report = []

    def say(s: str) -> None:
        print(s, flush=True)
        report.append(s)

    if a.mode == "analyze":
        by_node, marks, reads, first = load(a.out)
        last = max([r[0] for r in reads] + [m[0] for m in marks])
        analyze(by_node, marks, reads, first, last, a.as_mode, say)
        return 0

    raw = open(a.out + ".jsonl", "w", encoding="utf-8")
    t_start = time.time()
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
                cons[victim].hold_reset(True)
                time.sleep(a.hold_s)
                mark(f"release 0x{victim:04x}")
                cons[victim].hold_reset(False)
            else:
                others = [n for n in BOARDS if n != victim]
                mark(f"partition: isolate 0x{victim:04x}")
                for n in others:
                    cons[n].send(f"POT! deaf {MAC_TAIL[victim]}")
                    cons[victim].send(f"POT! deaf {MAC_TAIL[n]}")
                time.sleep(a.hold_s)
                mark("heal")
                for n in BOARDS:
                    cons[n].send("POT! deaf 0")
            time.sleep(a.settle_s)
        time.sleep(11)  # one more stats period, so every event has been printed
    finally:
        reader.close()
        for c in cons.values():
            c.hold_reset(False)
            c.close()
    raw.close()
    analyze({n: c.lines for n, c in cons.items()}, marks, reader.reads, t_start, time.time(), a.mode, say)
    with open(a.out + "-report.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
