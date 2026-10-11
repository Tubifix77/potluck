"""M13: what the owner's package lets a system outside the cell do -- the `outside` map, `safety_relevant`, the
image's outside section and the WRITE's origin bit (ARCHITECTURE.md section 13, the review's item 1).

Exposure is opt-in, one resource at a time; a write from outside needs a declared, writable resource that is not
safety-relevant. The board that owns the resource checks it again (tests/test_namespace.cpp, test_remote_read.cpp);
these tests are the host's half, and GOLDEN is parsed byte for byte by tests/test_deploy.cpp.
"""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp  # noqa: E402
from potluck.manifest import MAX_OUTSIDE, ManifestErrors, outside_rules, parse  # noqa: E402
from potluck.ns_payloads import WRITE_FROM_OUTSIDE, Write  # noqa: E402
from potluck.paths import path_hash  # noqa: E402
from potluck.value import NsError, Value  # noqa: E402

LED = "potluck://m3-test/node-6300/led/level"
BRAKE = "potluck://m3-test/node-6300/act/brake"
DIE = "potluck://m3-test/node-7368/hw/die_temp"


def manifest() -> dict:
    return {
        "schema": 1,
        "system": "m3-test",
        "min_core_version": 1,
        "nodes": [
            {"node_id": 0x6300, "label": "a", "owns": [
                {"path": LED, "unit": "none", "access": "read_write", "latency_class": 3},
                {"path": BRAKE, "unit": "none", "access": "read_write", "latency_class": 3, "safety_relevant": True},
            ]},
            {"node_id": 0x7368, "label": "b"},
        ],
        "actors": [],
        "outside": {LED: "read_write", DIE: "read", BRAKE: "read"},
    }


#: compile_image(parse(manifest()), 4), decoded by hand on 2026-10-12: magic POTD, version 3, 1 actor (the
#: cluster name, "m3-test"), counter 4, digest, body 11 B; guest count 0; outside count 3, sorted by path:
#: act/brake 0x4bb14f76 flags 05 (exposed, safety-relevant), led/level 0xd1db1882 flags 03 (exposed, writable),
#: node-7368 hw/die_temp 0x57d40a8e flags 01 (exposed).
GOLDEN = ("504f54440301000004000000bf6939c8cbb48a3f0b000000ffff09076d332d74657374"
          "0003764fb14b058218dbd1038e0ad45701")


def errors_of(doc: dict) -> str:
    try:
        parse(doc)
    except ManifestErrors as e:
        return e.report()
    return ""


class OutsideManifest(unittest.TestCase):
    def test_the_rules_carry_exposure_writability_and_safety(self) -> None:
        self.assertEqual(outside_rules(parse(manifest())), [(BRAKE, 0x05), (LED, 0x03), (DIE, 0x01)])

    def test_nothing_is_exposed_unless_the_package_says_so(self) -> None:
        doc = manifest()
        del doc["outside"]
        doc["nodes"][0]["owns"][1]["safety_relevant"] = False
        m = parse(doc)
        self.assertEqual(outside_rules(m), [])
        self.assertNotIn("outside", m.to_dict())  # every manifest signed before M13 keeps its digest

    def test_a_safety_relevant_resource_is_never_writable_from_outside(self) -> None:
        doc = manifest()
        doc["outside"][BRAKE] = "read_write"
        self.assertIn("safety-relevant", errors_of(doc))

    def test_an_outside_write_needs_a_declared_writable_resource(self) -> None:
        doc = manifest()
        doc["outside"][DIE] = "read_write"  # not in any node's 'owns'
        self.assertIn("must be a resource a node of this manifest declares", errors_of(doc))
        doc = manifest()
        doc["nodes"][0]["owns"][0]["access"] = "read"
        self.assertIn("the resource's access is 'read'", errors_of(doc))

    def test_only_this_cells_resources_and_only_read_or_read_write(self) -> None:
        doc = manifest()
        doc["outside"]["potluck://other/node-6300/led/level"] = "read"
        self.assertIn("is not a resource of this cell", errors_of(doc))
        doc = manifest()
        doc["outside"][LED] = "write"
        self.assertIn("outside", errors_of(doc))
        doc = manifest()
        doc["nodes"][0]["owns"][0]["safety_relevant"] = "yes"
        self.assertIn("must be true or false", errors_of(doc))

    def test_at_most_max_outside_rules(self) -> None:
        doc = manifest()
        for i in range(MAX_OUTSIDE):
            doc["outside"][f"potluck://m3-test/node-7368/x/{i}"] = "read"
        self.assertIn(f"at most {MAX_OUTSIDE}", errors_of(doc))


class OutsideImage(unittest.TestCase):
    def test_the_outside_section_compiles_to_the_node_format(self) -> None:
        self.assertEqual(dp.compile_image(parse(manifest()), 4).hex(), GOLDEN)
        self.assertEqual(path_hash(LED), 0xD1DB1882)

    def test_a_package_without_rules_is_the_image_it_was_before(self) -> None:
        doc = manifest()
        del doc["outside"]
        doc["nodes"][0]["owns"][1]["safety_relevant"] = False
        img = dp.compile_image(parse(doc), 4)
        self.assertEqual(img[4], dp.IMAGE_VERSION)

    def test_rules_do_not_depend_on_the_order_written(self) -> None:
        doc = manifest()
        doc["outside"] = dict(reversed(list(doc["outside"].items())))
        self.assertEqual(dp.compile_image(parse(doc), 4).hex(), GOLDEN)


class OutsideWrite(unittest.TestCase):
    def test_the_origin_bit_round_trips(self) -> None:
        w = Write(path_hash=0x12345678, value=Value.of_i32(7), from_outside=True)
        raw = w.encode()
        self.assertEqual(int.from_bytes(raw[6:8], "little"), WRITE_FROM_OUTSIDE)
        self.assertTrue(Write.parse(raw).from_outside)
        self.assertEqual(Write(path_hash=1, value=Value.of_i32(7)).encode()[6:8], b"\x00\x00")

    def test_the_refusal_has_the_nodes_name(self) -> None:
        self.assertEqual(NsError.name_of(9), "NOT_OUTSIDE_WRITABLE")


if __name__ == "__main__":
    unittest.main()
