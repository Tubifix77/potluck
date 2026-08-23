"""The host's namespace payloads against the firmware's own bytes.

Reads tests/fixtures/ns_corpus.jsonl, written by the C++ suite
(`pot_tests --emit-ns-corpus`), and requires this decoder to agree byte for byte.

Why this and not just the layout asserts on each side: swap `quality` and `latency_class` in *both*
implementations and every self-consistency test on both sides still passes, while a real fleet
reports the wrong staleness for ever. Only a corpus one side produced and the other consumed catches
that -- and section 14 asks for the wire format to be "specified and versioned independently of the
implementation", which two agreeing implementations is the practical form of.
"""

from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck.ns_payloads import (
    REPLY_SIZE,
    READ_SIZE,
    WRITE_SIZE,
    Read,
    Reply,
    Write,
)
from potluck.sys_paths import sys_paths_for
from potluck.paths import path_hash
from potluck.value import Value

FIXTURE = Path(__file__).parent / "fixtures" / "ns_corpus.jsonl"


def load() -> list[dict]:
    if not FIXTURE.exists():
        raise unittest.SkipTest(
            f"{FIXTURE} is missing; run tools\\run_all_tests.ps1 to regenerate the fixtures"
        )
    out = []
    with FIXTURE.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


class TestNsCorpus(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.corpus = load()

    def test_the_corpus_covers_all_three_opcodes(self):
        """A corpus that shrank silently would make this whole file pass while testing nothing.

        Exact counts rather than a round-number floor. The emitter's structure is:
          reads   4 synthetic hashes + 6 built-ins of node 0x1a2b
          writes  13 values, one per type plus the sign and width boundaries
          replies 5 qualities x 6 errors x {READ, WRITE}, plus 6 units = 66
        If the emitter changes, these numbers change with it deliberately -- which is the point.
        """
        counts: dict[str, int] = {}
        for e in self.corpus:
            counts[e["op"]] = counts.get(e["op"], 0) + 1
        self.assertEqual(counts, {"read": 10, "write": 13, "reply": 66, "call": 5},
                         f"the corpus changed shape: {counts}")

    def test_python_decodes_every_read_the_firmware_encoded(self):
        n = 0
        for e in (x for x in self.corpus if x["op"] == "read"):
            raw = bytes.fromhex(e["bytes"])
            self.assertEqual(len(raw), READ_SIZE)
            r = Read.parse(raw)
            self.assertEqual(r.path_hash, e["path_hash"])
            self.assertEqual(r.flags, e["flags"])
            n += 1
        self.assertGreater(n, 0)

    def test_python_re_encodes_every_read_to_the_same_bytes(self):
        for e in (x for x in self.corpus if x["op"] == "read"):
            raw = bytes.fromhex(e["bytes"])
            self.assertEqual(Read.parse(raw).encode().hex(), e["bytes"])

    def test_python_decodes_and_re_encodes_every_write(self):
        for e in (x for x in self.corpus if x["op"] == "write"):
            raw = bytes.fromhex(e["bytes"])
            self.assertEqual(len(raw), WRITE_SIZE)
            w = Write.parse(raw)
            self.assertEqual(w.path_hash, e["path_hash"])
            self.assertEqual(w.value.type, e["value_type"])
            self.assertEqual(w.value.len, e["value_len"])
            # Re-encoding must reproduce the firmware's bytes, including the zero padding in the
            # unused tail of value_raw -- a decoder that dropped the padding would still round-trip
            # against itself.
            self.assertEqual(w.encode().hex(), e["bytes"])

    def test_python_decodes_and_re_encodes_every_reply(self):
        for e in (x for x in self.corpus if x["op"] == "reply"):
            raw = bytes.fromhex(e["bytes"])
            self.assertEqual(len(raw), REPLY_SIZE)
            p = Reply.parse(raw)
            for field in ("path_hash", "timestamp_ms", "age_ms", "unit", "reply_to", "status",
                          "quality", "latency_class"):
                self.assertEqual(getattr(p, field), e[field],
                                 f"{field} disagrees on {e['bytes']}")
            self.assertEqual(p.value.type, e["value_type"])
            self.assertEqual(p.value.len, e["value_len"])
            self.assertEqual(p.encode().hex(), e["bytes"])

    def test_python_decodes_and_re_encodes_every_call(self):
        from potluck.ns_payloads import CALL_SIZE, Call

        n = 0
        for e in (x for x in self.corpus if x["op"] == "call"):
            raw = bytes.fromhex(e["bytes"])
            self.assertEqual(len(raw), CALL_SIZE + e["arg_len"])
            c = Call.parse(raw)
            self.assertEqual(c.path_hash, e["path_hash"])
            self.assertEqual(len(c.args), e["arg_len"])
            self.assertEqual(c.encode().hex(), e["bytes"])
            n += 1
        self.assertGreater(n, 0, "no CALL cases in the corpus")

    def test_a_call_declaring_more_args_than_it_carries_is_refused(self):
        """The bounds check, from the host side. The firmware has the mirror of this."""
        from potluck.ns_payloads import Call, ShortNsPayload

        # header claiming 100 argument bytes inside a 12-byte payload
        hostile = Call(path_hash=1, args=b"x" * 100).encode()[:12]
        with self.assertRaises(ShortNsPayload):
            Call.parse(hostile)

        # and a truncated header is refused too
        with self.assertRaises(ShortNsPayload):
            Call.parse(b"\x00" * 7)

    def test_the_call_corpus_includes_the_v1_ceiling(self):
        """An off-by-one in either side's length arithmetic shows up only at the boundary."""
        lens = {e["arg_len"] for e in self.corpus if e["op"] == "call"}
        self.assertIn(0, lens, "no zero-argument call")
        self.assertIn(218, lens, "no call at the v1 argument ceiling (226 - 8)")

    def test_the_reply_field_order_is_not_symmetric(self):
        """A guard against the failure this whole file exists for.

        quality (offset 16) and latency_class (offset 17) are adjacent single bytes. If the corpus
        never contained a case where they differ, swapping them on both sides would go unnoticed --
        so assert that such a case is present.
        """
        found = False
        for e in (x for x in self.corpus if x["op"] == "reply"):
            if e["quality"] != e["latency_class"]:
                found = True
                break
        self.assertTrue(found, "the corpus never distinguishes quality from latency_class")

    def test_reply_to_distinguishes_read_from_write(self):
        tos = {e["reply_to"] for e in self.corpus if e["op"] == "reply"}
        self.assertEqual(tos, {0x10, 0x11})

    def test_every_quality_and_every_error_appears(self):
        qualities = {e["quality"] for e in self.corpus if e["op"] == "reply"}
        statuses = {e["status"] for e in self.corpus if e["op"] == "reply"}
        self.assertEqual(qualities, {0, 1, 2, 3, 4})
        self.assertGreaterEqual(len(statuses), 5)

    def test_the_builtin_path_hashes_agree_with_the_firmwares(self):
        """The firmware emitted READs for node 0x1a2b's six built-ins; this recomputes them.

        This is the one place the host's FNV-1a and its path spelling are checked against the
        firmware's at the same time. Either being wrong alone would leave a fleet whose paths the
        tools cannot name.
        """
        expected = {path_hash(p) for p in sys_paths_for(0x1A2B)}
        seen = {e["path_hash"] for e in self.corpus if e["op"] == "read"}
        missing = expected - seen
        self.assertEqual(missing, set(),
                         f"{len(missing)} built-in hashes the firmware emitted do not match")

    def test_value_round_trip_preserves_the_exact_bits(self):
        """Every value type in the corpus survives decode -> Value -> encode unchanged.

        Bit-exact rather than approximately equal: a float that survives to within an epsilon has
        still lost something, and the wire is bytes.
        """
        for e in self.corpus:
            if e["op"] not in ("write", "reply"):
                continue
            raw = bytes.fromhex(e["bytes"])
            obj = Write.parse(raw) if e["op"] == "write" else Reply.parse(raw)
            v = obj.value
            again = Value.from_wire(*v.to_wire()[:2], v.to_wire()[2])
            self.assertEqual(again.type, v.type)
            self.assertEqual(again.raw, v.raw)


if __name__ == "__main__":
    unittest.main(verbosity=2)
