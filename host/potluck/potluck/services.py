"""M23: the PC's named services, served by the hub itself -- ARCHITECTURE.md section 7.5.

A host offers NAMED SERVICES (`potluck://<cluster>/svc/<name>`) that a board calls exactly as it calls any remote actor:
a CALL to the path, answered by a REPLY carrying a Value. M8 served them from a separate program (potluck.agent) the
tray never started; now the hub answers the ones the cluster file's `pc_services` names -- the PC's user's choice
(section 7.1), nothing served otherwise -- and counts each, so the model can show what the PC offers the cell
(`cell.services`).

THE SERVICES:

  time     Unix time in milliseconds (U64). A board has no battery-backed clock and, in Potluck's cell, no internet.
  speech   Says the CALL's arguments -- 1..200 bytes of UTF-8 -- with the system's own voice, then answers U32 1 (said)
           or 0 (not: empty, too long, a control character, or no voice on this PC). One phrase at a time; a phrase
           that would wait behind another for more than its deadline is refused rather than queued for ever. On
           Windows the System.Speech synthesizer (the text goes on its stdin, never on a command line); elsewhere
           espeak when it is installed.
  file     M25: a file from the folder the PC offers (the cluster file's `bulk.folder`, read-only): the CALL names it
           (pot/bulk.hpp: op 1, the app's input path hash, the name), the answer is its size (U32) or 4294967295 (no such
           file), and the hub then pushes it to the app's input in chunks (potluck.bulk). Only plain names (no folders,
           no leading dot), files up to 1 MiB.
"""

from __future__ import annotations

import os
import shutil
import struct
import subprocess
import threading
import time
from typing import Callable

from .value import Value

SPEECH_MAX = 200
SPEECH_TIMEOUT_S = 25.0  # under the board's 30 s deadline (pot/speak.hpp), so the board hears "no" rather than "gone"
_WINDOWS_SPEAK = ("Add-Type -AssemblyName System.Speech; $s = New-Object System.Speech.Synthesis.SpeechSynthesizer; "
                  "$s.Speak([Console]::In.ReadToEnd())")


def svc_time(_args: bytes) -> tuple[Value, bool]:
    return Value.of_u64(int(time.time() * 1000)), True


_speech_lock = threading.Lock()


def speak_text(text: str, runner: Callable[[list[str], str], int] | None = None) -> bool:
    """Say `text` aloud with the system's voice; True when it was said. `runner(cmd, stdin_text)` -> exit code (tests)."""
    if runner is None:
        def runner(cmd: list[str], stdin_text: str) -> int:
            flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)  # the tray's hub has no console: no window flashes
            return subprocess.run(cmd, input=stdin_text, text=True, encoding="utf-8", capture_output=True,
                                  timeout=SPEECH_TIMEOUT_S, creationflags=flags).returncode
    if os.name == "nt":
        cmd = ["powershell", "-NoProfile", "-NonInteractive", "-Command", _WINDOWS_SPEAK]
    elif shutil.which("espeak"):
        cmd = ["espeak", "--stdin"]
    else:
        return False
    if not _speech_lock.acquire(timeout=SPEECH_TIMEOUT_S):
        return False  # another phrase has held the voice too long: refused, not queued for ever
    try:
        return runner(cmd, text) == 0
    except (OSError, subprocess.TimeoutExpired):
        return False
    finally:
        _speech_lock.release()


def svc_speech(args: bytes) -> tuple[Value, bool]:
    try:
        text = args.decode("utf-8")
    except UnicodeDecodeError:
        return Value.of_u32(0), False
    if not 1 <= len(args) <= SPEECH_MAX or any(ch < " " for ch in text):
        return Value.of_u32(0), False
    said = speak_text(text)
    return Value.of_u32(1 if said else 0), said


#: name -> (what it is, the function -> (answer, served), slow). A slow one runs on a thread of its own, never on the
#: hub's reader.
SERVICES: dict[str, tuple[str, Callable[[bytes], tuple[Value, bool]] | None, bool]] = {
    "time": ("Unix time in milliseconds (U64)", svc_time, False),
    "speech": ("says up to 200 bytes of text aloud with the PC's voice; answers 1 said, 0 not (U32)", svc_speech, True),
    "file": ("a file from the folder this PC offers, pushed to the asking app in chunks; answers its size (U32), or "
             "4294967295 for none", None, False),  # served by PcServices itself: it needs the folder and the hub
}

