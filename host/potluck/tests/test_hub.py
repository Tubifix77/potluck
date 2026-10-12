"""M14: the hub -- one program owns the link, and tools share it through its relay.

Against FakeNode over a loopback pair (no hardware): two tools' Bridges connect to the relay and read at once, each
getting its own replies; a tool's HELLO is answered by the hub, its heartbeats never reach the cell; the API serves the
model and events per host/potluck/HUB-API.md, and refuses an outside write the package does not allow.
"""

from __future__ import annotations

import json
import os
import socket
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import frame as fr  # noqa: E402
from potluck import hub as hb  # noqa: E402
from potluck.bridge import Bridge  # noqa: E402
from potluck.fake_node import FakeNode  # noqa: E402
from potluck.sys_paths import sys_paths_for  # noqa: E402
from potluck.transport import LoopbackTransport, TcpTransport  # noqa: E402


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class HubFixture(unittest.TestCase):
    def setUp(self) -> None:
        host_side, node_side = LoopbackTransport.pair()
        self.node = FakeNode(node_side, node_id=0x1001)
        self.node.start()
        self.hub = hb.Hub(host_side)
        self.hub.start()
        self.assertIsNotNone(self.hub.hello(timeout=2.0))
        self.model = hb.Model(self.hub, pull_period_ms=200)
        self.relay_addr = f"127.0.0.1:{free_port()}"
        self.api_addr = f"127.0.0.1:{free_port()}"
        self.relay = hb.serve_relay(self.hub, self.relay_addr)
        self.api = hb.serve_api(self.model, self.api_addr)

    def tearDown(self) -> None:
        self.relay.shutdown()
        self.api.shutdown()
        self.hub.close()
        self.node.stop()

    def tool(self) -> Bridge:
        host, _, port = self.relay_addr.rpartition(":")
        b = Bridge(TcpTransport(host, int(port)), heartbeat=True)
        b.start()
        return b


class Relay(HubFixture):
    def test_a_tools_hello_is_answered_by_the_hub_and_its_heartbeats_stay_off_the_cell(self) -> None:
        hellos_before = self.node.hellos_seen
        t = self.tool()
        try:
            ack = t.hello(timeout=2.0)
            self.assertIsNotNone(ack)
            self.assertEqual(ack.node_id, 0x1001)
            time.sleep(0.5)  # the tool heartbeats every 100 ms
            # The cell saw one HELLO -- the hub's own, so a board that rebooted re-admits the PC -- and still one host.
            self.assertEqual(self.node.hellos_seen, hellos_before + 1)
            self.assertEqual(set(self.node.peers), {0x00FE})
        finally:
            t.close()

    def test_the_hub_says_hello_again_when_the_cabled_board_reboots(self) -> None:
        before = self.node.hellos_seen
        self.node.boot_epoch += 1  # the board on the cable rebooted: its heartbeats carry a new epoch
        deadline = time.time() + 3
        while time.time() < deadline and self.node.hellos_seen == before:
            time.sleep(0.05)
        self.assertGreater(self.node.hellos_seen, before)

    def test_two_tools_read_at_once_and_each_gets_its_own_replies(self) -> None:
        paths = sys_paths_for(0x1001)
        uptime = next(p for p in paths if p.endswith("sys/uptime"))
        a, b = self.tool(), self.tool()
        try:
            a.hello(timeout=2.0)
            b.hello(timeout=2.0)
            results: dict[str, list] = {"a": [], "b": []}

            def run(name: str, br: Bridge) -> None:
                for _ in range(20):
                    results[name].append(br.read(uptime, timeout=2.0))

            ta = threading.Thread(target=run, args=("a", a))
            tb = threading.Thread(target=run, args=("b", b))
            ta.start(), tb.start()
            ta.join(10), tb.join(10)
            self.assertEqual(len(results["a"]), 20)
            self.assertEqual(len(results["b"]), 20)
            self.assertEqual(a.stats.replies_unmatched + b.stats.replies_unmatched, 0)
            self.assertGreaterEqual(self.hub.relay_counts["replies"], 40)
            # And the hub's own reads still work alongside.
            self.assertTrue(self.hub.read(uptime, timeout=2.0).usable())
        finally:
            a.close()
            b.close()

    def test_a_running_hub_is_found_by_its_state_file(self) -> None:
        old = os.environ.get("LOCALAPPDATA")
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            os.environ["LOCALAPPDATA"] = d
            try:
                hb.state_file().parent.mkdir(parents=True, exist_ok=True)
                hb.state_file().write_text(json.dumps({"port": "COM6", "relay": self.relay_addr}), encoding="utf-8")
                self.assertEqual(hb.hub_endpoint("com6"), self.relay_addr)
                self.assertIsNone(hb.hub_endpoint("COM7"))
            finally:
                if old is None:
                    del os.environ["LOCALAPPDATA"]
                else:
                    os.environ["LOCALAPPDATA"] = old


class Api(HubFixture):
    def get(self, path: str):
        with urllib.request.urlopen(f"http://{self.api_addr}{path}", timeout=5) as r:
            return json.loads(r.read())

    def test_the_model_has_every_part_of_the_contract(self) -> None:
        self.model.pull()
        m = self.get("/api/model")
        for k in ("api", "at", "hub", "cell", "boards", "apps", "resources", "integrations", "deploys", "signals"):
            self.assertIn(k, m)
        self.assertEqual(m["api"], 1)
        self.assertEqual(m["hub"]["link"], "up")
        self.assertEqual([b["node"] for b in m["boards"]], ["1001"])
        self.assertIn(m["boards"][0]["state"], ("alive", "unknown"))  # FakeNode does not answer NODE_STATUS
        for k in ("latency_p50_ms", "latency_p99_ms", "traffic_frames_s", "errors_min", "saturation"):
            self.assertIn(k, m["signals"])

    def test_events_page_by_sequence(self) -> None:
        self.model.record("hub_started", None, {"port": "loopback"})
        self.model.record("board_dead", 0x1001, {"was": "alive"})
        e = self.get("/api/events")
        self.assertEqual([x["kind"] for x in e["events"]][-2:], ["hub_started", "board_dead"])
        later = self.get(f"/api/events?since={e['next']}")
        self.assertEqual(later["events"], [])

    def test_an_outside_write_the_package_does_not_allow_is_refused(self) -> None:
        body = json.dumps({"path": "potluck://lab/node-1001/sys/uptime", "value": 1}).encode()
        req = urllib.request.Request(f"http://{self.api_addr}/api/write", data=body, method="POST",
                                     headers={"Content-Type": "application/json"})
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=5)
        self.assertEqual(cm.exception.code, 403)

    def test_the_stream_sends_the_model(self) -> None:
        with urllib.request.urlopen(f"http://{self.api_addr}/api/stream", timeout=5) as r:
            first = r.readline().decode()
            data = r.readline().decode()
        self.assertEqual(first.strip(), "event: model")
        self.assertTrue(data.startswith("data: {"))


if __name__ == "__main__":
    unittest.main()
