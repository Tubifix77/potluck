"""M20: the tray icon's reading of the hub, its drawn icons and how it finds the cell -- no window is opened here."""

from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import tray  # noqa: E402

GOOD = {"hub": {"link": "up"}, "cell": {"system": "home"},
        "boards": [{"node": "6300", "label": "A", "state": "alive"}, {"node": "7368", "label": "B", "state": "asleep"}],
        "apps": [{"name": "rf_link", "working": True}]}


class Judge(unittest.TestCase):
    def test_each_state_and_what_the_tooltip_says(self):
        self.assertEqual(tray.judge(GOOD), ("ok", "Potluck home: all 2 boards fine"))  # asleep is not a fault
        self.assertEqual(tray.judge(None)[0], "none")
        self.assertEqual(tray.judge(dict(GOOD, hub={"link": "down"}))[0], "bad")
        st, tip = tray.judge(dict(GOOD, boards=[{"label": "A", "state": "dead"}], apps=[{"name": "x", "working": False}]))
        self.assertEqual((st, tip), ("warn", "Potluck home: A not answering; x not working"))
        self.assertIn("no boards yet", tray.judge(dict(GOOD, boards=[]))[1])


class Icon(unittest.TestCase):
    def test_a_valid_32x32_icon_per_state_with_a_transparent_corner(self):
        from potluck import brandmark
        for st in tray.STATES:
            b = brandmark.ico(32, st)
            reserved, kind, count = struct.unpack_from("<HHH", b, 0)
            w, h, _, _, planes, bpp, size, offset = struct.unpack_from("<BBBBHHII", b, 6)
            self.assertEqual((reserved, kind, count, w, h, bpp, offset, len(b)), (0, 1, 1, 32, 32, 32, 22, 22 + size))
            first_pixel = b[22 + 40:22 + 44]  # the bottom-left corner: outside the disc
            self.assertEqual(first_pixel[3], 0)


class CellFile(unittest.TestCase):
    def test_tray_json_first_else_the_one_cell_the_wizard_made(self):
        with tempfile.TemporaryDirectory() as d:
            saved = tray.potluck_dir
            tray.potluck_dir = lambda: Path(d)
            try:
                self.assertIsNone(tray.cell_file())  # nothing: the hub starts in setup mode
                (Path(d) / "cells" / "home").mkdir(parents=True)
                (Path(d) / "cells" / "home" / "home.json").write_text("{}")
                self.assertEqual(tray.cell_file(), Path(d) / "cells" / "home" / "home.json")
                (Path(d) / "tray.json").write_text(json.dumps({"cluster": "D:/x/bench.json"}))
                self.assertEqual(tray.cell_file(), Path("D:/x/bench.json"))
                # as Windows PowerShell's Set-Content -Encoding UTF8 writes it: with a BOM (read as no cell, 2026-10-16)
                (Path(d) / "tray.json").write_bytes(b"\xef\xbb\xbf" + json.dumps({"cluster": "D:/x/home.json"}).encode())
                self.assertEqual(tray.cell_file(), Path("D:/x/home.json"))
                self.assertEqual(tray.hub_command(Path("c.json"))[-2:], ["--cluster", "c.json"])
            finally:
                tray.potluck_dir = saved


class Desk(unittest.TestCase):
    """The owner's request (2026-10-19): the Potluck Desk in the tray's menu, found through tray.json's "desk"."""

    def test_the_desk_from_source_or_an_exe_and_none_when_it_is_not_there(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            py = root / "pythonw.exe"
            py.write_text("")
            checkout = root / "potluck-desk"
            (checkout / "potluck_desk").mkdir(parents=True)
            cfg = {"desk": {"python": str(py), "cwd": str(checkout)}}
            self.assertEqual(tray.desk_command(cfg=cfg), ([str(py), "-m", "potluck_desk"], str(checkout)))
            self.assertEqual(tray.desk_command("addboards", cfg)[0][-2:], ["--open", "addboards"])
            self.assertIsNone(tray.desk_command(cfg={}))  # none named: no menu item
            self.assertIsNone(tray.desk_command(cfg={"desk": {"python": str(py), "cwd": str(root)}}))  # not a checkout
            exe = root / "potluck-desk.exe"
            self.assertIsNone(tray.desk_command(cfg={"desk": {"exe": str(exe)}}))  # named, but not installed there
            exe.write_text("")
            self.assertEqual(tray.desk_command(cfg={"desk": {"exe": str(exe)}}), ([str(exe)], str(root)))


class Keeper(unittest.TestCase):
    def test_a_hub_that_stops_answering_is_replaced_once_a_minute_at_most(self):
        now = [0.0]
        started = []
        k = tray.HubKeeper(start=lambda: started.append(now[0]), clock=lambda: now[0])
        for _ in range(10):  # a hub that answers is never touched
            self.assertFalse(k.seen(GOOD))
        self.assertFalse(k.seen(None))
        self.assertFalse(k.seen(GOOD))  # a missed poll, then an answer: the count starts again
        self.assertFalse(k.seen(None))
        self.assertFalse(k.seen(None))
        self.assertTrue(k.seen(None))  # three polls in a row with no answer
        self.assertEqual(started, [0.0])
        for _ in range(19):  # 57 s of no answer: one that cannot start is not restarted in a loop
            now[0] += 3
            self.assertFalse(k.seen(None))
        now[0] += 3
        self.assertTrue(k.seen(None))
        self.assertEqual(started, [0.0, 60.0])


if __name__ == "__main__":
    unittest.main()
