"""Section 13-M2's acceptance test, as a repeatable gate.

    "a captured 10-minute session replays and produces byte-identical namespace state"

A ten-minute wall-clock run is not a unit test, but the *mechanism* it exercises is, and the mechanism
is the part that can be wrong. These tests capture a session, replay the capture, and require the
namespace-state digest to match byte for byte.

Why a digest rather than comparing objects: "byte-identical" has to mean something a machine can check
across two processes. `Monitor.namespace_canonical()` fixes the entry order and the field order, and
excludes every host-side quantity — host timestamps, wall clock, arrival order — because a digest that
moved when the host was busier would test nothing at all.

Two capture shapes are covered, because section 7.6's format carries both and they arrive by different
routes:

  * console captures, holding the `ns` statistics records a node prints;
  * frame captures, holding raw frames as potctl tees them, where namespace state has to be recovered
    by decoding REPLY payloads.

The second is the one that used to be silently broken: replay counted frames and discarded them, so a
potctl capture replayed to an empty namespace and the acceptance test would have passed by comparing
nothing to nothing. `test_a_frame_capture_does_not_replay_to_an_empty_namespace` exists to keep that
from coming back.
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import frame as fr
from potluck.bridge import Bridge
from potluck.capture import CaptureWriter, read_capture
from potluck.fake_node import FakeNode
from potluck.live import Monitor
from potluck.records import RecordStream
from potluck.sys_paths import sys_paths_for
from potluck.transport import LoopbackTransport
from potluck.value import Value

NODE_ID = 0x1001


def replay_capture(path: Path) -> Monitor:
    """Reconstruct a Monitor from a capture, the way __main__.replay does."""
    from potluck.__main__ import replay_frame
    from potluck.records import KNOWN_KINDS

    monitor = Monitor()
    for obj in read_capture(path):
        kind = obj.get("rec")
        if kind == "frame":
            replay_frame(monitor, obj)
        elif kind in KNOWN_KINDS:
            data = {k: v for k, v in obj.items() if k not in ("rec", "host_ts")}
            monitor.on_record(kind, data, host_ts=obj.get("host_ts"))
    return monitor


class TestFrameCaptureReplay(unittest.TestCase):
    """A potctl-style session: the bridge reads a node, teeing every frame to a capture."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="potluck-replay-")
        self.path = Path(self.tmp) / "session.jsonl"
        self.host_side, self.node_side = LoopbackTransport.pair()
        self.node = FakeNode(self.node_side, node_id=NODE_ID)
        self.node.start()
        self.writer = CaptureWriter(str(self.path))
        self.live = Monitor()
        self.bridge = Bridge(
            self.host_side,
            capture=self.writer,
            heartbeat=False,
            on_frame=self._on_frame,
        )
        self.bridge.start()

    def tearDown(self) -> None:
        self.bridge.close()
        self.node.stop()
        self.writer.close()

    def _on_frame(self, f: fr.Frame, direction: str) -> None:
        # The live side folds REPLYs in exactly as __main__ does.
        if direction == "rx" and f.opcode == fr.Op.REPLY:
            self.live.on_reply_frame(f.src, f.payload)

    def _session(self) -> None:
        self.assertIsNotNone(self.bridge.hello(timeout=3.0), "the node did not answer HELLO")
        for p in sys_paths_for(NODE_ID):
            self.bridge.read(p, timeout=3.0)
        self.bridge.write(self.node.writable_path, Value.of_f32(2.5), timeout=3.0)
        self.bridge.read(self.node.writable_path, timeout=3.0)
        # Let the tee finish writing before the file is read back.
        time.sleep(0.1)

    def test_a_frame_capture_replays_to_a_byte_identical_namespace(self):
        self._session()
        self.writer.close()

        replayed = replay_capture(self.path)
        self.assertEqual(
            replayed.namespace_digest(),
            self.live.namespace_digest(),
            "replayed namespace state differs from the live state:\n"
            f"  live     {self.live.namespace_canonical()!r}\n"
            f"  replayed {replayed.namespace_canonical()!r}",
        )

    def test_a_frame_capture_does_not_replay_to_an_empty_namespace(self):
        # The guard against passing by comparing nothing to nothing.
        self._session()
        self.writer.close()
        replayed = replay_capture(self.path)
        self.assertGreater(len(replayed.ns), 0, "replay recovered no namespace entries at all")
        self.assertEqual(len(replayed.ns), len(self.live.ns))
        self.assertNotEqual(replayed.namespace_digest(), Monitor().namespace_digest(),
                            "the digest of a populated namespace equals an empty one")

    def test_the_digest_is_stable_across_repeated_replays(self):
        self._session()
        self.writer.close()
        first = replay_capture(self.path).namespace_digest()
        second = replay_capture(self.path).namespace_digest()
        self.assertEqual(first, second)

    def test_the_digest_notices_a_changed_value(self):
        """A digest that could not fail would be decoration."""
        self._session()
        self.writer.close()
        good = replay_capture(self.path).namespace_digest()

        # Rewrite one REPLY's value byte in the capture and require the digest to move.
        lines = [json.loads(x) for x in self.path.read_text(encoding="utf-8").splitlines() if x.strip()]
        tampered = 0
        for obj in lines:
            if obj.get("rec") != "frame":
                continue
            raw = bytes.fromhex(obj["raw"])
            try:
                f = fr.parse(raw)
            except fr.FrameError:
                continue
            if f.opcode != fr.Op.REPLY:
                continue
            # REPLY value_raw starts at payload offset 20; flip a bit in it.
            body = bytearray(raw)
            vpos = fr.HEADER_SIZE + 20
            body[vpos] ^= 0xFF
            obj["raw"] = bytes(body).hex()
            tampered += 1
            break
        self.assertEqual(tampered, 1, "no REPLY frame found to tamper with")

        alt = Path(self.tmp) / "tampered.jsonl"
        alt.write_text("\n".join(json.dumps(o) for o in lines) + "\n", encoding="utf-8")
        self.assertNotEqual(replay_capture(alt).namespace_digest(), good)


