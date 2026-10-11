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


if __name__ == "__main__":
    unittest.main()
