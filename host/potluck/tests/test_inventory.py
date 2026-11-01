"""2026-11-01: a bridged node declares its own values -- TELEMETRY section 8, kept by the hub beside the cluster file,
made into inboxes by apply (telemetry.inventory, apply.bridge_inventory / inventory_fragment, Model._pull_bridged_inventory)."""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace as NS

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import apply as ap  # noqa: E402
from potluck import telemetry as tm  # noqa: E402

LINES = ["tablet_sense magnetic_ut:f32 steps:i32:count heading:f32:degree",
         "tablet_place latitude:f32:degree longitude:f32:degree location_accuracy:f32:metre"]


class FakeNode:
    """Answers section 8 as the tablet does: one line per request, `start` its index."""

    def __init__(self, lines, node=0x00FD):
        self.lines, self.node, self.asked = lines, node, 0

    def deploy_step(self, op, payload, timeout=2.0):
        self.asked += 1
        target, section, start = struct.unpack_from("<HBB", payload, 0)
        if section != tm.INVENTORY:
            return struct.pack("<BBH", 1, 0, target)
        text = self.lines[start].encode("ascii")
        return struct.pack("<BBHBBB", 1, tm.INVENTORY, self.node, len(self.lines), start, len(text)) + text


class Section(unittest.TestCase):
    def test_every_line_is_asked_for_in_turn(self) -> None:
        self.assertEqual(tm.inventory(FakeNode(LINES), 0x00FD), LINES)

    def test_a_node_without_the_section_says_so(self) -> None:
        class Old:  # a board, or a tablet before it: section 0
            def deploy_step(self, op, payload, timeout=2.0):
                return struct.pack("<BBH", 1, 0, 0x6300)
        with self.assertRaises(tm.TelemetryError):
            tm.inventory(Old(), 0x6300)

    def test_a_cut_line_is_refused(self) -> None:
        raw = struct.pack("<BBHBBB", 1, tm.INVENTORY, 0x00FD, 1, 0, 40) + b"short"
        with self.assertRaises(tm.TelemetryError):
            tm.parse_inventory(raw)


class Cluster(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.path = Path(self.tmp.name) / "home.json"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_the_entry_says_inventory_and_how_long_a_value_holds(self) -> None:
        br = {"nodes": [{"node": "00fd", "label": "Tablet", "inventory": True, "stale_ms": 20000},
                        {"node": "00fc", "label": "Phone"}]}
        self.assertEqual(ap.check_bridge(br, [(0x6300, "A")])[0], {0x00FD: "Tablet", 0x00FC: "Phone"})
        self.assertEqual(ap.bridge_inventory(br), {0x00FD: 20000})
        self.assertEqual(ap.bridge_inventory({"nodes": [{"node": "00fd", "label": "T", "inventory": True}]}),
                         {0x00FD: ap.INVENTORY_STALE_MS})
        for bad in ({"inventory": "yes"}, {"inventory": True, "stale_ms": 5}):
            with self.subTest(bad=bad), self.assertRaises(ap.ApplyError):
                ap.bridge_inventory({"nodes": [dict({"node": "00fd", "label": "T"}, **bad)]})

    def test_each_line_becomes_an_inbox_pinned_to_its_node(self) -> None:
        c = NS(path=self.path, system="lab", bridged={0x00FD: "Tablet"}, bridge_inventory={0x00FD: 15000})
        log: list[str] = []
        self.assertIsNone(ap.inventory_fragment(c, log.append))  # nothing read yet: said, not refused
        self.assertIn("no inventory from node 00fd yet", log[0])
        ap.inventory_file(self.path, 0x00FD).write_text(json.dumps({"node": "00fd", "lines": LINES}), encoding="utf-8")
        frag = ap.inventory_fragment(c, log.append)
        self.assertEqual(frag["nodes"], [{"node_id": 0x00FD, "label": "Tablet", "kind": "host"}])
        self.assertEqual([(a["name"], a["pin"], a["config"]) for a in frag["actors"]],
                         [("tablet_sense", 0x00FD, {"outputs": "magnetic_ut:f32 steps:i32:count heading:f32:degree",
                                                    "stale_ms": 15000}),
                          ("tablet_place", 0x00FD, {"outputs": "latitude:f32:degree longitude:f32:degree "
                                                               "location_accuracy:f32:metre", "stale_ms": 15000})])
        from potluck import manifest as mf
        from potluck import deploy as dp
        dp.compile_image(mf.parse(dict(frag, nodes=[{"node_id": 0x6300, "label": "A"}] + frag["nodes"])), 1, ".")


class HubKeepsIt(unittest.TestCase):
    def test_the_lines_are_kept_beside_the_cluster_file_and_asked_for_now_and_then(self) -> None:
        from potluck import hub as hb
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "home.json"
            model = hb.Model(NS(record=None), cluster=NS(path=str(path), boards=[(0x6300, "A")], system="lab",
                                                         bridge_inventory={0x00FD: 15000}))
            node = FakeNode(LINES)
            model.hub = node
            model._pull_bridged_inventory(tm)
            kept = json.loads(ap.inventory_file(path, 0x00FD).read_text(encoding="utf-8"))
            self.assertEqual(kept["lines"], LINES)
            asked = node.asked
            model._pull_bridged_inventory(tm)  # within INVENTORY_EVERY_S: not asked again
            self.assertEqual(node.asked, asked)
            self.assertTrue(any(e["kind"] == "bridged_inventory" for e in model.events))


if __name__ == "__main__":
    unittest.main()
