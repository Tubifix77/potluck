"""M18: each outside program's share of what the hub may ask of the cell, integrations reporting their own state, and a
writable resource that names who may write it."""

from __future__ import annotations

import http.client
import json
import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from potluck import apply as ap  # noqa: E402
from potluck import hub as hb  # noqa: E402
from potluck import hub_clients as hc  # noqa: E402
from test_hub import HubFixture  # noqa: E402


class Api(HubFixture):
    def req(self, method, path, body=None, client="t"):
        host, _, port = self.api_addr.rpartition(":")
        c = http.client.HTTPConnection(host, int(port), timeout=5)
        c.request(method, path, body=json.dumps(body) if body is not None else None,
                  headers={"Content-Type": "application/json", "X-Potluck-Client": client})
        r = c.getresponse()
        data = r.read()
        retry = r.getheader("Retry-After")
        c.close()
        return r.status, (json.loads(data) if data[:1] == b"{" else data), retry

    def test_a_program_pulling_as_fast_as_it_can_is_slowed_and_nobody_else_is(self) -> None:
        codes: list[int] = []
        stop = threading.Event()

        def hammer():
            while not stop.is_set():
                codes.append(self.req("GET", "/api/model", client="greedy")[0])

        t = threading.Thread(target=hammer, daemon=True)
        t.start()
        time.sleep(1.5)
        polite = [self.req("GET", "/api/model", client="dashboard")[0] for _ in range(5)]  # one every few ms
        stop.set()
        t.join(timeout=5)
        self.assertIn(429, codes)  # the greedy one was told to wait
        self.assertEqual(polite, [200] * 5)  # the other was not slowed at all
        served = codes.count(200)
        self.assertLessEqual(served, hc.READ_BURST + hc.READ_RATE * 2.5)  # its share, not more
        code, body, retry = 0, None, None
        while code != 429:
            code, body, retry = self.req("GET", "/api/events", client="greedy2")
        self.assertIn("slow down", body["result"])
        self.assertGreaterEqual(int(retry), 1)
        clients = {c["name"]: c for c in self.req("GET", "/api/model", client="dashboard")[1]["hub"]["clients"]}
        self.assertGreater(clients["greedy"]["refused"], 0)
        self.assertEqual(clients["dashboard"]["refused"], 0)

    def test_an_integration_reports_its_state_and_silence_reads_disconnected(self) -> None:
        code, body, _ = self.req("POST", "/api/integrations", {"name": "Node-RED", "state": "connected",
                                                               "detail": "3 flows"}, client="node-red")
        self.assertEqual((code, body), (200, {"ok": True}))
        ints = {i["name"]: i for i in self.req("GET", "/api/model")[1]["integrations"]}
        self.assertEqual((ints["Node-RED"]["state"], ints["Node-RED"]["kind"], ints["Node-RED"]["where"]),
                         ("connected", "program", "node-red"))
        self.assertEqual(self.req("POST", "/api/integrations", {"name": "x", "state": "great"})[0], 400)
        old = hc.REPORT_FRESH_S
        hc.REPORT_FRESH_S = 0.05
        try:
            time.sleep(0.1)
            ints = {i["name"]: i for i in self.req("GET", "/api/model")[1]["integrations"]}
            self.assertEqual(ints["Node-RED"]["state"], "disconnected")
            self.assertIn("silent for", ints["Node-RED"]["detail"])
        finally:
            hc.REPORT_FRESH_S = old


class Shares(unittest.TestCase):
    def test_writes_have_a_smaller_share_than_reads(self) -> None:
        f = hc.FairShare()
        writes = [f.take("p", "write") for _ in range(10)]
        self.assertEqual(sum(1 for w in writes if w == 0), int(hc.WRITE_BURST))
        self.assertTrue(all(w > 0 for w in writes[int(hc.WRITE_BURST):]))
        self.assertEqual(f.take("p", "read"), 0.0)  # its reads are their own bucket


class Writers(unittest.TestCase):
    def test_a_resource_that_names_its_writers_refuses_every_other_program(self) -> None:
        class Cl:
            outside = {"potluck://lab/node-7368/led/on": "read_write"}
            writers = {"potluck://lab/node-7368/led/on": ("Home Assistant",)}

        class M:
            cluster = Cl()
            recorded: list = []

            def record(self, kind, board, detail):
                self.recorded.append(detail)

            def _safety(self, path):
                return False

        m = M()
        code, body = hb.write_outside(m, "potluck://lab/node-7368/led/on", True, by="Node-RED")
        self.assertEqual((code, body["result"]), (403, "not writable by this program"))
        self.assertEqual(m.recorded[-1]["by"], "Node-RED")

    def test_a_number_is_written_as_the_resources_own_type(self) -> None:
        # From the Node-RED reader: JSON cannot tell 20 from 20.0, so the hub writes the type it knows the resource has.
        from potluck.value import Reading, Value, ValueType

        sent = []

        class Cl:
            outside = {"potluck://lab/node-7368/set/point": "read_write"}
            writers = {}

        class Hub:
            peer_node_id = 0x6300

            def _request(self, op, payload, h, timeout, dst):
                sent.append(payload)
                raise hb.RequestTimeout("test")

        class M:
            cluster = Cl()
            hub = Hub()
            lock = threading.Lock()
            resources = {"potluck://lab/node-7368/set/point": {"reading": Reading(value=Value.of_f32(1.5)), "at": 0}}

            def record(self, *a):
                pass

            def _safety(self, path):
                return False

        hb.write_outside(M(), "potluck://lab/node-7368/set/point", 20)
        self.assertEqual(sent[-1][4], int(ValueType.F32))  # WritePayload's value_type byte

    def test_the_cluster_file_names_writers_only_for_a_read_write_resource(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            def cluster(outside):
                p = Path(tmp) / "c.json"
                p.write_text(json.dumps({"schema": ap.SCHEMA, "system": "lab", "boards": [{"node": "7368"}],
                                         "firmware": None, "outside": outside}), encoding="utf-8")
                return ap.load_cluster(str(p))

            c = cluster({"node-7368/led/on": {"mode": "read_write", "writers": ["Home Assistant"]},
                         "node-7368/hw/die_temp": "read"})
            self.assertEqual(c.outside["potluck://lab/node-7368/led/on"], "read_write")
            self.assertEqual(c.writers, {"potluck://lab/node-7368/led/on": ("Home Assistant",)})
            with self.assertRaisesRegex(ap.ApplyError, "writers name who may write it"):
                cluster({"node-7368/led/on": {"mode": "read", "writers": ["x"]}})


if __name__ == "__main__":
    unittest.main()
