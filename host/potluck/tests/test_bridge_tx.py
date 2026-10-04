"""Two threads writing frames through one bridge -- the M2 hardware session's crash.

The bridge sends requests from the caller's thread and heartbeats from its own, and both went through
send_frame() with nothing serialising the write. Over TCP (all that emulation ever used) concurrent
sendall() calls go unnoticed. On a Windows COM port they do not: pyserial tracks every write through
one shared OVERLAPPED structure, so two writes in flight read back each other's byte count and one
raises "Write timeout". That killed the first ten-minute M2 session on real hardware after 70 s
(2026-10-04). Even without the exception, two frames' bytes interleaved on the wire are two corrupt
frames.
"""

from __future__ import annotations

import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import frame as fr
from potluck.bridge import Bridge
from potluck.serial_framing import SerialReassembler


class OverlapDetectingTransport:
    """Records every write and notices when two are in progress at once."""

    description = "overlap-detector"

    def __init__(self) -> None:
        self._guard = threading.Lock()
        self.inside = 0
        self.max_inside = 0
        self.chunks: list[bytes] = []

    def write(self, data: bytes) -> int:
        with self._guard:
            self.inside += 1
            self.max_inside = max(self.max_inside, self.inside)
        # Write in two halves with a pause between, as a slow driver would: an unserialised second
        # writer can now land in the middle of this frame.
        half = len(data) // 2
        self.chunks.append(data[:half])
        time.sleep(0.0005)
        self.chunks.append(data[half:])
        with self._guard:
            self.inside -= 1
        return len(data)

    def read(self, max_bytes: int = 4096) -> bytes:
        time.sleep(0.01)
        return b""

    def close(self) -> None:
        pass


class ConcurrentSendFrame(unittest.TestCase):
    def test_two_threads_never_write_at_once_and_every_frame_arrives_whole(self) -> None:
        t = OverlapDetectingTransport()
        bridge = Bridge(t, heartbeat=False)
        try:
            gate = threading.Barrier(2)
            per_thread = 150

            def sender(opcode: int) -> None:
                gate.wait(timeout=5)
                for _ in range(per_thread):
                    bridge.send_frame(opcode, b"\x01\x02\x03\x04" * 4, dst=0x6300)

            threads = [threading.Thread(target=sender, args=(fr.Op.READ,)),
                       threading.Thread(target=sender, args=(fr.Op.HEARTBEAT,))]
            for th in threads:
                th.start()
            for th in threads:
                th.join(timeout=30)
        finally:
            bridge.close()

        self.assertEqual(t.max_inside, 1, "two writes were in progress at the same time")
        frames = list(SerialReassembler().feed(b"".join(t.chunks)))
        self.assertEqual(len(frames), 2 * per_thread, "interleaved bytes destroyed some frames")
        for raw in frames:
            fr.parse(raw)  # raises on a corrupt frame


if __name__ == "__main__":
    unittest.main()
