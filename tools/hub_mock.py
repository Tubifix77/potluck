#!/usr/bin/env python3
"""A stand-in for `potluck-hub` (M14) that speaks the hub's API version 1 -- for building the dashboard (M16).

    python tools/hub_mock.py [--host 127.0.0.1] [--port 8470] [--period-ms 2000] [--death-window-ms 3000]
                             [--kill NODE[@SECONDS]]... [--asleep NODE]... [--stale] [--reverted]
                             [--other-cell] [--link-down]

Serves the dashboard's static files (host/potluck/potluck/dashboard/) at `/` and implements, exactly as
host/potluck/HUB-API.md states them, GET /api/model, GET /api/events?since=<seq>, GET /api/stream (server-sent
events) and POST /api/write -- from a simulated cell of three boards: A (6300, cabled to the PC), B (7368, the
extender and the presenter) and C (8160). Values move with time. It opens no serial port and talks to no board.

Faults, from the command line or at run time with GET /mock/fault?<name>=<value>[&...] (mock only, not the API):

    --kill 8160@20     board 8160 goes silent 20 s after start (default 10 s) and is dead one death window later;
                       its portable actors fail over 1.5 s after that.   /mock/fault?kill=8160&after=5
                                                                         /mock/fault?revive=8160
    --asleep 8160      board 8160 is asleep (a declared long window).    /mock/fault?asleep=8160 | wake=8160
    --stale            B's die temperature has been STALE for 30 s.      /mock/fault?stale=1 | stale=0
    --reverted         the last deploy (package 48) was just reverted.      /mock/fault?reverted=1 | reverted=0
    --other-cell       another cell's frames are heard on the channel.   /mock/fault?other_cell=1 | other_cell=0
    --link-down        the hub's serial link to A is down.               /mock/fault?link_down=1 | link_down=0

Only potluck://lab/node-7368/led/on is writable from outside (it flips B's LED); any other path gets 403 as the
contract says. Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import mimetypes
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parent.parent
DASHBOARD = ROOT / "host" / "potluck" / "potluck" / "dashboard"

SYSTEM = "lab"
CELL_ID = "ef76cc2e"
FIRMWARE = "78c5305-6df6a4"
FW_COUNTER = 20
PACKAGE = 47
NODES = ["6300", "7368", "8160"]
LABELS = {"6300": "A", "7368": "B", "8160": "C"}
PRESENTER = "7368"
POCKET = "8160"
LED_PATH = f"potluck://{SYSTEM}/node-7368/led/on"
DIE_PATH = f"potluck://{SYSTEM}/node-7368/hw/die_temp"
FAILOVER_MS = 1500
MIN = 60_000

# Everything a board carries that does not move. `modules` absent from a board's list = not on its image.
STATIC = {
    "6300": {"cabled": True, "roles": ["reconciler", "relay", "enrolled"],
             "modules": [("present", "idle", "the presenter is 7368"), ("wifi", "idle", "no credentials")],
             "heap": (161200, 149870, 94208, 8250000), "rssi": None},
    "7368": {"cabled": False, "roles": ["extender", "sandbox", "reconciler", "relay", "enrolled"],
             "modules": [("present", "active", ""), ("wifi", "idle", "another module is the connection")],
             "heap": (152831, 141379, 77824, 8100000), "rssi": -34},
    "8160": {"cabled": False, "roles": ["sandbox", "reconciler", "enrolled"],
             "modules": [("present", "idle", "the presenter is 7368"), ("wifi", "idle", "no credentials"),
                         ("pocket", "active", "")],
             "heap": (158400, 146020, 90112, 8240000), "rssi": -41},
}
LINK_BASE = {  # (from, to): (delivery, rtt p50, rtt p99, rssi)
    ("6300", "7368"): (0.9952, 5, 16, -34), ("6300", "8160"): (0.9981, 4, 14, -41),
    ("7368", "6300"): (0.9947, 5, 17, -35), ("7368", "8160"): (0.9915, 6, 22, -47),
    ("8160", "6300"): (0.9978, 4, 15, -40), ("8160", "7368"): (0.9921, 6, 21, -46),
}
# name, module, placement, nodes, outputs (a "{owner}" output belongs to whichever board runs the portable actor)
APPS = [
    ("die_b", "builtin:die_temp", "pinned", ["7368"], [DIE_PATH]),
    ("led_b", "builtin:led_switch", "pinned", ["7368"], [LED_PATH]),
    ("rf_link", "ext:passive_sensor/rf_link", "pinned", NODES,
     [f"potluck://{SYSTEM}/node-{n}/rf/link" for n in NODES]),
    ("rf_fusion", "ext:passive_sensor/rf_fusion", "portable", NODES, [f"potluck://{SYSTEM}/act/rf_fusion/out"]),
    ("alarm", "guest:acme/alarm", "portable", ["6300", "8160"], [f"potluck://{SYSTEM}/act/alarm/level"]),
]


def now_ms() -> int:
    return int(time.time() * 1000)


class SimCell:
    """The simulated cell and the hub's model of it. Thread-safe; every read advances the simulation to `clock()`."""

    def __init__(self, death_window_ms: int = 3000, period_ms: int = 2000, clock=None):
        self.clock = clock or now_ms
        self.death_window_ms = death_window_ms
        self.period_ms = period_ms
        self.lock = threading.RLock()
        t0 = self.clock()
        self.t0 = t0
        self.hub_start = t0 - 90 * MIN
        self.boot_at = {"6300": t0 - 60 * MIN, "7368": t0 - 59 * MIN + 4100, "8160": t0 - 59 * MIN + 9300}
        self.kill_at: dict[str, int] = {}  # node -> when it went silent
        self.asleep_at: dict[str, int] = {}
        self.owner = {"rf_fusion": "8160", "alarm": "6300"}
        self.moved_at: dict[str, int] = {}
        self.stale_since: int | None = None
        self.other_cell_since: int | None = None
        self.link_down_since: int | None = None
        self.reverted_at: int | None = None
        self.led = False
        self.led_at = t0 - 20 * MIN
        self.deaths: dict[tuple[str, str], int] = {}
        self.events: list[dict] = []
        self.seq = 5400
        self.done: set = set()
        self.cond = threading.Condition(self.lock)
        self._initial_events()

    # ---- events ---------------------------------------------------------------------------------------------------

    def _emit(self, at: int, board: str, kind: str, detail: dict) -> None:
        with self.lock:
            self.events.append({"seq": self.seq, "at": at, "board": board, "kind": kind, "detail": detail})
            self.seq += 1
            self.cond.notify_all()

    def _initial_events(self) -> None:
        h = self.hub_start
        self._emit(h, "hub", "hub_started", {"version": "mock"})
        self._emit(h + 180, "hub", "link_up", {"port": "COM6"})
        self._emit(self.t0 - 61 * MIN, "hub", "deploy",
                   {"kind": "firmware", "counter": FW_COUNTER, "result": "running", "detail": "rollout started"})
        for n in NODES:
            self._emit(self.boot_at[n], n, "peer_rebooted", {"reason": "firmware update"})
        self._emit(self.t0 - 57 * MIN, "hub", "deploy",
                   {"kind": "firmware", "counter": FW_COUNTER, "result": "confirmed", "detail": "3 board(s)"})
        self._emit(self.t0 - 50 * MIN, "hub", "deploy",
                   {"kind": "package", "counter": PACKAGE, "result": "verified", "detail": "live on 3 board(s)"})
        for name, _m, placement, nodes, _o in APPS:
            where = [self.owner[name]] if placement == "portable" else nodes
            for n in where:
                self._emit(self.t0 - 50 * MIN + 900, n, "actor_started", {"actor": name})
        self._emit(self.t0 - 20 * MIN, "7368", "outside_write", {"resource": LED_PATH, "value": False,
                                                                 "result": "applied"})

    def _advance(self, now: int) -> None:
        """Fire the events whose time has come (deaths, fail-overs)."""
        for n, k in list(self.kill_at.items()):
            dead_at = k + self.death_window_ms
            if now >= dead_at and ("dead", n, k) not in self.done:
                self.done.add(("dead", n, k))
                for peer in NODES:
                    if peer != n and self._state(peer, dead_at)[0] == "alive":
                        self.deaths[(peer, n)] = self.deaths.get((peer, n), 0) + 1
                        self._emit(dead_at, peer, "peer_dead", {"peer": n})
            for app, owner in list(self.owner.items()):
                if owner != n or now < dead_at + FAILOVER_MS or ("move", app, k) in self.done:
                    continue
                self.done.add(("move", app, k))
                nodes = next(a[3] for a in APPS if a[0] == app)
                alive = [m for m in nodes if self._state(m, now)[0] in ("alive", "asleep")]
                if alive:
                    to = min(alive)
                    self.owner[app] = to
                    self.moved_at[app] = dead_at + FAILOVER_MS
                    self._emit(dead_at + FAILOVER_MS, to, "actor_owner", {"actor": app, "from": n, "to": to})
                    self._emit(dead_at + FAILOVER_MS + 40, to, "actor_started", {"actor": app})

    # ---- faults ---------------------------------------------------------------------------------------------------

    def kill(self, node: str, after_s: float = 10.0) -> None:
        with self.lock:
            self.kill_at[node] = self.clock() + int(after_s * 1000)

    def revive(self, node: str) -> None:
        with self.lock:
            if self.kill_at.pop(node, None) is None:
                return
            now = self.clock()
            self.boot_at[node] = now
            for peer in NODES:
                if peer != node:
                    self._emit(now, peer, "peer_alive", {"peer": node})
            self._emit(now + 20, node, "peer_rebooted", {"reason": "power on"})

    def set_asleep(self, node: str, asleep: bool) -> None:
        with self.lock:
            if asleep:
                self.asleep_at.setdefault(node, self.clock())
            elif self.asleep_at.pop(node, None) is not None:
                self._emit(self.clock(), node, "peer_alive", {"peer": node, "woke": True})

    def set_stale(self, on: bool) -> None:
        with self.lock:
            self.stale_since = (self.clock() - 30_000) if on else None

    def set_other_cell(self, on: bool) -> None:
        with self.lock:
            self.other_cell_since = self.clock() if on else None

    def set_link_down(self, on: bool) -> None:
        with self.lock:
            now = self.clock()
            if on and self.link_down_since is None:
                self.link_down_since = now
                self._emit(now, "hub", "link_down", {"port": "COM6", "detail": "no frames"})
            elif not on and self.link_down_since is not None:
                self.link_down_since = None
                self._emit(now, "hub", "link_up", {"port": "COM6"})

    def set_reverted(self, on: bool) -> None:
        with self.lock:
            if on and self.reverted_at is None:
                self.reverted_at = self.clock()  # now: the event log stays in time order
                self._emit(self.reverted_at, "hub", "deploy",
                           {"kind": "package", "counter": PACKAGE + 1, "result": "reverted",
                            "detail": "8160 failed its trial (module present: NO_MEM); 3 board(s) back on 47"})
            elif not on:
                self.reverted_at = None

    def faults(self) -> dict:
        with self.lock:
            return {"kill": {n: k for n, k in self.kill_at.items()}, "asleep": sorted(self.asleep_at),
                    "stale": self.stale_since is not None, "reverted": self.reverted_at is not None,
                    "other_cell": self.other_cell_since is not None, "link_down": self.link_down_since is not None}

    # ---- the model ------------------------------------------------------------------------------------------------

    def _state(self, n: str, now: int) -> tuple[str, int, int]:
        """(state, since_at, silent_ms) as the hub sees board n at `now`."""
        k = self.kill_at.get(n)
        if k is not None and now >= k:
            if now >= k + self.death_window_ms:
                return "dead", k + self.death_window_ms, now - k
            return "alive", self.boot_at[n], now - k
        if n in self.asleep_at:
            return "asleep", self.asleep_at[n], now - self.asleep_at[n]
        return "alive", self.boot_at[n], 0

    def _frozen_at(self, n: str, now: int) -> int:
        """The last moment board n was heard (now, if it still is)."""
        k = self.kill_at.get(n)
        if k is not None and now >= k:
            return k
        if n in self.asleep_at:
            return self.asleep_at[n]
        return now

    def _resources(self, now: int) -> list[dict]:
        s = (now - self.t0) / 1000.0
        out = []

        def add(path, owner, value, unit, lc, exposed=False, writable=False, safety=False, phase=0, strict=False,
                quality=None, age=None):
            heard = self._frozen_at(owner, now)
            st = self._state(owner, now)[0]
            fresh = (now + phase * 331) % self.period_ms + 120
            a = age if age is not None else fresh + (now - heard)
            q = quality or ("UNAVAILABLE" if st == "dead" else "GOOD")
            v = value
            if q != "GOOD" and (strict or q == "UNAVAILABLE"):
                v = None
            out.append({"path": path, "name": path.split("://", 1)[1].split("/", 1)[1], "owner": owner,
                        "value": v, "unit": unit, "quality": q, "age_ms": int(a), "latency_class": lc,
                        "exposed": exposed, "outside_writable": writable, "safety_relevant": safety})

        die = round(51.5 + 1.2 * math.sin(s / 60.0), 1)
        if self.stale_since is not None and self._state("7368", now)[0] != "dead":
            add(DIE_PATH, "7368", 49.8, "C", 4, exposed=True, quality="STALE", age=now - self.stale_since)
        else:
            add(DIE_PATH, "7368", die, "C", 4, exposed=True, phase=1)
        add(LED_PATH, "7368", self.led, "", 4, exposed=True, writable=True, phase=2, strict=True)
        for i, n in enumerate(NODES):
            add(f"potluck://{SYSTEM}/node-{n}/rf/link", n, round(-38 + 3 * math.sin(s / 7.0 + i), 1), "dB", 4,
                phase=3 + i, strict=True)
        for app, path, value, unit, lc, safety in (
                ("rf_fusion", f"potluck://{SYSTEM}/act/rf_fusion/out", int(s // 40) % 2 == 1, "", 4, False),
                ("alarm", f"potluck://{SYSTEM}/act/alarm/level", 1 if int(s // 40) % 2 == 1 else 0, "", 3, True)):
            owner = self.owner[app]
            st = self._state(owner, now)[0]
            if st == "dead":  # fail-over pending: nobody publishes it
                k = self.kill_at[owner]
                add(path, owner, None, unit, lc, exposed=True, safety=safety, quality="UNAVAILABLE", age=now - k)
            else:
                add(path, owner, value, unit, lc, exposed=True, safety=safety, phase=6)
        for i, n in enumerate(NODES):
            up = (self._frozen_at(n, now) - self.boot_at[n]) // 1000
            add(f"potluck://{SYSTEM}/node-{n}/sys/uptime", n, up, "s", 4, phase=7 + i)
        return out

    def _apps(self, now: int, res: dict[str, dict]) -> list[dict]:
        out = []
        for name, module, placement, nodes, outputs in APPS:
            if placement == "portable":
                o = self.owner[name]
                running = [o] if self._state(o, now)[0] != "dead" else []
            else:
                running = [n for n in nodes if self._state(n, now)[0] != "dead"]
            detail = ""
            if placement == "pinned" and set(running) != set(nodes):
                detail = "not running on " + ", ".join(n for n in nodes if n not in running) + " (board dead)"
            elif placement == "portable" and len(running) != 1:
                detail = "not running anywhere (owner %s dead, fail-over pending)" % self.owner[name]
            else:
                for p in outputs:
                    r = res.get(p)
                    if r and r["quality"] != "GOOD":
                        detail = "output %s %s for %s s" % (r["name"], r["quality"], r["age_ms"] // 1000)
                        break
            out.append({"name": name, "module": module, "placement": placement, "nodes": list(nodes),
                        "running_on": running, "working": detail == "", "detail": detail,
                        "outputs": list(outputs)})
        return out

    def _boards(self, now: int) -> list[dict]:
        out = []
        for i, n in enumerate(NODES):
            st, since, silent = self._state(n, now)
            heard = self._frozen_at(n, now)
            hs = (heard - self.t0) / 1000.0
            free, low, largest, psram = STATIC[n]["heap"]
            links = []
            for (a, b), (dl, p50, p99, rssi) in LINK_BASE.items():
                if a != n:
                    continue
                pst = self._state(b, heard)[0]
                lk = {"peer": b, "state": pst,
                      "delivery": round(min(1.0, dl + 0.002 * math.sin(hs / 11.0 + i)), 4),
                      "rtt_p50_ms": p50, "rtt_p99_ms": p99 + int(3 * (1 + math.sin(hs / 13.0 + i))),
                      "deaths": self.deaths.get((a, b), 0), "rssi": rssi}
                if pst == "dead":
                    lk.update(delivery=0.0, rtt_p50_ms=None, rtt_p99_ms=None)
                links.append(lk)
            actors = []
            for name, _m, placement, nodes, _o in APPS:
                if (placement == "pinned" and n in nodes) or (placement == "portable" and self.owner[name] == n):
                    actors.append(name)
            if n == PRESENTER:
                actors.append("present")
            mods = [{"name": m, "state": ms, "reason": why} for m, ms, why in STATIC[n]["modules"]]
            out.append({
                "node": n, "label": LABELS[n], "state": st, "since_at": since, "silent_ms": silent,
                "rssi": STATIC[n]["rssi"], "cabled": STATIC[n]["cabled"],
                "firmware": {"version": FIRMWARE, "counter": FW_COUNTER, "state": "confirmed"},
                "package": {"counter": PACKAGE, "state": "confirmed", "live_applies": 1},
                "roles": list(STATIC[n]["roles"]), "modules": mods, "actors": actors,
                "heap": {"internal_free": int(free + 1800 * math.sin(hs / 17.0 + i)), "internal_min": low,
                         "internal_largest": largest, "psram_free": psram},
                "up_ms": heard - self.boot_at[n], "links": links})
        return out

    def _integrations(self, now: int) -> list[dict]:
        pst, psince, _ = self._state(PRESENTER, now)
        if pst == "dead":
            present = ("disconnected", psince, "presenter 7368 dead; no other board holds a connection")
        else:
            present = ("connected", self.boot_at[PRESENTER] + 21_000, "192.168.0.77:1883, 4 entities")
        cst, csince, _ = self._state(POCKET, now)
        pocket = ("disconnected", csince, "board 8160 dead") if cst == "dead" else \
            ("idle", self.boot_at[POCKET] + 2_000, "access point up, no phone joined")
        return [
            {"name": "presenting (MQTT)", "kind": "board", "where": PRESENTER, "state": present[0],
             "since_at": present[1], "detail": present[2]},
            {"name": "pocket page", "kind": "board", "where": POCKET, "state": pocket[0], "since_at": pocket[1],
             "detail": pocket[2]},
            {"name": "Home Assistant", "kind": "hub", "where": "hub", "state": "off", "since_at": self.hub_start,
             "detail": "not configured"},
        ]

    def _deploys(self) -> list[dict]:
        d = [{"at": self.t0 - 50 * MIN, "kind": "package", "counter": PACKAGE, "result": "verified",
              "detail": "live on 3 board(s)"},
             {"at": self.t0 - 57 * MIN, "kind": "firmware", "counter": FW_COUNTER, "result": "confirmed",
              "detail": "3 board(s)"},
             {"at": self.t0 - 26 * 60 * MIN, "kind": "package", "counter": PACKAGE - 1, "result": "verified",
              "detail": "live on 3 board(s)"}]
        if self.reverted_at is not None:
            d.insert(0, {"at": self.reverted_at, "kind": "package", "counter": PACKAGE + 1, "result": "reverted",
                         "detail": "8160 failed its trial (module present: NO_MEM); 3 board(s) back on 47"})
        return d

    def model(self) -> dict:
        with self.lock:
            now = self.clock()
            self._advance(now)
            s = (now - self.t0) / 1000.0
            res = self._resources(now)
            boards = self._boards(now)
            alive = [b for b in boards if b["state"] != "dead"]
            links = [lk for b in alive for lk in b["links"] if lk["state"] == "alive"]
            p50s = sorted(lk["rtt_p50_ms"] for lk in links) or [0]
            recent_death = any(now - (k + self.death_window_ms) < 5 * MIN for k in self.kill_at.values()
                               if now >= k + self.death_window_ms)
            other = self.other_cell_since
            link_up_for = now - self.hub_start if self.link_down_since is None else self.link_down_since - self.hub_start
            return {
                "api": 1,
                "at": now,
                "hub": {"version": "mock", "up_ms": now - self.hub_start, "port": "COM6",
                        "link": "down" if self.link_down_since is not None else "up",
                        "link_detail": "" if self.link_down_since is None else
                        "COM6: no frames for %d s" % ((now - self.link_down_since) // 1000),
                        "frames_rx": 120345 + link_up_for * 61 // 1000, "frames_tx": 30211 + link_up_for * 15 // 1000,
                        "pull_period_ms": self.period_ms, "clock_error_ms": 35},
                "cell": {"system": SYSTEM, "cell_id": CELL_ID, "channel": 1, "package": PACKAGE, "firmware": FIRMWARE,
                         "other_cell_frames": 0 if other is None else 1 + (now - other) * 3 // 1000,
                         "presenter": PRESENTER},
                "boards": boards,
                "apps": self._apps(now, {r["path"]: r for r in res}),
                "resources": res,
                "integrations": self._integrations(now),
                "deploys": self._deploys(),
                "signals": {
                    "latency_p50_ms": p50s[len(p50s) // 2],
                    "latency_p99_ms": max((lk["rtt_p99_ms"] for lk in links), default=0),
                    "traffic_frames_s": round((62.0 + 4 * math.sin(s / 9.0)) * len(alive) / len(NODES), 1),
                    "errors_min": round(0.4 + 0.2 * math.sin(s / 23.0) + (2.0 if recent_death else 0.0), 1),
                    "saturation": {"peers": 0.15, "namespace": 0.31, "actor_slots": 0.38,
                                   "internal_ram": round(0.24 + 0.01 * math.sin(s / 17.0), 2),
                                   "airtime": round(0.26 + 0.03 * math.sin(s / 5.0) + (0.12 if other else 0), 2)}},
            }

    def events_since(self, since: int | None) -> dict:
        with self.lock:
            self._advance(self.clock())
            ev = self.events[-200:] if since is None else [e for e in self.events if e["seq"] >= since]
            return {"api": 1, "next": self.seq, "events": [dict(e) for e in ev]}

    def write(self, path, value) -> tuple[int, dict]:
        with self.lock:
            now = self.clock()
            self._advance(now)
            if path != LED_PATH:
                if path in {r["path"] for r in self._resources(now)}:
                    self._emit(now, "hub", "outside_write", {"resource": path, "value": value, "result": "refused"})
                return 403, {"ok": False, "result": "not writable from outside"}
            if not isinstance(value, bool):
                return 400, {"ok": False, "result": "value must be true or false"}
            if self._state("7368", now)[0] == "dead":
                self._emit(now, "hub", "outside_write", {"resource": path, "value": value, "result": "unreachable"})
                return 504, {"ok": False, "result": "owner 7368 unreachable"}
            self.led = value
            self.led_at = now
            self._emit(now, "7368", "outside_write", {"resource": path, "value": value, "result": "applied"})
            return 200, {"ok": True, "result": "applied"}


# ---- HTTP ---------------------------------------------------------------------------------------------------------

class MockServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, addr, cell: SimCell, static_dir: Path = DASHBOARD):
        super().__init__(addr, Handler)
        self.cell = cell
        self.static_dir = static_dir
        self.stopping = threading.Event()

    def stop(self) -> None:
        self.stopping.set()
        with self.cell.lock:
            self.cell.cond.notify_all()
        self.shutdown()
        self.server_close()


class Handler(BaseHTTPRequestHandler):
    server: MockServer
    server_version = "potluck-hub-mock/1"

    def log_message(self, fmt, *args):  # quiet: the stream would fill the console
        pass

    def _json(self, code: int, obj) -> None:
        body = json.dumps(obj, separators=(",", ":")).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        cell = self.server.cell
        if u.path == "/api/model":
            return self._json(200, cell.model())
        if u.path == "/api/events":
            since = q.get("since", [None])[0]
            try:
                return self._json(200, cell.events_since(int(since) if since is not None else None))
            except ValueError:
                return self._json(400, {"ok": False, "result": "since must be a sequence number"})
        if u.path == "/api/stream":
            return self._stream()
        if u.path == "/mock/fault":
            return self._fault(q)
        return self._static(u.path)

    def do_POST(self):
        u = urlparse(self.path)
        if u.path != "/api/write":
            return self._json(404, {"ok": False, "result": "no such endpoint"})
        try:
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
            path, value = body["path"], body["value"]
        except (ValueError, KeyError, TypeError):
            return self._json(400, {"ok": False, "result": "body must be {\"path\": ..., \"value\": ...}"})
        code, obj = self.server.cell.write(path, value)
        self._json(code, obj)

    def _fault(self, q) -> None:
        cell = self.server.cell
        on = lambda k: q[k][0] not in ("0", "false", "off", "")  # noqa: E731
        if "kill" in q:
            cell.kill(q["kill"][0], float(q.get("after", ["0"])[0]))
        if "revive" in q:
            cell.revive(q["revive"][0])
        if "asleep" in q:
            cell.set_asleep(q["asleep"][0], True)
        if "wake" in q:
            cell.set_asleep(q["wake"][0], False)
        if "stale" in q:
            cell.set_stale(on("stale"))
        if "reverted" in q:
            cell.set_reverted(on("reverted"))
        if "other_cell" in q:
            cell.set_other_cell(on("other_cell"))
        if "link_down" in q:
            cell.set_link_down(on("link_down"))
        self._json(200, cell.faults())

    def _static(self, path: str) -> None:
        name = "index.html" if path in ("", "/") else path.lstrip("/")
        root = self.server.static_dir.resolve()
        f = (root / name).resolve()
        if root not in f.parents or not f.is_file():
            return self._json(404, {"ok": False, "result": "not found"})
        body = f.read_bytes()
        ctype = {".js": "text/javascript", ".css": "text/css", ".html": "text/html"}.get(
            f.suffix, mimetypes.guess_type(f.name)[0] or "application/octet-stream")
        self.send_response(200)
        self.send_header("Content-Type", ctype + "; charset=utf-8" if ctype.startswith("text/") else ctype)
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_event(self, name: str, obj) -> None:
        self.wfile.write(("event: %s\ndata: %s\n\n" % (name, json.dumps(obj, separators=(",", ":")))).encode())
        self.wfile.flush()

    def _stream(self) -> None:
        cell = self.server.cell
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(b"retry: 2000\n\n")
            self._send_event("model", cell.model())
            backlog = cell.events_since(None)
            self._send_event("events", backlog)
            nxt = backlog["next"]
            due = time.monotonic() + cell.period_ms / 1000.0
            while not self.server.stopping.is_set():
                with cell.lock:
                    cell.cond.wait(timeout=0.25)
                new = cell.events_since(nxt)
                if new["events"]:
                    self._send_event("events", new)
                nxt = new["next"]
                if time.monotonic() >= due:
                    self._send_event("model", cell.model())
                    due += cell.period_ms / 1000.0
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, OSError):
            return


# ---- command line -------------------------------------------------------------------------------------------------

def parse_args(argv=None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="A mock of potluck-hub's API v1, for the dashboard (M16).")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8470)
    ap.add_argument("--period-ms", type=int, default=2000, help="the hub's pull period: one model per stream tick")
    ap.add_argument("--death-window-ms", type=int, default=3000, help="silence before a board is declared dead")
    ap.add_argument("--kill", action="append", default=[], metavar="NODE[@SECONDS]",
                    help="board NODE goes silent SECONDS after start (default 10), dead one death window later")
    ap.add_argument("--asleep", action="append", default=[], metavar="NODE", help="board NODE is asleep")
    ap.add_argument("--stale", action="store_true", help="B's die temperature is STALE")
    ap.add_argument("--reverted", action="store_true", help="the last deploy was reverted")
    ap.add_argument("--other-cell", action="store_true", help="another cell shares the channel")
    ap.add_argument("--link-down", action="store_true", help="the hub's serial link is down")
    return ap.parse_args(argv)


def build(args: argparse.Namespace, clock=None) -> MockServer:
    cell = SimCell(death_window_ms=args.death_window_ms, period_ms=args.period_ms, clock=clock)
    for k in args.kill:
        node, _, after = k.partition("@")
        if node not in NODES:
            raise SystemExit("--kill: unknown board %s (boards: %s)" % (node, ", ".join(NODES)))
        cell.kill(node, float(after) if after else 10.0)
    for n in args.asleep:
        cell.set_asleep(n, True)
    if args.stale:
        cell.set_stale(True)
    if args.reverted:
        cell.set_reverted(True)
    if args.other_cell:
        cell.set_other_cell(True)
    if args.link_down:
        cell.set_link_down(True)
    return MockServer((args.host, args.port), cell)


def main(argv=None) -> int:
    args = parse_args(argv)
    srv = build(args)
    host, port = srv.server_address[:2]
    print("hub mock: http://%s:%d/  (API v1; faults: %s)" % (host, port, json.dumps(srv.cell.faults())), flush=True)
    try:
        srv.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        srv.stopping.set()
        srv.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
