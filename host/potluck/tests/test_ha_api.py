"""M17: the Home Assistant integration's hub client (custom_components/potluck/api.py), without Home Assistant.

api.py imports nothing from Home Assistant, so it is loaded here by path and run against tools/hub_mock.py (the hub's
API version 1, plain HTTP) and a pairing stub built on the mock's handler -- POST /api/pair and GET /api/pair/<id> as
host/potluck/HUB-API.md states them, and bearer tokens on GET /api/model. With openssl on the PATH, the stub is also
served over TLS with a throwaway self-signed certificate, to show that a pinned fingerprint is checked before the
token leaves: a wrong pin reaches no handler.

Skips when aiohttp is not importable (the ESP-IDF Python has none; Home Assistant ships it).
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))

try:
    import aiohttp  # noqa: F401
    HAVE_AIOHTTP = True
except ImportError:
    HAVE_AIOHTTP = False


def _load(name: str, path: str):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod  # dataclasses look their module up while the class is made
    spec.loader.exec_module(mod)
    return mod


hub_mock = _load("hub_mock_for_ha", os.path.join(ROOT, "tools", "hub_mock.py"))
api = _load("potluck_ha_api", os.path.join(ROOT, "custom_components", "potluck", "api.py")) if HAVE_AIOHTTP else None

TOKEN = "t0k3n-for-tests-only"


# ---- the pairing stub: the mock's model and handler, plus pairing and bearer tokens --------------------------------

class PairHandler(hub_mock.Handler):
    def _seen(self):
        self.server.seen.append((self.command, self.path, self.headers.get("Authorization"),
                                 self.headers.get("X-Potluck-Request")))

    def do_GET(self):
        self._seen()
        u = urlparse(self.path)
        srv = self.server
        if u.path.startswith("/api/pair/"):
            pid = u.path[len("/api/pair/"):]
            script = srv.pairs.get(pid)
            if not script:
                return self._json(404, {"ok": False, "result": "no such pairing"})
            state = script.pop(0)
            if state == "approved":
                del srv.pairs[pid]  # shown once: 404 afterwards
                return self._json(200, {"state": "approved", "name": "Home Assistant", "right": "look", "token": TOKEN})
            if state == "denied":
                del srv.pairs[pid]
            return self._json(200, {"state": state})
        if u.path == "/api/model" and srv.require_token:
            if self.headers.get("Authorization") != "Bearer " + TOKEN:
                return self._json(401, {"ok": False, "result": "login needed"})
        return super().do_GET()

    def do_POST(self):
        self._seen()
        u = urlparse(self.path)
        srv = self.server
        if u.path != "/api/pair":
            return super().do_POST()
        if self.headers.get("X-Potluck-Request") != "1":
            return self._json(403, {"ok": False, "result": "X-Potluck-Request: 1 missing"})
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}")
        srv.pair_bodies.append(body)
        if len(srv.pairs) >= 4:
            return self._json(429, {"ok": False, "result": "too many pending pairing requests"})
        pid = "p%d" % (len(srv.pair_bodies))
        srv.pairs[pid] = list(srv.next_script)
        return self._json(202, {"pair_id": pid, "expires_in_s": 300})


class PairServer(hub_mock.MockServer):
    def __init__(self, addr, cell, tls_ctx: ssl.SSLContext | None = None):
        ThreadingHTTPServer.__init__(self, addr, PairHandler)
        if tls_ctx is not None:
            self.socket = tls_ctx.wrap_socket(self.socket, server_side=True)
        self.cell = cell
        self.static_dir = hub_mock.DASHBOARD
        self.stopping = threading.Event()
        self.seen: list = []
        self.pairs: dict[str, list[str]] = {}
        self.pair_bodies: list = []
        self.next_script = ["pending", "pending", "approved"]
        self.require_token = True


def start(server) -> threading.Thread:
    t = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
    t.start()
    return t


def make_cert(tmp: str) -> tuple[str, str] | None:
    exe = shutil.which("openssl")
    if not exe:
        return None
    key, crt = os.path.join(tmp, "k.pem"), os.path.join(tmp, "c.pem")
    r = subprocess.run([exe, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt,
                        "-days", "1", "-subj", "/CN=potluck-hub-test"], capture_output=True, timeout=60)
    return (crt, key) if r.returncode == 0 and os.path.exists(crt) else None


# ---- the pure functions: no network ---------------------------------------------------------------------------------

@unittest.skipUnless(HAVE_AIOHTTP, "aiohttp is not installed in this Python")
class PureFunctions(unittest.TestCase):
    def test_fingerprint_and_cell_checks(self):
        fp = "AB:" * 31 + "CD"
        self.assertEqual(api.normalize_fingerprint(fp), "ab" * 31 + "cd")
        for bad in ("", None, "ab" * 31, "zz" * 32, "ab" * 33):
            with self.assertRaises(ValueError):
                api.normalize_fingerprint(bad)
        self.assertEqual(api.normalize_cell("EF76CC2E"), "ef76cc2e")
        for bad in ("", None, "ef76cc2", "ef76cc2eX", "gf76cc2e"):
            with self.assertRaises(ValueError):
                api.normalize_cell(bad)
        self.assertEqual(api.format_fingerprint("ab" * 32)[:8], "AB:AB:AB")
        self.assertEqual(api.url_host("fe80::1"), "[fe80::1]")
        self.assertEqual(api.url_host("192.168.0.20"), "192.168.0.20")

    def test_tls_needs_a_pin_and_a_pin_needs_tls(self):
        with self.assertRaises(ValueError):
            api.HubClient(None, "h", 1)
        with self.assertRaises(ValueError):
            api.HubClient(None, "h", 1, fingerprint="ab" * 32, tls=False)
        c = api.HubClient(None, "h", 8443, fingerprint="ab" * 32, token=TOKEN)
        self.assertEqual(c.base_url, "https://h:8443")
        self.assertNotIn(TOKEN, repr(c))
        self.assertNotIn(TOKEN, repr(api.PairResult("Home Assistant", "look", TOKEN)))

    def test_problems_name_what_the_dashboard_names(self):
        m = {
            "at": 1_000_000,
            "hub": {"link": "down", "link_detail": "COM6: no frames for 4 s"},
            "cell": {"channel": 1, "other_cell_frames": 3},
            "boards": [
                {"node": "6300", "label": "A", "state": "alive", "firmware": {"counter": 21, "state": "trial"},
                 "modules": [{"name": "present", "state": "failed", "reason": "no broker"}]},
                {"node": "8160", "label": "C", "state": "dead", "since_at": 990_000},
                {"node": "9999", "state": "unknown"},
            ],
            "apps": [
                {"name": "die_b", "placement": "pinned", "nodes": ["6300"], "running_on": ["6300"], "working": False,
                 "outputs": ["p1"]},
                {"name": "lost", "placement": "portable", "nodes": ["8160"], "running_on": [], "working": False,
                 "detail": "nowhere to run"},
                {"name": "on_c", "placement": "pinned", "nodes": ["8160"], "running_on": [], "working": False,
                 "outputs": ["p2"]},
            ],
            "resources": [
                {"path": "p1", "name": "node-6300/hw/die_temp", "owner": "6300", "quality": "STALE", "age_ms": 30_000},
                {"path": "p2", "name": "node-8160/x", "owner": "8160", "quality": "UNAVAILABLE", "age_ms": 9_000},
            ],
            "integrations": [{"name": "presenting (MQTT)", "state": "disconnected", "since_at": 995_000}],
            "deploys": [{"at": 900_000, "kind": "package", "counter": 48, "result": "reverted", "detail": "x"},
                        {"at": 800_000, "kind": "firmware", "counter": 20, "result": "confirmed"}],
        }
        p = api.problems(m)
        texts = [x["text"] for x in p]
        sev = {x["text"]: x["severity"] for x in p}
        self.assertTrue(any(t.startswith("The hub's link to the cell is down: COM6") for t in texts))
        self.assertIn("Board C 8160 is dead", texts)
        self.assertEqual(sev["Board 9999 has not been heard since the hub started"], "warning")
        self.assertEqual(sev["Board A 6300: firmware 21 trial"], "warning")
        self.assertIn("Module present failed on A 6300: no broker", texts)
        self.assertIn("App die_b not working: output node-6300/hw/die_temp STALE", texts)
        self.assertIn("App lost not working: nowhere to run", texts)
        self.assertIn("App on_c not working", texts)  # its output is on the dead board: the board is the headline
        self.assertFalse(any("node-8160/x" in t for t in texts))
        self.assertIn("Integration presenting (MQTT) disconnected", texts)
        self.assertIn("Last deploy (package 48) reverted: x", texts)
        self.assertEqual(sev["Another cell shares channel 1"], "warning")
        firsts = [x["severity"] for x in p]
        self.assertEqual(firsts, sorted(firsts, key=lambda s: 0 if s == "problem" else 1))  # problems first
        dead = next(x for x in p if x["text"] == "Board C 8160 is dead")
        self.assertEqual(dead["since"], "1970-01-01T00:16:30+00:00")

    def test_summary_counts(self):
        m = {"hub": {"link": "up", "version": "abc"}, "cell": {"system": "lab", "cell_id": "ef76cc2e"},
             "boards": [{"node": "1", "state": "alive"}, {"node": "2", "label": "B", "state": "asleep"},
                        {"node": "3", "state": "dead"}],
             "apps": [{"name": "a", "working": True}, {"name": "b", "working": False, "detail": "why"}],
             "deploys": [{"at": 5, "result": "verified"}, {"at": 9, "result": "confirmed"}]}
        s = api.summarize(m)
        self.assertEqual((s["boards_alive"], s["boards_total"]), (1, 3))
        self.assertEqual(s["boards_asleep"], ["B 2"])
        self.assertEqual(s["boards_dead"], ["3"])
        self.assertEqual((s["apps_working"], s["apps_total"]), (1, 2))
        self.assertEqual(s["apps_not_working"], [{"name": "b", "detail": "why"}])
        self.assertEqual(s["last_deploy"]["at"], 9)
        self.assertEqual(api.summarize({})["boards_total"], 0)
        self.assertEqual(api.problems({}), [])


# ---- against the mock, plain HTTP -----------------------------------------------------------------------------------

@unittest.skipUnless(HAVE_AIOHTTP, "aiohttp is not installed in this Python")
class AgainstTheMock(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        args = hub_mock.parse_args(["--port", "0"])
        self.cell = hub_mock.SimCell(death_window_ms=args.death_window_ms, period_ms=args.period_ms)
        self.srv = PairServer(("127.0.0.1", 0), self.cell)
        self.port = self.srv.server_address[1]
        self.thread = start(self.srv)

    def tearDown(self):
        self.srv.stop()
        self.thread.join(timeout=5)

    async def asyncSetUp(self):
        self.session = aiohttp.ClientSession()

    async def asyncTearDown(self):
        await self.session.close()

    def client(self, token=None):
        return api.HubClient(self.session, "127.0.0.1", self.port, tls=False, token=token)

    async def test_model_needs_the_token(self):
        with self.assertRaises(api.HubAuthError):
            await self.client().model()
        m = await self.client(TOKEN).model()
        self.assertEqual(m["api"], 1)
        self.assertEqual(m["cell"]["cell_id"], "ef76cc2e")
        s = api.summarize(m)
        self.assertEqual((s["boards_alive"], s["boards_total"]), (3, 3))
        self.assertEqual(s["apps_working"], s["apps_total"])
        self.assertEqual(api.problems(m), [])  # the mock's healthy cell: nothing to report
        self.assertIn(("GET", "/api/model", "Bearer " + TOKEN, None), self.srv.seen)

    async def test_faults_become_problems(self):
        self.cell.set_link_down(True)
        self.cell.set_reverted(True)
        self.cell.set_stale(True)
        m = await self.client(TOKEN).model()
        texts = [p["text"] for p in api.problems(m)]
        self.assertTrue(any(t.startswith("The hub's link to the cell is down") for t in texts), texts)
        self.assertTrue(any(t.startswith("Last deploy (package 48) reverted") for t in texts), texts)
        self.assertTrue(any("die_temp STALE" in t for t in texts), texts)

    async def test_pairing_approved(self):
        c = self.client()
        req = await c.request_pairing("Home Assistant", "look")
        self.assertEqual(req.expires_in_s, 300)
        self.assertEqual(self.srv.pair_bodies, [{"name": "Home Assistant", "right": "look"}])
        res = await c.wait_for_pairing(req, interval=0.01)
        self.assertEqual((res.name, res.right, res.token), ("Home Assistant", "look", TOKEN))
        polls = [s for s in self.srv.seen if s[1].startswith("/api/pair/")]
        self.assertEqual(len(polls), 3)  # pending, pending, approved
        self.assertTrue(all(s[2] is None for s in self.srv.seen))  # nothing carried a token before it had one
        self.assertEqual(await c.pairing_status(req.pair_id), {"state": "expired"})  # fetched once: 404

    async def test_pairing_denied_and_expired(self):
        c = self.client()
        self.srv.next_script = ["pending", "denied"]
        with self.assertRaises(api.PairingDenied):
            await c.wait_for_pairing(await c.request_pairing("Home Assistant"), interval=0.01)
        self.srv.next_script = ["pending"]  # then 404: expired
        with self.assertRaises(api.PairingExpired):
            await c.wait_for_pairing(await c.request_pairing("Home Assistant"), interval=0.01)

    async def test_pairing_full_is_busy(self):
        c = self.client()
        self.srv.next_script = ["pending"] * 50
        for _ in range(4):
            await c.request_pairing("Home Assistant")
        with self.assertRaises(api.HubBusyError):
            await c.request_pairing("Home Assistant")

    async def test_post_carries_the_csrf_header(self):
        await self.client().request_pairing("Home Assistant")
        post = next(s for s in self.srv.seen if s[0] == "POST")
        self.assertEqual(post[3], "1")

    async def test_unreachable_is_a_connection_error(self):
        port = self.port
        self.srv.stop()
        self.thread.join(timeout=5)
        with self.assertRaises(api.HubConnectionError):
            await api.HubClient(self.session, "127.0.0.1", port, tls=False, timeout=2).model()
        self.srv = PairServer(("127.0.0.1", 0), self.cell)  # for tearDown
        self.thread = start(self.srv)


# ---- over TLS, the certificate pinned ---------------------------------------------------------------------------

@unittest.skipUnless(HAVE_AIOHTTP, "aiohttp is not installed in this Python")
class PinnedTls(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="potluck-ha-tls-")
        pair = make_cert(cls.tmp)
        if pair is None:
            shutil.rmtree(cls.tmp, ignore_errors=True)
            raise unittest.SkipTest("openssl not found: no certificate to serve")
        cls.crt, cls.key = pair
        import hashlib
        with open(cls.crt) as f:
            cls.fp = hashlib.sha256(ssl.PEM_cert_to_DER_cert(f.read())).hexdigest()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self.crt, self.key)
        self.srv = PairServer(("127.0.0.1", 0), hub_mock.SimCell(), tls_ctx=ctx)
        self.port = self.srv.server_address[1]
        self.thread = start(self.srv)

    def tearDown(self):
        self.srv.stop()
        self.thread.join(timeout=5)

    async def asyncSetUp(self):
        self.session = aiohttp.ClientSession()

    async def asyncTearDown(self):
        await self.session.close()

    async def test_fetch_fingerprint_reads_the_served_certificate(self):
        self.assertEqual(await api.fetch_fingerprint("127.0.0.1", self.port), self.fp)

    async def test_the_right_pin_gets_the_model(self):
        m = await api.HubClient(self.session, "127.0.0.1", self.port, fingerprint=self.fp, token=TOKEN).model()
        self.assertEqual(m["api"], 1)

    async def test_a_wrong_pin_sends_nothing(self):
        wrong = ("00" if not self.fp.startswith("00") else "11") + self.fp[2:]
        c = api.HubClient(self.session, "127.0.0.1", self.port, fingerprint=wrong, token=TOKEN)
        with self.assertRaises(api.HubFingerprintError) as cm:
            await c.model()
        self.assertIn(self.fp, str(cm.exception))  # says what it was served
        await asyncio.sleep(0.1)
        self.assertEqual(self.srv.seen, [])  # no request -- and so no token -- reached the server

    async def test_nothing_listening(self):
        port = self.port
        self.srv.stop()
        self.thread.join(timeout=5)
        with self.assertRaises(api.HubConnectionError):
            await api.fetch_fingerprint("127.0.0.1", port, timeout=2)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self.crt, self.key)
        self.srv = PairServer(("127.0.0.1", 0), hub_mock.SimCell(), tls_ctx=ctx)
        self.thread = start(self.srv)


if __name__ == "__main__":
    unittest.main()
