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
                                     headers={"Content-Type": "application/json", "X-Potluck-Request": "1"})
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=5)
        self.assertEqual(cm.exception.code, 403)
        reason = json.loads(cm.exception.read())["result"]
        cm.exception.close()
        self.assertNotIn("X-Potluck-Request", reason)  # refused by the package, not for a missing header

    def test_the_stream_sends_the_model(self) -> None:
        with urllib.request.urlopen(f"http://{self.api_addr}/api/stream", timeout=5) as r:
            first = r.readline().decode()
            data = r.readline().decode()
        self.assertEqual(first.strip(), "event: model")
        self.assertTrue(data.startswith("data: {"))


class PlacementHost(HubFixture):
    """M14: M10's placement host as the hub's node client -- its frames unchanged, msg_ids split, the hub standing back."""

    def node_client(self, epoch: int = 1234):
        from potluck.payloads import Hello
        from potluck.serial_framing import SerialReassembler, write_serial_frame

        host, _, port = self.relay_addr.rpartition(":")
        s = socket.create_connection((host, int(port)), timeout=2.0)
        h = Hello(boot_epoch=epoch, caps=0, node_id=0x00FE, espnow_version=2, hb_period_ms=100, hb_miss_limit=6,
                  flags=0x01, pubkey_fp=b"")  # want an ACK
        hello = fr.encode(src=0x00FE, dst=fr.NODE_BROADCAST, opcode=fr.Op.HELLO, seq=77, payload=h.encode())
        s.sendall(write_serial_frame(hello))
        return s, SerialReassembler(), write_serial_frame

    def frames_until(self, s, ra, want, timeout: float = 2.0):
        # Every chunk is decoded whole (feed() is a generator: leaving it mid-chunk loses the rest of the stream's
        # bytes and corrupts the next frame), and frames not yet wanted wait in a queue for the next call.
        q = self.__dict__.setdefault("_pending", {}).setdefault(id(s), [])
        end = time.time() + timeout
        while True:
            while q:
                f = q.pop(0)
                if want(f):
                    return f
            if time.time() >= end:
                return None
            try:
                data = s.recv(4096)
            except socket.timeout:
                continue
            if not data:
                return None
            q.extend(fr.parse(raw) for raw in list(ra.feed(data)))

    def test_the_host_node_is_the_pcs_node_and_tools_still_work_beside_it(self) -> None:
        from potluck.ns_payloads import Read

        hellos = self.node.hellos_seen
        s, ra, wsf = self.node_client()
        try:
            ack = self.frames_until(s, ra, lambda f: f.opcode == fr.Op.HELLO_ACK)
            self.assertIsNotNone(ack, "the cell's HELLO_ACK reaches the host node")
            self.assertEqual(self.node.hellos_seen, hellos + 1)
            self.assertIs(self.hub.node_client is not None, True)
            # Its own request, numbered below 0x8000, comes back to it with its own msg_id.
            uptime = next(p for p in sys_paths_for(0x1001) if p.endswith("sys/uptime"))
            from potluck.paths import path_hash
            req = fr.encode(src=0x00FE, dst=0x1001, opcode=fr.Op.READ, seq=78, msg_id=5,
                            payload=Read(path_hash=path_hash(uptime)).encode())
            s.sendall(wsf(req))
            rep = self.frames_until(s, ra, lambda f: f.opcode == fr.Op.REPLY)
            self.assertIsNotNone(rep)
            self.assertEqual(rep.msg_id, 5)
            # A tool and the hub read beside it; their numbers are the hub's (from 0x8000), never the host node's.
            t = self.tool()
            try:
                hellos = self.node.hellos_seen
                self.assertIsNotNone(t.hello(timeout=2.0))
                self.assertEqual(self.node.hellos_seen, hellos, "no HELLO of the hub's while the host node is attached")
                self.assertTrue(t.read(uptime, timeout=2.0).usable())
                self.assertTrue(self.hub.read(uptime, timeout=2.0).usable())
            finally:
                t.close()
            # A second node is refused: the hub closes it.
            s2, ra2, _ = self.node_client(epoch=99)
            try:
                self.assertIsNone(self.frames_until(s2, ra2, lambda f: True, timeout=1.5))
            finally:
                s2.close()
            self.assertTrue(any(e["kind"] == "host_node_refused" for e in self.model.events))
        finally:
            s.close()
        deadline = time.time() + 2
        while time.time() < deadline and self.hub.node_client is not None:
            time.sleep(0.05)
        self.assertIsNone(self.hub.node_client)

    def test_the_hubs_msg_ids_stay_above_the_host_nodes(self) -> None:
        ids = [self.hub._next_msg_id() for _ in range(5)]
        self.assertTrue(all(i >= hb.Hub.HUB_MSG_ID_BASE for i in ids))
        self.hub._msg_id = 0xFFFF
        self.assertEqual(self.hub._next_msg_id(), hb.Hub.HUB_MSG_ID_BASE)

