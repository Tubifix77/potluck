"""Potluck's tray icon (M20): the cell's state at a glance and the way to its dashboard, with no terminal.

    potluck-tray                      (the packaged app; or: python -m potluck.tray)

The icon is Potluck's mark (potluck.brandmark), and what the hub says shows on it: the mark alone, every board alive
and every app working; an amber badge, something to look at (hover for what); a red badge, the hub has lost its link to
the cell; the mark greyed, no hub runs. It asks the hub on the PC's own address (http://127.0.0.1:8470/) every 3 seconds, so it is right whoever started
the hub. Its menu opens the dashboard or the setup wizard, and starts a hub when none runs: on the cell's cluster file
(`tray.json`'s "cluster", else the one cell the setup wizard made under %LOCALAPPDATA%/potluck/cells), or with no cell
at all, in setup mode, so the wizard can make one.

Windows only for now, through the Win32 calls Windows has (ctypes, no packages); a Linux panel would use what
ha-tray uses (GTK). The tray never stops a hub it did not start.
"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from ctypes import wintypes
from pathlib import Path

API = "http://127.0.0.1:8470"
POLL_MS = 3000

STATES = ("ok", "warn", "bad", "none")  # potluck.brandmark draws each: the mark, a badge, or the mark greyed


# ---- what the hub says ------------------------------------------------------------------------------------------

def judge(model: dict | None) -> tuple[str, str]:
    """(state, tooltip) from the hub's model, or (none, ...) when no hub answers."""
    if model is None:
        return "none", "Potluck: no hub runs on this PC"
    cell = (model.get("cell") or {}).get("system") or "the cell"
    if (model.get("hub") or {}).get("link") != "up":
        return "bad", f"Potluck {cell}: the hub has lost its link to the cell"
    boards = model.get("boards") or []
    down = [b.get("label") or b.get("node") for b in boards if b.get("state") not in ("alive", "asleep")]
    broken = [a.get("name") for a in model.get("apps") or [] if not a.get("working")]
    if not boards:
        return "warn", f"Potluck {cell}: no boards yet -- add some"
    if down or broken:
        parts = ([f"{', '.join(down)} not answering"] if down else []) + ([f"{', '.join(broken)} not working"] if broken else [])
        return "warn", f"Potluck {cell}: " + "; ".join(parts)
    return "ok", f"Potluck {cell}: all {len(boards)} boards fine"


def fetch_model(timeout: float = 1.5) -> dict | None:
    try:
        with urllib.request.urlopen(API + "/api/model", timeout=timeout) as r:
            return json.loads(r.read())
    except (OSError, ValueError):
        return None


# ---- starting a hub ----------------------------------------------------------------------------------------------

def potluck_dir() -> Path:
    """The same folder as apply.default_cache()'s parent: the wizard's cells, tray.json, the hub's logs."""
    from .apply import default_cache
    return default_cache().parent


def tray_config() -> dict:
    """%LOCALAPPDATA%/potluck/tray.json: {"cluster": <file>, "hub_args": [...], "desk": {...}} -- all optional. hub_args
    are added to the hub's command line (say --listen with its TLS files, for the dashboard on the house network); desk
    is where the Potluck Desk is (desk_command)."""
    try:
        cfg = json.loads((potluck_dir() / "tray.json").read_text(encoding="utf-8-sig"))  # Windows PowerShell writes a BOM
        return cfg if isinstance(cfg, dict) else {}
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as exc:
        # Said, not swallowed: a tray.json that does not parse used to start the hub with no cell and no word why.
        note(f"tray.json unreadable, so no cell from it: {exc}")
        return {}


