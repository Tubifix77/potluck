"""TcpTransport under two threads -- the bug that makes an emulated node look dead.

QEMU's `-serial tcp:...,server` chardev accepts exactly ONE connection per VM lifetime. The bridge
reads from a reader thread and writes from whichever thread is issuing requests, and both reach the
socket through the same lazy connect. If that connect is not serialised, two sockets get opened:
QEMU binds the guest's UART to the first and leaves the second sitting in the listen backlog
forever, so one thread is talking to the guest and the other is reading silence. The visible symptom
is a node that answers nothing -- indistinguishable, from the host, from a node that crashed.

It cost a real session: a soak run reported "no answer from tcp 127.0.0.1:5555" while the guest's own
console showed it had received the HELLO and answered it.
"""

from __future__ import annotations

import os
import socket
import sys
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from potluck import transport as tp


class OneConnectionOnly(unittest.TestCase):
    """A listener that behaves like the chardev: one accept, and the rest go nowhere."""

    def setUp(self) -> None:
        self.server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.port = self.server.getsockname()[1]
        self.accepted: list[socket.socket] = []
        self.ready = threading.Event()

        def accept_once() -> None:
            conn, _ = self.server.accept()
            self.accepted.append(conn)
            self.ready.set()

        self.acceptor = threading.Thread(target=accept_once, daemon=True)
        self.acceptor.start()

    def tearDown(self) -> None:
        for c in self.accepted:
            c.close()
        self.server.close()

    def test_concurrent_read_and_write_open_exactly_one_socket(self) -> None:
        connects = []
        real = tp.socket.create_connection

        def counting(*a, **kw):
            s = real(*a, **kw)
            connects.append(s)
            return s

        t = tp.TcpTransport("127.0.0.1", self.port, timeout=0.05)
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

        tp.socket.create_connection = counting
        try:
            threads = [threading.Thread(target=reader), threading.Thread(target=writer)]
            for th in threads:
                th.start()
            for th in threads:
                th.join(timeout=10)
        finally:
            tp.socket.create_connection = real
            t.close()

        self.assertEqual(errors, [], f"transport raised: {errors}")
        # The invariant. Counting accepts on the server would not catch this: the kernel completes
        # the second handshake into the listen backlog whether or not anyone ever accepts it.
        self.assertEqual(len(connects), 1,
                         f"opened {len(connects)} sockets; QEMU would only ever serve the first")
        self.assertTrue(self.ready.wait(timeout=5), "the listener never accepted anything")
        self.accepted[0].settimeout(2.0)
        self.assertEqual(self.accepted[0].recv(16), b"hello",
                         "the bytes went to a socket the guest is not attached to")

    def test_reads_see_what_arrives_on_the_one_socket(self) -> None:
        t = tp.TcpTransport("127.0.0.1", self.port, timeout=0.5)
        self.addCleanup(t.close)
        t.write(b"x")
        self.assertTrue(self.ready.wait(timeout=5))
        self.accepted[0].sendall(b"pong")
        got = b""
        for _ in range(20):
            got += t.read(64)
            if got:
                break
        self.assertEqual(got, b"pong")


if __name__ == "__main__":
    unittest.main()
