"""M14: the presenting role's MQTT transport on the PC (potluck.mqtt_link), against the bench broker
(tools/mqtt_bench_broker.py -- an MQTT 3.1.1 broker from the standard, with wills) on 127.0.0.1."""

from __future__ import annotations

import base64
import importlib.util
import io
import json
import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import mqtt_link as ml  # noqa: E402

TOOLS = os.path.join(os.path.dirname(__file__), "..", "..", "..", "tools")


def load_broker():
    spec = importlib.util.spec_from_file_location("bench_broker", os.path.join(TOOLS, "mqtt_bench_broker.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.LOG = io.StringIO()
    srv = socket.socket()
    srv.bind(("127.0.0.1", 0))
    srv.listen(8)

    def accept():
        while True:
            try:
                s, addr = srv.accept()
            except OSError:
                return
            threading.Thread(target=mod.serve, args=(mod.Client(s, addr),), daemon=True).start()

    threading.Thread(target=accept, daemon=True).start()
    return mod, srv, srv.getsockname()[1]


def events(mod) -> list[dict]:
    return [json.loads(line) for line in mod.LOG.getvalue().splitlines() if line.strip()]


def raw_publish(port: int, topic: str, payload: bytes) -> None:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(ml.connect_packet("tester", 30, "tester/will", b"x"))
    s.recv(4)  # CONNACK
    s.sendall(ml.publish_packet(topic, payload))
    s.sendall(ml.DISCONNECT)
    time.sleep(0.1)
    s.close()


def wait_for(pred, seconds=5.0) -> bool:
    end = time.time() + seconds
    while time.time() < end:
        if pred():
            return True
        time.sleep(0.02)
    return False


class Transport(unittest.TestCase):
    def setUp(self) -> None:
        self.mod, self.srv, self.port = load_broker()
        self.ctl: list[str] = []
        self.link = ml.PresenterLink(self.ctl.append)
        self.cfg = {"k": "S", "host": "127.0.0.1", "port": self.port, "tls": 0, "client_id": "potluck-ef76cc2e-00fe",
                    "keepalive_s": 10, "will_topic": "potluck/ef76cc2e/presenter/00fe",
                    "will_b64": base64.b64encode(b'{"online":0,"node":"00fe","nodes":[],"down":[]}').decode(),
                    "cmd_filter": "potluck/ef76cc2e/+/set", "status_topic": "homeassistant/status", "ca_b64": ""}

    def tearDown(self) -> None:
        self.link.stop()
        self.srv.close()

    def test_it_connects_with_its_will_publishes_and_hears_commands_and_home_assistants_birth(self) -> None:
        self.link.start(self.cfg)
        self.assertTrue(wait_for(lambda: "connected 1" in self.ctl))
        self.assertTrue(wait_for(lambda: sum(1 for e in events(self.mod) if e["ev"] == "subscribe") >= 1))
        self.link.publish("potluck/ef76cc2e/0badf00d/state", b'{"value":1}')
        self.assertTrue(wait_for(lambda: any(e["ev"] == "publish" and e.get("topic") == "potluck/ef76cc2e/0badf00d/state"
                                             for e in events(self.mod))))
        raw_publish(self.port, "potluck/ef76cc2e/1a2b3c4d/set", b"ON")
        self.assertTrue(wait_for(lambda: f"cmd 1a2b3c4d {base64.b64encode(b'ON').decode()}" in self.ctl))
        raw_publish(self.port, "potluck/ef76cc2e/nothex!!/set", b"ON")  # not a hash: ignored
        raw_publish(self.port, "homeassistant/status", b"online")
        self.assertTrue(wait_for(lambda: "discovery" in self.ctl))
        self.assertFalse(any(c.startswith("cmd") and "nothex" in c for c in self.ctl))

    def test_handing_the_role_on_says_offline_itself_and_never_fires_the_will(self) -> None:
        self.link.start(self.cfg)
        self.assertTrue(wait_for(lambda: "connected 1" in self.ctl))
        self.link.stop()
        self.assertTrue(wait_for(lambda: any(e["ev"] == "disconnect" for e in events(self.mod))))
        evs = events(self.mod)
        bye = [e for e in evs if e["ev"] == "publish" and e.get("topic") == self.cfg["will_topic"]]
        self.assertEqual(len(bye), 1)
        self.assertFalse(any(e["ev"] == "will" for e in evs))  # a clean DISCONNECT: the broker sends no will
        self.assertTrue(wait_for(lambda: "connected 0" in self.ctl))

    def test_a_broker_that_is_not_there_is_retried_and_reachable_says_so(self) -> None:
        dead = socket.socket()
        dead.bind(("127.0.0.1", 0))
        port = dead.getsockname()[1]
        dead.close()
        self.assertFalse(ml.reachable("127.0.0.1", port, timeout=0.5))
        self.assertTrue(ml.reachable("127.0.0.1", self.port))
        self.link.start(dict(self.cfg, port=port))
        time.sleep(0.5)
        self.assertNotIn("connected 1", self.ctl)

    def test_a_broker_that_wants_a_login_gets_it_and_refuses_without(self) -> None:
        self.mod.AUTH = ("potluck", "not-a-real-password")
        try:
            self.link.start(dict(self.cfg))  # no login
            time.sleep(1.0)
            self.assertNotIn("connected 1", self.ctl)
            self.link.stop()
            self.link.start(dict(self.cfg, user="potluck", **{"pass": "not-a-real-password"}))
            self.assertTrue(wait_for(lambda: "connected 1" in self.ctl))
        finally:
            self.mod.AUTH = None

    def test_a_login_file_is_two_lines(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, "login.txt")
            with open(p, "w", encoding="utf-8") as f:
                f.write("user\r\npass word\r\n")
            self.assertEqual(ml.read_login(p), ("user", "pass word"))
            with open(p, "w", encoding="utf-8") as f:
                f.write("only one\n")
            with self.assertRaises(ValueError):
                ml.read_login(p)

    def test_a_packet_split_anywhere_is_put_back_together(self) -> None:
        pkt = ml.publish_packet("a/b", b"x" * 300)  # a two-byte remaining length
        for cut in range(len(pkt)):
            self.assertIsNone(ml.take_packet(pkt[:cut]))
        h, body, used = ml.take_packet(pkt + b"\x30")
        self.assertEqual((h, used, body[5:]), (0x30, len(pkt), b"x" * 300))


if __name__ == "__main__":
    unittest.main()
