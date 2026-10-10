"""M7: guest actors, host half -- the author's bundle, the owner's manifest, the blob in a version-2 image.

The golden blob below is checked here AND parsed and verified by tests/test_guest.cpp, so the host tools
and the node cannot drift apart without one suite failing.
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp
from potluck import ed25519_ref as ed
from potluck import enrol as en
from potluck import guest as gs
from potluck import reconcile as rc
from potluck.manifest import parse
from potluck.paths import path_hash
from potluck.signing import KeyPair

ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "..")
WASM = os.path.join(ROOT, "guests", "build")

#: The guest blob for guests/build/endless_tick.wasm: no input, one i32 output at
#: potluck://lab/act/alarm/out, 20,000 fuel, one page, by author 7 under the test CA (seeds 0..31 and
#: 96..127, certificate issued 1790000000). tests/test_guest.cpp parses it and verifies the signature.
GOLDEN_BLOB = (
    "5047535401000101204e0000883bc3c202504e433101030700ed4242ea803bb16a174553b456dddfc6908ecab1c101fe6a"
    "b21e2baa0617795b7d43a63482993fd57c3846ebc2e3f677a9ef73f256d038602016954fc1aade1209bfa1909537cce97c"
    "87cbc023d2ea71f42cdf0958c8a9085cd267e7c22907ae6313161c2895b205656568d1caa27e4d824c9b372e95d2578a33"
    "f051727a70f8fd2ae15c4b9621c0aeb5f6e9393cf48553e001e8123242b378fee912bad02e164fe85eee63a74a0f006173"
    "6d0100000001050160017f00030201000504010101010616037f014180c0000b7f004180c0000b7f004180c0000b072c04"
    "066d656d6f72790200047469636b00000a5f5f646174615f656e6403010b5f5f686561705f6261736503020a3701350101"
    "7f23808080800041106b210103402001200041ed9c998e046c41b9e0006a36020c2001410c6a2100200128020c21000c00"
    "0b0b"
)


def keys():
    ca = KeyPair("ed25519", "ca", "test-ca", ed.public_key(bytes(range(32))), bytes(range(32)))
    author = bytes(range(96, 128))
    cert = en.build_cert(ca, 7, ed.public_key(author), issued=1790000000, role=en.ROLE_GUEST)
    return ca, author, cert


def module(name: str) -> bytes:
    with open(os.path.join(WASM, name + ".wasm"), "rb") as f:
        return f.read()


class Bundle(unittest.TestCase):
    def test_the_golden_blob_is_what_the_tools_make(self):
        ca, author, cert = keys()
        b = gs.make_bundle("endless_tick", module("endless_tick"), author, cert)
        self.assertEqual(gs.verify_bundle(b, ca.public), 7)
        blob = gs.encode_blob(b, [], [(path_hash("potluck://lab/act/alarm/out"), gs.TYPES["i32"])], 20000, 1)
        self.assertEqual(blob.hex(), GOLDEN_BLOB)

    def test_a_bundle_survives_json_and_says_who_wrote_it(self):
        ca, author, cert = keys()
        b = gs.make_bundle("overheat_alarm", module("overheat_alarm"), author, cert)
        again = gs.bundle_from_dict(json.loads(json.dumps(gs.bundle_to_dict(b))))
        self.assertEqual(again, b)
        self.assertEqual(gs.verify_bundle(again, ca.public), 7)

    def test_a_changed_module_another_ca_or_the_wrong_key_is_refused(self):
        ca, author, cert = keys()
        b = gs.make_bundle("overheat_alarm", module("overheat_alarm"), author, cert)
        d = gs.bundle_to_dict(b)
        tampered = bytearray(b.module)
        tampered[-2] ^= 1
        with self.assertRaises(gs.GuestError):  # the bundle's own sha256 no longer matches
            gs.bundle_from_dict(dict(d, module=__import__("base64").b64encode(bytes(tampered)).decode()))
        forged = gs.Bundle(b.name, bytes(tampered), b.cert, b.sig)
        with self.assertRaises(gs.GuestError):
            gs.verify_bundle(forged, ca.public)
        with self.assertRaises(gs.GuestError):
            gs.verify_bundle(b, ed.public_key(bytes(range(200, 232))))
        with self.assertRaises(gs.GuestError):  # signing with a key the certificate does not name
            gs.make_bundle("x", module("overheat_alarm"), bytes(range(150, 182)), cert)
        deploy_cert = en.build_cert(ca, 0, ed.public_key(author), issued=1790000000, role=en.ROLE_DEPLOY)
        with self.assertRaises(en.EnrolError):  # a deploy key is not a guest author
            gs.make_bundle("x", module("overheat_alarm"), author, deploy_cert)
        with self.assertRaises(gs.GuestError):
            gs.make_bundle("x", b"not wasm", author, cert)

    def test_outputs_are_named_and_typed(self):
        self.assertEqual(gs.parse_outputs("out:f32 alarm:i32", "t"), [("out", 4), ("alarm", 2)])
        for bad in ("", "out", "out:f64", "a:i32 a:f32", "a/b:i32", " ".join(f"o{i}:i32" for i in range(9))):
            with self.assertRaises(gs.GuestError):
                gs.parse_outputs(bad, "t")


def guest_manifest(bundle_file: str, sha: str, **cfg) -> dict:
    config = {"bundle": bundle_file, "sha256": sha, "outputs": "out:f32 alarm:i32 count:i32", "fuel": 100000,
              "memory_kib": 64, "period_ms": 1000}
    config.update(cfg)
    return {
        "schema": 1,
        "system": "lab",
        "min_core_version": 1,
        "nodes": [
            {"node_id": 0x6300, "label": "A"},
            {"node_id": 0x7368, "label": "B", "owns": [
                {"path": "potluck://lab/node-7368/hw/die_temp", "unit": "celsius", "kind": "sampled",
                 "access": "read", "latency_class": 4, "staleness_bound_ms": 5000}]},
            {"node_id": 0x8160, "label": "C"},
            {"node_id": 0x00FE, "label": "pc", "kind": "host", "placement": True},
        ],
        "actors": [{"name": "alarm", "module": "guest:overheat_alarm", "latency_class": 4,
                    "needs": ["potluck://lab/node-7368/hw/die_temp"], "config": config}],
    }


class Image(unittest.TestCase):
    def setUp(self):
        ca, author, cert = keys()
        self.bundle = gs.make_bundle("overheat_alarm", module("overheat_alarm"), author, cert)
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "overheat_alarm.guest.json")
        with open(self.path, "w", encoding="ascii") as f:
            json.dump(gs.bundle_to_dict(self.bundle), f)

    def tearDown(self):
        self.dir.cleanup()

    def test_a_guest_compiles_to_a_version_2_image_carrying_its_blob(self):
        img = dp.compile_image(parse(guest_manifest("overheat_alarm.guest.json", self.bundle.sha256)), 25,
                               guest_root=self.dir.name)
        magic, version, count = struct.unpack_from("<IBB", img)
        self.assertEqual((magic, version, count), (dp.IMAGE_MAGIC, 2, 1))
        (body_len,) = struct.unpack_from("<I", img, 20)
        node, typ, cfg_len = struct.unpack_from("<HBB", img, 24)
        self.assertEqual((node, typ), (0xFFFE, dp.ACTOR_GUEST))
        cfg = img[28:28 + cfg_len]
        key, period, n = struct.unpack_from("<IHB", cfg)
        self.assertEqual(key, path_hash("potluck://lab/act/alarm/out"))
        self.assertEqual(period, 1000)
        nodes = [struct.unpack_from("<HB", cfg, 7 + 3 * i)[0] for i in range(n)]
        self.assertEqual(sorted(nodes), [0x6300, 0x7368, 0x8160])  # never the host: it has no sandbox
        self.assertEqual(cfg[-1], 0)  # guest index
        at = 24 + body_len
        self.assertEqual(img[at], 1)
        (blen,) = struct.unpack_from("<I", img, at + 1)
        blob = img[at + 5:at + 5 + blen]
        self.assertEqual(at + 5 + blen, len(img))
        want = gs.encode_blob(
            self.bundle, [path_hash("potluck://lab/node-7368/hw/die_temp")],
            [(path_hash(f"potluck://lab/act/alarm/{n}"), t) for n, t in (("out", 4), ("alarm", 2), ("count", 2))],
            100000, 1)
        self.assertEqual(blob, want)
        # B owns the input, so it has the gravity: the guest goes where its data is.
        self.assertEqual(rc.plan(parse(guest_manifest("x", "y")))[0].order[0], 0x7368)

    def test_the_owner_pins_the_exact_module(self):
        m = parse(guest_manifest("overheat_alarm.guest.json", hashlib.sha256(b"another").hexdigest()))
        with self.assertRaises(dp.DeployError):
            dp.compile_image(m, 1, guest_root=self.dir.name)

    def test_every_guest_setting_is_checked(self):
        sha = self.bundle.sha256
        for bad in (dict(fuel=10), dict(fuel=10 ** 9), dict(memory_kib=128), dict(outputs="out:f64"),
                    dict(colour="red"), dict(bundle="missing.guest.json")):
            with self.subTest(bad=bad), self.assertRaises(dp.DeployError):
                dp.compile_image(parse(guest_manifest("overheat_alarm.guest.json", sha, **bad)), 1,
                                 guest_root=self.dir.name)
        doc = guest_manifest("overheat_alarm.guest.json", sha)
        doc["actors"][0]["module"] = "guest:another_name"
        with self.assertRaises(dp.DeployError):
            dp.compile_image(parse(doc), 1, guest_root=self.dir.name)
        doc = guest_manifest("overheat_alarm.guest.json", sha)
        doc["actors"][0]["latency_class"] = 1  # pinned by physics: a guest must be portable
        with self.assertRaises(dp.DeployError):
            dp.compile_image(parse(doc), 1, guest_root=self.dir.name)

    def test_the_host_checks_every_author_before_anything_leaves(self):
        ca, author, cert = keys()
        m = parse(guest_manifest("overheat_alarm.guest.json", self.bundle.sha256))
        self.assertEqual(dp.check_guest_authors(m, ca.public, self.dir.name), [("alarm", 7)])
        with self.assertRaises(dp.DeployError):  # another cluster's CA
            dp.check_guest_authors(m, ed.public_key(bytes(range(200, 232))), self.dir.name)
        d = gs.bundle_to_dict(self.bundle)
        sig = bytearray(bytes.fromhex(d["sig"]))
        sig[10] ^= 1
        with open(self.path, "w", encoding="ascii") as f:
            json.dump(dict(d, sig=sig.hex()), f)
        dp.compile_image(m, 1, guest_root=self.dir.name)  # the compiler only pins the module ...
        with self.assertRaises(dp.DeployError):  # ... the author check is what refuses the signature
            dp.check_guest_authors(m, ca.public, self.dir.name)

    def test_without_guests_the_image_stays_version_1(self):
        doc = guest_manifest("x", "y")
        doc["actors"] = [{"name": "ticker", "module": "builtin:ticker", "latency_class": 3, "config": {}}]
        self.assertEqual(dp.compile_image(parse(doc), 1)[4], 1)


if __name__ == "__main__":
    unittest.main()