def note(line: str) -> None:
    """One line in <potluck folder>/logs/tray.log -- the tray has no console to print to."""
    try:
        logs = potluck_dir() / "logs"
        logs.mkdir(parents=True, exist_ok=True)
        with open(logs / "tray.log", "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + line + "\n")
    except OSError:
        pass


def cell_file() -> Path | None:
    """The cluster file the tray starts a hub on: tray.json's "cluster", else the one cell the wizard made."""
    if tray_config().get("cluster"):
        return Path(tray_config()["cluster"])
    cells = sorted((potluck_dir() / "cells").glob("*/*.json"))
    cells = [p for p in cells if p.stem == p.parent.name]  # <name>/<name>.json, as setup.new_cell makes it
    return cells[0] if len(cells) == 1 else None


def desk_command(open_window: str | None = None, cfg: dict | None = None) -> tuple[list[str], str | None] | None:
    """The Potluck Desk's command line and folder, from tray.json's "desk" -- {"exe": <potluck-desk.exe>} once it ships
    one, or {"python": <pythonw.exe>, "cwd": <its checkout>} while it runs from source -- or None when there is none.
    `open_window`: a window to open on top (the desk's --open: addboards, library, homeassistant, boards). Asked for by
    the owner (2026-10-19): the desk in the tray's menu, beside the dashboard."""
    desk = (tray_config() if cfg is None else cfg).get("desk")
    if not isinstance(desk, dict):
        return None
    extra = ["--open", open_window] if open_window else []
    if desk.get("exe"):
        exe = Path(str(desk["exe"]))
        return ([str(exe)] + extra, str(exe.parent)) if exe.is_file() else None
    if desk.get("python") and desk.get("cwd"):
        py, cwd = Path(str(desk["python"])), Path(str(desk["cwd"]))
        if py.is_file() and (cwd / "potluck_desk").is_dir():
            return [str(py), "-m", "potluck_desk"] + extra, str(cwd)
    return None


def start_desk(open_window: str | None = None) -> bool:
    """Start the Potluck Desk, detached; False (and a line in tray.log) when tray.json names none that is there."""
    cmd = desk_command(open_window)
    if cmd is None:
        note("no Potluck Desk in tray.json (\"desk\": {\"python\", \"cwd\"} or {\"exe\"}), or not where it says")
        return False
    flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        subprocess.Popen(cmd[0], cwd=cmd[1], creationflags=flags, close_fds=True)
        return True
    except OSError as exc:
        note(f"the Potluck Desk did not start: {exc}")
        return False


def hub_command(cluster: Path | None, extra: list[str] | None = None) -> list[str]:
    if getattr(sys, "frozen", False):  # potluck-tray.exe: the hub is potluck.exe beside it
        base = [str(Path(sys.executable).with_name("potluck.exe")), "hub"]
    else:
        base = [sys.executable, "-u", "-m", "potluck.hub"]
    return base + (["--cluster", str(cluster)] if cluster else []) + [str(a) for a in (extra or [])]


def start_hub() -> subprocess.Popen | None:
    logs = potluck_dir() / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    cluster = cell_file()
    out = open(logs / "hub.log", "ab")
    cwd = str(cluster.parent) if cluster else str(potluck_dir())
    extra = tray_config().get("hub_args") if cluster else None  # a hub with no cell serves the wizard only
    cmd = hub_command(cluster, extra if isinstance(extra, list) else None)
    # Each start dated: the hub's own lines carry none, so a crash before its first line could not be placed in time
    # (found 2026-10-19 by the Desk: eleven tracebacks in hub.log, none with a time).
    out.write(f"===== {time.strftime('%Y-%m-%d %H:%M:%S')} the tray starts the hub: {' '.join(cmd)}\n".encode())
    out.flush()
    return subprocess.Popen(cmd, stdout=out,
                            stderr=subprocess.STDOUT, cwd=cwd,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


def hostnode_plan(model: dict | None) -> tuple[list[str], int] | None:
    """2026-10-21: the command that runs the PC's host node for the cell, and the package counter it runs, or None.

    The cell's presenting role is the PC's while it is in the cell when the cluster file says `present.hub` (M14) --
    and a board that presents over its own Wi-Fi wedges its radio now and then (C on the owner's bench, M0-LOG session
    42), so the PC takes that load off the boards. Run only with the PC's enrolled identity beside the cluster file
    (host-00fe.id), the cell's current package signed in the cache (an Apply from this PC keeps it), a hub to go through,
    and tray.json not saying "hostnode": false."""
    if model is None or tray_config().get("hostnode") is False:
        return None
    cluster = cell_file()
    counter = (model.get("cell") or {}).get("package")
    if cluster is None or not isinstance(counter, int):
        return None
    try:
        doc = json.loads(cluster.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError):
        return None
    if not isinstance(doc.get("present"), dict) or not doc["present"].get("hub"):
        return None
    identity = cluster.parent / "host-00fe.id"
    ca = cluster.parent / str((doc.get("keys") or {}).get("ca", "ca.pub"))
    pkg = potluck_dir() / "cache" / "packages" / f"{doc.get('system', 'lab')}-{counter}.pkg.json"
    if not (identity.is_file() and ca.is_file() and pkg.is_file()):
        return None
    if getattr(sys, "frozen", False):
        base = [str(Path(sys.executable).with_name("potluck.exe")), "hostnode"]
    else:
        base = [sys.executable, "-u", "-m", "potluck.hostnode"]
    cmd = base + ["--port", str(doc.get("port") or "COM6"), "--hub", "--package", str(pkg), "--identity", str(identity),
                  "--ca", str(ca), "--cluster", str(cluster)]
    login = potluck_dir() / "hub-broker-login.txt"  # the broker login the setup API wrote (hubtls.login_path)
    if login.is_file():
        cmd += ["--broker-login", str(login)]
    return cmd, counter


def start_hostnode(cmd: list[str], cwd: Path) -> subprocess.Popen:
    logs = potluck_dir() / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    out = open(logs / "hostnode.log", "ab")
    out.write(f"===== {time.strftime('%Y-%m-%d %H:%M:%S')} the tray starts the PC's host node\n".encode())
    out.flush()
    return subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=str(cwd),
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))


