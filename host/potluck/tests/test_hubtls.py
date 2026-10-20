"""2026-10-20: the hub on the house network, set up from the PC (potluck.hubtls, POST /api/setup/listen)."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import hubtls  # noqa: E402


class Listen(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.env = mock.patch.dict(os.environ, {"LOCALAPPDATA": self.tmp.name})  # never the owner's tray.json
        self.env.start()
        self.tray = Path(self.tmp.name) / "potluck" / "tray.json"
        self.tray.parent.mkdir(parents=True)
        self.tray.write_text(json.dumps({"cluster": "D:/cell/home.json", "desk": {"exe": "x.exe"},
                                         "hub_args": ["--pull-ms", "2000"]}), encoding="utf-8")

    def tearDown(self) -> None:
        self.env.stop()
        self.tmp.cleanup()

    def cfg(self) -> dict:
        return json.loads(self.tray.read_text(encoding="utf-8"))

    def test_on_makes_a_certificate_for_the_address_and_keeps_it_the_second_time(self) -> None:
        r = hubtls.set_listen(True, "192.168.0.7")
        self.assertEqual((r["certificate"], r["listen"], r["address"]), ("made", "0.0.0.0:8443", "192.168.0.7"))
        cert, key = hubtls.cert_paths()
        self.assertTrue(hubtls.covers(cert.read_bytes(), "192.168.0.7"))
        self.assertTrue(hubtls.covers(cert.read_bytes(), "127.0.0.1"))
        self.assertFalse(hubtls.covers(cert.read_bytes(), "192.168.0.8"))
        self.assertEqual(self.cfg()["hub_args"], ["--pull-ms", "2000", "--listen", "0.0.0.0:8443",
                                                  "--tls-cert", str(cert), "--tls-key", str(key)])
        self.assertEqual(self.cfg()["desk"], {"exe": "x.exe"})  # the rest of tray.json is kept
        again = hubtls.set_listen(True, "192.168.0.7")
        self.assertEqual((again["certificate"], again["fp"]), ("kept", r["fp"]))  # HA's pairing and a phone's trust hold
        self.assertEqual(self.cfg()["hub_args"].count("--listen"), 1)
        moved = hubtls.set_listen(True, "192.168.0.9")  # a new address: a new certificate (and a new fingerprint)
        self.assertEqual(moved["certificate"], "made")
        self.assertNotEqual(moved["fp"], r["fp"])

    def test_the_certificate_is_its_own_ca_so_a_phone_can_trust_it(self) -> None:
        from cryptography import x509
        hubtls.set_listen(True, "10.0.0.5")
        cert = x509.load_pem_x509_certificate(hubtls.cert_paths()[0].read_bytes())
        self.assertTrue(cert.extensions.get_extension_for_class(x509.BasicConstraints).value.ca)

    def test_off_takes_the_network_away_and_leaves_the_files(self) -> None:
        hubtls.set_listen(True, "192.168.0.7")
        r = hubtls.set_listen(False)
        self.assertEqual((r["listen"], r["certificate"]), (None, None))
        self.assertEqual(self.cfg()["hub_args"], ["--pull-ms", "2000"])
        self.assertTrue(hubtls.cert_paths()[0].exists())

    def test_an_address_that_is_no_house_network_address_is_refused(self) -> None:
        for bad in ("127.0.0.1", "0.0.0.0", "::1", "not-an-ip", "192.168.0.300"):
            with self.assertRaises(ValueError, msg=bad):
                hubtls.set_listen(True, bad)
        self.assertNotIn("--listen", self.cfg()["hub_args"])  # nothing written

    def test_the_tray_runs_this_hub_when_tray_json_names_its_cell(self) -> None:
        self.assertTrue(hubtls.runs_under_tray("D:/cell/home.json"))
        self.assertFalse(hubtls.runs_under_tray("D:/other/cell.json"))


if __name__ == "__main__":
    unittest.main()
