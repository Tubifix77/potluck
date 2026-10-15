"""M20: the setup wizard's hub side (potluck.setup) -- with pretend boards, so nothing on the bench is flashed."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import setup as st  # noqa: E402

FLASH_ID_S3 = """esptool v5.3.1
Serial port COM5:
Connecting....
Detecting chip type... ESP32-S3
Connected to ESP32-S3 on COM5:
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
MAC:                b8:1f:3f:da:81:60

Uploading stub flasher...
Running stub flasher...
Stub flasher running.

Flash Memory Information:
=========================
Manufacturer: 1c
Device: 7118
Detected flash size: 16MB
Flash type set in eFuse: quad (4 data lines)
Flash voltage set by eFuse: 3.3V

Hard resetting via RTS pin...
"""  # esptool v5.3.1's real output on board A, 2026-10-15 (the port and MAC changed to C's)


class Ports(unittest.TestCase):
    def test_every_usb_port_but_the_hubs_own_is_a_candidate(self):
        ports = [SimpleNamespace(device="COM6", vid=0x10C4, pid=0xEA60, description="CP2102", serial_number="0001"),
                 SimpleNamespace(device="COM5", vid=0x1A86, pid=0x55D3, description="CH343", serial_number="x"),
                 SimpleNamespace(device="COM1", vid=None, pid=None, description="Kommunikationsport", serial_number=None)]
        out = st.list_ports(exclude=("COM6",), lister=lambda: ports)
        self.assertEqual([(p["port"], p["chip"]) for p in out], [("COM5", "CH343")])


class Probing(unittest.TestCase):
    def test_an_s3_with_16_mb_and_psram_is_ready(self):
        info = st.probe("COM5", runner=lambda args: FLASH_ID_S3)
        self.assertEqual((info["chip"][:8], info["flash_mb"], info["psram_mb"], info["ok"]), ("ESP32-S3", 16, 8, True))
        self.assertEqual(info["mac"], "b8:1f:3f:da:81:60")

    def test_a_board_that_cannot_run_potluck_says_why(self):
        small = FLASH_ID_S3.replace("16MB", "4MB")
        self.assertIn("4 MB of flash", st.probe("COM7", runner=lambda a: small)["reason"])
        no_psram = FLASH_ID_S3.replace(", Embedded PSRAM 8MB (AP_3v3)", "")
        self.assertIn("no PSRAM", st.probe("COM7", runner=lambda a: no_psram)["reason"])
        c3 = FLASH_ID_S3.replace("ESP32-S3 (QFN56)", "ESP32-C3")
        self.assertIn("needs an ESP32-S3", st.probe("COM7", runner=lambda a: c3)["reason"])


class Flashing(unittest.TestCase):
    def test_a_build_folder_names_its_parts_and_a_flash_job_reports_progress(self):
        with tempfile.TemporaryDirectory() as d:
            b = Path(d)
            for f in ("bootloader.bin", "partition-table.bin", "potluck_m0.bin"):
                (b / f).write_bytes(b"x")
            (b / "flasher_args.json").write_text(json.dumps({"flash_files": {
                "0x10000": "potluck_m0.bin", "0x0": "bootloader.bin", "0x8000": "partition-table.bin"}}))
            parts = st.flash_parts(b)
            self.assertEqual([o for o, _ in parts], ["0x0", "0x8000", "0x10000"])
            seen = []

            def fake(args, on_pct):
                self.assertIn("write-flash", args)
                for n in (10, 55, 100):
                    on_pct(n)
                    seen.append(n)
            jobs = st.Jobs()
            jid = jobs.start("flash", "COM5", lambda pct: st.flash("COM5", parts, pct, runner=fake) or {"flashed": True})
            for _ in range(100):
                if jobs.get(jid)["state"] != "running":
                    break
                time.sleep(0.01)
            self.assertEqual((jobs.get(jid)["state"], jobs.get(jid)["pct"]), ("done", 100))
            with self.assertRaises(st.SetupError):
                st.flash_parts(Path(d) / "nowhere")

    def test_one_job_at_a_time_per_port_and_a_failure_is_reported(self):
        jobs = st.Jobs()
        jid = jobs.start("flash", "COM5", lambda pct: time.sleep(0.2))
        with self.assertRaises(st.SetupError):
            jobs.start("flash", "COM5", lambda pct: None)
        bad = jobs.start("probe", "COM4", lambda pct: (_ for _ in ()).throw(st.SetupError("no chip answered")))
        time.sleep(0.05)
        self.assertEqual((jobs.get(bad)["state"], jobs.get(bad)["error"]), ("failed", "no chip answered"))
        time.sleep(0.25)
        self.assertEqual(jobs.get(jid)["state"], "done")


class FakeConsole:
    def __init__(self, reply: dict):
        self.reply, self.out, self.sent = reply, b"", []

    def reset_input_buffer(self):
        pass

    def write(self, data: bytes):
        self.sent.append(data.decode())
        self.out += b"I (99) log line\n" + json.dumps(self.reply).encode() + b"\n"

    def read(self, n: int) -> bytes:
        chunk, self.out = self.out[:n], self.out[n:]
        return chunk

    def close(self):
        pass


class Console(unittest.TestCase):
    def test_identify_and_wifi_speak_the_boards_console_commands(self):
        fc = FakeConsole({"t": "test", "cmd": "identify", "seconds": 20})
        self.assertEqual(st.identify("COM5", 20, opener=lambda p: fc)["seconds"], 20)
        self.assertEqual(fc.sent, ["POT! identify 20\n"])
        fw = FakeConsole({"t": "wifi_cfg", "result": "sta_saved"})
        self.assertTrue(st.set_wifi("COM5", 'Home "Net"', "password123", opener=lambda p: fw))
        self.assertEqual(fw.sent, ['POT! wifi sta "Home \\"Net\\"" "password123"\n'])
        with self.assertRaises(st.SetupError):
            st.set_wifi("COM5", "x", "short", opener=lambda p: fw)


class ClusterFile(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.p = Path(self.tmp.name) / "home.json"
        self.p.write_text(json.dumps({"schema": "potluck-cluster/1", "system": "home", "firmware": None,
                                      "boards": [{"node": "6300", "label": "A"}]}), encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def boards(self):
        return json.loads(self.p.read_text(encoding="utf-8"))["boards"]

    def test_a_new_board_is_added_and_its_room_set_and_cleared(self):
        self.assertEqual(st.add_board(self.p, "8160", room="Kitchen"),
                         {"node": "8160", "label": "node-8160", "room": "Kitchen"})
        st.add_board(self.p, "6300", room="Hall")
        self.assertEqual([b.get("room") for b in self.boards()], ["Hall", "Kitchen"])
        st.add_board(self.p, "8160", room="")
        self.assertNotIn("room", self.boards()[1])
        self.assertTrue(self.p.with_suffix(".json.bak").exists())  # the previous version, kept

    def test_what_apply_would_refuse_is_never_written(self):
        before = self.p.read_text(encoding="utf-8")
        with self.assertRaisesRegex(st.SetupError, "taken"):
            st.add_board(self.p, "8160", label="A")
        with self.assertRaisesRegex(st.SetupError, "would not load"):
            st.add_board(self.p, "8160", room="x" * 41)
        with self.assertRaisesRegex(st.SetupError, "0100..fffe"):
            st.add_board(self.p, "00fe")
        self.assertEqual(json.loads(self.p.read_text(encoding="utf-8")), json.loads(before))


class ApplyJob(unittest.TestCase):
    def run_fake(self, out: str, code: int = 0):
        script = f"import sys; print({out!r}); sys.exit({code})"
        lines = []
        try:
            res = st.run_apply("x.json", lines.append, cmd=[sys.executable, "-c", script])
        except st.SetupError as exc:
            return lines, exc
        return lines, res

    def test_verified_is_done_and_anything_else_is_a_failure(self):
        lines, res = self.run_fake("plan\nVERIFIED: cluster 'home' runs package 4 on 3 board(s)")
        self.assertEqual((lines[0], res["verdict"][:9]), ("plan", "VERIFIED:"))
        self.assertIn("NOT VERIFIED", str(self.run_fake("NOT VERIFIED:\n  0x8160 runs abc")[1]))
        self.assertIn("REFUSED", str(self.run_fake("REFUSED, nothing changed: no port", 2)[1]))
        self.assertIn("exited with 3", str(self.run_fake("crash", 3)[1]))


if __name__ == "__main__":
    unittest.main()
