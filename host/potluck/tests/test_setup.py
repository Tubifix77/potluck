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

    def test_esptool_runs_as_a_program_of_its_own_and_its_progress_and_failures_come_through(self):
        # GPLv2+ beside the Apache-2.0 hub: never imported, only run (the release-day item, 2026-10-16)
        import subprocess  # noqa: F401 -- what flash() uses
        self.assertNotIn("esptool", sys.modules.get("potluck.setup").__dict__)
        with tempfile.TemporaryDirectory() as d:
            fake = Path(d) / "fake_esptool.py"
            fake.write_text("import sys\nsys.stdout.write('Writing (10 %)\\r(55 %)\\r(100 %)\\n')\n"
                            "sys.exit(0 if 'write-flash' in sys.argv else 2)\n")
            saved = st.esptool_command
            st.esptool_command = lambda: [sys.executable, str(fake)]
            try:
                seen = []
                st.flash("COM9", [("0x0", Path(d) / "x.bin")], seen.append)
                self.assertEqual(seen[:3], [10, 55, 100])
                with self.assertRaises(st.SetupError):
                    st._esptool(["flash-id"])  # exits 2: a failure, with esptool's last line
            finally:
                st.esptool_command = saved
        os.environ["POTLUCK_ESPTOOL"] = "C:/tools/esptool.exe"
        try:
            self.assertEqual(st.esptool_command(), ["C:/tools/esptool.exe"])
        finally:
            del os.environ["POTLUCK_ESPTOOL"]

    def test_a_pc_with_no_build_flashes_the_release_the_package_carries(self):
        def build(folder):
            folder.mkdir(parents=True)
            for f in ("bootloader.bin", "potluck_m0.bin"):
                (folder / f).write_bytes(b"x")
            (folder / "flasher_args.json").write_text(json.dumps({"flash_files": {
                "0x0": "bootloader.bin", "0x10000": "potluck_m0.bin"}}))
        with tempfile.TemporaryDirectory() as d:
            cache, rel = Path(d) / "cache", Path(d) / "firmware"
            (cache / "fw").mkdir(parents=True)
            saved = os.environ.get("POTLUCK_FIRMWARE_DIR")
            os.environ["POTLUCK_FIRMWARE_DIR"] = str(rel)
            try:
                with self.assertRaises(st.SetupError):
                    st.newest_build(cache)  # nothing built, nothing shipped
                build(rel / "27994a5-0a1b2c")
                self.assertEqual(st.newest_build(cache), rel / "27994a5-0a1b2c")
                self.assertEqual(st.cell_build("27994a5-0a1b2c", cache), rel / "27994a5-0a1b2c")
                with self.assertRaises(st.SetupError):
                    st.cell_build("1111111-ffffff", cache)  # the cell runs firmware this PC has neither built nor carries
                build(cache / "fw" / "9bb9fb19365b" / "build-apply")
                self.assertEqual(st.newest_build(cache), cache / "fw" / "9bb9fb19365b" / "build-apply")  # a build wins
            finally:
                if saved is None:
                    os.environ.pop("POTLUCK_FIRMWARE_DIR", None)
                else:
                    os.environ["POTLUCK_FIRMWARE_DIR"] = saved

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

    def test_a_presenting_boards_broker_login_goes_to_its_console_and_a_bad_one_never_does(self):
        """2026-10-20, the Potluck Desk: the broker login without a console typed by hand."""
        fp = FakeConsole({"t": "present_cfg", "result": "login_saved"})
        self.assertTrue(st.set_present_login("COM5", "potluck", "s3cret pass", opener=lambda p: fp))
        self.assertEqual(fp.sent, ["POT! present login potluck s3cret pass\n"])  # the password is the rest of the line
        bad = FakeConsole({"t": "present_cfg", "result": "login_saved"})
        for user, pw in (("two words", "x"), ("u", "a\nPOT! present forget"), ("", "x"), ("u", "p" * 65), (1, "x")):
            with self.assertRaises(st.SetupError):
                st.set_present_login("COM5", user, pw, opener=lambda p: bad)
        self.assertEqual(bad.sent, [])
        refused = FakeConsole({"t": "present_cfg", "result": "save_failed"})
        with self.assertRaises(st.SetupError):
            st.set_present_login("COM5", "potluck", "x", opener=lambda p: refused)

    def test_a_line_break_or_an_oversized_name_never_reaches_the_console(self):
        """The Potluck Desk, 2026-10-20: a line break in either value ended `POT! wifi sta ...` early, and the rest
        arrived as a second console command."""
        fw = FakeConsole({"t": "wifi_cfg", "result": "sta_saved"})
        for ssid, pw in (("Home\nPOT! reset", "password123"), ("Home", "pass\rword123"), ("Home", "pass\tword1"),
                         ("x" * 33, "password123"), ("\u00e6" * 17, "password123"), ("", "password123"),
                         ("Home", "p" * 65), (7, "password123"), ("Home", None)):
            with self.assertRaises(st.SetupError) as cm:
                st.set_wifi("COM5", ssid, pw, opener=lambda p: fw)
            self.assertNotIn("password1", str(cm.exception))  # says why, never the value
        self.assertEqual(fw.sent, [])  # nothing was sent for any of them
        self.assertTrue(st.set_wifi("COM5", "\u00c6blehaven", "password123", opener=lambda p: fw))  # UTF-8, 11 bytes

    def test_a_reset_pulses_rts_and_leaves_dtr_alone(self):
        seen = []

        class Pins:
            dtr = False

            def __setattr__(self, k, v):
                seen.append((k, v))

            def close(self):
                seen.append("closed")
        st.reset("COM5", opener=lambda p: Pins())
        self.assertEqual(seen, [("rts", True), ("rts", False), "closed"])


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

    def test_a_wiped_board_is_known_by_its_mac_and_keeps_its_label_and_room(self):
        """2026-10-23: the wizard did not know a wiped board; its MAC gives the id of one of the cell's boards."""
        st.add_board(self.p, "6300", room="Room")
        self.assertEqual(st.known_board(self.p, "b8:1f:3f:da:63:00"), {"node": "6300", "label": "A", "room": "Room"})
        self.assertIsNone(st.known_board(self.p, "b8:1f:3f:da:99:99"))  # not one of the cell's
        self.assertIsNone(st.known_board(self.p, None))
        self.assertEqual(st.add_board(self.p, "6300"), {"node": "6300", "label": "A", "room": "Room"})  # not a 2nd A

    def test_a_floor_plan_and_a_boards_place_on_it(self):
        from potluck import apply as ap
        plan = {"width": 13, "height": 9, "rooms": [{"name": "Living room", "x": 0, "y": 0, "w": 8, "h": 9}]}
        st.set_plan(self.p, plan)
        st.add_board(self.p, "6300", position=[3.456, 4])
        c = ap.load_cluster(str(self.p))
        self.assertEqual((c.plan["width"], c.plan["rooms"][0]["name"], c.positions), (13.0, "Living room",
                                                                                       {0x6300: (3.46, 4.0)}))
        with self.assertRaisesRegex(st.SetupError, "would not load"):  # a room outside the plan
            st.set_plan(self.p, dict(plan, rooms=[{"name": "x", "x": 10, "y": 0, "w": 8, "h": 1}]))
        st.add_board(self.p, "6300", position=[])
        st.set_plan(self.p, None)
        c = ap.load_cluster(str(self.p))
        self.assertEqual((c.plan, c.positions), (None, {}))

    def test_what_apply_would_refuse_is_never_written(self):
        before = self.p.read_text(encoding="utf-8")
        with self.assertRaisesRegex(st.SetupError, "taken"):
            st.add_board(self.p, "8160", label="A")
        with self.assertRaisesRegex(st.SetupError, "would not load"):
            st.add_board(self.p, "8160", room="x" * 41)
        with self.assertRaisesRegex(st.SetupError, "0100..fffe"):
            st.add_board(self.p, "00fe")
        self.assertEqual(json.loads(self.p.read_text(encoding="utf-8")), json.loads(before))


