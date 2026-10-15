"""M18: every outside program has a share of what the hub may ask of the cell, and reports its own state.

FAIR SHARE. Each program -- a login or a paired token by name; on 127.0.0.1 the `X-Potluck-Client` header it sends,
else its address -- has two token buckets: reads (every API request; 20 a second, bursts of 40) and writes (each a frame
to a board over the radio; 2 a second, bursts of 4). A program over its share gets `429` with `Retry-After`, and only
it is slowed: reads are served from the hub's one model, so they never reach the cell anyway, and writes, which do,
cannot crowd it. The open stream counts once, when it opens. `hub.clients` in the model lists every program seen in
the last 10 minutes with what it asked and what was refused.

INTEGRATION REPORTS. An integration says how it is (`POST /api/integrations {"name", "state": "connected" |
"disconnected" | "error" | "idle", "detail"}`); the model lists it beside the cell's own presenter in `integrations`,
and a report older than 60 s reads `disconnected` -- an integration that stopped reporting has stopped.
"""

from __future__ import annotations

import threading
import time

READ_RATE, READ_BURST = 20.0, 40.0
WRITE_RATE, WRITE_BURST = 2.0, 4.0
SEEN_S, REPORT_FRESH_S = 600.0, 60.0
STATES = ("connected", "disconnected", "error", "idle")


class Bucket:
    def __init__(self, rate: float, burst: float) -> None:
        self.rate, self.burst = rate, burst
        self.level = burst
        self.at = time.monotonic()

    def take(self) -> float:
        """0.0 if allowed, else the seconds until it would be."""
        now = time.monotonic()
        self.level = min(self.burst, self.level + (now - self.at) * self.rate)
        self.at = now
        if self.level >= 1.0:
            self.level -= 1.0
            return 0.0
        return (1.0 - self.level) / self.rate


class FairShare:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.clients: dict[str, dict] = {}

    def take(self, who: str, kind: str) -> float:
        with self.lock:
            c = self.clients.get(who)
            if c is None:
                c = self.clients[who] = {"read": Bucket(READ_RATE, READ_BURST), "write": Bucket(WRITE_RATE, WRITE_BURST),
                                         "reads": 0, "writes": 0, "refused": 0, "first_at": int(time.time() * 1000)}
            wait = c[kind].take()
            c["reads" if kind == "read" else "writes"] += 1
            if wait > 0:
                c["refused"] += 1
            c["last_at"] = int(time.time() * 1000)
            c["last_t"] = time.monotonic()
            return wait

    def snapshot(self) -> list[dict]:
        now = time.monotonic()
        with self.lock:
            for k in [k for k, c in self.clients.items() if now - c.get("last_t", now) > SEEN_S]:
                del self.clients[k]
            return [{"name": k, "requests": c["reads"], "writes": c["writes"], "refused": c["refused"],
                     "last_at": c.get("last_at")} for k, c in sorted(self.clients.items())]


class Reports:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.reports: dict[str, dict] = {}

    def report(self, name: str, state: str, detail: str, by: str) -> bool:
        if not name or state not in STATES:
            return False
        now = int(time.time() * 1000)
        with self.lock:
            r = self.reports.get(name)
            if r is None or r["state"] != state:
                since = now
            else:
                since = r["since_at"]
            self.reports[name] = {"name": name[:64], "kind": "program", "where": by[:64], "state": state,
                                  "since_at": since, "detail": str(detail)[:200], "last_at": now,
                                  "t": time.monotonic()}
        return True

    def forget(self, name: str) -> bool:
        """The owner's: drop a program that is no longer connected (it stopped for good, or was a test) -- the hub
        otherwise lists it as down for as long as it runs. A connected program is never forgotten."""
        with self.lock:
            r = self.reports.get(name)
            if r is None:
                return False
            if time.monotonic() - r["t"] <= REPORT_FRESH_S and r["state"] == "connected":
                return False
            del self.reports[name]
            return True

    def snapshot(self) -> list[dict]:
        now = time.monotonic()
        out = []
        with self.lock:
            for r in self.reports.values():
                x = {k: v for k, v in r.items() if k != "t"}
                if now - r["t"] > REPORT_FRESH_S and x["state"] != "disconnected":
                    x["state"], x["detail"] = "disconnected", f"silent for {int(now - r['t'])} s"
                out.append(x)
        return sorted(out, key=lambda x: x["name"])
