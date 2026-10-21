"""2026-10-20: an inbox -- named values a program outside the cell writes through the hub, held by one board
(pot/inbox.hpp, potluck.deploy's builtin:inbox)."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import apply as ap  # noqa: E402
from potluck import deploy as dp  # noqa: E402
from potluck.manifest import ManifestErrors, parse  # noqa: E402

#: tests/test_inbox.cpp's kGolden: these bytes parse on the board.
GOLDEN = "504f54440101000007000000058a82bf9a634aa21b00000000630e1788130000030834f8bf045a76714e6603008d0bfdbc0100"


def doc(config: dict, pin: int | None = 0x6300, outside: dict | None = None) -> dict:
    actor = {"name": "phone", "module": "builtin:inbox", "latency_class": 4, "config": config}
    if pin is not None:
        actor["pin"] = pin
    d = {"schema": 1, "system": "lab", "min_core_version": 1,
         "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": 0x7368, "label": "B"}], "actors": [actor]}
    if outside:
        d["outside"] = outside
    return d


PHONE = {"outputs": "heart_rate:f32:hertz steps:u32 moving:bool", "stale_ms": 5000}


class Inbox(unittest.TestCase):
    def test_it_compiles_to_the_bytes_the_board_parses(self) -> None:
        self.assertEqual(dp.compile_image(parse(doc(PHONE)), 7).hex(), GOLDEN)

    def test_its_outputs_are_under_its_actor_and_counted_as_its_own(self) -> None:
        m = parse(doc(PHONE))
        placement, reads = ap.actor_reads(m, m.actors[0], [0x6300, 0x7368])
        self.assertEqual(placement, "pinned")
        self.assertEqual(reads, ["potluck://lab/act/phone/heart_rate", "potluck://lab/act/phone/steps",
                                 "potluck://lab/act/phone/moving"])

    def test_the_verify_does_not_wait_for_the_program(self) -> None:
        _pinned, _portable, reads = ap.expected(parse(doc(PHONE)), [0x6300, 0x7368])
        self.assertFalse([r for r in reads if "/act/phone/" in r])  # NO_DATA until written: no failed apply

    def test_the_hub_reads_its_values_though_the_verify_does_not_wait_for_them(self) -> None:
        """2026-10-21: the hub reads what expected() lists; skipping inbox values there for everyone hid the phone's
        values from the hub's model and made every write to them answer 404."""
        from types import SimpleNamespace as NS
        from potluck import hub as hb
        m = parse(doc(PHONE))
        model = hb.Model(NS(record=None), package=NS(manifest=m))
        model.board_ids = lambda: [0x6300, 0x7368]
        self.assertIn("potluck://lab/act/phone/steps", model.resource_paths())
        self.assertNotIn("potluck://lab/act/phone/steps", ap.expected(m, [0x6300, 0x7368])[2])

    def test_a_program_may_be_let_write_it_from_outside(self) -> None:
        m = parse(doc(PHONE, outside={"potluck://lab/act/phone/heart_rate": "read_write",
                                        "potluck://lab/act/phone/steps": "read"}))
        img = dp.compile_image(m, 8)
        self.assertGreater(len(img), len(bytes.fromhex(GOLDEN)))  # the outside section rides along

    def test_a_bad_inbox_is_refused_with_the_reason(self) -> None:
        for cfg, pin, why in ((PHONE, None, "pin it"),
                              ({"outputs": "a:bytes"}, 0x6300, "type"),
                              ({"outputs": "a:f32:furlongs"}, 0x6300, "unit"),
                              ({"outputs": "a:f32 a:u32"}, 0x6300, "two outputs"),
                              ({"outputs": " ".join(f"o{i}:u32" for i in range(13))}, 0x6300, "at most 12"),
                              ({"outputs": "a:f32", "colour": 1}, 0x6300, "unknown"),
                              ({}, 0x6300, "outputs")):
            with self.assertRaises((dp.DeployError, ManifestErrors), msg=why) as cm:
                dp.compile_image(parse(doc(cfg, pin)), 9)
            self.assertIn(why, str(cm.exception))


if __name__ == "__main__":
    unittest.main()
