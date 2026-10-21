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
                mock.patch.object(tm, "actors", return_value=[]), mock.patch.object(tm, "events", return_value=([], 1)), \
                mock.patch.object(tm, "auth", return_value={"verified": 3, "stale_epoch": 1}):
            self.model.pull_telemetry()
        rec = json.loads(self.model.telemetry_log.getvalue())
        self.assertEqual((rec["rec"], rec["node"], rec["epoch"]), ("tel", 0x1001, 344))
        self.assertEqual(rec["auth"], {"verified": 3, "stale_epoch": 1})  # 2026-10-19: why a board is kept out
        self.assertEqual(self.model.snapshot()["boards"][0]["telemetry"]["auth"], {"verified": 3, "stale_epoch": 1})
        self.assertEqual(rec["links"][0], {"peer": 0x7368, "state": "alive", "rx_lost": 7, "rx_bcast": 40,
                                           "rx_frames": 2000, "tx_cb_ok": 990, "tx_cb_fail": 10,
                                           "rtt_p50_ms": 5.1, "rtt_p99_ms": 16.0, "rtt_timeouts": 0xFFFF})

    def test_a_boards_history_on_the_hubs_first_look_is_logged_as_earlier_at_its_own_time(self) -> None:
        from unittest import mock

        import test_telemetry as tt
        from potluck import telemetry as tm

        h = tm.parse_health(bytes.fromhex(tt.HEALTH_GOLDEN))  # up 3600 s
        _, _, links = tm.parse_links(bytes.fromhex(tt.LINKS_GOLDEN))
        with self.model.lock:
            self.model.boards[0x1001] = {"state": "alive", "since_at": 0, "last_ok": time.monotonic()}
        old = tm.EventTel(seq=1, at_ms=600_000, kind=tm.EVENT_KINDS.index("peer_dead"), node=0x7368, a=0, b=0)
        new = tm.EventTel(seq=2, at_ms=3_599_000, kind=tm.EVENT_KINDS.index("peer_dead"), node=0x7368, a=0, b=0)
        with mock.patch.object(self.model, "board_ids", return_value=[0x1001]), \
                mock.patch.object(tm, "health", return_value=h), mock.patch.object(tm, "links", return_value=links), \
                mock.patch.object(tm, "actors", return_value=[]), mock.patch.object(tm, "auth", return_value=None), \
                mock.patch.object(tm, "events", side_effect=[([old], 2), ([new], 3)]):
            self.model.pull_telemetry()  # the hub's first look: what the board kept happened before it watched
            first = [e for e in self.model.events if e["kind"] == "board_peer_dead"]
            self.assertEqual(len(first), 1)
            self.assertTrue(first[0]["earlier"])
            self.assertAlmostEqual(first[0]["at"], hb._hub_ms() - 3_000_000, delta=5_000)  # 50 min before now
            self.assertEqual(self.model.link_deaths, {})  # history is not a death the hub saw
            self.model.pull_telemetry()  # the same boot, later: news
            second = [e for e in self.model.events if e["kind"] == "board_peer_dead"][1]
            self.assertNotIn("earlier", second)
            self.assertEqual(self.model.link_deaths, {(0x1001, 0x7368): 1})

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

    def test_a_tool_that_stops_reading_is_dropped_not_waited_for(self) -> None:
        """2026-10-21: a hung host node stopped reading its relay socket; the hub's reader, sending it the cell's
        frames, would have waited on it for good once the socket's buffers filled."""
        a, b = socket.socketpair()  # b is never read
        old = hb._Client.SEND_TIMEOUT_S
        hb._Client.SEND_TIMEOUT_S = 0.3
        try:
            c = hb._Client(a, "stuck")
            t0 = time.monotonic()
            frame = fr.encode(src=0x6300, dst=0x00FE, opcode=fr.Op.HEARTBEAT, seq=1, payload=bytes(200))
            while c.alive and time.monotonic() - t0 < 10:
                c.send_raw(frame)
            self.assertFalse(c.alive)
            self.assertLess(time.monotonic() - t0, 5)
            c.send_raw(frame)  # dropped: returns at once
        finally:
            hb._Client.SEND_TIMEOUT_S = old
            a.close()
            b.close()


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


