"""M19/M20: a tree proposed from a measured survey -- and the cluster file it writes passes apply's plan."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import apply as ap  # noqa: E402
from potluck import survey as sv  # noqa: E402


def house(groups: list[list[int]], bridges: list[tuple[int, int]] = ()) -> dict:
    """Every board hears its own group's well; a bridge pair hears each other; everyone else is out of earshot."""
    links = {}
    for g in groups:
        for a in g:
            for b in g:
                if a != b:
                    links[(a, b)] = {"delivery": 1.0, "rssi": -55}
    for a, b in bridges:
        links[(a, b)] = {"delivery": 0.98, "rssi": -78}
    return links


class Propose(unittest.TestCase):
    def test_a_small_setup_that_all_hears_each_other_is_one_cell(self):
        boards = list(range(0x100, 0x10c))
        p = sv.propose(boards, house([boards]), chargers={0x100})
        self.assertTrue(p.flat)
        self.assertEqual(p.notes, [])

    def test_three_buildings_become_three_cells_under_one_upper_cell(self):
        a = list(range(0x100, 0x108))   # the house, 8 boards
        b = list(range(0x200, 0x206))   # the workshop, 6
        c = list(range(0x300, 0x30a))   # the barn, 10
        rooms = {**{n: "House" for n in a}, **{n: "Workshop" for n in b}, **{n: "Barn" for n in c}}
        # the chargers that hear each other across the yard: two in each building
        bridges = [(x, y) for x in (0x100, 0x101) for y in (0x200, 0x201, 0x300, 0x301)] + \
                  [(x, y) for x in (0x200, 0x201) for y in (0x300, 0x301)]
        p = sv.propose(a + b + c, house([a, b, c], bridges), chargers={0x100, 0x101, 0x102, 0x200, 0x201, 0x300,
                                                                          0x301}, rooms=rooms)
        self.assertFalse(p.flat)
        self.assertEqual(p.cells, {"house": a, "workshop": b, "barn": c})
        self.assertEqual(p.foremen["house"], [0x100, 0x101])  # 0x102 hears nobody outside: not chosen
        self.assertEqual(p.notes, [])
        # the cluster file it writes passes apply's plan: cells, parents, foremen, the peer budget
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "cell.json"
            boards = [{"node": f"{n:04x}", "label": f"b{n:04x}", "room": rooms[n]} for n in a + b + c]
            path.write_text(json.dumps({"schema": ap.SCHEMA, "system": "farm", "boards": boards, "firmware": None,
                                        "package": {"manifests": []}}), encoding="utf-8")
            sv.write(path, p)
            cl = ap.load_cluster(str(path))
            self.assertEqual(cl.parents, {"barn": "tree", "house": "tree", "workshop": "tree"})
            self.assertEqual(set(cl.foremen), {0x100, 0x101, 0x200, 0x201, 0x300, 0x301})
            lines = ap.check_peers(cl, 0x100)
            self.assertEqual(ap.peer_budget(cl, 0x100)[0x100]["peers"], 7 + 2 + 1)
            # and back to one cell: the tree's marks go
            sv.write(path, sv.Proposal(flat=True, cells={"": a + b + c}))
            self.assertEqual(ap.load_cluster(str(path)).cells, {})

    def test_a_big_room_splits_and_small_rooms_that_hear_each_other_merge(self):
        hall = list(range(0x100, 0x10e))  # 14 boards in one hall, all hearing each other: two cells
        kitchen, pantry = [0x200, 0x201, 0x202], [0x300, 0x301]
        rooms = {**{n: "Hall" for n in hall}, **{n: "Kitchen" for n in kitchen}, **{n: "Pantry" for n in pantry}}
        p = sv.propose(hall + kitchen + pantry, house([hall, kitchen + pantry]), rooms=rooms)
        self.assertEqual(sorted(len(g) for g in p.cells.values()), [4, 5, 10])  # 14 -> 10 + 4; kitchen and pantry -> 5
        self.assertIn("kitchen", p.cells)
        self.assertEqual(sorted(p.cells["kitchen"]), kitchen + pantry)

    def test_what_it_cannot_decide_it_says(self):
        a, b = list(range(0x100, 0x10c)), list(range(0x200, 0x20c))
        p = sv.propose(a + b, house([a, b]), chargers={0x100})
        self.assertTrue(any("no board on a charger" in n for n in p.notes))
        with self.assertRaises(ValueError):
            sv.write("unused.json", p)
        p = sv.propose(a + b, house([a, b]), chargers={0x100, 0x200})
        self.assertTrue(any("do not hear" in n for n in p.notes))  # 0x100 and 0x200 cannot hear each other

    def test_the_hubs_model_is_the_survey(self):
        model = {"boards": [{"node": "0100", "room": "A", "links": [{"peer": "0200", "delivery": 0.99, "rssi": -60}]},
                            {"node": "0200", "links": []}]}
        boards, links, rooms = sv.links_from_model(model)
        self.assertEqual((boards, rooms), ([0x100, 0x200], {0x100: "A"}))
        self.assertTrue(sv.hears(links, 0x200, 0x100))


if __name__ == "__main__":
    unittest.main()
