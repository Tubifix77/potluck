"""DEVELOPING-APPS.md's worked example, host half: its descriptor parses, and its manifest compiles to the
declaration bytes tests/test_hello_app.cpp parses on a real node -- so the manual's example cannot drift
from what the tools and the firmware actually do.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp
from potluck import modules as mods
from potluck.manifest import load

ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "..")
APP = os.path.join(ROOT, "examples", "apps")

#: node 0xFFFF (every node), type 0xF0, 6 config bytes: system_prefix of potluck://lab/, period_ms 5000.
GOLDEN_DECL = "fffff006271139618813"


class HelloExample(unittest.TestCase):
    def setUp(self):
        mods.clear()
        self.specs = mods.load(os.path.join(APP, "pot_hello", "potluck-modules.json"))

    def tearDown(self):
        mods.clear()

    def test_the_descriptor_describes_one_every_node_actor_in_potlucks_own_block(self):
        self.assertEqual([(s.module, s.type, s.placement) for s in self.specs], [("ext:hello/peers", 0xF0, "every")])

    def test_the_manifest_compiles_to_the_bytes_the_node_parses(self):
        img = dp.compile_image(load(os.path.join(APP, "hello.json")), 1)
        self.assertEqual(img[4], dp.IMAGE_VERSION)
        self.assertEqual(img[dp.IMAGE_HEADER_LEN:].hex(), GOLDEN_DECL)

    def test_a_config_outside_the_descriptors_range_is_refused(self):
        m = load(os.path.join(APP, "hello.json"))
        m.actors[0].config["period_ms"] = 100
        with self.assertRaises(dp.DeployError):
            dp.compile_image(m, 1)


class PlacementReport(unittest.TestCase):
    """`python -m potluck.reconcile --modules` plans an app's portable actors (the blind reader's finding 20)."""

    def test_an_apps_portable_actor_is_planned_only_with_its_descriptor(self):
        import contextlib
        import io
        import json
        import tempfile

        from potluck import reconcile as rc

        with tempfile.TemporaryDirectory() as d:
            desc = os.path.join(d, "potluck-modules.json")
            man = os.path.join(d, "m.json")
            with open(desc, "w", encoding="ascii") as f:
                json.dump({"component": "pot_x", "types": [144, 159],
                           "modules": [{"module": "ext:x/roam", "type": 144, "placement": "portable", "config": []}]}, f)
            with open(man, "w", encoding="ascii") as f:
                json.dump({"schema": 1, "system": "lab", "min_core_version": 1,
                           "nodes": [{"node_id": 25344, "label": "A"}, {"node_id": 29544, "label": "B"}],
                           "actors": [{"name": "roam", "module": "ext:x/roam", "latency_class": 4}]}, f)
            for argv, planned in (([man], False), ([man, "--modules", desc], True)):
                mods.clear()
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(rc.main(argv), 0)
                self.assertEqual("roam -> potluck://lab/act/roam/out" in out.getvalue(), planned, out.getvalue())
            mods.clear()

    def test_an_every_node_actor_is_reported_as_such_not_as_portable(self):
        import contextlib
        import io

        from potluck import reconcile as rc

        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(rc.main([os.path.join(APP, "hello.json"), "--modules",
                                      os.path.join(APP, "pot_hello", "potluck-modules.json")]), 0)
        mods.clear()
        self.assertIn("hello: runs on every node", out.getvalue())
        self.assertNotIn("hello: portable", out.getvalue())


if __name__ == "__main__":
    unittest.main()
