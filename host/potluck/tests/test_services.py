"""M23: the PC's named services, served by the hub (potluck.services) -- what each answers, what is counted, and the
hub answering a board's CALL of one."""

from __future__ import annotations

import os
import struct
import sys
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import services as sv  # noqa: E402
from potluck.paths import path_hash  # noqa: E402


class Services(unittest.TestCase):
    def test_time_answers_unix_milliseconds(self) -> None:
        v, ok = sv.svc_time(b"")
        (ms,) = struct.unpack("<Q", v.raw[:8])
        self.assertTrue(ok)
        self.assertLess(abs(ms - time.time() * 1000), 5000)

    def test_speech_says_the_text_and_refuses_what_it_should_not_say(self) -> None:
        said = []
        with mock.patch.object(sv, "speak_text", lambda text: said.append(text) or True):
            self.assertEqual(sv.svc_speech("Hello from board C".encode())[1], True)
            for bad in (b"", b"x" * 201, "line\nbreak".encode(), b"\xff\xfe"):
                v, ok = sv.svc_speech(bad)
                self.assertFalse(ok, bad)
                self.assertEqual(v.raw[:4], b"\x00\x00\x00\x00")
        self.assertEqual(said, ["Hello from board C"])  # nothing refused ever reached the voice

    def test_the_text_goes_on_stdin_never_on_the_command_line(self) -> None:
        seen = []

        def runner(cmd, stdin_text):
            seen.append((cmd, stdin_text))
            return 0

        evil = "'; Remove-Item C:\\ -Recurse; '"
        self.assertTrue(sv.speak_text(evil, runner=runner) or os.name != "nt")
        if seen:
            cmd, stdin_text = seen[0]
            self.assertEqual(stdin_text, evil)
            self.assertFalse(any(evil in part for part in cmd))

    def test_counts_per_service_and_caller_and_an_unknown_service_is_refused(self) -> None:
        with self.assertRaisesRegex(ValueError, "unknown service"):
            sv.PcServices("lab", ["teleport"])
        ps = sv.PcServices("lab", ["time", "speech"])
        self.assertEqual(ps.name_of(path_hash("potluck://lab/svc/speech")), "speech")
        self.assertIsNone(ps.name_of(path_hash("potluck://lab/svc/other")))
        self.assertTrue(ps.slow("speech") and not ps.slow("time"))
        with mock.patch.object(sv, "speak_text", lambda text: False):  # no voice on this PC
            ps.serve("speech", 0x8160, b"hello")
        ps.serve("time", 0x6300, b"")
        ps.serve("time", 0x7368, b"")
        snap = {s["name"]: s for s in ps.snapshot()}
        self.assertEqual((snap["time"]["calls"], snap["time"]["errors"], snap["time"]["callers"]), (2, 0, ["6300", "7368"]))
        self.assertEqual((snap["speech"]["calls"], snap["speech"]["errors"], snap["speech"]["callers"]), (1, 1, ["8160"]))
        self.assertEqual(snap["speech"]["path"], "potluck://lab/svc/speech")
        self.assertEqual(snap["speech"]["state"], "ready")


class HubAnswers(unittest.TestCase):
    """The hub answers a board's CALL of one of the PC's services, and leaves any other CALL to the tools."""

    def test_a_board_calls_speech_and_the_hub_says_it_and_replies(self) -> None:
        from potluck import frame as fr
        from potluck import hub as hb
        from potluck.ns_payloads import Call, Reply
        from potluck.serial_framing import SerialReassembler, write_serial_frame
        from potluck.transport import LoopbackTransport

        host_side, node_side = LoopbackTransport.pair()
        hub = hb.Hub(host_side)
        ps = sv.PcServices("lab", ["speech"])
        hub.services_for = lambda: ps
        fanned = []
        hub._fan_out = lambda raw: fanned.append(raw)
        said = threading.Event()
        hub.start()
        try:
            with mock.patch.object(sv, "speak_text", lambda text: said.set() or True):
                call = Call(path_hash=path_hash("potluck://lab/svc/speech"), args=b"Hello from board C")
                node_side.write(write_serial_frame(fr.encode(src=0x8160, dst=0x00FE, opcode=fr.Op.CALL, seq=1,
                                                             msg_id=41, payload=call.encode())))
                self.assertTrue(said.wait(5))
                reply = None
                ra = SerialReassembler()
                end = time.time() + 5
                while reply is None and time.time() < end:
                    for raw in ra.feed(node_side.read(4096)):
                        f = fr.parse(raw)
                        if f.opcode == fr.Op.REPLY and f.msg_id == 41:
                            reply = (f, Reply.parse(bytes(f.payload)))
                self.assertIsNotNone(reply)
                f, rep = reply
                self.assertEqual((f.dst, rep.status, rep.value.raw[:4]), (0x8160, 0, b"\x01\x00\x00\x00"))
                other = Call(path_hash=path_hash("potluck://lab/svc/time"), args=b"")  # not offered: the tools'
                node_side.write(write_serial_frame(fr.encode(src=0x8160, dst=0x00FE, opcode=fr.Op.CALL, seq=2,
                                                             msg_id=42, payload=other.encode())))
                end = time.time() + 3
                while not fanned and time.time() < end:
                    time.sleep(0.05)
                self.assertEqual(len(fanned), 1)
            self.assertEqual(ps.snapshot()[0]["calls"], 1)
        finally:
            hub.close()


if __name__ == "__main__":
    unittest.main()