class Starting(HubFixture):
    """hub.starting counts from the hub's first look at the cell, not from when it was made."""

    def test_a_hub_that_waited_before_its_first_pull_is_still_starting(self):
        from unittest import mock
        self.model.started -= 2 * hb.Model.STARTUP_S  # it waited out a killed hub's lease before serving
        self.assertTrue(self.model.snapshot()["hub"]["starting"])  # nothing looked at yet: no verdict
        with mock.patch.object(self.model, "board_ids", return_value=[0x1001]):
            self.model.pull()
        self.assertTrue(self.model.starting())  # its values have not settled yet
        self.model.first_pull -= hb.Model.STARTUP_S
        self.assertFalse(self.model.starting())  # and never longer than STARTUP_S after the first look


class Settled(unittest.TestCase):
    """One late read of another board's value is shown as read, but neither marks an app nor logs a change: the
    settled quality moves only when a new quality holds for two reads (found 2026-10-16 on an everyday cell)."""

    def test_a_single_stale_read_does_not_settle_two_in_a_row_do(self):
        from types import SimpleNamespace as NS
        from potluck.value import Quality, Reading
        events = []
        me = NS(lock=threading.Lock(), resources={}, SETTLE_READS=hb.Model.SETTLE_READS,
                STARTUP_S=hb.Model.STARTUP_S, _event_locked=lambda kind, n, d: events.append(d))
        path = "potluck://lab/act/alarm/out"

        def read(q):
            hb.Model._record(me, path, Reading(quality=int(q)))
            return me.resources[path]["quality"], me.resources[path]["settled"]

        self.assertEqual(read(Quality.GOOD), ("GOOD", None))       # one read: no verdict yet (STARTING)
        self.assertEqual(read(Quality.GOOD), ("GOOD", "GOOD"))     # it starts GOOD -- not a change, no event
        self.assertEqual(read(Quality.STALE), ("STALE", "GOOD"))   # shown as read; not settled
        self.assertEqual(read(Quality.GOOD), ("GOOD", "GOOD"))
        self.assertEqual(events, [])                               # nothing logged for the blip
        read(Quality.STALE)
        self.assertEqual(read(Quality.STALE), ("STALE", "STALE"))  # two in a row: it is stale
        self.assertEqual(events, [{"path": path, "from": "GOOD", "to": "STALE"}])
        self.assertEqual(read(Quality.GOOD), ("GOOD", "STALE"))    # and one good read does not settle it back
        self.assertEqual(read(Quality.GOOD), ("GOOD", "GOOD"))

    def test_a_hub_start_is_quiet_until_a_quality_holds_and_says_so(self):
        from types import SimpleNamespace as NS
        from unittest import mock
        from potluck.value import Quality, Reading
        events = []
        me = NS(lock=threading.Lock(), resources={}, SETTLE_READS=hb.Model.SETTLE_READS,
                STARTUP_S=hb.Model.STARTUP_S, _event_locked=lambda kind, n, d: events.append(d))
        hb.Model._record(me, "p", Reading(quality=int(Quality.STALE)))
        self.assertEqual(hb.Model._resource_json(NS(cluster=None, _safety=lambda p: False), "p",
                                                 me.resources["p"])["settled_quality"], "STARTING")
        hb.Model._record(me, "p", Reading(quality=int(Quality.STALE)))
        # two STALE reads at start are the cabled board's old copies, not a verdict: still starting
        self.assertIsNone(me.resources["p"]["settled"])
        with mock.patch.object(hb.time, "monotonic", return_value=time.monotonic() + hb.Model.STARTUP_S):
            hb.Model._record(me, "p", Reading(quality=int(Quality.STALE)))
        self.assertEqual((me.resources["p"]["settled"], events), ("STALE", []))  # it outlasted the start: STALE,
        # and where it starts is not a change
        # a resource that never holds a quality for two reads still gets a verdict once STARTUP_S has passed
        t = time.monotonic()
        for i, q in enumerate((Quality.GOOD, Quality.STALE, Quality.GOOD)):
            with mock.patch.object(hb.time, "monotonic", return_value=t + i * hb.Model.STARTUP_S):
                hb.Model._record(me, "q", Reading(quality=int(q)))
        self.assertEqual(me.resources["q"]["settled"], "STALE")