class TestConsoleCaptureReplay(unittest.TestCase):
    """A potluck-capture session: the node's own `ns` statistics lines, recorded and replayed."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="potluck-replay-ns-")
        self.path = Path(self.tmp) / "console.jsonl"

    def _ns_line(self, node: int, h: int, value, *, quality="GOOD", age=12, updates=3) -> str:
        return json.dumps({
            "t": "ns", "node": node, "hash": h, "owner": node, "kind": "sampled",
            "unit": 101, "class": 4, "bound_ms": 30000, "quality": quality,
            "age_ms": age, "ts": 41230, "updates": updates, "value": value,
        })

    def test_a_console_capture_replays_to_a_byte_identical_namespace(self):
        stream = RecordStream()
        live = Monitor()
        writer = CaptureWriter(str(self.path))

        from potluck.paths import path_hash
        from potluck.records import Record

        lines = []
        for i, p in enumerate(sys_paths_for(NODE_ID)):
            lines.append(self._ns_line(NODE_ID, path_hash(p), 1000 + i))
        # A second sample of one entry, so "last state wins" is exercised rather than "only state".
        lines.append(self._ns_line(NODE_ID, path_hash(sys_paths_for(NODE_ID)[0]), 999,
                                   quality="STALE", age=99_999, updates=4))

        for line in lines:
            item = stream.feed(line)
            self.assertIsInstance(item, Record, f"not parsed as a record: {line}")
            live.on_record(item.kind, item.data)
            writer.write_record(item.kind, item.data)
        writer.close()

        replayed = replay_capture(self.path)
        self.assertEqual(len(replayed.ns), len(sys_paths_for(NODE_ID)))
        self.assertEqual(replayed.namespace_digest(), live.namespace_digest())

        # And the last sample is the one that survived, marked as the node marked it.
        first_hash = path_hash(sys_paths_for(NODE_ID)[0])
        self.assertEqual(replayed.ns[(NODE_ID, first_hash)]["quality"], "STALE")
        self.assertEqual(replayed.ns[(NODE_ID, first_hash)]["value"], 999)

    def test_ns_records_are_not_reported_as_an_unknown_kind(self):
        # This warning firing on a node's own namespace is what teaches people to ignore warnings.
        stream = RecordStream()
        from potluck.paths import path_hash

        stream.feed(self._ns_line(NODE_ID, path_hash(sys_paths_for(NODE_ID)[0]), 1))
        self.assertEqual(stream.stats.unknown_kinds, {})


class TestEnvelopeCollision(unittest.TestCase):
    """A payload field must never be able to impersonate or destroy the capture envelope.

    This is the bug the acceptance work above uncovered: the envelope key used to be `kind`, the payload
    was splatted in after it, and both `ns` (resource kind) and `event` (event kind) carry a field of
    that name. Every such line written to a capture was silently unidentifiable, and a replay of a
    capture full of `ns` records produced an empty namespace — which would have made the M2 acceptance
    test pass by comparing nothing to nothing.
    """

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix="potluck-envelope-")
        self.path = Path(self.tmp) / "hostile.jsonl"

    def test_a_payload_cannot_overwrite_the_record_type(self):
        w = CaptureWriter(str(self.path))
        # A payload that names every envelope key on purpose.
        w.write_record("ns", {"kind": "sampled", "rec": "not-a-record", "host_ts": 0, "node": 1})
        w.close()

        rows = list(read_capture(self.path))
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["rec"], "ns", "the payload overwrote the record type")
        # And the payload's own colliding field survives under its own name, unharmed.
        self.assertEqual(rows[0]["kind"], "sampled")

    def test_an_event_record_keeps_both_its_type_and_its_event_kind(self):
        w = CaptureWriter(str(self.path))
        w.write_record("event", {"kind": "peer_dead", "node": 258, "peer": 259, "at_ms": 601})
        w.close()

        row = next(iter(read_capture(self.path)))
        self.assertEqual(row["rec"], "event")
        self.assertEqual(row["kind"], "peer_dead")

    def test_every_record_type_round_trips_through_a_capture(self):
        w = CaptureWriter(str(self.path))
        for kind in sorted(__import__("potluck.records", fromlist=["x"]).KNOWN_KINDS):
            w.write_record(kind, {"node": 1, "kind": f"payload-{kind}"})
        w.write_log("a log line")
        w.write_frame(node_id=1, direction="rx", raw=b"\x00\x01")
        w.close()

        seen = [r.get("rec") for r in read_capture(self.path)]
        from potluck.records import KNOWN_KINDS

        for kind in KNOWN_KINDS:
            self.assertIn(kind, seen, f"{kind} did not survive a capture round trip")
        self.assertIn("log", seen)
        self.assertIn("frame", seen)


if __name__ == "__main__":
    unittest.main(verbosity=2)
