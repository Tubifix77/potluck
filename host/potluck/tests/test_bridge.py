"""The bridge against a known far end -- ARCHITECTURE.md M2, section 4 rule 2, section 7.1.

A bridge bug and a firmware bug look identical from the host side, so these tests put the real
bridge on one end of a loopback and FakeNode on the other. Everything between them is the shipping
code: the real frame codec, the real COBS/CRC framing, the real correlation table.

What this pins down:
  - HELLO admits the host, and nothing works before it does (the firmware's own rule).
  - A READ returns section 4 rule 2's whole tuple, and there is no way to get the number alone.
  - A refused WRITE reports *why*, rather than collapsing to False.
  - A request whose reply never comes raises, rather than fabricating an empty Reading.
  - A reply for a request already abandoned is counted, not silently dropped.
"""

from __future__ import annotations

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import frame as fr
from potluck.bridge import Bridge, BridgeError, RequestTimeout
from potluck.fake_node import FakeNode
from potluck.ns_payloads import REPLY_TO_READ, REPLY_TO_WRITE, Reply
from potluck.paths import path_hash
from potluck.sys_paths import sys_paths_for
from potluck.transport import LoopbackTransport
from potluck.value import NsError, Quality, Unit, Value

NODE_ID = 0x1001


class BridgeFixture(unittest.TestCase):
    """A bridge and a node joined by a loopback, both pumped by hand.

    Pumped rather than threaded: a test that depends on two threads interleaving is a test that
    fails once a month for no reason anyone can reproduce. `exchange()` runs both sides until the
    traffic settles, which is deterministic.
    """

    def setUp(self) -> None:
        self.host_side, self.node_side = LoopbackTransport.pair()
        self.node = FakeNode(self.node_side, node_id=NODE_ID)
        self.bridge = Bridge(self.host_side, heartbeat=False)
        # No reader thread: _on_raw_frame is driven from exchange() instead.

    def tearDown(self) -> None:
        self.bridge.close()
        self.node.stop()

    def exchange(self, rounds: int = 8) -> None:
        """Let both sides run until neither has bytes left to read."""
        for _ in range(rounds):
            moved = self.node.pump()
            data = self.host_side.read(4096)
            if data:
                moved += 1
                self.bridge.stats.rx_bytes += len(data)
                for raw in self.bridge.reassembler.feed(data):
                    self.bridge._on_raw_frame(raw)
            if not moved:
                return

    def join(self) -> None:
        """HELLO, then let the ACK come back. hello() blocks, so it is done in two halves here."""
        self.bridge.send_frame(fr.Op.HELLO, self._hello_payload(), dst=fr.NODE_BROADCAST)
        self.exchange()

    def _hello_payload(self) -> bytes:
        from potluck.payloads import HELLO_FLAG_WANT_ACK, Hello

        return Hello(
            boot_epoch=0, caps=0, node_id=self.bridge.node_id, espnow_version=0,
            hb_period_ms=100, hb_miss_limit=6, flags=HELLO_FLAG_WANT_ACK, pubkey_fp=b"",
        ).encode()

    def request(self, opcode: int, payload: bytes, path_h: int) -> Reply:
        """Send a request and pump until the reply lands. The bridge's own _request() blocks."""
        msg_id = self.bridge._next_msg_id()
        from potluck.bridge import _Pending

        pend = _Pending(msg_id=msg_id, op=opcode, path_hash=path_h,
                        sent_monotonic=time.monotonic())
        self.bridge._pending[msg_id] = pend
        self.bridge.send_frame(opcode, payload, dst=self.bridge.peer_node_id or NODE_ID,
                               msg_id=msg_id, ack_req=True)
        self.exchange()
        assert pend.reply is not None, "no reply arrived"
        return pend.reply


class TestMembership(BridgeFixture):
    def test_a_stranger_is_not_answered_before_it_says_hello(self):
        # The firmware admits an unknown MAC only on HELLO. If the fake node were laxer than the
        # firmware, every later test would pass against a far end no real board matches.
        from potluck.ns_payloads import Read

        h = path_hash(sys_paths_for(NODE_ID)[3])
        self.bridge.send_frame(fr.Op.READ, Read(path_hash=h).encode(), dst=NODE_ID, msg_id=99)
        self.exchange()
        self.assertEqual(self.node.reads_served, 0)
        self.assertEqual(self.bridge.stats.rx_frames, 0)

    def test_hello_is_answered_with_hello_ack_and_teaches_the_bridge_the_node_id(self):
        self.assertIsNone(self.bridge.peer_node_id)
        self.join()
        self.assertEqual(self.bridge.peer_node_id, NODE_ID)
        self.assertEqual(self.bridge.peer_boot_epoch, self.node.boot_epoch)
        # The death window the node announced, not one the host assumed.
        self.assertEqual(self.bridge.peer_hb_period_ms, 100)
        self.assertEqual(self.bridge.peer_hb_miss_limit, 6)

    def test_a_read_before_hello_raises_rather_than_guessing_a_destination(self):
        with self.assertRaises(BridgeError):
            self.bridge.read("potluck://lab/node-1001/sys/uptime", timeout=0.01)