class PcPresents(unittest.TestCase):
    def test_the_pc_is_the_presenter_when_the_boards_telemetry_names_it(self) -> None:
        """2026-10-21: with the tray's host node holding the role no board runs `present`; the model said nobody."""
        from types import SimpleNamespace as NS
        from potluck import manifest as mf
        from potluck import reconcile as rc
        m = mf.parse({"schema": 1, "system": "lab", "min_core_version": 1,
                      "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": 0x00FE, "label": "pc", "kind": "host",
                                                                         "placement": True}],
                      "actors": [{"name": "present", "module": "builtin:present", "latency_class": 4,
                                  "config": {"broker": "192.168.0.77:1883", "hub": True}}]})
        model = hb.Model(NS(record=None), package=NS(manifest=m))
        key = f"{hb.path_hash(rc.output_path(m, m.actors[0])):08x}"
        boards = [{"node": "6300", "actors": [], "telemetry": {"actors": [{"key": key, "owner": "00fe"}]}}]
        self.assertEqual(model._cell_json(boards)["presenter"], "00fe")
        boards[0]["telemetry"]["actors"][0]["owner"] = "6300"
        boards[0]["actors"] = ["present"]
        self.assertEqual(model._cell_json(boards)["presenter"], "6300")

    def test_the_presenting_app_works_by_the_host_nodes_report_while_the_pc_holds_the_role(self) -> None:
        """2026-10-21: with the PC presenting, act/present/out read NO_DATA and the app showed red; the host node's own
        report of its broker connection is what says whether it works."""
        from types import SimpleNamespace as NS
        from potluck import manifest as mf
        from potluck import reconcile as rc
        m = mf.parse({"schema": 1, "system": "lab", "min_core_version": 1,
                      "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": 0x00FE, "label": "pc", "kind": "host",
                                                                         "placement": True}],
                      "actors": [{"name": "present", "module": "builtin:present", "latency_class": 4,
                                  "config": {"broker": "192.168.0.77:1883", "hub": True}}]})
        model = hb.Model(NS(record=None), package=NS(manifest=m))
        key = f"{hb.path_hash(rc.output_path(m, m.actors[0])):08x}"
        boards = [{"node": "6300", "state": "alive", "actors": [],
                   "telemetry": {"actors": [{"key": key, "owner": "00fe", "running_here": False}]}}]
        res = [{"path": "potluck://lab/act/present/out", "quality": "NO_DATA", "settled_quality": "NO_DATA"}]
        [app] = model._apps_json(boards, res)
        self.assertEqual((app["running_on"], app["working"]), (["00fe"], False))
        self.assertIn("no report", app["detail"])
        model.reports.report("presenting (MQTT, this PC)", "connected", "", "00fe")
        [app] = model._apps_json(boards, res)
        self.assertEqual((app["working"], app["detail"]), (True, ""))
        from potluck import apply as ap
        self.assertEqual(ap.expected(m, [0x6300])[2], [])  # apply's verify does not wait for that read

    def test_the_host_node_reports_its_presenting_state(self) -> None:
        from types import SimpleNamespace as NS
        from potluck import hostnode as hn
        self.assertEqual(hn.report_state(NS(cfg=None, connected=False), ("b", 8883))[0], "idle")
        self.assertEqual(hn.report_state(NS(cfg={}, connected=True), ("b", 8883))[0], "connected")
        self.assertEqual(hn.report_state(NS(cfg={}, connected=False), ("b", 8883))[0], "disconnected")


