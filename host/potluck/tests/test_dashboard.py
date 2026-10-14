"""M16: the dashboard's first cut and the hub mock it is built against (tools/hub_mock.py, host/potluck/HUB-API.md).

The mock is started on a free port in a thread. These tests hold the mock to the contract (every top-level key, the
documented field types and enumerations, the stream, the write rights) and the page to its first-screen landmarks,
without a browser and without any board.
"""

from __future__ import annotations

import http.client
import importlib.util
import json
import os
import re
import threading
import unittest
import urllib.error
import urllib.request
from html.parser import HTMLParser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
DASHBOARD = os.path.join(HERE, "..", "potluck", "dashboard")

_spec = importlib.util.spec_from_file_location("hub_mock", os.path.join(ROOT, "tools", "hub_mock.py"))
hub_mock = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hub_mock)

LED = "potluck://lab/node-7368/led/on"
DIE = "potluck://lab/node-7368/hw/die_temp"
TOP_KEYS = {"api", "at", "hub", "cell", "boards", "apps", "resources", "integrations", "deploys", "signals"}
# The first screen's landmarks, documented in index.html's header comment.
LANDMARKS = {"conn", "status", "verdict", "problems", "summary", "sum-boards", "sum-apps", "sum-integrations",
             "sum-deploy", "sum-hub", "sum-channel", "signals", "use", "tabs", "tab-cell", "tab-apps", "tab-data",
             "tab-integrations", "tab-deploys", "tab-events"}


class Mock:
    def __init__(self, *argv: str, clock=None):
        self.srv = hub_mock.build(hub_mock.parse_args(["--port", "0", *argv]), clock=clock)
        self.port = self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()

    def url(self, path: str) -> str:
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def get(self, path: str):
        with urllib.request.urlopen(self.url(path), timeout=5) as r:
            return r.status, r.headers, r.read()

    def get_json(self, path: str):
        return json.loads(self.get(path)[2])

    def post_json(self, path: str, obj):
        req = urllib.request.Request(self.url(path), data=json.dumps(obj).encode(), method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def stop(self):
        self.srv.stop()
        self.thread.join(timeout=5)


def is_int(x) -> bool:
    return isinstance(x, int) and not isinstance(x, bool)


def is_num(x) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool)


class ContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mock = Mock()

    @classmethod
    def tearDownClass(cls):
        cls.mock.stop()

    def test_model_has_every_top_level_key_and_types(self):
        status, headers, body = self.mock.get("/api/model")
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("application/json"))
        m = json.loads(body)
        self.assertTrue(TOP_KEYS <= set(m), TOP_KEYS - set(m))
        self.assertEqual(m["api"], 1)
        self.assertTrue(is_int(m["at"]))

        hub = m["hub"]
        for k in ("up_ms", "frames_rx", "frames_tx", "pull_period_ms", "clock_error_ms"):
            self.assertTrue(is_int(hub[k]), k)
        for k in ("version", "port", "link", "link_detail"):
            self.assertIsInstance(hub[k], str, k)

        cell = m["cell"]
        for k in ("system", "cell_id", "firmware", "presenter"):
            self.assertIsInstance(cell[k], str, k)
        for k in ("channel", "package", "other_cell_frames"):
            self.assertTrue(is_int(cell[k]), k)

        self.assertEqual([b["node"] for b in m["boards"]], ["6300", "7368", "8160"])
        for b in m["boards"]:
            self.assertIn(b["state"], {"alive", "dead", "asleep", "unknown"})
            self.assertTrue(is_int(b["since_at"]) and is_int(b["silent_ms"]) and is_int(b["up_ms"]))
            self.assertIsInstance(b["label"], str)
            self.assertIsInstance(b["cabled"], bool)
            self.assertTrue(b["rssi"] is None or is_int(b["rssi"]))
            self.assertIsInstance(b["firmware"]["version"], str)
            self.assertTrue(is_int(b["firmware"]["counter"]))
            self.assertIsInstance(b["firmware"]["state"], str)
            self.assertTrue(is_int(b["package"]["counter"]) and is_int(b["package"]["live_applies"]))
            self.assertTrue(all(isinstance(r, str) for r in b["roles"]))
            self.assertTrue(all(isinstance(a, str) for a in b["actors"]))
            for md in b["modules"]:
                self.assertIn(md["state"], {"active", "idle", "failed"})
                self.assertIsInstance(md["name"], str)
                self.assertIsInstance(md["reason"], str)
            for k in ("internal_free", "internal_min", "internal_largest", "psram_free"):
                self.assertTrue(is_int(b["heap"][k]), k)
            for lk in b["links"]:
                self.assertIsInstance(lk["peer"], str)
                self.assertIn(lk["state"], {"alive", "dead", "asleep", "unknown"})
                self.assertTrue(is_num(lk["delivery"]) and 0 <= lk["delivery"] <= 1)
                self.assertTrue(lk["rtt_p50_ms"] is None or is_int(lk["rtt_p50_ms"]))
                self.assertTrue(lk["rtt_p99_ms"] is None or is_int(lk["rtt_p99_ms"]))
                self.assertTrue(is_int(lk["deaths"]))
        self.assertEqual([b["cabled"] for b in m["boards"]], [True, False, False])
        self.assertIn("extender", m["boards"][1]["roles"])
        self.assertEqual(cell["presenter"], "7368")

        self.assertTrue(m["apps"])
        for a in m["apps"]:
            self.assertIn(a["placement"], {"pinned", "portable"})
            self.assertIsInstance(a["working"], bool)
            self.assertIsInstance(a["detail"], str)
            for k in ("nodes", "running_on", "outputs"):
                self.assertTrue(all(isinstance(x, str) for x in a[k]), k)
            self.assertTrue(a["working"])  # a healthy cell when no fault is set

        paths = set()
        for r in m["resources"]:
            paths.add(r["path"])
            self.assertTrue(r["path"].startswith("potluck://lab/"))
            self.assertTrue(r["value"] is None or isinstance(r["value"], (bool, int, float)))
            self.assertIn(r["quality"], {"GOOD", "STALE", "FAULTY", "UNAVAILABLE"})
            self.assertTrue(is_int(r["age_ms"]) and r["age_ms"] >= 0)
            self.assertTrue(is_int(r["latency_class"]))
            for k in ("exposed", "outside_writable", "safety_relevant"):
                self.assertIsInstance(r[k], bool, k)
            self.assertIsInstance(r["unit"], str)
            self.assertFalse(r["outside_writable"] and r["safety_relevant"])
        for a in m["apps"]:
            self.assertTrue(set(a["outputs"]) <= paths, a["name"])
        self.assertEqual([r["path"] for r in m["resources"] if r["outside_writable"]], [LED])

        for i in m["integrations"]:
            self.assertIn(i["state"], {"connected", "disconnected", "idle", "off"})
            self.assertTrue(is_int(i["since_at"]))
            for k in ("name", "kind", "where", "detail"):
                self.assertIsInstance(i[k], str, k)
        for d in m["deploys"]:
            self.assertIn(d["result"], {"verified", "confirmed", "reverted", "refused", "failed", "running"})
            self.assertTrue(is_int(d["at"]) and is_int(d["counter"]))
            self.assertIn(d["kind"], {"package", "firmware"})

        sig = m["signals"]
        for k in ("latency_p50_ms", "latency_p99_ms", "traffic_frames_s", "errors_min"):
            self.assertTrue(is_num(sig[k]), k)
        self.assertEqual(set(sig["saturation"]), {"peers", "namespace", "actor_slots", "internal_ram", "airtime"})
        self.assertTrue(all(is_num(v) and 0 <= v <= 1 for v in sig["saturation"].values()))

    def test_events_page_by_sequence(self):
        ev = self.mock.get_json("/api/events")
        self.assertEqual(ev["api"], 1)
        self.assertTrue(ev["events"])
        seqs = [e["seq"] for e in ev["events"]]
        self.assertEqual(seqs, sorted(seqs))
        self.assertEqual(ev["next"], seqs[-1] + 1)
        for e in ev["events"]:
            self.assertTrue(is_int(e["at"]))
            self.assertIsInstance(e["board"], str)
            self.assertIsInstance(e["kind"], str)
            self.assertIsInstance(e["detail"], dict)
        self.assertIn("hub_started", {e["kind"] for e in ev["events"]})
        self.assertEqual(self.mock.get_json("/api/events?since=%d" % ev["next"])["events"], [])
        self.assertEqual(self.mock.get_json("/api/events?since=%d" % seqs[-1])["events"][0]["seq"], seqs[-1])

    def test_stream_yields_a_model_event(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.mock.port, timeout=5)
        try:
            conn.request("GET", "/api/stream")
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            self.assertTrue(r.getheader("Content-Type").startswith("text/event-stream"))
            event, seen = None, {}
            for _ in range(50):
                line = r.fp.readline().decode().rstrip("\r\n")
                if line.startswith("event: "):
                    event = line[7:]
                elif line.startswith("data: ") and event:
                    seen[event] = json.loads(line[6:])
                    if {"model", "events"} <= set(seen):
                        break
            self.assertIn("model", seen)
            self.assertEqual(seen["model"]["api"], 1)
            self.assertTrue(TOP_KEYS <= set(seen["model"]))
            self.assertIn("events", seen)
            self.assertIn("next", seen["events"])
        finally:
            conn.close()

    def test_write_led_applies_and_die_temp_is_refused(self):
        before = next(r for r in self.mock.get_json("/api/model")["resources"] if r["path"] == LED)["value"]
        code, body = self.mock.post_json("/api/write", {"path": LED, "value": not before})
        self.assertEqual((code, body), (200, {"ok": True, "result": "applied"}))
        after = next(r for r in self.mock.get_json("/api/model")["resources"] if r["path"] == LED)["value"]
        self.assertEqual(after, not before)
        code, body = self.mock.post_json("/api/write", {"path": DIE, "value": 20})
        self.assertEqual((code, body), (403, {"ok": False, "result": "not writable from outside"}))
        code, _ = self.mock.post_json("/api/write", {"path": "potluck://lab/act/alarm/level", "value": 0})
        self.assertEqual(code, 403)  # safety-relevant: never from outside
        kinds = [(e["kind"], e["detail"].get("result")) for e in self.mock.get_json("/api/events")["events"]]
        self.assertIn(("outside_write", "applied"), kinds)
        self.assertIn(("outside_write", "refused"), kinds)

    def test_index_serves_the_page_and_its_files(self):
        status, headers, body = self.mock.get("/")
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("text/html"))
        html = body.decode()
        self.assertIn('src="app.js"', html)
        self.assertIn('href="style.css"', html)
        for name, ctype in (("/app.js", "text/javascript"), ("/style.css", "text/css")):
            status, headers, _ = self.mock.get(name)
            self.assertEqual(status, 200)
            self.assertTrue(headers["Content-Type"].startswith(ctype), name)
        with self.assertRaises(urllib.error.HTTPError):
            self.mock.get("/../tools/hub_mock.py")

    def test_fault_by_query(self):
        self.mock.get_json("/mock/fault?stale=1&other_cell=1")
        try:
            m = self.mock.get_json("/api/model")
            die = next(r for r in m["resources"] if r["path"] == DIE)
            self.assertEqual(die["quality"], "STALE")
            self.assertGreaterEqual(die["age_ms"], 30_000)
            app = next(a for a in m["apps"] if a["name"] == "die_b")
            self.assertFalse(app["working"])
            self.assertIn("STALE", app["detail"])
            self.assertGreater(m["cell"]["other_cell_frames"], 0)
        finally:
            self.mock.get_json("/mock/fault?stale=0&other_cell=0")