class TestRead(BridgeFixture):
    def setUp(self) -> None:
        super().setUp()
        self.join()

    def test_a_read_returns_the_whole_section_4_tuple(self):
        from potluck.ns_payloads import Read

        path = sys_paths_for(NODE_ID)[3]  # sys/boot-epoch
        rep = self.request(fr.Op.READ, Read(path_hash=path_hash(path)).encode(), path_hash(path))
        self.assertTrue(rep.ok)
        self.assertEqual(rep.reply_to, REPLY_TO_READ)

        r = rep.reading()
        # Every element present, none of them optional.
        self.assertEqual(r.quality, int(Quality.GOOD))
        self.assertEqual(r.unit, int(Unit.COUNT))
        self.assertGreaterEqual(r.timestamp_ms, 0)
        self.assertGreaterEqual(r.age_ms, 0)
        self.assertEqual(r.latency_class, 3)
        number, quality = r.number()
        self.assertEqual(number, self.node.boot_epoch)
        self.assertEqual(quality, int(Quality.GOOD))

    def test_a_reading_never_converts_to_a_bare_number(self):
        from potluck.ns_payloads import Read

        path = sys_paths_for(NODE_ID)[0]
        rep = self.request(fr.Op.READ, Read(path_hash=path_hash(path)).encode(), path_hash(path))
        r = rep.reading()
        # This is section 4 rule 2 enforced in the type system rather than in a review comment.
        with self.assertRaises(TypeError):
            float(r)
        self.assertNotIn("value", [n for n in dir(r) if n == "value_only"])

    def test_an_unknown_path_is_not_found_and_carries_no_value(self):
        from potluck.ns_payloads import Read

        h = path_hash("potluck://lab/node-1001/does/not/exist")
        rep = self.request(fr.Op.READ, Read(path_hash=h).encode(), h)
        self.assertFalse(rep.ok)
        self.assertEqual(rep.status, int(NsError.NOT_FOUND))
        r = rep.reading()
        self.assertFalse(r.usable())
        number, quality = r.number()
        self.assertIsNone(number)
        self.assertEqual(quality, int(Quality.UNAVAILABLE))

    def test_the_formatted_reading_always_names_the_age(self):
        from potluck.ns_payloads import Read

        for path in sys_paths_for(NODE_ID):
            h = path_hash(path)
            rep = self.request(fr.Op.READ, Read(path_hash=h).encode(), h)
            text = rep.reading().format()
            self.assertIn("age", text, f"{path} formatted without an age: {text}")

    def test_every_builtin_resource_answers(self):
        from potluck.ns_payloads import Read

        for path in sys_paths_for(NODE_ID):
            h = path_hash(path)
            rep = self.request(fr.Op.READ, Read(path_hash=h).encode(), h)
            self.assertTrue(rep.ok, f"{path} -> {NsError.name_of(rep.status)}")
            self.assertEqual(rep.path_hash, h)


class TestWrite(BridgeFixture):
    def setUp(self) -> None:
        super().setUp()
        self.join()

    def test_a_write_to_a_writable_path_takes_effect_and_reads_back(self):
        from potluck.ns_payloads import Read, Write

        h = path_hash(self.node.writable_path)
        rep = self.request(fr.Op.WRITE, Write(path_hash=h, value=Value.of_f32(3.25)).encode(), h)
        self.assertTrue(rep.ok, NsError.name_of(rep.status))
        self.assertEqual(rep.reply_to, REPLY_TO_WRITE)

        back = self.request(fr.Op.READ, Read(path_hash=h).encode(), h)
        number, quality = back.reading().number()
        self.assertAlmostEqual(number, 3.25, places=5)
        self.assertEqual(quality, int(Quality.GOOD))

    def test_a_write_to_a_read_only_path_is_refused_by_name(self):
        from potluck.ns_payloads import Write

        h = path_hash(sys_paths_for(NODE_ID)[0])  # sys/heap-free, read-only to the cluster
        rep = self.request(fr.Op.WRITE, Write(path_hash=h, value=Value.of_u32(1)).encode(), h)
        self.assertFalse(rep.ok)
        # NOT_WRITABLE, not just "failed": a refusal that does not say why cannot tell a manifest
        # error from a permissions error.
        self.assertEqual(rep.status, int(NsError.NOT_WRITABLE))
        self.assertEqual(self.node.writes_rejected, 1)

    def test_a_write_of_the_wrong_type_is_refused_as_a_type_mismatch(self):
        from potluck.ns_payloads import Write

        h = path_hash(self.node.writable_path)  # declared F32
        rep = self.request(fr.Op.WRITE, Write(path_hash=h, value=Value.of_u32(7)).encode(), h)
        self.assertFalse(rep.ok)
        self.assertEqual(rep.status, int(NsError.TYPE_MISMATCH))

    def test_a_write_is_always_answered_even_when_refused(self):
        from potluck.ns_payloads import Write

        h = path_hash("potluck://lab/node-1001/nope")
        before = self.bridge.stats.rx_frames
        rep = self.request(fr.Op.WRITE, Write(path_hash=h, value=Value.of_u32(1)).encode(), h)
        self.assertGreater(self.bridge.stats.rx_frames, before)
        self.assertEqual(rep.status, int(NsError.NOT_FOUND))