class UpdateStatus(unittest.TestCase):
    def test_an_update_is_pending_when_the_boards_run_other_firmware_than_this_potluck_carries(self) -> None:
        """2026-10-20, the owner: updating the boards should not hide inside the library's Apply."""
        import tempfile
        from pathlib import Path
        from types import SimpleNamespace as NS
        from unittest import mock
        with tempfile.TemporaryDirectory() as d:
            rel = Path(d) / "ba193e1-c1480d"
            rel.mkdir()
            (rel / "release.json").write_text(json.dumps({"version": "ba193e1-c1480d", "elf_sha256": "00" * 32}))
            model = hb.Model(NS(record=None))
            with mock.patch.dict(os.environ, {"POTLUCK_FIRMWARE_DIR": d}):
                def boards(*versions):
                    model.boards = {i: {"state": "alive", "status": NS(fw_version=v)} for i, v in enumerate(versions)}
                boards("0c1162c-028577", "0c1162c-028577")
                self.assertEqual(model._update_json("0c1162c-028577"),
                                 {"boards": "0c1162c-028577", "available": "ba193e1-c1480d", "mixed": False,
                                  "pending": True})
                boards("ba193e1-c1480d", "ba193e1-c1480d")
                self.assertFalse(model._update_json("ba193e1-c1480d")["pending"])
                boards("ba193e1-c1480d", "0c1162c-028577")  # one left behind (a rollout stopped half-way)
                self.assertTrue(model._update_json("ba193e1-c1480d")["pending"])
            with mock.patch.dict(os.environ, {"POTLUCK_FIRMWARE_DIR": str(Path(d) / "none")}):
                self.assertEqual(model._update_json("x")["available"], None)  # a hub run from source carries none


class AppParts(unittest.TestCase):
    def test_an_app_lists_what_it_needs_wired_with_its_gpios(self) -> None:
        """2026-10-20, the Potluck Desk's Wiring window: apps[].parts from the module's descriptor and the actor's
        config, so a sensor app brings its own wiring guide."""
        from types import SimpleNamespace as NS
        import test_modules as tmods
        from potluck import manifest as mf
        from potluck import modules as mods
        mods.clear()
        try:
            mods.register(mods.parse_descriptor(tmods.SENSOR))
            m = mf.parse(tmods.manifest([{"name": "air_b", "module": "ext:probe/air", "latency_class": 4,
                                          "pin": 0x7368, "config": {"sda_gpio": 1}}]))
            model = hb.Model(NS(record=None), package=NS(manifest=m))
            [app] = model._apps_json([{"node": "7368", "state": "alive", "actors": [], "telemetry": None}], [])
            self.assertEqual(app["parts"], [{"part": "SHT41", "bus": "i2c", "pins": {"scl": 9, "sda": 1},
                                             "guide": "VCC to 3V3, GND to G, SDA and SCL to the pins below"}])
        finally:
            mods.clear()


class StaleReadsTwice(unittest.TestCase):
    def test_a_stale_read_is_read_again_and_the_fresh_one_kept(self):
        """2026-10-19: another board's value comes from the cabled board's copy, which a read refreshes; with 42
        resources the hub's pass outlasted a 5 s bound. The second read answers from the refreshed copy."""
        from types import SimpleNamespace as NS
        from potluck.value import Quality, Reading
        answers = {"stale": [Reading(quality=int(Quality.STALE), age_ms=5220), Reading(quality=int(Quality.GOOD), age_ms=1326)],
                   "good": [Reading(quality=int(Quality.GOOD), age_ms=300)]}
        asked = []

        def read(path, timeout):
            asked.append(path)
            return answers[path].pop(0)
        hub = NS(record=None, _quiet_until=0.0, peer_node_id=0, stats=NS(rx_frames=0, tx_frames=0), read=read)
        model = hb.Model(hub, telemetry_period_ms=0)
        model.resource_paths = lambda: ["stale", "good"]
        model.pull()
        self.assertEqual(asked, ["stale", "stale", "good"])  # only the STALE one twice
        self.assertEqual((model.resources["stale"]["quality"], model.resources["stale"]["reading"].age_ms), ("GOOD", 1326))


