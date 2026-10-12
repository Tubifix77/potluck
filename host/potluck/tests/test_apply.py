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

    def test_bad_files_are_refused_with_a_reason(self):
        with self.assertRaisesRegex(ap.ApplyError, "not a cluster file"):
            ap.load_cluster(str(write(self.d, "x.json", {"schema": "nope"})))
        with self.assertRaisesRegex(ap.ApplyError, "cluster name"):
            self.cluster(system="two words")
        with self.assertRaisesRegex(ap.ApplyError, "appears twice"):
            self.cluster(boards=[{"node": "0100"}, {"node": "0100"}])
        with self.assertRaisesRegex(ap.ApplyError, "unknown key"):
            self.cluster(colour="blue")

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


if __name__ == "__main__":
    unittest.main()