FILE_MAX = 1 << 20  # pot/bulk.hpp's kBulkSizeMax
FILE_NAME_MAX = 64  # pot/deploy.hpp's kFileNameMax
NO_FILE = 0xFFFFFFFF  # pot/bulk.hpp's kBulkNoFile


def file_name_ok(name: str) -> bool:
    """A plain name in the offered folder: no folder part, no leading dot, printable, 1..64 bytes of UTF-8."""
    raw = name.encode("utf-8")
    return (1 <= len(raw) <= FILE_NAME_MAX and not name.startswith(".") and all(ch >= " " for ch in name) and
            not any(ch in name for ch in '/\\:*?"<>|'))


class PcServices:
    """The services this PC offers its cell (the cluster file's `pc_services`), and what each has done since the hub
    started. Thread-safe: calls arrive on the hub's reader and slow ones finish on their own threads."""

    def __init__(self, system: str, names: list[str], folder: str | None = None, on_file=None) -> None:
        from .paths import path_hash
        unknown = [n for n in names if n not in SERVICES]
        if unknown:
            raise ValueError(f"pc_services: unknown service(s) {unknown}; this hub offers {sorted(SERVICES)}")
        self.system = system
        self.folder = folder  # M25: the folder `file` serves from, or None
        self.on_file = on_file  # M25: (caller, input path hash, name, bytes) -> None, the hub's push
        self.by_hash = {path_hash(f"potluck://{system}/svc/{n}"): n for n in names}
        self.lock = threading.Lock()
        self.stats = {n: {"calls": 0, "errors": 0, "last_call_at": None, "callers": set(), "busy": 0} for n in names}

    def name_of(self, h: int) -> str | None:
        return self.by_hash.get(h)

    def slow(self, name: str) -> bool:
        return SERVICES[name][2]

    def serve(self, name: str, caller: int, args: bytes) -> Value:
        with self.lock:
            st = self.stats[name]
            st["calls"] += 1
            st["busy"] += 1
            st["last_call_at"] = int(time.time() * 1000)
            st["callers"].add(caller)
        try:
            v, ok = self._file(caller, args) if name == "file" else SERVICES[name][1](args)
        except Exception:  # noqa: BLE001 -- a service that breaks answers "not done", never kills the reader
            v, ok = Value.of_u32(0), False
        with self.lock:
            st["busy"] -= 1
            if not ok:
                st["errors"] += 1
        return v

    def _file(self, caller: int, args: bytes) -> tuple[Value, bool]:
        """M25: the file the CALL names, from the offered folder, handed to the hub to push; its size, or NO_FILE."""
        from pathlib import Path
        if self.folder is None or self.on_file is None or len(args) < 6 or args[0] != 1:
            return Value.of_u32(NO_FILE), False
        (in_hash,) = struct.unpack_from("<I", args, 1)
        try:
            name = args[5:].decode("utf-8")
        except UnicodeDecodeError:
            return Value.of_u32(NO_FILE), False
        if not file_name_ok(name):
            return Value.of_u32(NO_FILE), False
        root = Path(self.folder).resolve()
        p = (root / name).resolve()
        if p.parent != root or not p.is_file() or p.stat().st_size > FILE_MAX:
            return Value.of_u32(NO_FILE), False
        data = p.read_bytes()
        self.on_file(caller, in_hash, name, data)
        return Value.of_u32(len(data)), True

    def files(self) -> list[dict]:
        """M25: what the offered folder holds that `file` would serve."""
        from pathlib import Path
        if self.folder is None:
            return []
        try:
            entries = sorted(Path(self.folder).iterdir())
        except OSError:
            return []
        return [{"name": p.name, "bytes": p.stat().st_size} for p in entries
                if p.is_file() and file_name_ok(p.name) and p.stat().st_size <= FILE_MAX][:100]

    def snapshot(self) -> list[dict]:
        with self.lock:
            return [{"name": n, "description": SERVICES[n][0], "path": f"potluck://{self.system}/svc/{n}",
                     "state": "serving" if st["busy"] else "ready", "calls": st["calls"], "errors": st["errors"],
                     "last_call_at": st["last_call_at"], "callers": [f"{c:04x}" for c in sorted(st["callers"])]}
                    for n, st in self.stats.items()]
