"""M20: the store (potluck.store) -- an app fetched from a pretend GitHub, checked, pinned, certified and written into a
cell's cluster file; nothing here reaches the network."""

from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import apply as ap  # noqa: E402
from potluck import ed25519_ref as ed  # noqa: E402
from potluck import enrol as en  # noqa: E402
from potluck import guest as gu  # noqa: E402
from potluck import signing as sg  # noqa: E402
from potluck import store as so  # noqa: E402

WASM = (Path(__file__).resolve().parents[3] / "guests" / "build" / "overheat_alarm.wasm").read_bytes()


def app_doc(secret: bytes, wasm: bytes = WASM, **over) -> dict:
    d = {"type": so.APP_TYPE, "name": "overheat_alarm", "description": "warns when a temperature runs high",
         "author_key": ed.public_key(secret).hex(), "module": "build/overheat_alarm.wasm",
         "sha256": hashlib.sha256(wasm).hexdigest(), "sig": ed.sign(secret, gu.signed_message(wasm)).hex(),
         "inputs": [{"name": "temperature", "type": "f32", "unit": "celsius"}],
         "outputs": "out:f32 alarm:i32 count:i32", "fuel": 100000, "period_ms": 1000}
    d.update(over)
    return d


def github(files: dict[str, bytes]):
    def fetch(url: str) -> bytes:
        for k, v in files.items():
            if url.endswith(k):
                return v
        raise OSError("404")
    return fetch


class Fetching(unittest.TestCase):
    def setUp(self) -> None:
        self.sk = os.urandom(32)

    def test_an_app_signed_by_its_author_is_fetched_and_checked(self):
        f = github({"potluck-app.json": json.dumps(app_doc(self.sk)).encode(), "build/overheat_alarm.wasm": WASM})
        app, module = so.fetch("acme/alarm", "v1.0.0", f)
        self.assertEqual((app.name, module, app.inputs[0]["type"]), ("overheat_alarm", WASM, "f32"))

    def test_a_tampered_module_or_a_foreign_signature_is_refused(self):
        bad = WASM[:-1] + bytes([WASM[-1] ^ 1])
        f = github({"potluck-app.json": json.dumps(app_doc(self.sk)).encode(), "build/overheat_alarm.wasm": bad})
        with self.assertRaisesRegex(so.StoreError, "SHA-256"):
            so.fetch("acme/alarm", "main", f)
        other = os.urandom(32)
        doc = app_doc(self.sk, sig=ed.sign(other, gu.signed_message(WASM)).hex())
        f = github({"potluck-app.json": json.dumps(doc).encode(), "build/overheat_alarm.wasm": WASM})
        with self.assertRaisesRegex(so.StoreError, "signature"):
            so.fetch("acme/alarm", "main", f)

    def test_what_does_not_belong_in_a_repository_name_or_an_app_is_refused(self):
        f = github({})
        for repo, ref in (("acme", "main"), ("acme/alarm/x", "main"), ("acme/alarm", "../../etc"), ("a b/c", "main")):
            with self.assertRaises(so.StoreError):
                so.fetch(repo, ref, f)
        for over in ({"module": "../outside.wasm"}, {"name": "Bad Name"}, {"inputs": [{"name": "x", "type": "u64"}]},
                     {"fuel": 1}, {"type": "something-else"}):
            with self.assertRaises(so.StoreError):
                so.parse_app(app_doc(self.sk, **over))


