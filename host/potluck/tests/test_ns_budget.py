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

    def test_a_module_with_nearest_peers_is_counted_for_its_set_and_positions_cost_only_their_board(self) -> None:
        # M20 / D20: twenty boards, four rooms of five, "peers": {"nearest": 4} -- flat in cell size
        from potluck import deploy as dp
        mods._REGISTRY["ext:t/near"] = mods.ModuleSpec("ext:t/near", 0xF4, "every", (), "t",
                                                        outputs=("{node}/rf/evidence", "{node}/link/{peer}/mean"),
                                                        peers_nearest=4)
        boards = [0x1000 + i for i in range(20)]
        actors = [{"name": "link", "module": "ext:t/near", "latency_class": 4}]
        for i, n in enumerate(boards):
            near = [q for q in boards if q != n and (q - 0x1000) // 5 == i // 5][:4]
            actors.append({"name": f"pos_{n:04x}", "module": "builtin:position", "latency_class": 4, "pin": n,
                           "config": {"x": float(i), "y": 0.0, "room": i // 5 + 1,
                                      "near": ",".join(f"{q:04x}" for q in near)}})
        b, _ = ap.ns_budget(manifest(boards, actors), boards, cabled=boards[0])
        n = boards[7]
        self.assertEqual(b[n]["own"], 1 + 4 + 4)  # evidence, 4 links, and its own x, y, room, near
        self.assertEqual(b[n]["package"], 0)      # no board holds another's position at boot
        self.assertLess(b[boards[0]]["total"], 128)
        self.assertIn(dp.near_path("lab", n), {p for p in ap.expected(manifest(boards, actors), boards)[2]})

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

    def test_in_a_tree_each_board_counts_its_cells_and_a_foreman_both(self) -> None:
        # M19 (Review of M19 #3): two cells of three, a die temperature on every board, a foreman each -- a member
        # counts its own cell's three temperatures and its foreman's summary; a board that may hold the role also
        # the other cell's summary, which it reads in the upper cell; per-link outputs only toward its own cell
        boards = [0x100, 0x101, 0x102, 0x200, 0x201, 0x202]
        actors = [{"name": "link", "module": "ext:t/link", "latency_class": 4}]
        for n in boards:
            cell = 1 if n < 0x200 else 2
            actors.append({"name": f"t_{n:04x}", "module": "builtin:die_temp", "latency_class": 4, "pin": n})
            actors.append({"name": f"pos_{n:04x}", "module": "builtin:position", "latency_class": 4, "pin": n,
                           "config": {"cells": str(cell)}})
        for cell, base in ((1, 0x100), (2, 0x200)):
            actors.append({"name": f"foreman_c{cell}", "module": "builtin:foreman", "latency_class": 4,
                           "nodes": [base, base + 1],
                           "config": {"cell": cell, "cell_name": f"c{cell}", "parent": 3, "members":
                                      ",".join(f"{base + i:04x}" for i in range(3)), "export:warm": "max|hw/t|none|f32"}})
        b, _ = ap.ns_budget(manifest(boards, actors), boards, cabled=0x102)
        self.assertEqual(b[0x102]["package"], 3 + 2)      # its cell's three temperatures, cell 1's alive and warm
        self.assertEqual(b[0x100]["package"], 3 + 2 + 2)  # ... and cell 2's summary
        self.assertEqual(b[0x102]["own"], 1 + 2)          # link/count and a link to each cell-mate, not to cell 2
        self.assertEqual(b[0x102]["replicas"], 2)         # the cabled board reads its cell-mates' link/count
