"""SerialTransport under two threads -- the TcpTransport race, on a real port.

The bridge reads from a reader thread and writes from whichever thread issues requests, and both
reach the port through the same lazy open. TcpTransport was fixed for this with a lock (see
test_transport.py); SerialTransport kept the unguarded version. On Windows a COM port can be open
once, so the second open fails with "Access denied" -- and the bridge's very first HELLO died that
way on 2026-10-04, the first time potctl ever met a real serial port instead of an emulated one.
"""

from __future__ import annotations

import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import transport as tp


class FakePort:
    """Enough of a pyserial port for read() and write()."""

    in_waiting = 0

    def read(self, n: int) -> bytes:
        return b""

    def write(self, data: bytes) -> int:
        return len(data)

    def close(self) -> None:
        pass


class ConcurrentOpen(unittest.TestCase):
    def test_concurrent_read_and_write_open_the_port_once(self) -> None:
        opens: list[FakePort] = []
        busy = threading.Lock()

        def slow_open(self_: tp.SerialTransport) -> FakePort:
            # Like Windows: a port that is already open refuses a second open. The sleep widens the
            # window the way a real driver's open does, so the race is reproduced rather than hoped for.
            if not busy.acquire(blocking=False):
                raise PermissionError(13, "Access denied")
            time.sleep(0.05)
            port = FakePort()
            opens.append(port)
            return port

        t = tp.SerialTransport("COM_TEST", reconnect=False)
        t._open = slow_open.__get__(t)  # type: ignore[method-assign]
        gate = threading.Barrier(2)
        errors: list[BaseException] = []

        def reader() -> None:
            try:
                gate.wait(timeout=5)
                t.read(64)
            except BaseException as exc:  # noqa: BLE001 -- reported, not swallowed
                errors.append(exc)

        def writer() -> None:
            try:
                gate.wait(timeout=5)
                t.write(b"hello")
            except BaseException as exc:  # noqa: BLE001
                errors.append(exc)

        threads = [threading.Thread(target=reader), threading.Thread(target=writer)]
        for th in threads:
            th.start()
        for th in threads:
            th.join(timeout=5)

        self.assertEqual(errors, [], "a second open was attempted while the first was in progress")
        self.assertEqual(len(opens), 1)


if __name__ == "__main__":
    unittest.main()