class Panels(unittest.TestCase):
    """M20's app panels: one per room for a per_room module, only for exposed resources, and a reading that is not
    GOOD is neither on nor off."""

    def test_a_room_panel_per_room_from_exposed_resources_only(self):
        from types import SimpleNamespace as NS
        from potluck import modules as mods
        mods.clear()
        try:
            mods.register(mods.parse_descriptor({"component": "s", "types": [128, 143], "modules": [{
                "module": "ext:s/fusion", "type": 130, "placement": "portable", "per_room": True,
                "outputs": ["act/{actor}/out"],
                "panel": {"kind": "room", "output": "act/{actor}/out", "on": "Someone is here", "off": "Empty"}}]}))
            actors = [NS(name=n, module="ext:s/fusion") for n in ("fusion_kitchen", "fusion_hall", "fusion_garden")]
            actors.append(NS(name="tick", module="builtin:ticker"))
            me = NS(package=NS(manifest=NS(system="home", actors=actors)),
                    cluster=NS(rooms={1: "Kitchen", 2: "Hall", 3: "Garden"}))
            res = [{"path": "potluck://home/act/fusion_kitchen/out", "value": True, "quality": "GOOD", "exposed": True},
                   {"path": "potluck://home/act/fusion_hall/out", "value": False, "quality": "STALE", "exposed": True},
                   {"path": "potluck://home/act/fusion_garden/out", "value": True, "quality": "GOOD", "exposed": False}]
            p = hb.Model._panels_json(me, res)
            self.assertEqual([(x["room"], x["on"], x["quality"]) for x in p],
                             [("Kitchen", True, "GOOD"), ("Hall", None, "STALE")])  # the garden is not exposed
            self.assertEqual((p[0]["on_text"], p[0]["off_text"]), ("Someone is here", "Empty"))
        finally:
            mods.clear()


