"""M12: potluck apply's parts that need no board -- the cluster file, composing a package from fragments,
the inventory reply (the same bytes as tests/test_node_status.cpp), what each board should run, the cluster
name in the image, and a portable actor in a cell bigger than a node image's list."""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import apply as ap
from potluck import deploy as dp
from potluck import manifest as mf
from potluck import modules as mods
from potluck import reconcile as rc
from potluck.paths import path_hash

ROOT = Path(__file__).resolve().parents[3]


def status_bytes(**kw) -> bytes:
    """pot/node_status.hpp's layout, written out field by field."""
    v = dict(node=0x7368, fw_running=10, fw_floor=9, fw_state=1, version=b"6235e98", elf=bytes(range(8)), slot=2,
             pending=0, trial=0, pkg=31, pkg_floor=31, digest=b"\x11" * 8, roles=0x2D, live=1, ck=4, up=99,
             pinned=2, ptotal=1, phere=1, cluster=b"home", actors=b"die_temp,cluster,guest", version_byte=1,
             modules=b"")
    v.update(kw)
    raw = struct.pack("<BHIIB", v["version_byte"], v["node"], v["fw_running"], v["fw_floor"], v["fw_state"])
    raw += v["version"].ljust(32, b"\0") + v["elf"] + bytes([v["slot"], v["pending"], v["trial"]])
    raw += struct.pack("<II", v["pkg"], v["pkg_floor"]) + v["digest"] + bytes([v["roles"]])
    raw += struct.pack("<III", v["live"], v["ck"], v["up"]) + bytes([v["pinned"], v["ptotal"], v["phere"]])
    raw += bytes([len(v["cluster"])]) + v["cluster"] + bytes([len(v["actors"])]) + v["actors"]
    if v["version_byte"] == 2:  # M13: the module section
        raw += bytes([len(v["modules"]) // 3]) + v["modules"]
    return raw


class Inventory(unittest.TestCase):
    def test_the_reply_parses(self):
        raw = status_bytes()
        self.assertEqual(len(raw), ap.FIXED_LEN + 1 + 4 + 1 + len(b"die_temp,cluster,guest"))
        st = ap.parse_node_status(raw)
        self.assertEqual((st.node, st.fw_running, st.fw_floor, st.fw_state, st.fw_version), (0x7368, 10, 9, "confirmed", "6235e98"))
        self.assertEqual((st.slot, st.pending, st.pkg_counter, st.pkg_floor), (2, 0, 31, 31))
        self.assertEqual(st.role_names(), ["extender-build", "sandbox", "reconciler", "enrolled"])
        self.assertEqual((st.live_applies, st.ck_writes, st.uptime_s), (1, 4, 99))
        self.assertEqual((st.pinned_running, st.portable_total, st.portable_here), (2, 1, 1))
        self.assertEqual(st.cluster, "home")
        self.assertEqual(st.actors, ["die_temp", "cluster", "guest"])
        self.assertIn("package 31", st.line())

    def test_unreachable_and_short_replies_are_not_a_status(self):
        self.assertIsNone(ap.parse_node_status(bytes([0, 0x68, 0x73])))
        self.assertIsNone(ap.parse_node_status(status_bytes()[:-1]))
        self.assertIsNone(ap.parse_node_status(b"\x03" + status_bytes()[1:]))
        self.assertIsNone(ap.parse_node_status(status_bytes(version_byte=2, modules=b"\x01\x01\x00")[:-1]))

    def test_version_2_lists_the_modules_and_what_each_is_doing(self):
        st = ap.parse_node_status(status_bytes(version_byte=2, modules=b"\x01\x01\x00\x02\x02\x01"))
        self.assertEqual(st.modules, [("sandbox", "active", ""), ("extender", "idle", "no credentials")])
        self.assertIn("modules sandbox:active,extender:idle (no credentials)", st.line())
        self.assertEqual(ap.parse_node_status(status_bytes()).modules, [])  # version 1: none listed


def write(d: Path, name: str, doc) -> Path:
    p = d / name
    p.write_text(json.dumps(doc), encoding="utf-8")
    return p


class ClusterFile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()
        mods.clear()

    def cluster(self, **kw):
        doc = {"schema": ap.SCHEMA, "system": "home", "boards": [{"node": "0100", "label": "kitchen"},
                                                                  {"node": "0200", "label": "hall"}],
               "firmware": None, "package": {"manifests": ["frag.json"]}}
        doc.update(kw)
        return ap.load_cluster(str(write(self.d, "cell.json", doc)))

    def test_the_pcs_services_are_named_from_the_ones_the_hub_offers(self):
        """M23: the cluster file's pc_services -- the PC user's choice of what the hub answers."""
        self.assertEqual(self.cluster().pc_services, ())
        self.assertEqual(self.cluster(pc_services=["time", "speech"]).pc_services, ("time", "speech"))
        for bad in (["teleport"], "speech", ["time", "time"]):
            with self.assertRaisesRegex(ap.ApplyError, "pc_services"):
                self.cluster(pc_services=bad)

    def test_bad_files_are_refused_with_a_reason(self):
        with self.assertRaisesRegex(ap.ApplyError, "not a cluster file"):
            ap.load_cluster(str(write(self.d, "x.json", {"schema": "nope"})))
        with self.assertRaisesRegex(ap.ApplyError, "cluster name"):
            self.cluster(system="two words")
        with self.assertRaisesRegex(ap.ApplyError, "appears twice"):
            self.cluster(boards=[{"node": "0100"}, {"node": "0100"}])
        with self.assertRaisesRegex(ap.ApplyError, "unknown key"):
            self.cluster(colour="blue")
        with self.assertRaisesRegex(ap.ApplyError, "room name"):
            self.cluster(boards=[{"node": "0100", "room": ""}])

    def test_a_board_may_name_its_room(self):
        # M20: where a board lives, for people; `label` stays its identity
        c = self.cluster(boards=[{"node": "0100", "label": "A", "room": "Living room"}, {"node": "0200", "label": "B"}])
        self.assertEqual(c.rooms, {0x0100: "Living room"})

    def test_a_boards_position_becomes_its_own_pinned_actor_and_two_entries(self):
        # M20: the floor plan's positions travel in the signed package, so a cell with no PC keeps them
        c = self.cluster(boards=[{"node": "0100", "label": "A", "position": [1.5, 2.25]}, {"node": "0200", "label": "B"}])
        self.assertEqual(c.positions, {0x0100: (1.5, 2.25)})
        m = mf.parse(ap.compose(c, []))
        self.assertEqual(mf.cross_check(m), [])
        img = dp.compile_image(m, 5)
        hx, hy = (dp.path_hash(p) for p in dp.position_paths("home", 0x0100))
        hr, hn = dp.path_hash(dp.room_path("home", 0x0100)), dp.path_hash(dp.near_path("home", 0x0100))
        self.assertIn(struct.pack("<HBB", 0x0100, dp.ACTOR_POSITION, 40) +
                      struct.pack("<IIIffII4H4B", hx, hy, hr, 1.5, 2.25, 0, hn, 0, 0, 0, 0, 0, 0, 0, 0), img)
        _, _, reads = ap.expected(m, [0x100, 0x200])
        self.assertIn("potluck://home/node-0100/sys/pos/y", reads)
        # Each actor's own outputs, never its board's other outputs (the hub's app view took the whole board's, so
        # one stale resource marked every app pinned there as not working -- found 2026-10-16); and the cell's list
        # is exactly the actors' lists together.
        pos = next(a for a in m.actors if a.module == "builtin:position" and m.placement_of(a) == 0x0100)
        kind, own = ap.actor_reads(m, pos, [0x100, 0x200])
        self.assertEqual(kind, "pinned")
        self.assertTrue(own and all("/node-0100/sys/" in p for p in own), own)
        together = [p for a in m.actors for p in ap.actor_reads(m, a, [0x100, 0x200])[1]]
        self.assertEqual(set(together) | {r.path for n in m.nodes if n.kind != "host" for r in n.owns}, set(reads))
        for bad in ([1], [-1, 0], [0, 1001], ["1", 2], [True, 1]):
            with self.assertRaisesRegex(ap.ApplyError, "position"):
                self.cluster(boards=[{"node": "0100", "position": bad}])

    def test_rooms_are_numbered_by_name_and_held_by_the_boards(self):
        # M20 / passive-sensor's D20: room-mates are boards whose node-<id>/sys/room is equal and non-zero
        c = self.cluster(boards=[{"node": "0100", "label": "A", "room": "Kitchen"},
                                 {"node": "0200", "label": "B", "room": "Hall", "position": [1, 1]},
                                 {"node": "0300", "label": "C"}])
        self.assertEqual(ap.room_numbers(c), {"Hall": 1, "Kitchen": 2})
        m = mf.parse(ap.compose(c, []))
        cfg = {a.name: dict(a.config) for a in m.actors if a.module == "builtin:position"}
        self.assertEqual(cfg, {"pos_0100": {"room": 2}, "pos_0200": {"x": 1.0, "y": 1.0, "room": 1}})
        dp.compile_image(m, 5)
        _, _, reads = ap.expected(m, [0x100, 0x200, 0x300])
        self.assertIn("potluck://home/node-0100/sys/room", reads)
        self.assertNotIn("potluck://home/node-0100/sys/pos/x", reads)

    def test_cells_are_numbered_by_name_and_each_board_carries_its_own(self):
        # M19: a board admits only boards of its cells; a foreman holds two
        c = self.cluster(boards=[{"node": "0100", "label": "a", "cells": ["house"]},
                                 {"node": "0200", "label": "b", "cells": ["house", "upper"]},
                                 {"node": "0300", "label": "c"}])
        self.assertEqual(ap.cell_numbers(c), {"house": 1, "upper": 2})
        m = mf.parse(ap.compose(c, []))
        cfg = {a.name: a.config.get("cells") for a in m.actors if a.module == "builtin:position"}
        self.assertEqual(cfg, {"pos_0100": "1", "pos_0200": "1,2"})
        dp.compile_image(m, 5)
        for bad in (["a", "b", "c"], [], ["a", "a"], [3]):
            with self.assertRaisesRegex(ap.ApplyError, "cells"):
                self.cluster(boards=[{"node": "0100", "cells": bad}])
        # Review of M19 #2: an upper cell's own window rides with each of its boards
        c = self.cluster(boards=[{"node": "0100", "label": "a", "cells": ["house", "upper"]},
                                 {"node": "0200", "label": "b", "cells": ["upper"]}],
                         cells={"upper": {"window_ms": 5000}})
        m = mf.parse(ap.compose(c, []))
        cfg = {a.name: a.config.get("cell_windows") for a in m.actors if a.module == "builtin:position"}
        self.assertEqual(cfg, {"pos_0100": "0,5000", "pos_0200": "5000"})
        dp.compile_image(m, 5)
        with self.assertRaisesRegex(ap.ApplyError, "window_ms"):
            self.cluster(boards=[{"node": "0100", "cells": ["u"]}], cells={"u": {"window_ms": 150}})
        with self.assertRaisesRegex(ap.ApplyError, "no board"):
            self.cluster(boards=[{"node": "0100", "cells": ["u"]}], cells={"v": {"window_ms": 1000}})

    def tree(self, cells_of, eligible=2, upper=None, **kw):
        """M19: base cells of the given sizes under one upper cell 'tree'; the first `eligible` boards of each may
        be foreman. `upper`: boards that live in the upper cell itself."""
        boards, nid = [], 0x100
        for k, size in enumerate(cells_of):
            for i in range(size):
                boards.append({"node": f"{nid:04x}", "label": f"c{k}b{i}", "cells": [f"cell{k}"],
                               **({"foreman": True} if i < eligible else {})})
                nid += 1
        for i in range(upper or 0):
            boards.append({"node": f"{nid:04x}", "label": f"up{i}", "cells": ["tree"]})
            nid += 1
        cells = {f"cell{k}": {"parent": "tree"} for k in range(len(cells_of))}
        cells["tree"] = {"window_ms": 5000}
        cells.update(kw.pop("cells", {}))
        return self.cluster(boards=boards, cells=cells, **kw)

    def test_the_peer_budget_counts_both_memberships_and_names_the_board_over_it(self):
        # Review of M19 #6: a foreman holds its cell and one seat per sibling cell in the upper cell (the standby joins
        # on taking the role); the PC takes one on the cabled board's cells
        c = self.tree([9, 9, 9])
        self.assertEqual(ap.cell_numbers(c), {"cell0": 1, "cell1": 2, "cell2": 3, "tree": 4})
        b = ap.peer_budget(c, cabled=0x100)
        self.assertEqual(b[0x100], {"peers": 8 + 2 + 1, "cells": {"cell0": 8, "tree": 2}, "host": 1})
        self.assertEqual(b[0x102]["peers"], 8 + 1)       # a member, in the cabled board's cell
        self.assertEqual(b[0x109]["peers"], 8 + 2)       # a foreman-eligible board of cell1: no PC
        self.assertEqual(b[0x10b]["peers"], 8)
        self.assertTrue(any("c0b0 (0x0100): 11 of 19 (cell0 8, tree 2, the PC 1)" in x for x in ap.check_peers(c, 0x100)))
        # the accept's tree: ten cells of ten under one upper cell fit -- 9 + 9, and the PC on the cabled cell
        c = self.tree([10] * 10)
        b = ap.peer_budget(c, 0x100)
        self.assertEqual((b[0x100]["peers"], b[0x10a]["peers"]), (9 + 9 + 1, 9 + 9))
        ap.check_peers(c, 0x100)
        # eleven do not: the cabled cell's foremen would hold 9 + 10 + 1 (the others 9 + 10: just)
        c = self.tree([10] * 11)
        with self.assertRaisesRegex(ap.ApplyError, r"c0b0 \(0x0100\) would need 20 unicast peers; c0b1 \(0x0101\) would need 20"):
            ap.check_peers(c, 0x100)
        # an upper cell's own boards count in it too
        c = self.tree([6] * 4, upper=2)
        self.assertEqual(ap.peer_budget(c, 0x100)[0x100]["peers"], 5 + 3 + 2 + 1)
        up = [n for n, lab in c.boards if lab == "up0"][0]
        self.assertEqual(ap.peer_budget(c, 0x100)[up]["cells"], {"tree": 1 + 4})
        # a board in no cell is everyone's peer
        c = self.cluster(boards=[{"node": "0100", "cells": ["a"]}, {"node": "0200"}])
        self.assertEqual(ap.peer_budget(c, 0x100)[0x100]["peers"], 1 + 1)
        self.assertEqual(ap.check_peers(self.cluster(), 0x100), [])  # no cells: one cell, as before

    def test_a_trees_package_is_one_image_each_board_keeps_its_cells_of(self):
        # Review of M19 #3: 27 boards' positions are more than a board keeps (64 since M20, 16 before), and more than
        # it needs; a version-5 image lists them all and each board keeps its own cell's
        c = self.tree([9, 9, 9])
        m = mf.parse(ap.compose(c, []))
        img = dp.compile_image(m, 5)
        self.assertEqual(img[4], dp.IMAGE_VERSION_CELLS)
        _, _, count, _, _, _, body_len = struct.unpack_from("<IBBHI8sI", img)
        body = img[dp.IMAGE_HEADER_LEN:dp.IMAGE_HEADER_LEN + body_len]
        scope = dp.scope_of(body, count)
        self.assertEqual(img[-(1 + 2 * count):], bytes([count]) + b"".join(bytes(p) for p in scope))
        kept = dp.kept_per_board(body, count, scope)
        # its cell's nine positions, its cell's foreman and the cluster's name; a board that may be foreman also the
        # other two cells' foremen, whose summaries it reads in the upper cell
        self.assertEqual(sorted(set(kept.values())), [9 + 1 + 1, 9 + 3 + 1])
        # a package with no cells is byte for byte what it was
        self.assertNotEqual(dp.compile_image(mf.parse(ap.compose(self.cluster(), [])), 5)[4], dp.IMAGE_VERSION_CELLS)

    def test_each_federating_cell_gets_a_foreman_with_its_exports(self):
        # M19: one portable foreman per cell with a parent, eligible on its "foreman": true boards; its summary
        c = self.tree([3, 3], cells={"cell0": {"parent": "tree", "period_ms": 1000,
                                               "export": {"warmest": {"max": "hw/die_temp", "unit": "celsius"},
                                                          "a_temp": {"copy": "node-0100/hw/die_temp",
                                                                     "unit": "celsius"}}},
                                     "cell1": {"parent": "tree"}, "tree": {"window_ms": 5000}})
        m = mf.parse(ap.compose(c, []))
        fm = {a.name: a for a in m.actors if a.module == "builtin:foreman"}
        self.assertEqual(sorted(fm), ["foreman_cell0", "foreman_cell1"])
        a = fm["foreman_cell0"]
        self.assertEqual(a.nodes, (0x100, 0x101))
        self.assertEqual(a.config["members"], "0100,0101,0102")
        self.assertEqual((a.config["cell"], a.config["parent"], a.config["parent_window_ms"]), (1, 3, 5000))
        self.assertEqual([e[0] for e in dp.foreman_exports(a)], ["alive", "a_temp", "warmest"])
        img = dp.compile_image(m, 5)
        _, _, count, _, _, _, body_len = struct.unpack_from("<IBBHI8sI", img)
        body = img[dp.IMAGE_HEADER_LEN:dp.IMAGE_HEADER_LEN + body_len]
        decls = dp._decls(body, count)
        scope = dp.scope_of(body, count)
        k = next(i for i, (node, typ, _) in enumerate(decls) if typ == dp.ACTOR_FOREMAN)
        self.assertEqual(scope[k], (1, 3))  # its cell, and the parent where its summary is read
        self.assertEqual(struct.unpack_from("<I", decls[k][2])[0],
                         path_hash("potluck://home/cell-cell0/alive"))
        kept = dp.kept_per_board(body, count, scope)
        self.assertEqual(kept[0x100], 3 + 2 + 1)  # its cell's positions, both foremen, the cluster's name
        self.assertEqual(kept[0x102], 3 + 1 + 1)  # a member: its own cell's foreman only
        # the declaration the firmware's test parses (tests/test_foreman.cpp, kHostForeman)
        self.assertEqual(len(decls[k][2]), 7 + 3 * 2 + 8 + 2 * 3 + 1 + 7 + 12 + 8 + 11)
        with self.assertRaisesRegex(ap.ApplyError, "export"):
            self.tree([3], cells={"cell0": {"export": {"x": {"max": "hw/t"}}}, "tree": {}})
        for bad in ({"alive": {"max": "a"}}, {"x": {"max": "a", "min": "b"}}, {"x": {"median": "a"}},
                    {"x": {"max": "a", "unit": "parsec"}}, {"X": {"max": "a"}}):
            with self.assertRaises(ap.ApplyError):
                self.tree([3], cells={"cell0": {"parent": "tree", "export": bad}, "tree": {}})

    def test_a_trees_firmware_rollout_takes_members_then_the_standby_then_the_foreman(self):
        c = self.tree([3, 3])

        class St:
            def __init__(self, actors):
                self.actors = actors
        inv = {0x100: St(["foreman_cell0", "pos_0100"]), 0x101: St(["pos_0101"]), 0x103: St(["foreman_cell1"])}
        need = [0x100, 0x101, 0x102, 0x103, 0x104, 0x105]
        self.assertEqual(ap.rollout_order(c, need, 0x102, inv), [0x105, 0x101, 0x104, 0x100, 0x103, 0x102])

    def test_in_a_tree_the_presenting_role_lives_in_the_cell_it_names(self):
        fw = {"modules": ["present", "wifi"]}
        with self.assertRaisesRegex(ap.ApplyError, "present.cell"):
            ap.compose(self.tree([3, 3], firmware=fw, present={"broker": "10.0.0.2"}), [])
        c = self.tree([3, 3], firmware=fw, present={"broker": "10.0.0.2", "cell": "cell1"})
        m = mf.parse(ap.compose(c, []))
        pres = next(a for a in m.actors if a.module == "builtin:present")
        self.assertEqual(pres.nodes, (0x103, 0x104, 0x105))
        self.assertNotIn("cell", pres.config)
        self.assertTrue(any("one channel for the whole tree" in x for x in ap.check_peers(c, 0x100)))

    def test_stub_boards_are_in_the_tree_and_the_verify_expects_only_what_the_real_boards_keep(self):
        # M19's bench: three real boards in "bench", a stub cell the PC hosts, both under "upper"
        boards = [{"node": f"{n:04x}", "label": f"r{i}", "cells": ["bench"], **({"foreman": True} if i else {})}
                  for i, n in enumerate((0x100, 0x101, 0x102))]
        boards += [{"node": f"{n:04x}", "label": f"s{i}", "cells": ["s1"], "stub": True,
                    **({"foreman": True} if i < 2 else {})} for i, n in enumerate((0xF101, 0xF102, 0xF103))]
        c = self.cluster(boards=boards, cells={"bench": {"parent": "upper"}, "s1": {"parent": "upper"},
                                               "upper": {"window_ms": 5000}})
        self.assertEqual(c.stubs, {0xF101, 0xF102, 0xF103})
        m = mf.parse(ap.compose(c, []))
        pkg = ap.Package(m, {}, dp.compile_image(m, 5))
        real = [0x100, 0x101, 0x102]
        _, _, reads = ap.expected(m, real)
        per, here, kept = ap.tree_expectations(c, pkg, real, reads)
        self.assertEqual(per, {0x100: 1, 0x101: 2, 0x102: 2})  # its cell's role; a board that may hold it, s1's too
        self.assertEqual(here, 1)
        self.assertIn("potluck://home/cell-bench/alive", kept)
        self.assertFalse(any("cell-s1" in p or "node-f1" in p for p in kept))
        with self.assertRaisesRegex(ap.ApplyError, "stub"):
            self.cluster(boards=[{"node": "0100", "stub": True}])

    def test_boards_of_cells_the_cable_does_not_reach_are_not_required_to_answer_through_it(self):
        c = self.tree([3, 3])
        self.assertEqual(ap.beyond_cable(c, [n for n, _ in c.boards], 0x100), [0x103, 0x104, 0x105])
        self.assertEqual(ap.beyond_cable(self.cluster(), [0x100, 0x200], 0x100), [])  # one cell: everyone

    def test_a_foreman_is_a_board_of_one_federating_cell(self):
        with self.assertRaisesRegex(ap.ApplyError, "exactly one cell"):  # two memberships at most, the role adds one
            self.cluster(boards=[{"node": "0100", "cells": ["a", "b"], "foreman": True}], cells={"a": {"parent": "t"}})
        with self.assertRaisesRegex(ap.ApplyError, "exactly one cell"):  # its cell must have a parent
            self.cluster(boards=[{"node": "0100", "cells": ["a"], "foreman": True}])
        with self.assertRaisesRegex(ap.ApplyError, "no board that may be its foreman"):
            self.cluster(boards=[{"node": "0100", "cells": ["a"]}], cells={"a": {"parent": "t"}})
        with self.assertRaisesRegex(ap.ApplyError, "own ancestor"):
            self.cluster(boards=[{"node": "0100", "cells": ["a"], "foreman": True},
                                 {"node": "0200", "cells": ["b"], "foreman": True}],
                         cells={"a": {"parent": "b"}, "b": {"parent": "a"}})
        with self.assertRaisesRegex(ap.ApplyError, "true or false"):
            self.cluster(boards=[{"node": "0100", "cells": ["a"], "foreman": 1}], cells={"a": {"parent": "t"}})
        with self.assertRaisesRegex(ap.ApplyError, "no board"):
            self.cluster(boards=[{"node": "0100", "cells": ["a"]}], cells={"t": {"parent": "a"}})

    def test_each_boards_nearest_set_follows_passive_sensors_rule(self):
        # D20: same room when the board has one; by distance, ties to the lower id; unplaced after placed, by id
        c = self.cluster(boards=[
            {"node": "0100", "label": "a", "room": "R", "position": [0, 0]},
            {"node": "0200", "label": "b", "room": "R", "position": [3, 0]},
            {"node": "0300", "label": "c", "room": "R", "position": [0, 3]},   # ties with b: the lower id first
            {"node": "0400", "label": "d", "room": "R"},                        # unplaced: after the placed
            {"node": "0500", "label": "e", "room": "S", "position": [1, 0]},   # nearest, but another room
            {"node": "0600", "label": "f"}])                                    # no room, no place: the lowest ids
        near = ap.near_sets(c, 4)
        self.assertEqual(near[0x0100], [0x0200, 0x0300, 0x0400])  # fewer than 4 room-mates: all of them
        self.assertEqual(near[0x0500], [])                         # alone in its room
        self.assertEqual(near[0x0600], [0x0100, 0x0200, 0x0300, 0x0400])
        self.assertEqual(ap.near_sets(c, 2)[0x0400], [0x0100, 0x0200])  # unplaced itself: the lowest ids

    def test_a_per_room_module_runs_once_per_room_among_that_rooms_boards(self):
        from potluck import reconcile as rc
        mods._REGISTRY["ext:t/fuse"] = mods.ModuleSpec(
            "ext:t/fuse", 0xF5, "portable", (mods.Field("outputs", "path"), mods.Field("mirror", "path")), "t",
            outputs=("act/{actor}/out",), per_room=True)
        frag = {"schema": 1, "system": "lab", "nodes": [],
                "actors": [{"name": "fuse", "module": "ext:t/fuse", "latency_class": 4,
                            "config": {"outputs": "potluck://lab/act/fuse/", "mirror": "potluck://lab/x/<room>"}}]}
        self.assertEqual((ap.room_slug("Living room"), ap.room_slug("Børn's #2"), ap.room_slug("!!")),
                         ("living_room", "b_rn_s_2", "room"))
        c = self.cluster(boards=[{"node": "0100", "label": "a", "room": "Living room"},
                                 {"node": "0200", "label": "b", "room": "Kitchen"},
                                 {"node": "0300", "label": "c", "room": "Living room"}])
        m = mf.parse(ap.compose(c, [(frag, self.d, "frag")]))
        acts = {a.name: a for a in m.actors if a.module == "ext:t/fuse"}
        self.assertEqual(sorted(acts), ["fuse_kitchen", "fuse_living_room"])
        a = acts["fuse_living_room"]
        self.assertEqual((a.nodes, a.config["outputs"], a.config["mirror"]),
                         ((0x100, 0x300), "potluck://home/act/fuse_living_room/", "potluck://home/x/living_room"))
        self.assertEqual([n for n, _ in rc.eligible(m, a)], [0x100, 0x300])
        dp.compile_image(m, 5)
        # no rooms: the one actor, unchanged
        m0 = mf.parse(ap.compose(self.cluster(), [(frag, self.d, "frag")]))
        self.assertEqual([a.name for a in m0.actors if a.module == "ext:t/fuse"], ["fuse"])

    def test_fragments_are_matched_by_label_renumbered_and_rehomed(self):
        c = self.cluster()
        frag = {"schema": 1, "system": "lab", "nodes": [
            {"node_id": 25344, "label": "kitchen", "owns": [
                {"path": "potluck://lab/node-6300/hw/die_temp", "unit": "celsius", "staleness_bound_ms": 5000}]}],
            "actors": [{"name": "die", "module": "builtin:die_temp", "latency_class": 4, "pin": 25344,
                        "config": {"period_ms": 1000}}]}
        doc = ap.compose(c, [(frag, self.d, "frag")])
        self.assertEqual(doc["system"], "home")
        self.assertEqual([n["node_id"] for n in doc["nodes"]], [0x100, 0x200])
        self.assertEqual(doc["nodes"][0]["owns"][0]["path"], "potluck://home/node-0100/hw/die_temp")
        self.assertEqual(doc["actors"][0]["pin"], 0x100)
        m = mf.parse(doc)
        self.assertEqual(mf.cross_check(m), [])
        img = dp.compile_image(m, 5)
        # The cluster-name actor rides at the end: every node, type 9, the name.
        self.assertTrue(img.endswith(struct.pack("<HBB", 0xFFFF, dp.ACTOR_CLUSTER, 4) + b"home"))
        pinned, portable, reads = ap.expected(m, [0x100, 0x200])
        self.assertEqual(pinned, {0x100: 2, 0x200: 1})  # die_temp + cluster; cluster
        self.assertEqual(portable, 0)
        self.assertEqual(reads, ["potluck://home/node-0100/hw/die_temp"])

    def test_what_the_outside_may_do_is_the_cluster_files_and_reaches_the_image(self):
        # M13: the owner's choice, in the cluster file; a path may leave out potluck://<system>/.
        c = self.cluster(outside={"node-0100/hw/die_temp": "read"})
        self.assertEqual(c.outside, {"potluck://home/node-0100/hw/die_temp": "read"})
        frag = {"schema": 1, "system": "lab", "nodes": [], "actors": []}
        m = mf.parse(ap.compose(c, [(frag, self.d, "frag")]))
        self.assertEqual(mf.outside_rules(m), [("potluck://home/node-0100/hw/die_temp", mf.OUTSIDE_EXPOSED)])
        self.assertEqual(dp.compile_image(m, 5)[4], dp.IMAGE_VERSION_OUTSIDE)
        with self.assertRaisesRegex(ap.ApplyError, "owner's choice"):
            ap.compose(c, [({**frag, "outside": {"potluck://lab/node-0100/hw/die_temp": "read"}}, self.d, "app")])
        with self.assertRaisesRegex(ap.ApplyError, "read_write"):
            self.cluster(outside={"node-0100/hw/die_temp": "write"})

    def test_a_cell_that_presents_builds_without_ampdu_transmit_aggregation(self):
        # M0-LOG session 42: aggregation on a board presenting over its own station wedges its radio.
        present = {"broker": "192.168.0.77:1883"}
        c = self.cluster(firmware={"modules": ["present", "wifi"]}, present=present)
        self.assertIn(ap.AMPDU_TX_OFF, c.extra)
        self.assertNotIn(ap.AMPDU_TX_OFF, self.cluster(firmware={"modules": ["wifi"]}).extra)
        own = self.cluster(firmware={"modules": ["present", "wifi"], "extra": ["CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=y"]},
                           present=present)
        self.assertNotIn(ap.AMPDU_TX_OFF, own.extra)  # the owner's explicit choice stands
        self.assertNotIn(ap.AMPDU_TX_OFF, self.cluster(firmware=None).extra)  # firmware left alone: nothing to build

    def test_the_brokers_ca_file_goes_into_the_signed_manifest_as_der(self):
        # M13: TLS to the broker. The cluster file names the CA's file; its DER rides in the manifest (the owner
        # signs it) and the image carries it (version 4).
        (self.d / "ca.pem").write_text("-----BEGIN CERTIFICATE-----\nMAMCAQE=\n-----END CERTIFICATE-----\n")
        fw = {"modules": ["present", "wifi"]}
        c = self.cluster(firmware=fw, present={"broker": "192.168.0.77:8883", "tls_ca": "ca.pem"})
        self.assertEqual(c.present["tls_ca"], "MAMCAQE=")
        (self.d / "ca.der").write_bytes(bytes.fromhex("3003020101"))
        self.assertEqual(self.cluster(firmware=fw, present={"broker": "192.168.0.77:8883", "tls_ca": "ca.der"})
                         .present["tls_ca"], "MAMCAQE=")
        frag = {"schema": 1, "system": "lab", "nodes": [], "actors": []}
        m = mf.parse(ap.compose(c, [(frag, self.d, "frag")]))
        img = dp.compile_image(m, 5)
        self.assertEqual(img[4], dp.IMAGE_VERSION_BROKER_CA)
        self.assertTrue(img.endswith(bytes.fromhex("05003003020101")))
        with self.assertRaisesRegex(ap.ApplyError, "cannot read"):
            self.cluster(firmware=fw, present={"broker": "192.168.0.77:8883", "tls_ca": "missing.pem"})
        (self.d / "key.pem").write_text("-----BEGIN PRIVATE KEY-----\nMAMCAQE=\n-----END PRIVATE KEY-----\n")
        with self.assertRaisesRegex(ap.ApplyError, "CERTIFICATE"):
            self.cluster(firmware=fw, present={"broker": "192.168.0.77:8883", "tls_ca": "key.pem"})

    def test_a_board_the_cluster_does_not_have_and_clashing_actors_are_refused(self):
        c = self.cluster()
        stranger = {"schema": 1, "system": "lab", "nodes": [{"node_id": 0x900, "label": "garage", "owns": [
            {"path": "potluck://lab/node-0900/hw/x", "unit": "count"}]}], "actors": []}
        with self.assertRaisesRegex(ap.ApplyError, "garage"):
            ap.compose(c, [(stranger, self.d, "s")])
        pinned = {"schema": 1, "system": "lab", "nodes": [{"node_id": 0x900, "label": "garage"}], "actors": [
            {"name": "d", "module": "builtin:die_temp", "latency_class": 4, "pin": 0x900, "config": {"period_ms": 1000}}]}
        with self.assertRaisesRegex(ap.ApplyError, "garage"):
            ap.compose(c, [(pinned, self.d, "p")])
        a = {"schema": 1, "system": "lab", "nodes": [], "actors": [
            {"name": "t", "module": "builtin:ticker", "latency_class": 4, "config": {"period_ms": 100}}]}
        with self.assertRaisesRegex(ap.ApplyError, "also in"):
            ap.compose(c, [(a, self.d, "one"), (a, self.d, "two")])

    def test_a_lab_package_is_unchanged_by_m12(self):
        m = mf.load(str(ROOT / "manifests" / "m6-ticker.json"))
        img = dp.compile_image(m, 5)
        self.assertNotIn(bytes([dp.ACTOR_CLUSTER]) + b"lab", img)
        self.assertEqual(img[5], len(m.actors))  # actor count: nothing added


class Presets(unittest.TestCase):
    """examples/cells: each preset loads, and its package (if any) composes for the boards it names -- the
    hello app's fragment lists the maintainer's three boards, which a user's cell does not have."""

    def tearDown(self):
        mods.clear()

    def test_every_preset_plans_without_a_board(self):
        cells = sorted((ROOT / "examples" / "cells").glob("*.json"))
        self.assertGreaterEqual(len(cells), 2)
        for p in cells:
            with self.subTest(preset=p.name):
                c = ap.load_cluster(str(p))
                frags = [(json.loads(mp.read_text(encoding="utf-8")), root, str(mp)) for mp, root in c.manifests]
                for a in c.apps:
                    mods.clear()
                    if a.components:
                        for desc in (ROOT / a.components).glob("*/potluck-modules.json"):
                            mods.load(str(desc))
                    frags += [(json.loads((ROOT / m).read_text(encoding="utf-8")), ROOT, m) for m in a.manifests]
                if frags:
                    m = mf.parse(ap.compose(c, frags))
                    self.assertEqual(mf.cross_check(m), [])
                    dp.compile_image(m, 1)


class AppOutputs(unittest.TestCase):
    """The blind reader's run (2026-10-11): verify read nothing of an app whose output the board builds itself.
    A descriptor's `outputs` now name them, and `apply` reads each one back."""

    def tearDown(self):
        mods.clear()

    def test_the_hello_apps_output_is_read_back_on_every_board(self):
        mods.clear()
        mods.load(str(ROOT / "examples" / "apps" / "pot_hello" / "potluck-modules.json"))
        m = mf.load(str(ROOT / "examples" / "apps" / "hello.json"))
        _, _, reads = ap.expected(m, [0x6300, 0x7368])
        self.assertEqual(reads, ["potluck://lab/node-6300/hello/peers", "potluck://lab/node-7368/hello/peers"])

    def test_the_declaration_helper_gives_the_bytes_a_rig_takes(self):
        mods.clear()
        mods.load(str(ROOT / "examples" / "apps" / "pot_hello" / "potluck-modules.json"))
        img = dp.compile_image(mf.load(str(ROOT / "examples" / "apps" / "hello.json")), 1)
        self.assertEqual([d.hex() for d in dp.declarations(img)], ["fffff006271139618813"])


class BiggerCell(unittest.TestCase):
    def test_a_portable_actor_gets_the_best_eight_of_twelve(self):
        nodes = [{"node_id": 0x100 + i, "label": f"n{i}"} for i in range(12)]
        nodes[11]["owns"] = [{"path": "potluck://lab/node-010b/hw/x", "unit": "count"}]
        doc = {"schema": 1, "system": "lab", "nodes": nodes, "actors": [
            {"name": "t", "module": "builtin:ticker", "latency_class": 4, "needs": ["potluck://lab/node-010b/hw/x"],
             "config": {"period_ms": 100}}]}
        m = mf.parse(doc)
        el = rc.eligible(m, m.actors[0])
        self.assertEqual(len(el), rc.MAX_ELIGIBLE)
        self.assertIn((0x10B, 2), el)  # the one with the data is always among them
        self.assertEqual(el, rc.eligible(m, m.actors[0]))  # stable
        dp.compile_image(m, 1)  # and the image takes it


class BoardModules(unittest.TestCase):
    """M13: Potluck's optional board modules -- named in the cluster file, required by descriptors, costed."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def cluster(self, fw):
        doc = {"schema": ap.SCHEMA, "system": "home", "boards": [{"node": "0100", "label": "kitchen"}], "firmware": fw}
        return ap.load_cluster(str(write(self.d, "cell.json", doc)))

    def test_a_named_module_turns_its_config_on_and_the_old_spelling_still_counts(self):
        c = self.cluster({"modules": ["sandbox"]})
        self.assertEqual(c.board_modules, ("sandbox",))
        self.assertIn("CONFIG_POT_WASM=y", c.extra)
        c = self.cluster({"extra": ["CONFIG_POT_WASM=y"]})  # M12's cluster files
        self.assertEqual(c.board_modules, ("sandbox",))
        self.assertEqual(c.extra, ["CONFIG_POT_WASM=y"])  # the same build key as before
        self.assertEqual(self.cluster({}).board_modules, ())

    def test_unknown_unbuilt_and_sourceless_modules_are_refused(self):
        with self.assertRaisesRegex(ap.ApplyError, "not one of Potluck's board modules"):
            self.cluster({"modules": ["teleport"]})
        with self.assertRaisesRegex(ap.ApplyError, "own repository"):
            self.cluster({"modules": ["extender"]})

    def test_a_descriptor_that_requires_a_module_the_cell_lacks_is_refused(self):
        desc = {"component": "needs_box", "types": [0xF4, 0xF4], "requires": ["sandbox"],
                "modules": [{"module": "ext:needs_box/a", "type": 0xF4, "placement": "pinned"}]}
        with self.assertRaisesRegex(mods.ModuleError, "requires"):
            mods.parse_descriptor({**desc, "requires": ["teleport"]})
        self.assertEqual(mods.parse_descriptor(desc)[0].requires, ("sandbox",))
        write(self.d, "needs_box.json", desc)
        frag = {"schema": 1, "system": "home", "nodes": [], "actors": []}
        write(self.d, "frag.json", frag)
        doc = {"schema": ap.SCHEMA, "system": "home", "boards": [{"node": "0100", "label": "kitchen"}],
               "firmware": None, "package": {"manifests": ["frag.json"], "modules": ["needs_box.json"]}}
        c = ap.load_cluster(str(write(self.d, "cell.json", doc)))
        try:
            with self.assertRaisesRegex(ap.ApplyError, "requires sandbox"):
                ap.plan_package(c, self.d, lambda m: None)
        finally:
            mods.clear()

    def test_each_modules_cost_comes_from_the_builds_size_report(self):
        def row(flash=0, dram=0, ext=0):
            return {"memory_types": {"Flash Code": {"size": flash}, "DIRAM": {"size": dram},
                                     "External RAM": {"size": ext}}}
        report = {"a/libpot_wasm.a": {"abbrev_name": "libpot_wasm.a", **row(80000, 2673)},
                  "a/libpot_room.a": {"abbrev_name": "libpot_room.a", **row(7000, 82, 1390)},
                  "a/libmain.a": {"abbrev_name": "libmain.a", **row(20000, 43000)},
                  "a/libpot_espnow.a": {"abbrev_name": "libpot_espnow.a", **row(3000, 18520)}}
        sj = write(self.d, "size-components.json", report)
        c = self.cluster({"modules": ["sandbox"]})
        fw = ap.Firmware(sj, sj, b"", "v", False, sj, ("pot_room",))
        lines = ap.module_costs(c, fw)
        self.assertEqual(lines[0], "module sandbox: 80,000 B flash, 2,673 B internal RAM, 0 B PSRAM (static; its own archives -- what it pulls in from ESP-IDF is not counted here)")
        self.assertEqual(lines[1], "module app component pot_room: 7,000 B flash, 82 B internal RAM, 1,390 B PSRAM (static; its own archives -- what it pulls in from ESP-IDF is not counted here)")
        self.assertEqual(lines[2], "Potluck core: 61,520 B of 65,536 B static internal RAM (4,016 B left)")


class BusyBoard(unittest.TestCase):
    """2026-10-23: right after a wiped board came back, the board on the cable was busy sending it the cell's package
    (catch-up) and answered BUSY; apply failed instead of asking again."""

    def run_send(self, busy_answers: int):
        import struct
        from potluck import deploy as dp
        answers = []

        class B:
            def deploy_step(self, op, payload, timeout, dst):
                answers.append(op)
                busy = len(answers) <= busy_answers
                status = dp.STATUS_NAMES.index("BUSY") if busy else 0
                return struct.pack("<HBBIIBB", status, 0, 0, 0, 0, 2, 0).ljust(16, b"\x00")

        old = ap.BUSY_WAIT_S
        ap.BUSY_WAIT_S = 0.0
        try:
            return ap.send_package(B(), 0x6300, b"x" * 10, 7, b"d" * 8, lambda m: None), answers
        finally:
            ap.BUSY_WAIT_S = old

    def test_a_busy_board_is_asked_again(self):
        (ok, failed), answers = self.run_send(busy_answers=3)
        self.assertEqual((ok, failed), (2, 0))
        self.assertGreater(len(answers), 3)

    def test_a_board_that_stays_busy_still_fails_with_the_reason(self):
        with self.assertRaisesRegex(ap.ApplyError, "BUSY"):
            self.run_send(busy_answers=100)


class LeaseInAPartition(unittest.TestCase):
    """M0-LOG session 43: each side of a split has an arbiter of its own (tests/test_deploy_partition.cpp,
    [lease_partition]), so a deploy lease counts only when the cluster's lowest board granted it."""

    BOARDS = [0x100 + i for i in range(8)]

    def test_the_lowest_boards_lease_counts(self):
        self.assertIsNone(ap.lease_counts(0x100, self.BOARDS))
        self.assertIsNone(ap.lease_counts(0x6300, [0x8160, 0x6300, 0x7368]))  # the bench: A is the lowest

    def test_a_lease_from_another_sides_arbiter_does_not(self):
        why = ap.lease_counts(0x104, self.BOARDS)
        self.assertIn("0x0104", why)
        self.assertIn("0x0100", why)
        self.assertIn("partition", why)

    def test_a_lower_board_outside_the_cluster_file_is_named(self):
        self.assertIn("not in the cluster file", ap.lease_counts(0x0101, [0x0102, 0x0103]))

    def test_a_host_in_the_list_is_not_the_lowest_board(self):
        self.assertIsNone(ap.lease_counts(0x6300, [0x00FE, 0x6300, 0x7368]))



class WaitReachable(unittest.TestCase):
    """2026-10-19/20: the rollout stopped at C seconds after B restarted -- the cell was settling between a mesh's
    channels, and C was back 2-3 minutes later. The next board is waited for before it is given up."""

    def test_a_board_back_after_a_while_is_waited_for_and_one_that_never_answers_is_not(self) -> None:
        from unittest import mock
        from potluck import fw as fwm
        from potluck.bridge import RequestTimeout
        logs = []
        answers = iter([RequestTimeout("no"), {"status": "unreachable"}, {"status": "ok"}])

        def status(b, n):
            a = next(answers)
            if isinstance(a, Exception):
                raise a
            return a
        with mock.patch.object(fwm, "status", status):
            self.assertTrue(ap.wait_reachable(None, 0x8160, logs.append, settle_s=5.0, poll_s=0.01))
        self.assertEqual(len(logs), 2)  # "waiting for", then "answers again"
        self.assertTrue(logs[0].startswith("firmware: waiting for 0x8160"))
        with mock.patch.object(fwm, "status", lambda b, n: {"status": "unreachable"}):
            self.assertFalse(ap.wait_reachable(None, 0x8160, logs.append, settle_s=0.05, poll_s=0.01))
        with mock.patch.object(fwm, "status", lambda b, n: {"status": "ok"}):
            logs.clear()
            self.assertTrue(ap.wait_reachable(None, 0x8160, logs.append))
            self.assertEqual(logs, [])  # there at once: nothing said


class PackagedRelease(unittest.TestCase):
    """2026-10-19: the owner's first Apply from the Desk -- an `outside` change only -- rebuilt firmware the installed
    package already carried, and the build stopped on a Windows error box. The release image is used when it matches."""

    def test_the_release_the_package_carries_is_used_for_its_own_version_only(self):
        with tempfile.TemporaryDirectory() as d:
            rel = Path(d) / "639e1f6-910870"
            rel.mkdir()
            (rel / "potluck_m0.bin").write_bytes(b"\xe9" + b"\0" * 255)
            sha = "6b4203943c58a3df039671bca620c2e413bc0d2c53a1be114f0b7e21bbf3d6fa"
            (rel / "release.json").write_text(json.dumps({"version": "639e1f6-910870", "elf_sha256": sha}))
            old = os.environ.get("POTLUCK_FIRMWARE_DIR")
            os.environ["POTLUCK_FIRMWARE_DIR"] = d
            try:
                got = ap.packaged_release("639e1f6-910870")
                self.assertIsNotNone(got)
                self.assertEqual(got, (rel / "potluck_m0.bin", bytes.fromhex(sha)))
                self.assertIsNone(ap.packaged_release("639e1f6-ffffff"))  # another build: built as before
                (rel / "release.json").write_text(json.dumps({"version": "639e1f6-910870"}))
                self.assertIsNone(ap.packaged_release("639e1f6-910870"))  # no ELF hash to compare boards with
            finally:
                if old is None:
                    os.environ.pop("POTLUCK_FIRMWARE_DIR", None)
                else:
                    os.environ["POTLUCK_FIRMWARE_DIR"] = old


class ExportTwiceAtOnce(unittest.TestCase):
    """2026-10-26: the hub's apply and a session's plan exported one commit at the same moment, shared one ".tmp" and
    both failed. Each export now unpacks into a folder of its own and the later one uses the first's."""

    def test_two_exports_of_one_commit_at_once_both_succeed(self):
        import subprocess
        import threading
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d) / "repo"
            repo.mkdir()
            for i in range(200):
                (repo / f"f{i}.txt").write_text("x" * 2000, encoding="ascii")
            git = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
            subprocess.run(git + ["init", "-q", str(repo)], check=True)
            subprocess.run(git + ["-C", str(repo), "add", "-A"], check=True)
            subprocess.run(git + ["-C", str(repo), "commit", "-q", "-m", "x"], check=True)
            src = ap.Source(name="t", git=str(repo), rev="HEAD")
            cache = Path(d) / "cache"
            out, errors = [], []

            def run():
                try:
                    out.append(ap.export(src, cache))
                except Exception as e:  # noqa: BLE001 -- the test reports it
                    errors.append(e)

            threads = [threading.Thread(target=run) for _ in range(4)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            self.assertEqual(errors, [])
            self.assertEqual(len({p for p, _ in out}), 1)
            dest = out[0][0]
            self.assertTrue((dest / ".potluck-export").exists())
            self.assertEqual(len(list(dest.glob("f*.txt"))), 200)
            self.assertEqual([p.name for p in dest.parent.iterdir() if ".tmp" in p.name], [])


class Bridge(unittest.TestCase):
    """2026-10-30: the cluster file's `bridge` -- the nodes the hub takes from the house network, and its port."""

    BOARDS = [(0x6300, "A"), (0x7368, "B")]

    def test_a_tablet_is_taken_with_the_default_port(self) -> None:
        self.assertEqual(ap.check_bridge({"nodes": [{"node": "00fd", "label": "Tablet"}]}, self.BOARDS),
                         ({0x00FD: "Tablet"}, ap.BRIDGE_PORT))
        self.assertEqual(ap.check_bridge(None, self.BOARDS), ({}, None))

    def test_what_is_refused(self) -> None:
        bad = [
            {"nodes": [{"node": "00fe", "label": "PC"}]},          # the PC's own id: not a bridged one
            {"nodes": [{"node": "6301", "label": "T"}]},           # a board's range
            {"nodes": [{"node": "00fd", "label": "A"}]},           # a board's label
            {"nodes": [{"node": "00fd", "label": "T"}, {"node": "00fd", "label": "U"}]},
            {"nodes": [{"node": f"{0xF0 + i:04x}", "label": f"T{i}"} for i in range(ap.BRIDGE_MAX + 1)]},
            {"nodes": []},
            {"port": 0, "nodes": [{"node": "00fd", "label": "T"}]},
            {"nodes": [{"node": "00fd", "label": "T", "key": "x"}]},
            {"nodes": [{"node": 253, "label": "T"}]},
            {"port": 8472},
        ]
        for b in bad:
            with self.subTest(b=b), self.assertRaises(ap.ApplyError):
                ap.check_bridge(b, self.BOARDS)

    def test_an_inbox_or_a_lender_may_be_pinned_to_a_bridged_node_and_nothing_else_to_a_host(self) -> None:
        from potluck import manifest as mf

        def doc(module, cfg, node):
            return {"schema": 1, "system": "lab", "min_core_version": 1,
                    "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": node, "label": "h", "kind": "host"}],
                    "actors": [{"name": "x", "module": module, "latency_class": 4, "pin": node, "config": cfg}]}
        for module, cfg in (("builtin:inbox", {"outputs": "battery:f32:percent"}), ("builtin:mc_lender", {"slots": 2})):
            dp.compile_image(mf.parse(doc(module, cfg, 0x00FD)), 1, ".")  # a bridged node: allowed
            with self.assertRaises(dp.DeployError):
                dp.compile_image(mf.parse(doc(module, cfg, 0x00FE)), 1, ".")  # the PC's node: firmware runs built-ins
        with self.assertRaises(dp.DeployError):
            dp.compile_image(mf.parse(doc("builtin:die_temp", {}, 0x00FD)), 1, ".")

    def test_each_bridged_node_is_one_more_peer_on_every_board(self) -> None:
        c = ap.Cluster.__new__(ap.Cluster)
        c.boards, c.cells, c.foremen, c.parents, c.bridged = list(self.BOARDS), {}, [], {}, {0x00FD: "Tablet"}
        budget = ap.peer_budget(c)
        self.assertEqual({n: (u["peers"], u["host"]) for n, u in budget.items()}, {0x6300: (3, 2), 0x7368: (3, 2)})


if __name__ == "__main__":
    unittest.main()
