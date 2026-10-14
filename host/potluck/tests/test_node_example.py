"""M18: the worked example program in a second language (examples/programs/node/potluck-watch.mjs) reads, watches and
writes the cell through the hub's API v1 (host/potluck/HUB-API.md) -- run here against the hub mock (tools/hub_mock.py),
with its faults, a stream that fails (polling) and a login in front of it. Skipped when Node.js is not installed.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import secrets
import shutil
import subprocess
import threading
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
PROGRAM = os.path.join(ROOT, "examples", "programs", "node", "potluck-watch.mjs")
_spec = importlib.util.spec_from_file_location("hub_mock", os.path.join(ROOT, "tools", "hub_mock.py"))
hub_mock = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hub_mock)

NODE = shutil.which("node")
QUALITIES = ("GOOD", "STALE", "UNAVAILABLE", "NO_DATA", "FAULTY")  # potluck.value.Quality


def node_version() -> tuple[int, ...]:
    out = subprocess.run([NODE, "--version"], capture_output=True, text=True, timeout=10).stdout.strip()
    return tuple(int(x) for x in out.lstrip("v").split(".")[:2])


class NoStreamHandler(hub_mock.Handler):
    """The mock with a broken stream: GET /api/stream is 503, so the program must poll."""

    def do_GET(self):
        if self.path.startswith("/api/stream"):
            return self._json(503, {"ok": False, "result": "stream off for the test"})
        return super().do_GET()


def login_handler(name: str, password: str):
    """The mock behind a login, as HUB-API.md's Logins section states it (without TLS: the test runs on 127.0.0.1)."""
    sessions: set[str] = set()

    class LoginHandler(hub_mock.Handler):
        def _session(self) -> bool:
            for part in (self.headers.get("Cookie") or "").split(";"):
                k, _, v = part.strip().partition("=")
                if k == "potluck_session" and v in sessions:
                    return True
            return False

        def do_GET(self):
            if self._session():
                return super().do_GET()
            self._json(401, {"error": "login", "result": "log in first"})

        def do_POST(self):
            if self.headers.get("X-Potluck-Request") != "1":
                return self._json(403, {"ok": False, "result": "missing X-Potluck-Request"})
            if self.path == "/api/login":
                n = int(self.headers.get("Content-Length") or 0)
                req = json.loads(self.rfile.read(n) or b"{}")
                if req.get("name") != name or req.get("password") != password:
                    return self._json(403, {"ok": False, "result": "unknown name or wrong password"})
                token = secrets.token_urlsafe(32)
                sessions.add(token)
                body = json.dumps({"ok": True, "name": name, "right": "write"}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Set-Cookie", "potluck_session=%s; Path=/; HttpOnly; Secure; SameSite=Strict; "
                                               "Max-Age=43200" % token)
                self.end_headers()
                self.wfile.write(body)
                return
            if not self._session():
                return self._json(401, {"error": "login", "result": "log in first"})
            return super().do_POST()

    return LoginHandler