class HostnodeKeeper:
    """2026-10-21: the tray keeps the PC's host node running beside the hub (hostnode_plan), started again when it stops
    (at most once in BACKOFF_S) and whenever the cell's package changes -- the node runs the package it was started with."""

    BACKOFF_S = 60.0

    def __init__(self, plan=None, start=None, clock=time.monotonic) -> None:
        self._plan = plan or hostnode_plan
        self._start = start or (lambda cmd: start_hostnode(cmd, potluck_dir()))
        self._clock = clock
        self.child = None
        self.counter: int | None = None
        self.last_start: float | None = None
        self.starts = 0

    def _stop(self) -> None:
        if self.child is not None and self.child.poll() is None:
            self.child.terminate()
            try:
                self.child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.child.kill()
        self.child = None

    def seen(self, model: dict | None) -> bool:
        """One poll; True when it started the host node."""
        plan = self._plan(model)
        if plan is None:
            if model is not None:  # the hub answers and says no host node (or hostnode: false): none runs
                self._stop()
            return False
        cmd, counter = plan
        running = self.child is not None and self.child.poll() is None
        if running and counter == self.counter:
            return False
        if running:  # a new package: the node runs the one it was started with
            self._stop()
        elif self.last_start is not None and self._clock() - self.last_start < self.BACKOFF_S:
            return False
        self.child = self._start(cmd)
        self.counter = counter
        self.last_start = self._clock()
        self.starts += 1
        return True


class HubKeeper:
    """A tray started at login keeps a hub running for as long as it runs. Found 2026-10-16 (M0-LOG session 46): the
    tray started a hub only when it started itself, and only if none answered -- so when the hub that was running then
    stopped, the dashboard and Home Assistant lost the cell until someone noticed. A hub that answers is never touched;
    one that stops answering for MISSES polls is replaced, and never more often than once in BACKOFF_S, so a hub that
    cannot start (no cable, a broken cell file) is not restarted in a loop."""

    MISSES = 3  # polls with no answer (POLL_MS apart) before a hub is started
    BACKOFF_S = 60.0

    def __init__(self, start=None, clock=time.monotonic) -> None:
        self._start = start or start_hub
        self._clock = clock
        self.child: subprocess.Popen | None = None
        self.misses = 0
        self.last_start: float | None = None
        self.starts = 0

    def start(self) -> None:
        self.child = self._start()
        self.last_start = self._clock()
        self.starts += 1
        self.misses = 0

    def seen(self, model: dict | None) -> bool:
        """One poll's answer; True when it started a hub."""
        if model is not None:
            self.misses = 0
            return False
        self.misses += 1
        if self.misses < self.MISSES:
            return False
        if self.last_start is not None and self._clock() - self.last_start < self.BACKOFF_S:
            return False  # ours may still be coming up, or cannot start: once a minute at most
        self.start()
        return True


# ---- Win32 ------------------------------------------------------------------------------------------------------

