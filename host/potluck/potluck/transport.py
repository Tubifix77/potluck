"""Byte transports for the frame link -- a serial port, a TCP socket, or a loopback pair.

ARCHITECTURE.md section 5.3 lists "USB-serial / UART" as a transport of the same standing as
ESP-NOW, so the host's side of it is a transport object rather than something the bridge opens
inline. Three implementations, and each one earns its place:

  SerialTransport    a real board on a real port.
  TcpTransport       QEMU exposes an emulated UART as a TCP socket
                     (`-serial tcp:127.0.0.1:5555,server`), so the identical bridge code drives an
                     emulated node and a physical one. That is the whole reason the emulator is
                     worth having: the thing under test is the firmware, not a mock of it.
  LoopbackTransport  an in-process pipe to a Python node, which is what lets the bridge and potctl
                     be tested with neither hardware nor an emulator.

All three are byte pipes with a read timeout. Framing is serial_framing's business, and the bridge
does not know which of these it is talking to -- section 8.1's "a mode transition is a non-event"
only holds if nothing above the transport branches on the transport.
"""

from __future__ import annotations

import socket
import sys
import threading
import time
from typing import Protocol


class Transport(Protocol):
    def read(self, max_bytes: int = 4096) -> bytes:
        """Read what is available, up to `max_bytes`. Returns b"" on timeout, never blocks forever."""
        ...

    def write(self, data: bytes) -> int: ...
    def close(self) -> None: ...
    @property
    def description(self) -> str: ...


class SerialTransport:
    """A real serial port. Reconnects, for the same reason SerialSource does."""

    def __init__(self, port: str, baud: int = 921600, *, timeout: float = 0.1,
                 reconnect: bool = True) -> None:
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.reconnect = reconnect
        self._ser = None
        self.reconnects = 0

    @property
    def description(self) -> str:
        return f"serial {self.port} @ {self.baud}"

    def _open(self):
        try:
            import serial  # type: ignore[import-untyped]
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise SystemExit(
                "pyserial is required for a serial frame link.\n"
                "  pip install pyserial\n"
                "Or drive an emulated node instead:  potluck-bridge --tcp 127.0.0.1:5555"
            ) from exc
        return serial.serial_for_url(self.port, baudrate=self.baud, timeout=self.timeout)

    def _ensure(self):
        if self._ser is None:
            self._ser = self._open()
        return self._ser

    def read(self, max_bytes: int = 4096) -> bytes:
        try:
            ser = self._ensure()
        except Exception as exc:  # pragma: no cover - hardware dependent
            if not self.reconnect:
                raise
            print(f"# cannot open {self.port}: {exc}; retrying", file=sys.stderr)
            time.sleep(1.0)
            return b""
        try:
            # in_waiting first so a burst is taken in one read rather than one byte at a time.
            n = getattr(ser, "in_waiting", 0) or 1
            return ser.read(min(max_bytes, max(n, 1)))
        except Exception as exc:  # pragma: no cover - hardware dependent
            print(f"# {self.port} read failed: {exc}", file=sys.stderr)
            self.close()
            self.reconnects += 1
            return b""

    def write(self, data: bytes) -> int:
        ser = self._ensure()
        return ser.write(data) or 0

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            finally:
                self._ser = None


class TcpTransport:
    """A TCP socket carrying the raw byte stream of an emulated UART.

    QEMU's `-serial tcp:HOST:PORT,server,nowait` makes the guest's UART a listening socket, so the
    host connects as a client. There is no framing here beyond what COBS provides, and a socket read
    of zero bytes means the guest is gone rather than idle -- distinguishing those two is why this
    is not just a file object.
    """

    def __init__(self, host: str, port: int, *, timeout: float = 0.1,
                 connect_timeout: float = 10.0) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self.connect_timeout = connect_timeout
        self._sock: socket.socket | None = None
        self.closed_by_peer = False

    @property
    def description(self) -> str:
        return f"tcp {self.host}:{self.port}"

    def _ensure(self) -> socket.socket:
        if self._sock is not None:
            return self._sock
        deadline = time.time() + self.connect_timeout
        last: Exception | None = None
        while time.time() < deadline:
            try:
                s = socket.create_connection((self.host, self.port), timeout=2.0)
                s.settimeout(self.timeout)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self._sock = s
                return s
            except OSError as exc:
                last = exc
                time.sleep(0.2)
        raise ConnectionError(f"could not connect to {self.host}:{self.port}: {last}")

    def read(self, max_bytes: int = 4096) -> bytes:
        s = self._ensure()
        try:
            data = s.recv(max_bytes)
        except (socket.timeout, TimeoutError):
            return b""
        except OSError:
            self.closed_by_peer = True
            return b""
        if not data:
            # A clean zero-length read is EOF: QEMU exited or the guest closed the port.
            self.closed_by_peer = True
        return data

    def write(self, data: bytes) -> int:
        s = self._ensure()
        s.sendall(data)
        return len(data)

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None


class LoopbackTransport:
    """One end of an in-process byte pipe. `pair()` builds both ends.

    This is what makes the bridge testable without hardware or an emulator: a Python node on the far
    end, the real bridge on this one, and the real framing between them.
    """

    def __init__(self, name: str = "loopback") -> None:
        self.name = name
        self._in = bytearray()
        self._lock = threading.Lock()
        self._peer: LoopbackTransport | None = None
        self._closed = False

    @staticmethod
    def pair(a_name: str = "host", b_name: str = "node") -> tuple[LoopbackTransport, LoopbackTransport]:
        a, b = LoopbackTransport(a_name), LoopbackTransport(b_name)
        a._peer, b._peer = b, a
        return a, b

    @property
    def description(self) -> str:
        return f"loopback:{self.name}"

    def read(self, max_bytes: int = 4096) -> bytes:
        with self._lock:
            if not self._in:
                return b""
            out = bytes(self._in[:max_bytes])
            del self._in[: len(out)]
            return out

    def write(self, data: bytes) -> int:
        if self._peer is None or self._peer._closed:
            return 0
        with self._peer._lock:
            self._peer._in += data
        return len(data)

    def pending(self) -> int:
        with self._lock:
            return len(self._in)

    def close(self) -> None:
        self._closed = True


def open_transport(*, port: str | None = None, tcp: str | None = None,
                   baud: int = 921600, timeout: float = 0.1) -> Transport:
    """Build a transport from command-line style arguments. `tcp` is "host:port" or ":port"."""
    if port and tcp:
        raise ValueError("give a serial port or a tcp endpoint, not both")
    if port:
        return SerialTransport(port, baud, timeout=timeout)
    if tcp:
        host, _, p = tcp.rpartition(":")
        if not p.isdigit():
            raise ValueError(f"expected host:port, got {tcp!r}")
        return TcpTransport(host or "127.0.0.1", int(p), timeout=timeout)
    raise ValueError("no transport given")