class Telemetry(HubFixture):
    """M15: what a board's telemetry puts in the model -- heap, links, actors -- and an asleep board shown as asleep."""

    def board_with_tel(self, flags: int):
        import test_telemetry as tt
        from potluck import telemetry as tm

        h = tm.parse_health(bytes.fromhex(tt.HEALTH_GOLDEN))
        h.flags = flags
        h.int_min, h.int_total_kib = 180000, 300
        _, _, links = tm.parse_links(bytes.fromhex(tt.LINKS_GOLDEN))
        with self.model.lock:
            self.model.boards[0x1001] = {"state": "alive", "since_at": 0, "last_ok": time.monotonic(),
                                         "tel": {"health": h, "links": links, "actors": [], "events_next": 1,
                                                 "at": 1}}

    def test_the_model_shows_a_boards_heap_and_links(self) -> None:
        self.board_with_tel(0x14)
        b = self.model.snapshot()["boards"][0]
        self.assertEqual(b["heap"]["internal_free"], 191603)
        self.assertEqual(b["links"][0]["peer"], "7368")
        self.assertEqual(b["links"][0]["rtt_p99_ms"], 16.0)
        self.assertEqual(b["links"][0]["window_ms"], 6000)
        self.assertEqual(b["rssi"], -45)
        self.assertEqual(b["telemetry"]["flags"], ["connected", "enrolled"])
        self.assertEqual(self.model.snapshot()["signals"]["saturation"]["internal_ram"], round(1 - 180000 / 307200, 2))

    def test_a_board_that_declared_a_long_window_and_goes_quiet_is_asleep_not_dead(self) -> None:
        from potluck import telemetry as tm

        self.model.telemetry_period_ms = 0
        self.board_with_tel(tm.HEALTH_LONG_WINDOW)
        for _ in range(hb.Model.DEAD_AFTER_MISSES - 1):
            self.model.pull()  # FakeNode answers no NODE_STATUS: the board is quiet -- one miss is not enough
            self.assertEqual(self.model.boards[0x1001]["state"], "alive")
        self.model.pull()
        self.assertEqual(self.model.boards[0x1001]["state"], "asleep")
        self.board_with_tel(0)
        for _ in range(hb.Model.DEAD_AFTER_MISSES):
            self.model.pull()
        self.assertEqual(self.model.boards[0x1001]["state"], "dead")

    def test_every_pull_goes_to_the_telemetry_log_so_no_console_is_needed(self) -> None:
        import io
        import json
        from unittest import mock

        import test_telemetry as tt
        from potluck import telemetry as tm

        self.board_with_tel(0)
        h = tm.parse_health(bytes.fromhex(tt.HEALTH_GOLDEN))
        _, _, links = tm.parse_links(bytes.fromhex(tt.LINKS_GOLDEN))
        self.model.telemetry_log = io.StringIO()
        with mock.patch.object(self.model, "board_ids", return_value=[0x1001]), \
                mock.patch.object(tm, "health", return_value=h), mock.patch.object(tm, "links", return_value=links), \
                mock.patch.object(tm, "actors", return_value=[]), mock.patch.object(tm, "events", return_value=([], 1)):
            self.model.pull_telemetry()
        rec = json.loads(self.model.telemetry_log.getvalue())
        self.assertEqual((rec["rec"], rec["node"], rec["epoch"]), ("tel", 0x1001, 344))
        self.assertEqual(rec["links"][0], {"peer": 0x7368, "state": "alive", "rx_lost": 7, "rx_bcast": 40,
                                           "rx_frames": 2000, "tx_cb_ok": 990, "tx_cb_fail": 10,
                                           "rtt_p50_ms": 5.1, "rtt_p99_ms": 16.0, "rtt_timeouts": 0xFFFF})