if sys.platform == "win32":
    user32, shell32, kernel32 = ctypes.windll.user32, ctypes.windll.shell32, ctypes.windll.kernel32
    LRESULT = ctypes.c_ssize_t
    WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM)

    class WNDCLASSEXW(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.UINT), ("style", wintypes.UINT), ("lpfnWndProc", WNDPROC),
                    ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int), ("hInstance", wintypes.HINSTANCE),
                    ("hIcon", wintypes.HICON), ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH),
                    ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR), ("hIconSm", wintypes.HICON)]

    class NOTIFYICONDATAW(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("hWnd", wintypes.HWND), ("uID", wintypes.UINT),
                    ("uFlags", wintypes.UINT), ("uCallbackMessage", wintypes.UINT), ("hIcon", wintypes.HICON),
                    ("szTip", wintypes.WCHAR * 128), ("dwState", wintypes.DWORD), ("dwStateMask", wintypes.DWORD),
                    ("szInfo", wintypes.WCHAR * 256), ("uVersion", wintypes.UINT), ("szInfoTitle", wintypes.WCHAR * 64),
                    ("dwInfoFlags", wintypes.DWORD), ("guidItem", ctypes.c_byte * 16), ("hBalloonIcon", wintypes.HICON)]

    user32.DefWindowProcW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.DefWindowProcW.restype = LRESULT
    user32.CreateWindowExW.argtypes = [wintypes.DWORD, wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD,
                                       ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HWND,
                                       wintypes.HMENU, wintypes.HINSTANCE, wintypes.LPVOID]
    user32.CreateWindowExW.restype = wintypes.HWND
    user32.LoadImageW.argtypes = [wintypes.HINSTANCE, wintypes.LPCWSTR, wintypes.UINT, ctypes.c_int, ctypes.c_int,
                                  wintypes.UINT]
    user32.LoadImageW.restype = wintypes.HANDLE
    user32.TrackPopupMenu.argtypes = [wintypes.HMENU, wintypes.UINT, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                      wintypes.HWND, wintypes.LPVOID]
    user32.AppendMenuW.argtypes = [wintypes.HMENU, wintypes.UINT, ctypes.c_size_t, wintypes.LPCWSTR]
    user32.SetTimer.argtypes = [wintypes.HWND, ctypes.c_size_t, wintypes.UINT, wintypes.LPVOID]
    shell32.Shell_NotifyIconW.argtypes = [wintypes.DWORD, ctypes.POINTER(NOTIFYICONDATAW)]

WM_DESTROY, WM_COMMAND, WM_TIMER, WM_APP = 0x0002, 0x0111, 0x0113, 0x8000
WM_LBUTTONUP, WM_RBUTTONUP = 0x0202, 0x0205
NIM_ADD, NIM_MODIFY, NIM_DELETE = 0, 1, 2
NIF_MESSAGE, NIF_ICON, NIF_TIP = 1, 2, 4
MF_STRING, MF_GRAYED, MF_SEPARATOR = 0, 1, 0x800
CMD_OPEN, CMD_SETUP, CMD_START, CMD_DESK, CMD_QUIT = 1, 2, 3, 4, 9