class Interest(HubFixture):
    """On-demand reads (POST /api/interest): a viewer asks for paths the package's reads leave out -- a board's sys/,
    a module's {peer} and ? outputs -- the hub reads them each pull while asked, and drops them when nobody renews."""

    def post(self, path: str, body: dict, header: bool = True):
        req = urllib.request.Request(f"http://{self.api_addr}{path}", data=json.dumps(body).encode(), method="POST",
                                     headers={"Content-Type": "application/json", "X-Potluck-Client": "desk",
                                              **({"X-Potluck-Request": "1"} if header else {})})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def model_json(self) -> dict:
        with urllib.request.urlopen(f"http://{self.api_addr}/api/model", timeout=5) as r:
            return json.loads(r.read())

    def test_a_board_sys_resource_is_read_while_asked_and_shown_on_demand(self) -> None:
        heap = sys_paths_for(0x1001)[0]  # sys/heap-free
        code, rep = self.post("/api/interest", {"paths": [heap], "ttl_s": 30})
        self.assertEqual((code, rep["accepted"], rep["refused"]), (200, [heap], []))
        self.model.pull()
        r = {x["path"]: x for x in self.model_json()["resources"]}[heap]
        self.assertEqual((r["value"], r["unit"], r["quality"], r["on_demand"]), (213400, "B", "GOOD", True))
        self.assertEqual(self.model_json()["hub"]["interest"]["paths"], 1)

    def test_an_expired_interest_stops_the_reads_and_drops_the_value(self) -> None:
        heap = sys_paths_for(0x1001)[0]
        self.model.want("desk", [heap], 30)
        self.model.pull()
        self.assertIn(heap, self.model.resources)
        self.model.interest[heap]["desk"] = 0.0  # nobody renewed it
        served = self.node.reads_served
        self.model.pull()
        self.assertNotIn(heap, self.model.resources)
        self.assertEqual(self.node.reads_served, served)  # and it was not read again

    def test_paths_outside_what_the_cell_publishes_are_refused(self) -> None:
        acc, ref = self.model.want("desk", ["potluck://lab/node-1001/hw/anything", "potluck://other/node-1001/sys/uptime",
                                            42], 30)
        self.assertEqual(acc, [])
        self.assertEqual(len(ref), 3)

    def test_each_program_and_the_hub_have_a_ceiling(self) -> None:
        paths = [f"potluck://lab/node-1001/sys/x{i}" for i in range(self.model.INTEREST_PER_CLIENT + 1)]
        acc, ref = self.model.want("a", paths, 30)
        self.assertEqual((len(acc), len(ref)), (self.model.INTEREST_PER_CLIENT, 1))
        self.assertIn("per program", ref[0]["result"])
        self.model.want("b", [f"potluck://lab/node-1001/sys/y{i}" for i in range(32)], 30)
        acc, ref = self.model.want("c", ["potluck://lab/node-1001/sys/z"], 30)
        self.assertEqual(acc, [])
        self.assertIn("in all", ref[0]["result"])
        acc, ref = self.model.want("a", paths[:3], 30)  # renewing what it has is never refused
        self.assertEqual((len(acc), ref), (3, []))

    def test_a_module_peer_and_sometimes_output_may_be_asked_for(self) -> None:
        from types import SimpleNamespace as NS
        from potluck import modules as mods
        mods.clear()
        try:
            mods.register(mods.parse_descriptor({"component": "s", "types": [128, 143], "modules": [
                {"module": "ext:s/link", "type": 128, "placement": "every", "outputs": ["{node}/rf/link/{peer}/mean"]},
                {"module": "ext:s/fusion", "type": 129, "placement": "portable", "outputs": ["act/{actor}/out", "?act/{actor}/x"]}]}))
            m = hb.Model(self.hub, package=NS(manifest=NS(system="lab", actors=[
                NS(name="rf_link", module="ext:s/link"), NS(name="fusion_room", module="ext:s/fusion")])))
            acc, ref = m.want("desk", ["potluck://lab/node-7368/rf/link/node-8160/mean",
                                       "potluck://lab/act/fusion_room/x",
                                       "potluck://lab/act/fusion_other/x",  # no such actor
                                       "potluck://lab/node-7368/rf/link/node-8160/var"], 30)  # not declared
            self.assertEqual(acc, ["potluck://lab/node-7368/rf/link/node-8160/mean", "potluck://lab/act/fusion_room/x"])
            self.assertEqual(len(ref), 2)
        finally:
            mods.clear()

    def test_the_api_needs_the_request_header_and_a_list(self) -> None:
        code, _ = self.post("/api/interest", {"paths": [sys_paths_for(0x1001)[0]]}, header=False)
        self.assertEqual(code, 403)
        code, rep = self.post("/api/interest", {"paths": "not a list"})
        self.assertEqual(code, 400)
        code, rep = self.post("/api/interest", {"paths": [], "ttl_s": 9999})
        self.assertEqual((code, rep["ttl_s"]), (200, 120.0))  # clamped
        req = urllib.request.Request(f"http://{self.api_addr}/api/interest", data=b'{"paths": [], "ttl_s": NaN}',
                                     method="POST", headers={"X-Potluck-Request": "1"})
        with self.assertRaises(urllib.error.HTTPError) as cm:
            urllib.request.urlopen(req, timeout=5)
        cm.exception.close()
        self.assertEqual(cm.exception.code, 400)  # never a NaN back in the reply (not JSON)

    def test_a_refusal_before_the_body_is_read_still_answers_and_keeps_the_connection_usable(self) -> None:
        # Found by the Potluck Desk 2026-10-18: an unknown POST path got its 404 without the body being read; on
        # Windows the client was often reset instead, and on a kept-alive connection the body is the next "request".
        import http.client
        host, _, port = self.api_addr.rpartition(":")
        c = http.client.HTTPConnection(host, int(port), timeout=5)
        try:
            for _ in range(3):
                c.request("POST", "/api/no-such-thing", body=b"x" * 60000, headers={"X-Potluck-Request": "1"})
                r = c.getresponse()
                r.read()
                self.assertEqual(r.status, 404)
            c.request("GET", "/api/hello")  # the same connection, still in step
            r = c.getresponse()
            self.assertEqual((r.status, json.loads(r.read())["api"]), (200, hb.API_VERSION))
        finally:
            c.close()



