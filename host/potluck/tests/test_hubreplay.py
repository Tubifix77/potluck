"""M16: the hub's recording and its replay through the same API the dashboard reads."""

from __future__ import annotations

import http.client
import json
import os
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))

from potluck import hub as hb  # noqa: E402
from potluck import hubreplay as hr  # noqa: E402
from test_hub import free_port  # noqa: E402


def model_at(at: int, state: str) -> dict:
    return {"api": 1, "at": at, "hub": {"pull_period_ms": 2000},
            "boards": [{"node": "7368", "label": "B", "state": state}]}


class Recording(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        rec = hr.Recorder(self.tmp.name, hours=24)
        t0 = 1_791_900_000_000
        evs = []
        for i in range(10):  # B dies at the sixth model, 10 s in
            state = "alive" if i < 5 else "dead"
            if i == 5:
                evs.append({"seq": 1, "at": t0 + i * 2000, "board": "7368", "kind": "board_dead", "detail": {}})
            rec.write(model_at(t0 + i * 2000, state), evs)
        rec.close()
        self.file = os.path.join(self.tmp.name, os.listdir(self.tmp.name)[0])
        self.t0 = t0

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_the_recording_holds_every_model_and_each_event_once(self) -> None:
        models, events = hr.load(self.file)
        self.assertEqual(len(models), 10)
        self.assertEqual([e["seq"] for e in events], [1])

    def test_a_replay_is_served_through_the_same_api_and_seeks(self) -> None:
        model = hr.ReplayModel(self.file, speed=1.0)
        model.control(paused=True)
        addr = f"127.0.0.1:{free_port()}"
        srv = hb.serve_api(model, addr)
        host, _, port = addr.rpartition(":")

        def call(method, path, body=None):
            c = http.client.HTTPConnection(host, int(port), timeout=5)
            c.request(method, path, body=json.dumps(body) if body is not None else None,
                      headers={"Content-Type": "application/json"})
            r = c.getresponse()
            out = (r.status, json.loads(r.read()))
            c.close()
            return out

        try:
            code, m = call("GET", "/api/model")
            self.assertEqual((code, m["boards"][0]["state"]), (200, "alive"))
            self.assertEqual((m["hub"]["replay"]["from_at"], m["hub"]["replay"]["to_at"]), (self.t0, self.t0 + 18000))
            self.assertEqual(call("GET", "/api/events")[1]["events"], [])  # B has not died yet in the replay
            code, r = call("POST", "/api/replay", {"seek_at": self.t0 + 12000})
            self.assertEqual(r["replay"]["position_at"], self.t0 + 12000)
            self.assertEqual(call("GET", "/api/model")[1]["boards"][0]["state"], "dead")
            self.assertEqual([e["kind"] for e in call("GET", "/api/events")[1]["events"]], ["board_dead"])
            code, r = call("POST", "/api/write", {"path": "potluck://lab/node-7368/led/on", "value": True})
            self.assertEqual(code, 403)  # nothing behind a replay can be written
        finally:
            srv.shutdown()

    def test_playing_moves_forward_at_its_speed_and_stops_at_the_end(self) -> None:
        model = hr.ReplayModel(self.file, speed=100.0)
        time.sleep(0.25)  # 25 s of recording at 100x: past the 18 s end
        r = model.snapshot()["hub"]["replay"]
        self.assertEqual((r["position_at"], r["paused"]), (self.t0 + 18000, True))

    def test_a_hub_states_its_retention(self) -> None:
        rec = hr.Recorder(self.tmp.name, hours=6)
        ret = rec.retention()
        self.assertEqual((ret["hours"], ret["files"]), (6, 1))


if __name__ == "__main__":
    unittest.main()
