"""M28: sound across the cell -- the tone actor's package (pot/deploy.hpp's ToneConfig), the hub's `sound` service and
its player (potluck.sound), and the write route for act/<name>/play."""

from __future__ import annotations

import os
import struct
import sys
import threading
import types
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import deploy as dp  # noqa: E402
from potluck import manifest as mf  # noqa: E402
from potluck.paths import path_hash  # noqa: E402
from potluck.sound import PATTERNS, Player  # noqa: E402

NODES = [{"node_id": 0x8160, "label": "C", "owns": [
             {"path": "potluck://m3-test/node-8160/test/alarm", "unit": "none", "kind": "sampled", "access": "read",
              "latency_class": 4}]},
         {"node_id": 0x00FE, "label": "pc", "kind": "host",
          "owns": [{"path": "potluck://m3-test/svc/sound", "unit": "none", "kind": "sampled", "access": "read",
                    "latency_class": 4, "staleness_bound_ms": 0}]}]
BELL = {"name": "bell", "module": "builtin:tone", "latency_class": 4, "pin": 0x8160,
        "needs": ["potluck://m3-test/svc/sound"],
        "config": {"source": "potluck://m3-test/node-8160/test/alarm", "pattern": 2}}


def doc(actor: dict, **extra) -> dict:
    return {"schema": 1, "system": "m3-test", "min_core_version": 1, "nodes": NODES, "actors": [actor], **extra}


class TonePackage(unittest.TestCase):
    def test_it_compiles_to_the_layout_the_node_parses(self) -> None:
        img = dp.compile_image(mf.parse(doc(BELL)), 3)
        at = dp.IMAGE_HEADER_LEN
        node, typ, cfg_len = struct.unpack_from("<HBB", img, at)
        self.assertEqual((node, typ, cfg_len), (0x8160, dp.ACTOR_TONE, 19))  # kToneCfgLen
        svc, provider, play, out, src, pattern = struct.unpack_from("<IHIIIB", img, at + 4)
        self.assertEqual((svc, provider, pattern), (path_hash("potluck://m3-test/svc/sound"), 0x00FE, 2))
        self.assertEqual((play, out), (path_hash("potluck://m3-test/act/bell/play"),
                                       path_hash("potluck://m3-test/act/bell/result")))
        self.assertEqual(src, path_hash("potluck://m3-test/node-8160/test/alarm"))
        plain = dict(BELL, config={})
        img = dp.compile_image(mf.parse(doc(plain)), 3)
        self.assertEqual(struct.unpack_from("<IHIIIB", img, dp.IMAGE_HEADER_LEN + 4)[4:], (0, 0))  # no source

    def test_its_play_may_be_written_from_outside_but_not_its_result(self) -> None:
        ok = doc(BELL, outside={"potluck://m3-test/act/bell/play": "read_write",
                                "potluck://m3-test/act/bell/result": "read"})
        self.assertEqual(mf.cross_check(mf.parse(ok)), [])
        with self.assertRaises(mf.ManifestErrors):
            mf.cross_check(mf.parse(doc(BELL, outside={"potluck://m3-test/act/bell/result": "read_write"})))

    def test_a_bad_pattern_source_or_service_is_refused(self) -> None:
        for bad in (dict(BELL, config={"pattern": 9}), dict(BELL, config={"pattern": True}),
                    dict(BELL, config={"source": "potluck://elsewhere/x"}), dict(BELL, config={"volume": 3}),
                    dict(BELL, config={"source": "potluck://m3-test/act/bell/play"}),
                    dict(BELL, needs=["potluck://m3-test/svc/nobody"]),
                    {k: v for k, v in BELL.items() if k != "pin"}):
            with self.assertRaises((dp.DeployError, mf.ManifestErrors)):
                dp.compile_image(mf.parse(doc(bad)), 1)

    def test_the_hub_routes_its_play_to_the_board_and_writes_it_as_an_i32(self) -> None:
        from potluck.hub import _inbox_owner, _inbox_type
        model = types.SimpleNamespace(package=types.SimpleNamespace(manifest=mf.parse(doc(BELL))))
        self.assertEqual(_inbox_owner(model, "potluck://m3-test/act/bell/play"), 0x8160)
        self.assertEqual(_inbox_type(model, "potluck://m3-test/act/bell/play"), 2)  # ValueType I32
        self.assertIsNone(_inbox_owner(model, "potluck://m3-test/act/bell/result"))


class SoundPlayer(unittest.TestCase):
    def test_a_pattern_plays_its_notes_rests_are_rests_and_an_unknown_one_is_refused(self) -> None:
        notes, rests = [], []
        p = Player(beep=lambda hz, ms: notes.append((hz, ms)), sleep=rests.append)
        self.assertTrue(p.play(1))
        p.wait()
        self.assertEqual(notes, [(880, 120), (880, 120)])
        self.assertEqual(rests, [0.08])
        self.assertFalse(p.play(9))
        self.assertFalse(p.play(-1))
        self.assertTrue(p.play(0))  # stop: always taken

    def test_a_stop_ends_a_pattern_at_the_next_note(self) -> None:
        started, release = threading.Event(), threading.Event()
        notes = []

        def beep(hz, ms):
            notes.append(hz)
            started.set()
            release.wait(2)
        p = Player(beep=beep, sleep=lambda s: None)
        self.assertTrue(p.play(2))  # alarm: eight notes
        started.wait(2)
        p.play(0)
        release.set()
        p.wait()
        self.assertEqual(len(notes), 1)  # the note that was sounding, then nothing

    def test_no_sound_on_this_pc_refuses_every_pattern(self) -> None:
        p = Player(beep=None, have_sound=False)
        self.assertFalse(p.play(1))
        self.assertTrue(p.play(0))

    def test_every_pattern_is_short_and_in_beeps_range(self) -> None:
        self.assertEqual(sorted(PATTERNS), list(range(1, 9)))  # pot/deploy.hpp's kTonePatternMax
        for name, notes in PATTERNS.values():
            for hz, ms in notes:
                self.assertTrue(hz == 0 or 37 <= hz <= 32767, name)
                self.assertTrue(0 < ms <= 600, name)  # a stop waits at most one note


class SoundService(unittest.TestCase):
    def test_the_service_takes_an_i32_and_answers_1_or_0(self) -> None:
        from potluck import services, sound
        saved = sound._player
        sound._player = Player(beep=lambda hz, ms: None, sleep=lambda s: None, have_sound=True)
        try:
            v, ok = services.svc_sound(struct.pack("<i", 3))
            self.assertTrue(ok)
            self.assertEqual(int.from_bytes(bytes(v.raw[:4]), "little"), 1)
            v, ok = services.svc_sound(struct.pack("<i", 42))
            self.assertFalse(ok)
            v, ok = services.svc_sound(b"\x01")  # not four bytes
            self.assertFalse(ok)
            sound._player.wait()
        finally:
            sound._player = saved
        self.assertIn("sound", services.SERVICES)
        self.assertFalse(services.SERVICES["sound"][2])  # answers at once: never on a slow thread


if __name__ == "__main__":
    unittest.main()