class Mock:
    def __init__(self, *argv: str, handler=None):
        self.srv = hub_mock.build(hub_mock.parse_args(["--port", "0", *argv]))
        if handler is not None:
            self.srv.RequestHandlerClass = handler
        self.url = "http://127.0.0.1:%d" % self.srv.server_address[1]
        self.thread = threading.Thread(target=self.srv.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()

    def stop(self):
        self.srv.stop()
        self.thread.join(timeout=5)


def run(mock: Mock, *args: str, env: dict | None = None, timeout: float = 60) -> tuple[int, str]:
    e = {k: v for k, v in os.environ.items() if not k.startswith("POTLUCK_HUB")}
    e.update(env or {})
    p = subprocess.run([NODE, PROGRAM, "--hub", mock.url, *args], capture_output=True, timeout=timeout, env=e)
    return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")


@unittest.skipIf(NODE is None, "Node.js is not installed")
class NodeExampleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if node_version() < (18, 0):
            raise unittest.SkipTest("the example needs Node.js 18 or newer")

    def setUp(self):
        self.mocks: list[Mock] = []

    def tearDown(self):
        for m in self.mocks:
            m.stop()

    def mock(self, *argv: str, handler=None) -> Mock:
        m = Mock(*argv, handler=handler)
        self.mocks.append(m)
        return m

    def test_read_prints_every_value_with_unit_quality_and_age(self):
        code, out = run(self.mock(), "read")
        self.assertEqual(code, 0, out)
        self.assertIn("Boards (3)", out)
        self.assertRegex(out, r"B \(7368\)\s+alive")
        lines = out.split("Resources (", 1)[1].splitlines()[1:]
        lines = [ln for ln in lines if ln.strip()]
        self.assertEqual(len(lines), 10, out)
        for ln in lines:
            self.assertTrue(any(", %s, " % q in ln for q in QUALITIES), ln)
            self.assertRegex(ln, r" old")
        self.assertRegex(out, r"node-7368/hw/die_temp\s+[-\d.]+ C, GOOD, ")
        self.assertRegex(out, r"node-7368/led/on\s+(true|false), GOOD, .*\[writable from outside\]")
        self.assertRegex(out, r"act/alarm/level\s+\d \(no unit\), GOOD, .*\[safety-relevant\]")

    def test_once_reads_writes_is_refused_watches_and_restores(self):
        m = self.mock()
        before = m.srv.cell.led
        code, out = run(m, "--once", "--for", "20")
        self.assertEqual(code, 0, out)
        self.assertRegex(out, r"write potluck://lab/node-7368/led/on = (true|false): applied \(HTTP 200")
        self.assertRegex(out, r"write potluck://lab/node-7368/hw/die_temp = [-\d.]+: refused \(HTTP 403, "
                              r"\{\"ok\":false,\"result\":\"not writable from outside\"\}\)")
        self.assertIn("event #", out)
        self.assertIn("outside_write", out)
        self.assertIn("OK: write applied; write refused (403); the model shows the new value; "
                      "the hub reported the write as an event; put back", out)
        self.assertEqual(m.srv.cell.led, before)  # the cell left as it was found

    def test_write_command_exit_codes(self):
        m = self.mock()
        code, out = run(m, "write", hub_mock.LED_PATH, "true")
        self.assertEqual(code, 0, out)
        self.assertTrue(m.srv.cell.led)
        code, out = run(m, "write", "potluck://lab/act/alarm/level", "1")  # safety-relevant: never from outside
        self.assertEqual(code, 3, out)
        self.assertIn("refused (HTTP 403", out)
        code, out = run(m, "write", hub_mock.LED_PATH, "on")
        self.assertEqual(code, 2, out)  # not a JSON bool or number: refused before asking the hub

    def test_watch_prints_a_board_dying_and_its_events(self):
        m = self.mock("--kill", "8160@2", "--death-window-ms", "1000", "--period-ms", "500")
        code, out = run(m, "watch", "--for", "6")
        self.assertEqual(code, 0, out)
        self.assertIn("watching %s/api/stream" % m.url, out)
        self.assertRegex(out, r"board C \(8160\): alive -> dead since ")
        self.assertRegex(out, r"event #\d+\s+(6300|7368)\s+peer_dead \{\"peer\":\"8160\"\}")
        self.assertRegex(out, r"node-8160/rf/link: [-\d.]+ dB, GOOD, .* -> no value, UNAVAILABLE, ")
        self.assertIn("actor_owner", out)  # rf_fusion failed over

    def test_watch_falls_back_to_polling_when_the_stream_fails(self):
        m = self.mock(handler=NoStreamHandler)
        code, out = run(m, "watch", "--for", "8", "--retry-ms", "3000")
        self.assertEqual(code, 0, out)
        self.assertIn("stream unavailable (HTTP 503); polling every 2.0 s", out)
        self.assertIn("trying the stream again", out)
        self.assertRegex(out, r"\(\d+ earlier events; the last 5:\)")  # GET /api/events while polling
        self.assertRegex(out, r"node-\w+/rf/link: [-\d.]+ dB, GOOD, .* -> ")  # GET /api/model while polling

    def test_login_from_the_environment_only_and_never_printed(self):
        name, password = "user-" + secrets.token_hex(6), secrets.token_urlsafe(16)
        m = self.mock(handler=login_handler(name, password))
        code, out = run(m, "--once", "--for", "20", env={"POTLUCK_HUB_USER": name, "POTLUCK_HUB_PASSWORD": password})
        self.assertEqual(code, 0, out)
        self.assertIn("logged in, right: write", out)
        self.assertIn("OK: write applied", out)
        self.assertNotIn(password, out)
        self.assertNotIn(name, out)
        code, out = run(m, "read")
        self.assertEqual(code, 1, out)
        self.assertIn("set POTLUCK_HUB_USER and POTLUCK_HUB_PASSWORD", out)
        code, out = run(m, "read", env={"POTLUCK_HUB_USER": name, "POTLUCK_HUB_PASSWORD": "wrong"})
        self.assertEqual(code, 1, out)
        self.assertIn("login refused (HTTP 403: unknown name or wrong password)", out)

    def test_no_dependencies(self):
        with open(PROGRAM, encoding="utf-8") as f:
            src = f.read()
        imports = re.findall(r"^import .* from \"([^\"]+)\";", src, re.M)
        self.assertTrue(all(i.startswith("node:") for i in imports), imports)
        self.assertNotIn("require(", src)
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(PROGRAM), "package.json")))


if __name__ == "__main__":
    unittest.main()
