"""M16: the hub's login -- three rights kept apart, sessions, and a dashboard never served beyond the PC unencrypted."""

from __future__ import annotations

import http.client
import json
import os
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from potluck import hub as hb  # noqa: E402
from potluck import hubauth as ha  # noqa: E402
from test_hub import HubFixture, free_port  # noqa: E402

FAST_N = 2 ** 10  # the tests' scrypt cost; the real one is 2^17 (128 MiB, about half a second)


def users(tmp: str) -> ha.Users:
    u = ha.Users(Path(tmp) / "users.json")
    u.add("viewer", "look", "look-only-password", n=FAST_N)
    u.add("owner", "write", "write-also-password", n=FAST_N)
    return u


class Passwords(unittest.TestCase):
    def test_a_password_checks_only_against_itself_and_is_never_stored(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            u = users(tmp)
            u.save()
            raw = (Path(tmp) / "users.json").read_text(encoding="utf-8")
            self.assertNotIn("look-only-password", raw)
            u2 = ha.Users(Path(tmp) / "users.json")
            self.assertEqual(u2.check("viewer", "look-only-password"), "look")
            self.assertEqual(u2.check("owner", "write-also-password"), "write")
            self.assertIsNone(u2.check("viewer", "write-also-password"))
            self.assertIsNone(u2.check("nobody", "look-only-password"))

    def test_the_default_cost_is_owasps_and_salts_differ(self) -> None:
        a, b = ha.hash_password("same password", n=FAST_N), ha.hash_password("same password", n=FAST_N)
        self.assertNotEqual((a["salt"], a["hash"]), (b["salt"], b["hash"]))
        self.assertEqual((ha.SCRYPT_N, ha.SCRYPT_R, ha.SCRYPT_P), (2 ** 17, 8, 1))

    def test_there_is_no_deploy_right_and_weak_passwords_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            u = ha.Users(Path(tmp) / "u.json")
            with self.assertRaisesRegex(ValueError, "deploy is never through the hub"):
                u.add("x", "deploy", "long enough password", n=FAST_N)
            with self.assertRaisesRegex(ValueError, "10-1024"):
                u.add("x", "look", "short", n=FAST_N)

    def test_five_failures_lock_the_address_out_and_a_good_login_then_waits_too(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            s = ha.Sessions(users(tmp))
            for _ in range(ha.FAIL_LIMIT):
                self.assertEqual(s.login("viewer", "wrong", "10.0.0.9"), (None, "wrong name or password"))
            tok, why = s.login("viewer", "look-only-password", "10.0.0.9")
            self.assertIsNone(tok)
            self.assertIn("too many failed logins", why)
            tok, right = s.login("viewer", "look-only-password", "10.0.0.10")  # another address is not locked out
            self.assertEqual((s.get(tok), right), (("viewer", "look"), "look"))
            s.logout(tok)
            self.assertIsNone(s.get(tok))


class KeptSessions(unittest.TestCase):
    """2026-10-19: every reinstall and PC restart logged the owner's phone out -- sessions lived in memory for 12 h."""

    def test_a_session_outlives_a_restart_and_only_its_hash_is_on_disk(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            u = users(tmp)
            path = Path(tmp) / "hub-sessions.json"
            token, right = ha.Sessions(u, path).login("owner", "write-also-password", "10.0.0.2")
            self.assertEqual(right, "write")
            self.assertNotIn(token, path.read_text(encoding="utf-8"))  # never the token itself
            again = ha.Sessions(u, path)                                 # the hub restarted
            self.assertEqual(again.get(token), ("owner", "write"))
            again.logout(token)
            self.assertIsNone(ha.Sessions(u, path).get(token))           # a logout outlives the restart too

    def test_an_expired_or_removed_login_is_gone_and_the_session_lasts_thirty_days(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            u = users(tmp)
            path = Path(tmp) / "hub-sessions.json"
            s = ha.Sessions(u, path)
            token, _ = s.login("viewer", "look-only-password", "10.0.0.2")
            self.assertEqual(ha.SESSION_S, 30 * 24 * 3600)
            from unittest import mock
            with mock.patch.object(ha.time, "time", return_value=ha.time.time() + ha.SESSION_S + 1):
                self.assertIsNone(s.get(token))
            token, _ = s.login("viewer", "look-only-password", "10.0.0.2")
            u.users.pop("viewer")  # removed with `potluck hubauth remove`
            self.assertIsNone(s.get(token))


class StartsLikeTheTray(unittest.TestCase):
    """The hub as the owner's tray starts it -- potluck.hub with --listen and a certificate. 2026-10-19: 234c1bf's kept
    sessions called hubauth.sessions_file() in main(), where hubauth was not imported, and every such hub died at start;
    the tests built Sessions directly and never ran main() with --listen."""

    def test_the_hub_with_listen_starts_and_serves_its_certificate(self):
        import shutil
        import socket
        import ssl
        import subprocess
        import urllib.request
        if shutil.which("openssl") is None:
            self.skipTest("no openssl to make a certificate")
        with tempfile.TemporaryDirectory() as tmp:
            cert, key = Path(tmp) / "c.pem", Path(tmp) / "k.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                            "-nodes", "-keyout", str(key), "-out", str(cert), "-days", "2", "-subj", "/CN=potluck-hub",
                            "-addext", "subjectAltName=IP:127.0.0.1"], check=True, capture_output=True,
                           env={**os.environ, "MSYS_NO_PATHCONV": "1"})
            link = socket.socket()  # a silent stand-in for the cable: the hub serves anyway
            link.bind(("127.0.0.1", 0))
            link.listen(1)
            api, relay, lan = free_port(), free_port(), free_port()
            env = {**os.environ, "LOCALAPPDATA": tmp,  # its own hub.json: the owner's running hub is no second hub
                   "POTLUCK_HUB_USERS": str(Path(tmp) / "users.json"), "POTLUCK_HUB_NO_ANNOUNCE": "1",
                   "PYTHONPATH": os.path.join(os.path.dirname(__file__), "..")}
            proc = subprocess.Popen([sys.executable, "-m", "potluck.hub", "--tcp", f"127.0.0.1:{link.getsockname()[1]}",
                                     "--api", f"127.0.0.1:{api}", "--relay", f"127.0.0.1:{relay}",
                                     "--listen", f"127.0.0.1:{lan}", "--tls-cert", str(cert), "--tls-key", str(key)],
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            try:
                hello = None
                for _ in range(60):
                    if proc.poll() is not None:
                        self.fail("the hub exited: " + proc.stdout.read().decode(errors="replace")[-800:])
                    try:
                        with urllib.request.urlopen(f"http://127.0.0.1:{api}/api/hello", timeout=1) as r:
                            hello = json.loads(r.read())
                        break
                    except OSError:
                        time.sleep(0.25)
                self.assertIsNotNone(hello, "the hub never answered")
                ctx = ssl.create_default_context(cafile=str(cert))  # the hub's own certificate is a CA for itself
                with urllib.request.urlopen(f"https://127.0.0.1:{lan}/hub-certificate.crt", context=ctx,
                                            timeout=3) as r:
                    der = r.read()
                self.assertEqual(der, ssl.PEM_cert_to_DER_cert(cert.read_text(encoding="ascii")))
            finally:
                proc.kill()
                proc.wait(timeout=10)
                proc.stdout.close()
                link.close()


class LoginApi(HubFixture):
    def setUp(self) -> None:
        super().setUp()
        self.tmp = tempfile.TemporaryDirectory()
        self.sessions = ha.Sessions(users(self.tmp.name))
        self.lan_addr = f"127.0.0.1:{free_port()}"
        self.lan = hb.serve_api(self.model, self.lan_addr, sessions=self.sessions)

    def tearDown(self) -> None:
        self.lan.shutdown()
        self.tmp.cleanup()
        super().tearDown()

    def req(self, method: str, path: str, body=None, cookie=None, header=True):
        host, _, port = self.lan_addr.rpartition(":")
        c = http.client.HTTPConnection(host, int(port), timeout=5)
        h = {"Content-Type": "application/json"}
        if header:
            h["X-Potluck-Request"] = "1"
        if cookie:
            h["Cookie"] = cookie
        c.request(method, path, body=json.dumps(body) if body is not None else None, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, (json.loads(data) if data[:1] in (b"{", b"[") else data), r.getheader("Set-Cookie")

    def login(self, name, password):
        code, body, cookie = self.req("POST", "/api/login", {"name": name, "password": password})
        return code, body, cookie.split(";")[0] if cookie else None

    def test_nothing_but_the_login_page_is_served_without_a_login(self) -> None:
        self.assertEqual(self.req("GET", "/api/model")[0], 401)
        self.assertEqual(self.req("GET", "/api/events")[0], 401)
        self.assertEqual(self.req("GET", "/api/stream")[0], 401)
        self.assertEqual(self.req("POST", "/api/write", {"path": "x", "value": 1})[0], 401)
        self.assertEqual(self.req("GET", "/")[0], 200)  # the page, which asks for the login
        self.assertEqual(self.req("GET", "/api/whoami")[1]["name"], None)

    def test_a_look_login_reads_everything_and_writes_nothing(self) -> None:
        code, body, cookie = self.login("viewer", "look-only-password")
        self.assertEqual((code, body["right"]), (200, "look"))
        self.assertEqual(self.req("GET", "/api/model", cookie=cookie)[0], 200)
        self.assertEqual(self.req("GET", "/api/whoami", cookie=cookie)[1]["right"], "look")
        code, body, _ = self.req("POST", "/api/write", {"path": "potluck://lab/node-1001/led/on", "value": True},
                                 cookie=cookie)
        self.assertEqual((code, body["result"]), (403, "this login may look, not write"))

    def test_a_write_login_reaches_the_packages_own_check(self) -> None:
        _, _, cookie = self.login("owner", "write-also-password")
        code, body, _ = self.req("POST", "/api/write", {"path": "potluck://lab/node-1001/led/on", "value": True},
                                 cookie=cookie)
        self.assertEqual((code, body["result"]), (403, "not writable from outside"))  # no cluster: nothing is

    def test_a_post_without_the_custom_header_is_refused_so_another_site_cannot_write(self) -> None:
        _, _, cookie = self.login("owner", "write-also-password")
        code, body, _ = self.req("POST", "/api/write", {"path": "x", "value": 1}, cookie=cookie, header=False)
        self.assertEqual((code, body["result"]), (403, "missing X-Potluck-Request"))

    def test_a_wrong_password_gets_no_cookie_and_a_logout_ends_the_session(self) -> None:
        code, body, cookie = self.login("viewer", "not it")
        self.assertEqual((code, cookie), (403, None))
        _, _, cookie = self.login("viewer", "look-only-password")
        self.assertIn("HttpOnly", self.req("POST", "/api/login",
                                           {"name": "viewer", "password": "look-only-password"})[2])
        self.req("POST", "/api/logout", {}, cookie=cookie)
        self.assertEqual(self.req("GET", "/api/model", cookie=cookie)[0], 401)

    def test_the_hubs_certificate_is_offered_without_a_login(self) -> None:
        code, body, _ = self.req("GET", "/hub-certificate.crt")
        self.assertEqual(code, 404)  # a hub without --listen has none
        self.model.tls_der = b"0\x82\x01\x00der-bytes"
        code, body, _ = self.req("GET", "/hub-certificate.crt")
        self.assertEqual((code, body), (200, b"0\x82\x01\x00der-bytes"))

    def test_there_is_no_deploy_endpoint(self) -> None:
        _, _, cookie = self.login("owner", "write-also-password")
        for path in ("/api/deploy", "/api/apply", "/api/firmware"):
            self.assertEqual(self.req("POST", path, {}, cookie=cookie)[0], 404)


@unittest.skipIf(shutil.which("openssl") is None, "no openssl to make a test certificate")
class Tls(HubFixture):
    def test_the_dashboard_is_served_over_tls_with_the_login(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            crt, key = os.path.join(tmp, "c.pem"), os.path.join(tmp, "k.pem")
            subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
                            "-nodes", "-keyout", key, "-out", crt, "-days", "1", "-subj", "/CN=potluck-test"],
                           check=True, capture_output=True)
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(crt, key)
            addr = f"127.0.0.1:{free_port()}"
            srv = hb.serve_api(self.model, addr, sessions=ha.Sessions(users(tmp)), tls=ctx)
            try:
                cli = ssl.create_default_context(cafile=crt)
                cli.check_hostname = False
                host, _, port = addr.rpartition(":")
                c = http.client.HTTPSConnection(host, int(port), context=cli, timeout=5)
                c.request("GET", "/api/model")
                self.assertEqual(c.getresponse().status, 401)
                c.close()
            finally:
                srv.shutdown()


if __name__ == "__main__":
    unittest.main()