class NewCell(unittest.TestCase):
    PASS = "a long enough passphrase"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name) / "home"

    def tearDown(self):
        from potluck import keystore
        keystore.lock()
        self.tmp.cleanup()

    def test_a_new_cell_keeps_its_secrets_only_in_the_vault_and_signs_through_it(self):
        from potluck import apply as ap
        from potluck import keystore, signing
        cluster = st.new_cell(self.d, "home", self.PASS)
        self.assertEqual(sorted(p.name for p in self.d.iterdir()),
                         ["ca.pub", "deploy.bcert", "deploy.cert", "deploy.pub", "home.json", "nodes.json",
                          "vault.json"])  # no .key file on disk
        self.assertNotIn(b"secret", (self.d / "vault.json").read_bytes())
        c = ap.load_cluster(str(cluster))
        with self.assertRaises(OSError):
            signing.read_key(str(c.keys["deploy_key"]))  # locked
        keystore.unlock(self.d / "vault.json", self.PASS)
        dk, ca = signing.read_key(str(c.keys["deploy_key"])), signing.read_key(str(self.d / "ca.key"))
        cert = json.loads(c.keys["deploy_cert"].read_text(encoding="utf-8"))
        self.assertEqual(signing.verify_cert(cert, ca.public).public, dk.public)

    def test_a_folder_that_holds_a_cell_is_never_written_over(self):
        st.new_cell(self.d, "home", self.PASS)
        before = (self.d / "vault.json").read_bytes()
        with self.assertRaisesRegex(st.SetupError, "already holds a cell"):
            st.new_cell(self.d, "home", self.PASS)
        with self.assertRaisesRegex(st.SetupError, "already holds a cell"):
            st.new_cell(self.d, "other", self.PASS)
        self.assertEqual((self.d / "vault.json").read_bytes(), before)
        with self.assertRaises(st.SetupError):
            st.new_cell(Path(self.tmp.name) / "x", "my home", self.PASS)

    def test_apply_opens_the_vault_from_stdin_and_refuses_a_wrong_passphrase(self):
        import contextlib
        import io
        from potluck import apply as ap
        cluster = st.new_cell(self.d, "home", self.PASS)  # no port: apply stops right after the vault
        for phrase, want in ((self.PASS + "!", "did not open"), (self.PASS, "no port")):
            out, old = io.StringIO(), sys.stdin
            sys.stdin = io.StringIO(phrase + "\n")
            try:
                with contextlib.redirect_stdout(out):
                    self.assertEqual(ap.main([str(cluster), "--passphrase-stdin"]), 2)
            finally:
                sys.stdin = old
            self.assertIn(want, out.getvalue())