class TwoGuests(unittest.TestCase):
    """2026-10-19: the store's overheat_alarm (A) beside the earlier guest alarm (B), one module: the hub's app view
    showed one app "running on 2 nodes"."""

    def test_each_portable_actor_is_counted_where_its_board_says_it_runs(self) -> None:
        from types import SimpleNamespace as NS
        from potluck import manifest as mf
        cfg = {"bundle": "x.guest.json", "sha256": "00" * 32, "outputs": "out:f32 alarm:i32 count:i32",
               "fuel": 100000, "memory_kib": 64, "period_ms": 1000}
        m = mf.parse({"schema": 1, "system": "lab", "min_core_version": 1,
                      "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": 0x7368, "label": "B"}],
                      "actors": [{"name": n, "module": "guest:overheat_alarm", "latency_class": 4, "config": cfg}
                                 for n in ("alarm", "overheat_alarm")]})
        model = hb.Model(NS(record=None), package=NS(manifest=m))
        key = lambda name: f"{hb.path_hash(f'potluck://lab/act/{name}/out'):08x}"  # noqa: E731
        tel = lambda here: {"actors": [{"key": key("alarm"), "running_here": here == "7368", "owner": "7368"},  # noqa: E731
                                       {"key": key("overheat_alarm"), "running_here": here == "6300", "owner": "6300"}]}
        boards = [{"node": n, "state": "alive", "actors": ["guest"], "telemetry": tel(n)} for n in ("6300", "7368")]
        apps = {a["name"]: a for a in model._apps_json(boards, [])}
        self.assertEqual((apps["alarm"]["running_on"], apps["alarm"]["working"]), (["7368"], True))
        self.assertEqual((apps["overheat_alarm"]["running_on"], apps["overheat_alarm"]["working"]), (["6300"], True))
        for b in boards:  # no telemetry yet: the kind is all there is, as before
            b["telemetry"] = None
        self.assertEqual(model._apps_json(boards, [])[0]["running_on"], ["6300", "7368"])

    def test_a_new_package_on_the_boards_is_planned_again(self) -> None:
        from types import SimpleNamespace as NS
        model = hb.Model(NS(record=None), cluster=NS(path="cell.json", boards=[(0x6300, "A")], system="lab"))
        calls = []
        model._replan = lambda counter: calls.append(counter)  # noqa: E731 -- the thread's work, counted
        model.boards[0x6300].update(state="alive", status=NS(pkg_counter=99))
        model._follow_package()
        self.assertEqual(calls, [])  # where the hub starts is not a change
        model.boards[0x6300]["status"] = NS(pkg_counter=100)
        model._follow_package()
        for _ in range(50):
            if calls:
                break
            time.sleep(0.02)
        self.assertEqual(calls, [100])


if __name__ == "__main__":
    unittest.main()
