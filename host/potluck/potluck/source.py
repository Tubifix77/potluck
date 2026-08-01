"""Line sources: a serial port, a file, or stdin.

A file source is not a convenience. It is what lets potluck-capture be developed and
tested with no hardware attached, and what lets a capture be re-analysed
afterwards -- which is section 7.6's whole argument, that distributed embedded bugs
are not reproducible by hand.

pyserial is imported lazily so that the file and stdin paths work without it.
ESP-IDF installs pyserial into its own Python environment, so on a machine set up
to build the firmware it is already there; it is just not necessarily on the
Python that runs this tool.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Iterator, Protocol


class LineSource(Protocol):
    def lines(self) -> Iterator[str]: ...
    def close(self) -> None: ...
    @property
    def description(self) -> str: ...


class SerialSource:
    """Read newline-terminated lines from a serial port.

    Reconnects rather than exiting when the port disappears. A 24-hour soak on a
    USB port will survive a re-enumeration, and a capture tool that dies at hour
    three because someone nudged a cable has failed at its one job.
    """

    def __init__(self, port: str, baud: int = 921600, *, reconnect: bool = True) -> None:
        self.port = port
        self.baud = baud
        self.reconnect = reconnect
        self._ser = None
        self.reconnects = 0

    @property
    def description(self) -> str:
        return f"{self.port} @ {self.baud}"

    def _open(self):
        try:
            import serial  # type: ignore[import-untyped]
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise SystemExit(
                "pyserial is required to read a serial port.\n"
                "  pip install pyserial\n"
                "Or read a saved log instead:  potluck-capture --file node1.log"
            ) from exc
        # A read timeout rather than a blocking read, so a silent node does not
        # look like a hung tool and the caller can still tick its display.
        return serial.serial_for_url(self.port, baudrate=self.baud, timeout=1.0)

    def lines(self) -> Iterator[str]:
        import time

        while True:
            if self._ser is None:
                try:
                    self._ser = self._open()
                except Exception as exc:  # pragma: no cover - hardware dependent
                    if not self.reconnect:
                        raise
                    print(f"# cannot open {self.port}: {exc}; retrying in 2 s", file=sys.stderr)
                    time.sleep(2.0)
                    continue

            try:
                raw = self._ser.readline()
            except Exception as exc:  # pragma: no cover - hardware dependent
                print(f"# {self.port} read failed: {exc}", file=sys.stderr)
                self.close()
                self.reconnects += 1
                if not self.reconnect:
                    return
                time.sleep(2.0)
                continue

            if not raw:
                continue  # read timeout: no data, not an error
            yield raw.decode("utf-8", "replace").rstrip("\r\n")

    def close(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            finally:
                self._ser = None


class FileSource:
    """Read lines from a file, optionally following it as it grows."""

    def __init__(self, path: str | Path, *, follow: bool = False) -> None:
        self.path = Path(path)
        self.follow = follow
        self._fh = None

    @property
    def description(self) -> str:
        return f"{self.path}{' (following)' if self.follow else ''}"

    def lines(self) -> Iterator[str]:
        import time

        self._fh = open(self.path, "r", encoding="utf-8", errors="replace")
        try:
            while True:
                line = self._fh.readline()
                if line:
                    yield line.rstrip("\r\n")
                    continue
                if not self.follow:
                    return
                time.sleep(0.2)
        finally:
            self.close()

    def close(self) -> None:
        if self._fh is not None:
            try:
                self._fh.close()
            finally:
                self._fh = None


class StdinSource:
    """Read lines from stdin, so `idf.py monitor | potluck-capture -` works."""

    @property
    def description(self) -> str:
        return "stdin"

    def lines(self) -> Iterator[str]:
        for line in sys.stdin:
            yield line.rstrip("\r\n")

    def close(self) -> None:
        return


def list_serial_ports() -> list[tuple[str, str]]:
    """Enumerate serial ports as (device, description), or [] if pyserial is absent."""
    try:
        from serial.tools import list_ports  # type: ignore[import-untyped]
    except ImportError:
        return []
    return [(p.device, p.description or "") for p in list_ports.comports()]
