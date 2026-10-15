"""M20: on a board's console the frame link shares the line with the board's log -- the host keeps frames and text apart."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck.serial_framing import SerialReassembler, write_serial_frame  # noqa: E402


class ConsoleText(unittest.TestCase):
    def test_log_lines_between_frames_go_to_the_text_sink_and_frames_stay_whole(self):
        got: list[bytes] = []
        r = SerialReassembler(got.append)
        f1, f2 = write_serial_frame(b"\x01\x00\x02"), write_serial_frame(b"\x0a" * 40)
        data = b"I (12) pot: boot\r\n" + b"\x00" + f1 + b"W (13) deploy: refused\r\n\x00" + f2
        frames = []
        for i in range(0, len(data), 7):  # any read size
            frames += list(r.feed(data[i:i + 7]))
        self.assertEqual(frames, [b"\x01\x00\x02", b"\x0a" * 40])
        self.assertEqual(got, [b"I (12) pot: boot\r\n", b"W (13) deploy: refused\r\n"])

    def test_without_a_sink_text_is_dropped_as_before(self):
        r = SerialReassembler()
        self.assertEqual(list(r.feed(b"some text\x00" + write_serial_frame(b"\x05"))), [b"\x05"])


if __name__ == "__main__":
    unittest.main()