class FaultTest(unittest.TestCase):
    def test_kill_makes_the_board_dead(self):
        mock = Mock("--kill", "8160@0", "--death-window-ms", "0")
        try:
            m = mock.get_json("/api/model")
            c = next(b for b in m["boards"] if b["node"] == "8160")
            self.assertEqual(c["state"], "dead")
            for b in m["boards"]:
                for lk in b["links"]:
                    if lk["peer"] == "8160" and b["node"] != "8160":
                        self.assertEqual(lk["state"], "dead")
            rf = next(a for a in m["apps"] if a["name"] == "rf_link")
            self.assertFalse(rf["working"])
            self.assertNotIn("8160", rf["running_on"])
            kinds = [(e["kind"], e["detail"].get("peer")) for e in mock.get_json("/api/events")["events"]]
            self.assertIn(("peer_dead", "8160"), kinds)
            code, _ = mock.post_json("/api/write", {"path": LED, "value": True})
            self.assertEqual(code, 200)  # B is alive: its LED still answers
        finally:
            mock.stop()

    def test_kill_timeline_on_a_fake_clock(self):
        t = [1_791_780_000_000]
        cell = hub_mock.SimCell(death_window_ms=3000, clock=lambda: t[0])
        cell.kill("8160", 5)
        board = lambda: next(b for b in cell.model()["boards"] if b["node"] == "8160")  # noqa: E731
        t[0] += 6000
        self.assertEqual((board()["state"], board()["silent_ms"]), ("alive", 1000))
        t[0] += 2000  # 8 s: silent for one death window
        self.assertEqual(board()["state"], "dead")
        self.assertEqual(board()["since_at"], 1_791_780_008_000)
        fusion = lambda: next(a for a in cell.model()["apps"] if a["name"] == "rf_fusion")  # noqa: E731
        self.assertEqual(fusion()["running_on"], [])
        self.assertFalse(fusion()["working"])
        t[0] += 1500  # fail-over
        self.assertEqual(fusion()["running_on"], ["6300"])
        self.assertTrue(fusion()["working"])
        kinds = [e["kind"] for e in cell.events_since(None)["events"]]
        self.assertEqual(kinds.count("peer_dead"), 2)
        self.assertIn("actor_owner", kinds)
        cell.revive("8160")
        self.assertEqual(board()["state"], "alive")

    def test_reverted_and_link_down(self):
        mock = Mock("--reverted", "--link-down", "--asleep", "6300")
        try:
            m = mock.get_json("/api/model")
            self.assertEqual(max(m["deploys"], key=lambda d: d["at"])["result"], "reverted")
            self.assertEqual(m["hub"]["link"], "down")
            self.assertEqual(m["boards"][0]["state"], "asleep")
        finally:
            mock.stop()