class Feed(unittest.TestCase):
    def test_a_list_signed_by_the_store_key_is_shown_and_any_other_is_not(self):
        key = os.urandom(32)
        raw = json.dumps({"type": so.FEED_TYPE, "apps": [{"repo": "Tubifix77/potluck-store", "dir": "apps/overheat_alarm",
                                                          "name": "overheat_alarm", "description": "an alarm"}],
                          "removed": ["someone/bad-app"]}).encode()
        sig = ed.sign(key, so.feed_message(raw)).hex().encode()
        feed = so.fetch_feed(github({"feed.json": raw, "feed.sig": sig}), ed.public_key(key).hex())
        self.assertEqual((feed["apps"][0]["dir"], feed["removed"]), ("apps/overheat_alarm", ["someone/bad-app"]))
        with self.assertRaisesRegex(so.StoreError, "not signed"):  # one byte changed after signing
            so.fetch_feed(github({"feed.json": raw.replace(b"an alarm", b"an alarM"), "feed.sig": sig}),
                          ed.public_key(key).hex())
        with self.assertRaisesRegex(so.StoreError, "not signed"):  # signed by someone else
            so.fetch_feed(github({"feed.json": raw, "feed.sig": sig}), ed.public_key(os.urandom(32)).hex())
        self.assertEqual(len(bytes.fromhex(so.FEED_KEY)), 32)  # the store key's public half is built in

    def test_an_app_in_a_folder_is_fetched_from_there_and_a_folder_cannot_climb_out(self):
        sk = os.urandom(32)
        f = github({"apps/alarm/potluck-app.json": json.dumps(app_doc(sk)).encode(),
                    "apps/alarm/build/overheat_alarm.wasm": WASM})
        app, _ = so.fetch("Tubifix77/potluck-store", "HEAD", f, folder="apps/alarm")
        self.assertEqual(app.name, "overheat_alarm")
        with self.assertRaises(so.StoreError):
            so.fetch("Tubifix77/potluck-store", "HEAD", f, folder="apps/../../x")
        self.assertEqual(so.source("a/b", "apps/alarm"), "a/b/apps/alarm")


class Installing(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.cell = Path(self.tmp.name)
        self.ca = sg.generate("ca", "test")
        (self.cell / "ca.pub").write_text(json.dumps(self.ca.public_dict()))
        self.cluster = self.cell / "home.json"
        self.cluster.write_text(json.dumps({"schema": ap.SCHEMA, "system": "home",
                                            "boards": [{"node": "6300", "label": "A"}],
                                            "keys": {"ca": "ca.pub"}}, indent=2))
        self.sk = os.urandom(32)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def certify(self, author: int, key: bytes) -> bytes:
        return en.build_cert(self.ca, author, key, role=en.ROLE_GUEST)

    def test_the_first_install_pins_the_author_certifies_it_and_writes_the_cluster_file(self):
        app = so.parse_app(app_doc(self.sk))
        r = so.install(self.cluster, "acme/alarm", "v1.0.0", app, WASM,
                       ["potluck://home/node-6300/hw/die_temp"], self.certify)
        self.assertEqual(r["author"], so.FIRST_AUTHOR)
        b = gu.load_bundle(str(self.cell / "store" / "overheat_alarm.guest.json"))
        self.assertEqual(gu.verify_bundle(b, self.ca.public), so.FIRST_AUTHOR)  # what the boards check, again
        c = ap.load_cluster(str(self.cluster))  # the cluster file still loads, and names the fragment
        doc = json.loads(self.cluster.read_text())
        self.assertIn({"manifest": "store/overheat_alarm.json", "root": "."}, doc["package"]["manifests"])
        frag = json.loads((self.cell / "store" / "overheat_alarm.json").read_text())
        self.assertEqual(frag["actors"][0]["needs"], ["potluck://home/node-6300/hw/die_temp"])
        self.assertEqual(frag["system"], c.system)
        # the same app again (an update): the same author number, one fragment
        so.install(self.cluster, "acme/alarm", "v1.0.1", app, WASM, ["potluck://home/node-6300/hw/die_temp"],
                   self.certify)
        doc = json.loads(self.cluster.read_text())
        self.assertEqual(len(doc["package"]["manifests"]), 1)
        so.remove(self.cluster, "overheat_alarm")
        self.assertEqual(json.loads(self.cluster.read_text())["package"]["manifests"], [])

    def test_a_changed_key_is_a_new_author_and_is_refused(self):
        app = so.parse_app(app_doc(self.sk))
        so.install(self.cluster, "acme/alarm", "v1", app, WASM, ["potluck://home/node-6300/hw/die_temp"], self.certify)
        other = so.parse_app(app_doc(os.urandom(32)))
        with self.assertRaisesRegex(so.StoreError, "author key changed"):
            so.install(self.cluster, "acme/alarm", "v2", other, WASM, ["potluck://home/node-6300/hw/die_temp"],
                       self.certify)

    def test_every_input_must_be_bound(self):
        app = so.parse_app(app_doc(self.sk))
        with self.assertRaisesRegex(so.StoreError, "bind each"):
            so.install(self.cluster, "acme/alarm", "v1", app, WASM, [], self.certify)
        self.assertFalse((self.cell / "store").exists())  # refused before anything was written


if __name__ == "__main__":
    unittest.main()