class RelayedFrames(unittest.TestCase):
    """M0-LOG session 42: with the cabled board relaying (M10), other boards' frames come down the cable too."""

    def test_the_cabled_board_is_learned_from_what_is_said_to_us_never_from_a_relayed_member(self) -> None:
        import struct as st
        from potluck.payloads import Heartbeat
        from potluck.serial_framing import write_serial_frame

        host_side, node_side = LoopbackTransport.pair()
        hub = hb.Hub(host_side)
        hub.start()
        try:
            beacon = Heartbeat(uptime_ms=1, boot_epoch=9, hb_seq=1, tx_frames=0, tx_cb_ok=0, tx_cb_fail=0,
                               rx_frames=0, rx_lost_seqgap=0, turnaround_us=0, ack_of_msg_id=0, rtt_min_us=None,
                               rtt_max_us=None, free_dram_kib=0, espnow_version=2, hb_flags=0)
            relayed = fr.encode(src=0x7368, dst=fr.NODE_BROADCAST, opcode=fr.Op.HEARTBEAT, seq=1, payload=beacon.encode())
            node_side.write(write_serial_frame(relayed))
            time.sleep(0.3)
            self.assertIsNone(hub.peer_node_id, "a relayed member's broadcast is not the cabled board")
            ack = st.pack("<IHBBBBH", 5, 0x6300, 2, 0, 10, 6, 0)
            node_side.write(write_serial_frame(fr.encode(src=0x6300, dst=0x00FE, opcode=fr.Op.HELLO_ACK, seq=2,
                                                         payload=ack)))
            time.sleep(0.3)
            self.assertEqual(hub.peer_node_id, 0x6300)
            other = st.pack("<IHBBBBH", 7, 0x7368, 2, 0, 10, 6, 0)  # B answering a host node's HELLO, relayed
            node_side.write(write_serial_frame(fr.encode(src=0x7368, dst=0x00FE, opcode=fr.Op.HELLO_ACK, seq=3,
                                                         payload=other)))
            time.sleep(0.3)
            self.assertEqual(hub.peer_node_id, 0x6300)
        finally:
            hub.close()

class SecondHub(unittest.TestCase):
    def test_a_second_hub_on_this_pc_is_refused_naming_the_first_before_it_opens_the_port(self) -> None:
        import contextlib
        import io
        import tempfile
        from pathlib import Path
        from unittest import mock

        with tempfile.TemporaryDirectory() as d:
            st = Path(d) / "hub.json"
            st.write_text(json.dumps({"pid": 4242, "port": "COM6", "relay": "127.0.0.1:8471", "api": "127.0.0.1:8470"}))
            out = io.StringIO()
            with (mock.patch.object(hb, "state_file", return_value=st),
                  mock.patch.object(hb, "hub_endpoint", return_value="127.0.0.1:8471"),
                  mock.patch.object(hb, "open_transport") as opened, contextlib.redirect_stdout(out)):
                rc = hb.main(["--port", "COM6"])
            self.assertEqual(rc, 2)
            self.assertIn("REFUSED", out.getvalue())
            self.assertIn("pid 4242", out.getvalue())
            opened.assert_not_called()


if __name__ == "__main__":
    unittest.main()