class _Ids(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids: set[str] = set()
        self.refs: list[str] = []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if a.get("id"):
            self.ids.add(a["id"])
        if tag in ("script", "link"):
            self.refs.append(a.get("src") or a.get("href") or "")


class PageTest(unittest.TestCase):
    def read(self, name: str) -> str:
        with open(os.path.join(DASHBOARD, name), encoding="utf-8") as f:
            return f.read()

    def test_first_screen_landmarks(self):
        p = _Ids()
        p.feed(self.read("index.html"))
        self.assertTrue(LANDMARKS <= p.ids, LANDMARKS - p.ids)
        # nothing from the network: every script and stylesheet is a file next to the page
        self.assertTrue(all(r and "//" not in r for r in p.refs), p.refs)

    def test_page_uses_only_the_contract_and_never_deploys(self):
        js = self.read("app.js")
        self.assertEqual(set(re.findall(r'"(api/[a-z]+)', js)), {"api/stream", "api/model", "api/events", "api/write",
                                                                  "api/whoami", "api/login", "api/logout",
                                                                  "api/replay"})  # M16
        self.assertNotIn("http://", js)
        self.assertNotIn("https://", js)
        self.assertNotRegex(js + self.read("index.html"), r"api/deploy|potluck\.apply")


if __name__ == "__main__":
    unittest.main()