class TestCorrelation(BridgeFixture):
    def setUp(self) -> None:
        super().setUp()
        self.join()

    def test_a_timeout_raises_rather_than_returning_an_empty_reading(self):
        # Nothing pumps the node, so no reply can arrive. A Reading fabricated here would be the
        # unmarked value section 4 rule 2 exists to forbid.
        with self.assertRaises(RequestTimeout):
            self.bridge.read(sys_paths_for(NODE_ID)[0], timeout=0.05)
        self.assertEqual(self.bridge.stats.timeouts, 1)

    def test_a_reply_to_an_abandoned_request_is_counted_not_dropped(self):
        with self.assertRaises(RequestTimeout):
            self.bridge.read(sys_paths_for(NODE_ID)[0], timeout=0.01)
        # Now let the node answer the request the host already gave up on.
        self.exchange()
        self.assertEqual(self.bridge.stats.replies_unmatched, 1)

    def test_msg_id_is_never_zero(self):
        # msg_id 0 means "no correlation" to the firmware, so a request using it could never be
        # matched to its reply. 65535 allocations walks the wrap.
        seen = {self.bridge._next_msg_id() for _ in range(70000)}
        self.assertNotIn(0, seen)

    def test_replies_are_matched_to_their_own_request(self):
        from potluck.ns_payloads import Read

        a = path_hash(sys_paths_for(NODE_ID)[0])
        b = path_hash(sys_paths_for(NODE_ID)[3])
        rep_a = self.request(fr.Op.READ, Read(path_hash=a).encode(), a)
        rep_b = self.request(fr.Op.READ, Read(path_hash=b).encode(), b)
        self.assertEqual(rep_a.path_hash, a)
        self.assertEqual(rep_b.path_hash, b)
        self.assertEqual(self.bridge.stats.replies_matched, 2)


class TestThreaded(unittest.TestCase):
    """The same thing with both sides on real threads, which is how it runs in anger."""

    def test_a_full_session_over_threads(self):
        host_side, node_side = LoopbackTransport.pair()
        node = FakeNode(node_side, node_id=NODE_ID)
        node.start()
        bridge = Bridge(host_side, heartbeat=True, hb_period_ms=20)
        bridge.start()
        try:
            ack = bridge.hello(timeout=3.0)
            self.assertIsNotNone(ack, "the node did not answer HELLO")
            self.assertEqual(ack.node_id, NODE_ID)

            r = bridge.read(sys_paths_for(NODE_ID)[3], timeout=3.0)
            number, quality = r.number()
            self.assertEqual(number, node.boot_epoch)
            self.assertEqual(quality, int(Quality.GOOD))

            rep = bridge.write(node.writable_path, Value.of_f32(-1.5), timeout=3.0)
            self.assertTrue(rep.ok, NsError.name_of(rep.status))

            # read_many keeps a failure beside the path that produced it.
            out = bridge.read_many(list(sys_paths_for(NODE_ID)) + ["potluck://lab/nope"], timeout=3.0)
            self.assertEqual(len(out), 7)
            self.assertFalse(out["potluck://lab/nope"].usable())

            # The host heartbeats, so the node counts it as a live peer.
            deadline = time.time() + 3.0
            while time.time() < deadline and node.heartbeats_seen == 0:
                time.sleep(0.02)
            self.assertGreater(node.heartbeats_seen, 0, "the bridge never heartbeated")

            bridge.bye()
            time.sleep(0.2)
        finally:
            bridge.close()
            node.stop()


if __name__ == "__main__":
    unittest.main(verbosity=2)
