"""Section 15 question 2 (2026-10-15): `potluck apply --plan` counts every board's namespace entries -- built-ins,
the package's outputs, its own (one per link for a `{peer}` path) and the replicas its role reads -- against the 128
a board holds, and refuses a package that would not fit."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import apply as ap  # noqa: E402
from potluck import modules as mods  # noqa: E402
from potluck.manifest import load  # noqa: E402


def manifest(boards: list[int], actors: list[dict]) -> object:
    doc = {"schema": 1, "system": "lab", "min_core_version": 1,
           "nodes": [{"node_id": n, "label": f"b{n:04x}"} for n in boards], "actors": actors}
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "m.json")
        with open(p, "w", encoding="utf-8") as f:
            json.dump(doc, f)
        return load(p)


class Budget(unittest.TestCase):
    def setUp(self) -> None:
        self.saved = dict(mods._REGISTRY)
        mods._REGISTRY.clear()
        mods._REGISTRY["ext:t/link"] = mods.ModuleSpec("ext:t/link", 0xF1, "every", (), "t",
                                                        outputs=("{node}/link/count", "{node}/link/{peer}/rssi"))
        mods._REGISTRY["ext:t/quiet"] = mods.ModuleSpec("ext:t/quiet", 0xF2, "every", (), "t")

    def tearDown(self) -> None:
        mods._REGISTRY.clear()
        mods._REGISTRY.update(self.saved)

    def test_three_boards_count_built_ins_the_package_their_own_and_the_cabled_boards_replicas(self) -> None:
        boards = [0x6300, 0x7368, 0x8160]
        m = manifest(boards, [
            {"name": "temp", "module": "builtin:die_temp", "latency_class": 4, "pin": 0x7368},
            {"name": "tick", "module": "builtin:ticker", "latency_class": 3},
            {"name": "link", "module": "ext:t/link", "latency_class": 4},
        ])
        b, silent = ap.ns_budget(m, boards, cabled=0x6300)
        self.assertEqual(silent, [])
        for n in boards:
            self.assertEqual((b[n]["fixed"], b[n]["package"], b[n]["own"]), (7, 2, 1 + 2))  # count + 2 links
        self.assertEqual(b[0x7368]["replicas"], 0)
        self.assertEqual(b[0x6300]["replicas"], 2)  # the other two boards' link/count, read through the cable --
        self.assertEqual(b[0x6300]["total"], 7 + 2 + 3 + 2)  # not their {peer} outputs, which neither verify nor hub reads

    def test_an_output_published_only_sometimes_is_counted_but_never_waited_for(self) -> None:
        # M20: a position's x..conf exist only while someone is placed ("?" in the descriptor)
        mods._REGISTRY["ext:t/fuse"] = mods.ModuleSpec("ext:t/fuse", 0xF3, "portable", (), "t",
                                                        outputs=("act/{actor}/state", "?act/{actor}/x"))
        boards = [0x6300, 0x7368]
        m = manifest(boards, [{"name": "fuse", "module": "ext:t/fuse", "latency_class": 4}])
        _, _, reads = ap.expected(m, boards)
        self.assertIn("potluck://lab/act/fuse/state", reads)
        self.assertNotIn("potluck://lab/act/fuse/x", reads)
        b, _ = ap.ns_budget(m, boards, cabled=0x6300)
        self.assertEqual(b[0x7368]["package"], 2)  # both declared on every board

    def test_twenty_boards_with_per_link_outputs_do_not_fit(self) -> None:
        boards = [0x1000 + i for i in range(20)]
        mods._REGISTRY["ext:t/link"] = mods.ModuleSpec("ext:t/link", 0xF1, "every", (), "t", outputs=tuple(
            ["{node}/link/count"] + [f"{{node}}/link/{{peer}}/m{i}" for i in range(6)]))
        m = manifest(boards, [{"name": "link", "module": "ext:t/link", "latency_class": 4}])
        b, _ = ap.ns_budget(m, boards)
        self.assertEqual(b[0x1000]["total"], 7 + 1 + 6 * 19)  # 122: fits, just
        mods._REGISTRY["ext:t/link"] = mods.ModuleSpec("ext:t/link", 0xF1, "every", (), "t", outputs=tuple(
            ["{node}/link/count"] + [f"{{node}}/link/{{peer}}/m{i}" for i in range(7)]))
        b, _ = ap.ns_budget(m, boards)
        self.assertGreater(b[0x1000]["total"], ap.NS_CAP)  # 141: refused by the plan

    def test_a_presenter_holds_the_outside_paths_and_a_silent_module_is_named(self) -> None:
        boards = [0x6300, 0x7368]
        m = manifest(boards, [{"name": "q", "module": "ext:t/quiet", "latency_class": 4}])
        b, silent = ap.ns_budget(m, boards, presenters=(0x7368,), outside=("node-6300/sys/uptime",))
        self.assertEqual(silent, ["ext:t/quiet"])
        self.assertEqual(b[0x7368]["replicas"], 1)  # A's uptime, which B presents
        self.assertEqual(b[0x6300]["replicas"], 0)  # its own built-in already
        mods._REGISTRY["ext:t/quiet"] = mods.ModuleSpec("ext:t/quiet", 0xF2, "every", (), "t", outputs_declared=True)
        self.assertEqual(ap.ns_budget(m, boards)[1], [])  # "outputs": [] -- publishes nothing, not uncounted


if __name__ == "__main__":
    unittest.main()
