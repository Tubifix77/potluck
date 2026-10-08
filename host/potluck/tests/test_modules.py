"""M8.2 (PS-1): an external component's modules, described by its potluck-modules.json."""

from __future__ import annotations

import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp  # noqa: E402
from potluck import modules as mods  # noqa: E402
from potluck.manifest import parse  # noqa: E402
from potluck.paths import path_hash  # noqa: E402

DEMO = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "test_components",
                    "pot_app_demo", "potluck-modules.json")


def manifest(actors: list[dict]) -> dict:
    return {
        "schema": 1,
        "system": "lab",
        "min_core_version": 1,
        "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": 0x7368, "label": "B"},
                  {"node_id": 0x8160, "label": "C"}],
        "actors": actors,
    }


def body(img: bytes) -> list[tuple[int, int, bytes]]:
    out, at = [], dp.IMAGE_HEADER_LEN
    while at < len(img):
        node, typ, n = struct.unpack_from("<HBB", img, at)
        out.append((node, typ, img[at + 4:at + 4 + n]))
        at += 4 + n
    return out


class Descriptor(unittest.TestCase):
    def setUp(self) -> None:
        mods.clear()

    def tearDown(self) -> None:
        mods.clear()

    def test_the_demo_components_descriptor_loads(self) -> None:
        specs = mods.load(DEMO)
        self.assertEqual([s.module for s in specs], ["ext:pot_app_demo/demo_counter", "ext:pot_app_demo/demo_roamer"])
        self.assertTrue(mods.is_portable("ext:pot_app_demo/demo_roamer"))
        self.assertFalse(mods.is_portable("ext:pot_app_demo/demo_counter"))

    def test_a_type_outside_the_components_block_or_below_0x80_is_refused(self) -> None:
        bad = {"component": "x", "types": [128, 143], "modules": [{"module": "ext:x/a", "type": 150, "placement": "pinned"}]}
        with self.assertRaises(mods.ModuleError):
            mods.parse_descriptor(bad)
        bad["types"] = [100, 143]
        with self.assertRaises(mods.ModuleError):
            mods.parse_descriptor(bad)

    def test_two_components_claiming_one_type_are_refused_naming_both(self) -> None:
        mods.load(DEMO)
        other = {"component": "intruder", "types": [240, 255],
                 "modules": [{"module": "ext:intruder/x", "type": 240, "placement": "pinned"}]}
        with self.assertRaises(mods.ModuleError) as cm:
            mods.register(mods.parse_descriptor(other))
        self.assertIn("pot_app_demo", str(cm.exception))
        self.assertIn("intruder", str(cm.exception))


class Compile(unittest.TestCase):
    def setUp(self) -> None:
        mods.clear()
        mods.load(DEMO)

    def tearDown(self) -> None:
        mods.clear()

    def test_a_pinned_external_actor_compiles_with_its_own_bytes(self) -> None:
        m = parse(manifest([{"name": "count_b", "module": "ext:pot_app_demo/demo_counter", "latency_class": 4,
                             "pin": 0x7368, "config": {"out": "potluck://lab/node-7368/app/count", "period_ms": 500}}]))
        [(node, typ, cfg)] = body(dp.compile_image(m, 9))
        self.assertEqual((node, typ), (0x7368, 0xF0))
        self.assertEqual(cfg, struct.pack("<IH", path_hash("potluck://lab/node-7368/app/count"), 500))

    def test_a_portable_external_actor_gets_the_standard_header_then_its_own_bytes(self) -> None:
        m = parse(manifest([{"name": "roamer", "module": "ext:pot_app_demo/demo_roamer", "latency_class": 4,
                             "config": {"period_ms": 200, "step": 3}}]), require_placement=True)
        [(node, typ, cfg)] = body(dp.compile_image(m, 9))
        self.assertEqual((node, typ), (0xFFFE, 0xF1))
        key, period, count = struct.unpack_from("<IHB", cfg)
        self.assertEqual(key, path_hash("potluck://lab/act/roamer/out"))
        self.assertEqual((period, count), (200, 3))
        self.assertEqual(len(cfg), 7 + 3 * 3 + 1)
        self.assertEqual(cfg[-1], 3)

    def test_an_undescribed_module_and_a_missing_value_are_refused(self) -> None:
        mods.clear()
        m = parse(manifest([{"name": "count_b", "module": "ext:pot_app_demo/demo_counter", "latency_class": 4,
                             "pin": 0x7368, "config": {"out": "potluck://lab/x"}}]))
        with self.assertRaises(dp.DeployError):
            dp.compile_image(m, 1)  # no descriptor loaded
        mods.load(DEMO)
        m = parse(manifest([{"name": "count_b", "module": "ext:pot_app_demo/demo_counter", "latency_class": 4,
                             "pin": 0x7368, "config": {}}]))
        with self.assertRaises(dp.DeployError):
            dp.compile_image(m, 1)  # 'out' has no default

    def test_system_prefix_is_the_hash_state_a_board_can_continue(self) -> None:
        spec = mods.parse_descriptor({"component": "y", "types": [128, 143], "modules": [
            {"module": "ext:y/a", "type": 128, "placement": "every",
             "config": [{"name": "prefix", "kind": "system_prefix"}]}]})[0]
        m = parse(manifest([]))
        (state,) = struct.unpack("<I", mods.encode_config(spec, {}, m, "t"))
        # FNV-1a continued from the state over the rest equals the hash of the whole path.
        h = state
        for b in b"node-7368/rf/link-8160/mean":
            h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
        self.assertEqual(h, path_hash("potluck://lab/node-7368/rf/link-8160/mean"))


if __name__ == "__main__":
    unittest.main()
