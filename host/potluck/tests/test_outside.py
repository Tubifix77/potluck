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
#: node-7368 hw/die_temp 0x57d40a8e flags 01 (exposed) -- each followed by its name, the path after potluck://m3-test/.
GOLDEN = ("504f54440301000004000000bf6939c8cbb48a3f0b000000ffff09076d332d74657374"
          "0003764fb14b05136e6f64652d363330302f6163742f6272616b65"
          "8218dbd103136e6f64652d363330302f6c65642f6c6576656c"
          "8e0ad45701156e6f64652d373336382f68772f6469655f74656d70")


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


#: compile_image() of a present actor (broker 192.168.0.77:1883, keep-alive 10 s, every 2 s, A and B eligible) and
#: led_b on B (255, 140, 0, steady) at counter 3. tests/test_led_switch.cpp parses these bytes.
PRESENT_GOLDEN = ("504f54440102000003000000cd8c12d9a4f8758a25000000feff0a159e35af6ed00702006301687301c0a8004d5b070a00"
                  "68730b08a41b2e6bff8c0000")


#: The same with TLS: tls_ca a stand-in DER (30 03 02 01 01, a SEQUENCE holding INTEGER 1 -- its shape is all the host
#: checks), at counter 3. Version 4; the present config's flags byte 01; then the guest section (0), the outside
#: section (0) and the CA, 0005 bytes. tests/test_deploy.cpp parses these bytes.
TLS_CA = "MAMCAQE="
PRESENT_TLS_GOLDEN = ("504f54440402000003000000a7cd7195e09bef4625000000feff0a159e35af6ed00702006301687301c0a8004d5b070a01"
                      "68730b08a41b2e6bff8c0000000005003003020101")


def present_manifest(**cfg) -> dict:
    return {"schema": 1, "system": "lab", "min_core_version": 1,
            "nodes": [{"node_id": 0x6300, "label": "A"}, {"node_id": 0x7368, "label": "B"}],
            "actors": [{"name": "present", "module": "builtin:present", "latency_class": 4,
                        "config": {"broker": "192.168.0.77:1883", "keepalive_s": 10, "period_ms": 2000, **cfg}},
                       {"name": "led_b", "module": "builtin:led_switch", "latency_class": 3, "pin": 0x7368,
                        "config": {"r": 255, "g": 140, "b": 0}}]}


class PresentActor(unittest.TestCase):
    def test_the_present_and_led_switch_declarations_compile_to_the_node_format(self) -> None:
        self.assertEqual(dp.compile_image(parse(present_manifest()), 3).hex(), PRESENT_GOLDEN)

    def test_tls_puts_the_brokers_ca_in_a_version_4_image(self) -> None:
        self.assertEqual(dp.compile_image(parse(present_manifest(tls_ca=TLS_CA)), 3).hex(), PRESENT_TLS_GOLDEN)
        pem = "-----BEGIN CERTIFICATE-----\nMAMC\nAQE=\n-----END CERTIFICATE-----\n"
        # A PEM is a different manifest (another package digest, bytes 12..19); the rest is the same image.
        self.assertEqual(dp.compile_image(parse(present_manifest(tls_ca=pem)), 3)[20:].hex(), PRESENT_TLS_GOLDEN[40:])

    def test_a_ca_that_is_not_one_der_certificate_is_refused(self) -> None:
        import base64

        for bad in ("not base64!", base64.b64encode(b"\x02\x01\x01").decode(),
                    base64.b64encode(b"\x30" + bytes(dp.MAX_BROKER_CA)).decode(),
                    "-----BEGIN PRIVATE KEY-----\nMAMCAQE=\n-----END PRIVATE KEY-----"):
            with self.assertRaises(dp.DeployError):
                dp.compile_image(parse(present_manifest(tls_ca=bad)), 3)

    def test_a_broker_that_is_not_an_address_and_a_keepalive_out_of_range_are_refused(self) -> None:
        for bad in ({"broker": "broker.local"}, {"broker": "192.168.0.300"}, {"keepalive_s": 2}, {"colour": 1}):
            with self.assertRaises(dp.DeployError):
                dp.compile_image(parse(present_manifest(**bad)), 3)


if __name__ == "__main__":
    unittest.main()