class Tray:
    def __init__(self, keeper: HubKeeper | None = None, hostkeeper: HostnodeKeeper | None = None) -> None:
        self.hostkeeper = hostkeeper
        self.state, self.tip = "none", "Potluck"
        self.icons: dict[str, int] = {}
        self.child: subprocess.Popen | None = None
        self.keeper = keeper
        self.model: dict | None = None
        self._lock = threading.Lock()
        folder = Path(tempfile.gettempdir()) / "potluck-tray"
        folder.mkdir(exist_ok=True)
        from .brandmark import ico
        for st in STATES:
            p = folder / f"{st}.ico"
            p.write_bytes(ico(32, st))
            self.icons[st] = user32.LoadImageW(None, str(p), 1, 0, 0, 0x10)  # IMAGE_ICON, LR_LOADFROMFILE
        self._proc = WNDPROC(self._wndproc)  # kept: Windows calls it for as long as the window lives
        wc = WNDCLASSEXW(cbSize=ctypes.sizeof(WNDCLASSEXW), lpfnWndProc=self._proc,
                         hInstance=kernel32.GetModuleHandleW(None), lpszClassName="PotluckTray")
        user32.RegisterClassExW(ctypes.byref(wc))
        self.hwnd = user32.CreateWindowExW(0, "PotluckTray", "Potluck", 0, 0, 0, 0, 0, None, None, wc.hInstance, None)
        self.nid = NOTIFYICONDATAW(cbSize=ctypes.sizeof(NOTIFYICONDATAW), hWnd=self.hwnd, uID=1,
                                   uFlags=NIF_MESSAGE | NIF_ICON | NIF_TIP, uCallbackMessage=WM_APP + 1,
                                   hIcon=self.icons["none"])
        self.nid.szTip = "Potluck"
        shell32.Shell_NotifyIconW(NIM_ADD, ctypes.byref(self.nid))
        user32.SetTimer(self.hwnd, 1, POLL_MS, None)
        self._poll_async()

    def _poll_async(self) -> None:
        def run():
            m = fetch_model()
            with self._lock:
                self.model = m
                self.state, self.tip = judge(m)
                if self.keeper is not None:
                    self.keeper.seen(m)
            if self.hostkeeper is not None:
                try:
                    self.hostkeeper.seen(m)
                except OSError as exc:
                    note(f"the PC's host node did not start: {exc}")
        threading.Thread(target=run, daemon=True).start()

    def _show(self) -> None:
        with self._lock:
            st, tip = self.state, self.tip
        self.nid.hIcon = self.icons[st]
        self.nid.szTip = tip[:127]
        shell32.Shell_NotifyIconW(NIM_MODIFY, ctypes.byref(self.nid))

    def _menu(self) -> None:
        menu = user32.CreatePopupMenu()
        with self._lock:
            have_hub = self.model is not None
        if desk_command() is not None:  # the Potluck Desk first, when tray.json says where it is (2026-10-19)
            user32.AppendMenuW(menu, MF_STRING, CMD_DESK, "Open Potluck Desk")
        user32.AppendMenuW(menu, MF_STRING | (0 if have_hub else MF_GRAYED), CMD_OPEN, "Open the dashboard")
        user32.AppendMenuW(menu, MF_STRING | (0 if have_hub else MF_GRAYED), CMD_SETUP, "Add boards")
        user32.AppendMenuW(menu, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(menu, MF_STRING | (MF_GRAYED if have_hub else 0), CMD_START, "Start the hub")
        user32.AppendMenuW(menu, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(menu, MF_STRING, CMD_QUIT, "Quit the tray icon")
        pt = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        user32.SetForegroundWindow(self.hwnd)  # so the menu closes when the user clicks elsewhere
        user32.TrackPopupMenu(menu, 0, pt.x, pt.y, 0, self.hwnd, None)
        user32.DestroyMenu(menu)

    def _wndproc(self, hwnd, msg, wparam, lparam):
        if msg == WM_TIMER:
            self._show()
            self._poll_async()
            return 0
        if msg == WM_APP + 1 and lparam in (WM_LBUTTONUP, WM_RBUTTONUP):
            self._menu()
            return 0
        if msg == WM_COMMAND:
            cmd = wparam & 0xFFFF
            if cmd == CMD_OPEN:
                os.startfile(API + "/")
            elif cmd == CMD_DESK:
                start_desk()
            elif cmd == CMD_SETUP:
                if desk_command() is None or not start_desk("addboards"):  # the desk's Add boards where there is one
                    os.startfile(API + "/setup.html")
            elif cmd == CMD_START and self.keeper is not None:
                with self._lock:
                    self.keeper.start()
            elif cmd == CMD_START and self.child is None:
                self.child = start_hub()
            elif cmd == CMD_QUIT:
                user32.DestroyWindow(hwnd)
            return 0
        if msg == WM_DESTROY:
            shell32.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(self.nid))
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    def run(self) -> int:
        msg = wintypes.MSG()
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        return 0


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    keeper = HubKeeper() if "--start-hub" in argv else None
    if keeper is not None and fetch_model() is None:
        keeper.start()  # at login: a hub if none runs, then the icon -- and again whenever it stops answering
    if sys.platform != "win32":
        print("potluck-tray: the tray icon is Windows-only for now; the hub and dashboard work without it")
        return 2
    return Tray(keeper, HostnodeKeeper() if keeper is not None else None).run()


if __name__ == "__main__":
    sys.exit(main())
