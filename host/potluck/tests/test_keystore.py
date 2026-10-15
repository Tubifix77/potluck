"""M20: the owner's keys encrypted at rest, and their backup (potluck.keystore)."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import keystore as ks  # noqa: E402
from potluck import signing  # noqa: E402

PASS = "a long enough passphrase"


class Sealing(unittest.TestCase):
    def test_what_is_sealed_opens_with_the_passphrase_and_nothing_else(self):
        doc = ks.seal({"ca.key": b"secret one", "deploy.key": b"secret two"}, PASS, n=2 ** 4)
        self.assertNotIn("secret", json.dumps(doc))
        self.assertEqual(ks.open_sealed(doc, PASS), {"ca.key": b"secret one", "deploy.key": b"secret two"})
        with self.assertRaisesRegex(ks.KeystoreError, "wrong passphrase"):
            ks.open_sealed(doc, PASS + "!")

    def test_a_changed_file_is_refused_header_and_contents_alike(self):
        doc = ks.seal({"ca.key": b"secret"}, PASS, n=2 ** 4)
        bad = dict(doc, names=["ca.key", "extra.key"])
        with self.assertRaisesRegex(ks.KeystoreError, "changed"):
            ks.open_sealed(bad, PASS)
        ct = bytearray(__import__("base64").b64decode(doc["ct"]))
        ct[0] ^= 1
        with self.assertRaises(ks.KeystoreError):
            ks.open_sealed(dict(doc, ct=__import__("base64").b64encode(bytes(ct)).decode()), PASS)

    def test_a_short_passphrase_is_refused(self):
        with self.assertRaises(ks.KeystoreError):
            ks.seal({"ca.key": b"x"}, "short", n=2 ** 4)


class Using(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)

    def tearDown(self):
        ks.lock()
        self.tmp.cleanup()

    def test_read_key_finds_a_key_in_an_unlocked_vault_when_its_file_is_gone(self):
        kp = signing.generate("deploy", "test")
        signing.write_keypair(kp, str(self.d / "deploy"))
        key_path = self.d / "deploy.key"
        content = key_path.read_bytes()
        (self.d / "vault.json").write_text(json.dumps(ks.seal({"deploy.key": content}, PASS, n=2 ** 4)), encoding="utf-8")
        key_path.unlink()
        with self.assertRaises(OSError):
            signing.read_key(str(key_path))  # locked: the key is not there
        ks.unlock(self.d / "vault.json", PASS)
        self.assertEqual(signing.read_key(str(key_path)).public, kp.public)
        ks.lock()
        with self.assertRaises(OSError):
            signing.read_key(str(key_path))  # locked again: forgotten

    def test_backup_and_restore_carry_a_cell_to_a_new_folder(self):
        (self.d / "ca.key").write_bytes(b'{"secret": 1}')
        (self.d / "ca.pub").write_bytes(b"pub")
        (self.d / "nodes.json").write_bytes(b'{"schema": 1, "nodes": {}}')
        cluster = self.d / "home.json"
        cluster.write_bytes(b'{"schema": "potluck-cluster/1"}')
        os.environ["POTLUCK_PASSPHRASE"] = PASS
        try:
            out = self.d / "backup.json"
            self.assertEqual(ks.main(["backup", str(self.d), "--cluster", str(cluster), "--out", str(out)]), 0)
            self.assertNotIn(b"secret", out.read_bytes())
            new = self.d / "newpc"
            self.assertEqual(ks.main(["restore", str(out), "--to", str(new)]), 0)
            self.assertEqual((new / "ca.key").read_bytes(), b'{"secret": 1}')
            self.assertEqual(sorted(p.name for p in new.iterdir()), ["ca.key", "ca.pub", "home.json", "nodes.json"])
            self.assertEqual(ks.main(["restore", str(out), "--to", str(new)]), 1)  # never over existing files
        finally:
            del os.environ["POTLUCK_PASSPHRASE"]


if __name__ == "__main__":
    unittest.main()