class Library(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.p = Path(self.tmp.name) / "home.json"
        self.p.write_text(json.dumps({"schema": "potluck-cluster/1", "system": "home", "firmware": None,
                                      "boards": [{"node": "6300", "label": "A"}],
                                      "apps": [{"name": "passive-sensor", "git": ".", "components": "c"}],
                                      "outside": {"node-6300/led/on": {"mode": "read_write", "writers": ["HA"]}}}),
                          encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_an_app_switched_off_stays_in_the_image_and_leaves_the_package(self):
        from potluck import apply as ap
        st.set_app(self.p, "passive-sensor", False)
        self.assertFalse(ap.load_cluster(str(self.p)).apps[0].enabled)
        st.set_app(self.p, "passive-sensor", True)
        self.assertNotIn("enabled", json.loads(self.p.read_text(encoding="utf-8"))["apps"][0])
        with self.assertRaises(st.SetupError):
            st.set_app(self.p, "nope", True)

    def test_what_the_outside_may_see_and_control_in_either_path_form(self):
        out = st.set_outside(self.p, {"potluck://home/act/rf_fusion/out": "read"})
        self.assertEqual(out["act/rf_fusion/out"], "read")
        out = st.set_outside(self.p, {"potluck://home/node-6300/led/on": "read_write"})
        self.assertEqual(out["node-6300/led/on"], {"mode": "read_write", "writers": ["HA"]})  # who may write, kept
        out = st.set_outside(self.p, {"node-6300/led/on": "read"})
        self.assertEqual(out["node-6300/led/on"], "read")  # a writers list only for what is writable
        out = st.set_outside(self.p, {"node-6300/led/on": "off", "act/rf_fusion/out": "off"})
        self.assertEqual(out, {})
        with self.assertRaises(st.SetupError):
            st.set_outside(self.p, {"x": "everything"})


class Broker(unittest.TestCase):
    """M20: the broker Home Assistant's boards present to, tried from the PC before the wizard goes on."""

    def serve(self, reply: bytes):
        import socket
        import threading
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)

        def run():
            try:
                c, _ = srv.accept()
                c.recv(1024)
                if reply:
                    c.sendall(reply)
                c.close()
            finally:
                srv.close()
        threading.Thread(target=run, daemon=True).start()
        return f"127.0.0.1:{srv.getsockname()[1]}"

    def test_each_answer_is_said_plainly(self):
        self.assertEqual(st.broker_test(self.serve(b"\x20\x02\x00\x00"))["result"], "connected")
        self.assertIn("wants a login", st.broker_test(self.serve(b"\x20\x02\x00\x05"))["result"])  # 0x05, OASIS 3.1.1
        self.assertIn("refused the login", st.broker_test(self.serve(b"\x20\x02\x00\x04"))["result"])
        self.assertIn("wants TLS", st.broker_test(self.serve(b"\x15\x03\x03\x00\x02\x02\x46"))["result"])
        self.assertIn("closed the connection", st.broker_test(self.serve(b""))["result"])
        self.assertIn("not as an MQTT broker", st.broker_test(self.serve(b"HTTP/1.1 400\r\n\r\n"))["result"])
        with self.assertRaises(st.SetupError):
            st.broker_test("no-port-here")


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

    def test_the_bar_moves_with_apply_s_own_lines(self):
        """2026-10-19, the owner through the Desk: an Apply's progress was "0 % and then done"."""
        def bar(lines):
            p, seen = st.ApplyProgress(), []
            for ln in lines:
                n = p.feed(ln)
                if n is not None:
                    seen.append(n)
            return seen
        # A package change only, as the owner's Apply from the Desk: no firmware sent.
        pkg_only = bar(["[    0.4 s] package: 'home', 12 actor(s) (3 portable), image 9000 B, for 3 board(s)",
                        "[    6.3 s] firmware: the release image this package carries (639e1f6-910870) -- nothing to build",
                        "[    7.0 s] inventory: 0x6300 ...", "[    7.1 s] plan: firmware unchanged on every board",
                        "[    7.2 s] lease: the cell's deploy lease is ours", "[    7.3 s] firmware: every board already runs x",
                        "[    7.5 s] package: signed at counter 97, kept as k", "[    9.0 s] package: counter 97 committed on 0x6300",
                        "[   13.0 s] verify: 0x6300 confirmed package 97: a", "[   17.0 s] verify: 0x7368 confirmed package 97: b",
                        "[   21.0 s] verify: 0x8160 confirmed package 97: c", "[   21.5 s] verify: sys/uptime = 5"])
        self.assertEqual(pkg_only, [3, 30, 32, 35, 40, 60, 73, 86, 99])
        # A firmware rollout to three boards moves by board.
        rollout = bar(["package: 'home', 1 actor(s), for 3 board(s)", "firmware: building 1-2 (2 app component dir(s))",
                       "firmware: built in 300 s", "firmware: 1-2 to 0x7368, 0x8160, 0x6300 at counter 64",
                       "firmware: 0x7368 confirmed 1-2 in 110 s", "firmware: 0x8160 confirmed 1-2 in 100 s",
                       "firmware: 0x6300 confirmed 1-2 in 120 s", "package: signed at counter 9, kept as k",
                       "package: counter 9 committed on 0x6300", "verify: 0x6300 confirmed package 9: a"])
        self.assertEqual(rollout, [3, 5, 30, 40, 53, 66, 80, 82, 87, 91])
        self.assertTrue(all(a < b for a, b in zip(rollout, rollout[1:])))

    def test_the_job_gets_the_progress_and_the_lines(self):
        jobs = st.Jobs()
        script = ("print(\"[ 0.1 s] package: 'h', 1 actor(s), for 1 board(s)\"); "
                  "print('[ 0.2 s] package: counter 3 committed on 0x6300'); "
                  "import time; time.sleep(0.5); print('VERIFIED: x')")
        seen = []
        def fn(pct):
            def spy(x):
                seen.append(x)
                pct(x)
            return st.run_apply("x.json", spy, cmd=[sys.executable, "-c", script])
        jid = jobs.start("apply", "cell", fn)
        for _ in range(100):
            if jobs.get(jid)["state"] != "running":
                break
            time.sleep(0.05)
        self.assertEqual([x for x in seen if isinstance(x, int)], [3, 60])
        self.assertEqual(jobs.get(jid)["state"], "done")
        self.assertIn("VERIFIED: x", jobs.get(jid)["lines"])


if __name__ == "__main__":
    unittest.main()
