"""M28: the patterns of notes the PC plays when a board's tone actor asks (the `sound` service, potluck.services).

A pattern is a number: 0 stops whatever plays, 1..8 one of PATTERNS below -- short, fixed sequences of (frequency in Hz,
milliseconds), 0 Hz a rest. No audio files and nothing from outside: what the PC plays is chosen here, as speak's phrase
is the package's. A pattern plays on a thread of its own, so the hub answers the board at once; a new request replaces
the one playing, and a stop takes effect at the next note (no note is longer than 600 ms).

On Windows the notes go through winsound.Beep, which since Windows 7 plays on the session's default sound device
(ledger: the Python and Win32 docs); it blocks for each note. Elsewhere, or with no sound device, nothing plays and the
service answers 0, so the board's result reads refused rather than a sound nobody hears.
"""

from __future__ import annotations

import threading
import time
from typing import Callable

#: number -> (name, notes). A note is (Hz, ms); 0 Hz is a rest. Beep's range is 37..32,767 Hz.
PATTERNS: dict[int, tuple[str, list[tuple[int, int]]]] = {
    1: ("notice", [(880, 120), (0, 80), (880, 120)]),
    2: ("alarm", [(1200, 200), (800, 200)] * 4),
    3: ("rising", [(523, 150), (659, 150), (784, 150), (1047, 250)]),
    4: ("falling", [(1047, 150), (784, 150), (659, 150), (523, 250)]),
    5: ("three", [(1000, 100), (0, 100)] * 2 + [(1000, 100)]),
    6: ("long", [(600, 600), (600, 200)]),
    7: ("chime", [(1319, 150), (988, 300)]),
    8: ("low", [(300, 400), (0, 100), (300, 400)]),
}
PATTERN_MAX = max(PATTERNS)


def _beep() -> Callable[[int, int], None] | None:
    try:
        import winsound
    except ImportError:
        return None
    return winsound.Beep


class Player:
    """Plays one pattern at a time on a thread of its own. `beep(hz, ms)` blocks for the note (winsound.Beep);
    `sleep(seconds)` makes a rest. Both are injectable for tests."""

    def __init__(self, beep: Callable[[int, int], None] | None = None, sleep: Callable[[float], None] = time.sleep,
                 have_sound: bool | None = None) -> None:
        self._beep = beep if beep is not None else _beep()
        self._sleep = sleep
        self.have_sound = self._beep is not None if have_sound is None else have_sound
        self._lock = threading.Lock()
        self._sounding = threading.Lock()  # one pattern sounds at a time
        self._gen = 0             # each request bumps it; a playing thread stops when it no longer matches
        self._thread: threading.Thread | None = None
        self.played = 0            # patterns started
        self.errors = 0            # notes the sound device refused

    def play(self, pattern: int) -> bool:
        """Start a pattern (0 stops). True if it was taken: 0, or a known pattern on a PC that has sound."""
        if pattern == 0:
            with self._lock:
                self._gen += 1
            return True
        if pattern not in PATTERNS or not self.have_sound:
            return False
        with self._lock:
            self._gen += 1
            gen = self._gen
            self.played += 1
        t = threading.Thread(target=self._run, args=(gen, PATTERNS[pattern][1]), name=f"potluck-sound-{pattern}",
                             daemon=True)
        self._thread = t
        t.start()
        return True

    def _run(self, gen: int, notes: list[tuple[int, int]]) -> None:
        # One pattern at a time: a newer one waits for the note now sounding, then this one sees it is outdated.
        with self._sounding:
            for hz, ms in notes:
                with self._lock:
                    if gen != self._gen:
                        return
                if hz == 0:
                    self._sleep(ms / 1000.0)
                    continue
                try:
                    self._beep(hz, ms)
                except RuntimeError:
                    self.errors += 1  # the device refused a note: this pattern ends
                    return

    def wait(self, timeout: float = 5.0) -> None:
        """Tests: until the current pattern is done."""
        t = self._thread
        if t is not None:
            t.join(timeout)


_player: Player | None = None


def player() -> Player:
    global _player
    if _player is None:
        _player = Player()
    return _player
