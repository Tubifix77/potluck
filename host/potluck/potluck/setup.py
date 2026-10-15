"""M20's setup wizard, the hub's side: find boards on USB, ask each what it is, flash it, enrol it, give it the house
Wi-Fi, light it up so a person can find it. Owner actions, served by the hub on 127.0.0.1 only (Review of M20 #5).

    python -m potluck.setup ports                      # the USB serial ports and what they look like
    python -m potluck.setup probe COM5                 # ask the chip on COM5 what it is (resets the board)
    python -m potluck.setup identify COM5 20           # blink its LED blue for 20 s

FINDING BOARDS. A port's USB id alone cannot tell a board from an adapter: on the bench the boards' serial chips are
CH343 (1A86:55D3) and the hub's frame-link adapter is a CP2102 (10C4:EA60), a chip other boards also use (ledger, "USB
IDs on the bench"). So every port with a USB id is a candidate, the port the hub itself holds is left out, and the
chip is asked (`probe`: esptool's flash-id, which resets the board into its bootloader and back).

WHAT A BOARD NEEDS. An ESP32-S3 with 16 MB of flash and PSRAM: the image's partition table assumes 16 MB (M12) and the
reconciler is compiled out without PSRAM (CLAUDE.md, the PSRAM build). `probe` says which, with the reason.

JOBS. Flashing takes a minute or two a board: it runs as a job the dashboard follows (`Jobs`), with a percentage and
a short log that never holds a password or a key.
"""

from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import threading
import time
import uuid
from pathlib import Path
from typing import Callable

#: USB ids of serial chips seen on ESP32-S3 boards; the rest of the candidates are asked all the same.
#: The board's console rate -- 115200, what CONFIG_ESP_CONSOLE_UART_DEFAULT (sdkconfig.defaults.esp32s3) builds, as
#: the build's sdkconfig.h shows; sdkconfig.defaults' 921600 does not apply then. M20's frame link on the board's own
#: USB cable runs at it.
CONSOLE_BAUD = 115200
#: One cell's boards: ESP-NOW's peer table holds 20 (ESP_NOW_MAX_TOTAL_PEER_NUM, esp_now.h; ledger) -- 19 other
#: boards and the PC's placement node. Past it is M19's cells of cells.
MAX_BOARDS = 20
KNOWN = {(0x1A86, 0x55D3): "CH343", (0x10C4, 0xEA60): "CP2102", (0x303A, None): "ESP32-S3 USB"}
NEED_FLASH_MB = 16


class SetupError(Exception):
    pass


def list_ports(exclude: tuple[str, ...] = (), lister: Callable | None = None) -> list[dict]:
    """Every USB serial port but those named in `exclude` (the hub's own frame link)."""
    if lister is None:
        from serial.tools import list_ports as lp
        lister = lp.comports
    out = []
    for p in lister():
        if p.vid is None or p.device in exclude:
            continue
        kind = KNOWN.get((p.vid, p.pid)) or KNOWN.get((p.vid, None)) or "USB serial"
        out.append({"port": p.device, "vid": f"{p.vid:04x}", "pid": f"{(p.pid or 0):04x}", "chip": kind,
                    "description": p.description, "serial": p.serial_number})
    return sorted(out, key=lambda d: d["port"])


def _esptool(args: list[str]) -> str:
    import esptool
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(buf):
        try:
            esptool.main(args)
        except SystemExit as exc:  # esptool exits on a failed connection
            if exc.code not in (0, None):
                raise SetupError(_last_line(buf.getvalue()) or "esptool failed") from None
        except Exception as exc:  # noqa: BLE001 -- esptool raises its own FatalError and serial errors
            raise SetupError(f"{type(exc).__name__}: {exc}") from None
    return buf.getvalue()


def _last_line(text: str) -> str:
    lines = [ln.strip() for ln in text.splitlines() if ln.strip()]
    return lines[-1] if lines else ""


def parse_probe(text: str) -> dict:
    """What esptool's flash-id says, tolerant of its wording."""
    def find(*pats):
        for p in pats:
            m = re.search(p, text, re.IGNORECASE | re.MULTILINE)
            if m:
                return m.group(1).strip()
        return None
    chip = find(r"^Chip type:\s*(.+)$", r"Detecting chip type\.*\s*(ESP32[\w-]*)", r"^Chip is\s+(.+)$")
    flash = find(r"Detected flash size:\s*(\d+)\s*MB", r"flash size:\s*(\d+)\s*MB")
    psram = find(r"Embedded PSRAM\s*(\d+)\s*MB", r"PSRAM\s*(\d+)\s*MB")
    mac = find(r"^MAC:\s*([0-9a-f:]{17})")
    return {"chip": chip, "flash_mb": int(flash) if flash else None, "psram_mb": int(psram) if psram else 0, "mac": mac}


def judge(info: dict) -> tuple[bool, str]:
    """Can Potluck run here, and if not, the reason a person reads."""
    chip = (info.get("chip") or "").upper()
    if not chip.startswith("ESP32-S3"):
        return False, f"this is {info.get('chip') or 'not an Espressif chip'}; Potluck's S3 Edition needs an ESP32-S3"
    if not info.get("flash_mb"):
        return False, "its flash size could not be read"
    if info["flash_mb"] < NEED_FLASH_MB:
        return False, f"it has {info['flash_mb']} MB of flash; Potluck needs {NEED_FLASH_MB} MB"
    if not info.get("psram_mb"):
        return False, "it has no PSRAM; Potluck needs it"
    return True, "ready"


def probe(port: str, runner: Callable[[list[str]], str] = _esptool) -> dict:
    info = parse_probe(runner(["--port", port, "--after", "hard-reset", "flash-id"]))
    ok, why = judge(info)
    return dict(info, port=port, ok=ok, reason=why)


# ---- flashing ---------------------------------------------------------------------------------------------------

def flash_parts(build_dir: str | Path) -> list[tuple[str, Path]]:
    """The (offset, file) pairs ESP-IDF's flasher_args.json names for a build."""
    d = Path(build_dir)
    fa = d / "flasher_args.json"
    if not fa.exists():
        raise SetupError(f"{d}: no flasher_args.json -- not an ESP-IDF build folder")
    doc = json.loads(fa.read_text(encoding="utf-8"))
    parts = [(off, d / f) for off, f in sorted(doc.get("flash_files", {}).items(), key=lambda kv: int(kv[0], 16))]
    if not parts:
        raise SetupError(f"{fa}: no flash_files")
    for _, f in parts:
        if not f.exists():
            raise SetupError(f"{f}: missing")
    return parts


def newest_build(cache: Path | None = None) -> Path:
    """M20's first run, when no board reports a firmware yet: the newest complete build on this PC (a packaged release
    carries its own image instead)."""
    if cache is None:
        from .apply import default_cache
        cache = default_cache()
    builds = [b for b in (cache / "fw").glob("*/build-*")
              if (b / "flasher_args.json").exists() and (b / "potluck_m0.bin").exists()]
    if not builds:
        raise SetupError("no Potluck firmware is built on this PC yet -- `potluck apply --plan` builds it")
    return max(builds, key=lambda b: (b / "potluck_m0.bin").stat().st_mtime)


def cell_build(version: str, cache: Path | None = None) -> Path:
    """The cached build of firmware `version` ("<commit>-<key6>", as boards report it): new boards get exactly the
    firmware the cell runs (M20, Add boards), never merely the newest on the PC."""
    if cache is None:
        from .apply import default_cache
        cache = default_cache()
    key6 = version.rsplit("-", 1)[-1]
    for d in sorted((cache / "fw").glob(f"{key6}*")):
        for b in d.glob("build-*"):
            if (b / "flasher_args.json").exists() and (b / "potluck_m0.bin").exists():
                return b
    raise SetupError(f"no build of firmware {version} on this PC -- `potluck apply` builds it")


_PCT = re.compile(r"\((\d{1,3})\s?%\)")


class _Progress(io.TextIOBase):
    """A stream esptool writes into; each '(NN %)' it prints moves the job on."""

    def __init__(self, on_pct: Callable[[int], None]):
        self.on_pct, self.text = on_pct, []

    def write(self, s: str) -> int:
        self.text.append(s)
        for m in _PCT.finditer(s):
            self.on_pct(min(100, int(m.group(1))))
        return len(s)


def flash(port: str, parts: list[tuple[str, Path]], on_pct: Callable[[int], None],
          runner: Callable | None = None) -> None:
    args = ["--port", port, "--baud", "921600", "--after", "hard-reset", "write-flash"]
    for off, f in parts:
        args += [off, str(f)]
    if runner is not None:
        runner(args, on_pct)
        return
    import esptool
    stream = _Progress(on_pct)
    with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
        try:
            esptool.main(args)
        except SystemExit as exc:
            if exc.code not in (0, None):
                raise SetupError(_last_line("".join(stream.text)) or "flashing failed") from None
    on_pct(100)


# ---- talking to a board's console ---------------------------------------------------------------------------------

def console(port: str, line: str, want: str, timeout: float = 5.0, opener: Callable | None = None) -> dict:
    """Send one `POT! ...` line, return the first JSON line whose fields contain `want` (k=v)."""
    k, _, v = want.partition("=")
    if opener is None:
        import serial

        def opener(p):
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = p, 115200, 0.05
            s.dtr = False  # never reset the board by opening its port
            s.rts = False
            s.open()
            return s
    s = opener(port)
    try:
        s.reset_input_buffer()
        s.write(line.encode("utf-8") + b"\n")
        buf, end = b"", time.time() + timeout
        while time.time() < end:
            buf += s.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                raw = raw.strip()
                if raw.startswith(b"{"):
                    try:
                        d = json.loads(raw)
                    except ValueError:
                        continue
                    if str(d.get(k)) == v:
                        return d
        raise SetupError(f"{port}: no answer to {line.split(' ')[1]!r}")
    finally:
        s.close()


def identify(port: str, seconds: int, opener: Callable | None = None) -> dict:
    return console(port, f"POT! identify {int(seconds)}", "cmd=identify", opener=opener)


def set_wifi(port: str, ssid: str, password: str, opener: Callable | None = None) -> bool:
    """Potluck's own station (the `wifi` module). Neither value is logged or returned."""
    def q(x):
        return '"' + x.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if not 8 <= len(password) <= 64:
        raise SetupError("a Wi-Fi password is 8 to 64 characters")
    d = console(port, f"POT! wifi sta {q(ssid)} {q(password)}", "t=wifi_cfg", opener=opener)  # as tools/wifi_set.py
    if d.get("result") != "sta_saved":
        raise SetupError(f"the board did not save it ({d.get('result')})")
    return True  # it takes effect at the board's next boot


def reset(port: str, opener: Callable | None = None) -> dict:
    """Restart the board, as esptool's hard reset does (RTS pulls EN low, DTR stays off so it boots its firmware):
    an enrolment under a new id and a saved Wi-Fi take effect at the next boot."""
    if opener is None:
        import serial

        def opener(p):
            s = serial.Serial()
            s.port, s.baudrate = p, 115200
            s.dtr = s.rts = False
            s.open()
            return s
    s = opener(port)
    try:
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    finally:
        s.close()
    return {"reset": port}


def enrol_board(port: str, ca_key: str, registry: str, node_id: int | None = None) -> dict:
    """potluck.enrol with the cell's registry (M19/M20 step 1). The CA key comes from an unlocked vault or its file."""
    from . import enrol as en
    from .signing import read_key
    info, cert = en.enrol(port, read_key(ca_key), registry=en.Registry(registry), node_id=node_id)
    return {"node": f"{info['assigned']:04x}", "mac_node": f"{int(info['node']):04x}",
            "reboot_needed": info["assigned"] != int(info["node"])}


# ---- a new cell --------------------------------------------------------------------------------------------------

def new_cell(folder: str | Path, system: str, passphrase: str, port: str | None = None) -> Path:
    """A cell from nothing (M20's first run): its CA, a deploy key certified by it, an empty node registry and a
    cluster file -- the secret halves sealed into `vault.json` under the owner's passphrase and never written in the
    clear. Refuses a folder that already holds keys or a vault: a CA written over orphans every board enrolled under
    it. Returns the cluster file."""
    from . import apply as ap
    from . import enrol as en
    from . import keystore
    from . import signing as sg
    d = Path(folder)
    if not 1 <= len(system) <= 31 or any(c in system for c in " /") or not system.isascii() or \
            not system.isprintable():
        raise SetupError("a cell's name is 1..31 printable ASCII characters, without spaces or '/'")
    cluster = d / f"{system}.json"
    taken = [n for n in ("vault.json", "ca.pub", "ca.key", "deploy.key", "deploy.pub", "nodes.json", cluster.name)
             if (d / n).exists()]
    if taken:
        raise SetupError(f"{d} already holds a cell ({', '.join(taken)}); nothing was changed")
    ca, deploy = sg.generate("ca", f"{system} CA"), sg.generate("deploy", f"{system} deploy")

    def blob(obj) -> bytes:
        return (json.dumps(obj, indent=2, sort_keys=True) + "\n").encode("ascii")
    vault = keystore.seal({"ca.key": blob(ca.private_dict()), "deploy.key": blob(deploy.private_dict())}, passphrase)
    bcert = en.build_cert(ca, 0, deploy.public, role=en.ROLE_DEPLOY)
    en.verify_cert(bcert, ca.public, en.ROLE_DEPLOY)
    d.mkdir(parents=True, exist_ok=True)
    files = {"vault.json": (json.dumps(vault, indent=1) + "\n").encode("ascii"),
             "ca.pub": blob(ca.public_dict()), "deploy.pub": blob(deploy.public_dict()),
             "deploy.cert": blob(sg.certify(ca, deploy)), "deploy.bcert": (bcert.hex() + "\n").encode("ascii"),
             "nodes.json": blob({"schema": 1, "nodes": {}}),
             cluster.name: (json.dumps({"schema": ap.SCHEMA, "system": system, "port": port, "boards": [],
                                        "keys": {"ca": "ca.pub", "deploy_key": "deploy.key",
                                                 "deploy_cert": "deploy.cert", "deploy_bcert": "deploy.bcert"}},
                                       indent=2) + "\n").encode("utf-8")}
    for name, data in files.items():
        with open(d / name, "xb") as f:  # x: never over a file that appeared since the check
            f.write(data)
    ap.load_cluster(str(cluster))
    return cluster


# ---- the cluster file -------------------------------------------------------------------------------------------

def add_board(cluster: str | Path, node: str, label: str | None = None, room: str | None = None,
              position: list | None = None) -> dict:
    """Add a board to the cluster file, or set its room -- the file the owner may also edit by hand, so the previous
    version is kept beside it (<name>.bak) and the result must still load (`apply` refuses it otherwise)."""
    from . import apply as ap
    p = Path(cluster)
    doc = json.loads(p.read_text(encoding="utf-8"))
    nid = int(node, 16)
    if not ap.LEASE_MIN_BOARD_ID <= nid <= 0xFFFE:
        raise SetupError(f"node {node}: a board's id is 0100..fffe")
    boards = doc.setdefault("boards", [])
    entry = next((b for b in boards if int(str(b.get("node")), 16) == nid), None)
    if entry is None:
        used = {b.get("label") for b in boards}
        lab = label or f"node-{nid:04x}"
        if lab in used:
            raise SetupError(f"label {lab!r} is taken in this cell")
        entry = {"node": f"{nid:04x}", "label": lab}
        boards.append(entry)
    elif label and label != entry.get("label"):  # a label may name the board in the package: renamed by hand only
        raise SetupError(f"node {nid:04x} is already in this cell as {entry.get('label')!r}")
    if room is not None:
        if room:
            entry["room"] = room
        else:
            entry.pop("room", None)
    if position is not None:  # [x, y] on the floor plan; [] takes the board off it
        if position:
            entry["position"] = [round(float(v), 2) for v in position]
        else:
            entry.pop("position", None)
    _write_cluster(p, doc)
    return entry


def set_plan(cluster: str | Path, plan: dict | None) -> dict | None:
    """The floor plan in the cluster file (apply.check_plan's shape), or none (None)."""
    p = Path(cluster)
    doc = json.loads(p.read_text(encoding="utf-8"))
    if plan is None:
        doc.pop("plan", None)
    else:
        doc["plan"] = plan
    _write_cluster(p, doc)
    return doc.get("plan")


def _write_cluster(p: Path, doc: dict) -> None:
    from . import apply as ap
    text = json.dumps(doc, indent=2) + "\n"
    bak = p.with_suffix(p.suffix + ".bak")
    bak.write_text(p.read_text(encoding="utf-8"), encoding="utf-8")
    p.write_text(text, encoding="utf-8")
    try:
        ap.load_cluster(str(p))
    except ap.ApplyError as exc:
        p.write_text(bak.read_text(encoding="utf-8"), encoding="utf-8")  # never leave a file apply refuses
        raise SetupError(f"the cluster file would not load ({exc}); left as it was") from None


def run_apply(cluster: str | Path, on_line: Callable[[str], None], cmd: list[str] | None = None,
              passphrase: str | None = None) -> dict:
    """`potluck apply <cluster>` as a child process (it runs beside the hub, through it); its lines go to `on_line`.
    Done when it says VERIFIED, failed on NOT VERIFIED, REFUSED or a non-zero exit. A sealed cell's passphrase goes
    to the child on its stdin, never in its arguments or environment. `cmd`: for the tests."""
    import subprocess
    cmd = cmd or [sys.executable, "-u", "-m", "potluck.apply", str(cluster)] + \
        (["--passphrase-stdin"] if passphrase is not None else [])
    verdict = None
    with subprocess.Popen(cmd, cwd=str(Path(__file__).resolve().parent.parent), stdout=subprocess.PIPE,
                          stdin=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                          errors="replace") as proc:
        try:
            proc.stdin.write((passphrase or "") + "\n")  # type: ignore[union-attr]
            proc.stdin.close()  # type: ignore[union-attr]
        except OSError:
            pass  # it exited before reading: its own output says why
        for line in proc.stdout:  # type: ignore[union-attr]
            line = line.rstrip()
            on_line(line)
            if line.startswith(("VERIFIED", "NOT VERIFIED", "REFUSED")):
                verdict = line
        code = proc.wait()
    if code != 0 or not verdict or not verdict.startswith("VERIFIED"):
        raise SetupError(verdict or f"apply exited with {code}")
    return {"verdict": verdict}


# ---- jobs -------------------------------------------------------------------------------------------------------

class Jobs:
    """Background work the dashboard follows: {id, kind, port, state: running|done|failed, pct, result, error}."""

    def __init__(self):
        self.lock = threading.Lock()
        self.jobs: dict[str, dict] = {}

    def start(self, kind: str, port: str, fn: Callable[[Callable[[int], None]], dict | None]) -> str:
        jid = uuid.uuid4().hex[:12]
        job = {"id": jid, "kind": kind, "port": port, "state": "running", "pct": 0, "result": None, "error": None,
               "started": time.time()}
        with self.lock:
            busy = [j for j in self.jobs.values() if j["port"] == port and j["state"] == "running"]
            if busy:
                raise SetupError(f"{port} is busy with {busy[0]['kind']}")
            self.jobs[jid] = job

        def pct(n: int | str) -> None:  # a number is progress; a line of text is the job's output (apply's, say)
            with self.lock:
                if isinstance(n, str):
                    job.setdefault("lines", []).append(n)
                    del job["lines"][:-200]
                else:
                    job["pct"] = max(job["pct"], n)

        def run() -> None:
            try:
                res = fn(pct)
                with self.lock:
                    job.update(state="done", pct=100, result=res)
            except Exception as exc:  # noqa: BLE001 -- a job reports what went wrong, never takes the hub down
                with self.lock:
                    job.update(state="failed", error=str(exc))

        threading.Thread(target=run, daemon=True, name=f"setup-{kind}").start()
        return jid

    def get(self, jid: str) -> dict | None:
        with self.lock:
            j = self.jobs.get(jid)
            return dict(j, lines=list(j.get("lines", []))) if j else None

    def all(self) -> list[dict]:
        with self.lock:
            return [dict(j) for j in self.jobs.values()]


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        if not argv or argv[0] == "ports":
            for p in list_ports():
                print(f"{p['port']:8} {p['vid']}:{p['pid']}  {p['chip']:13} {p['description']}")
            return 0
        if argv[0] == "probe":
            print(json.dumps(probe(argv[1])))
            return 0
        if argv[0] == "identify":
            print(json.dumps(identify(argv[1], int(argv[2]) if len(argv) > 2 else 20)))
            return 0
        print(__doc__.split("\n\n")[1])
        return 2
    except (SetupError, OSError) as exc:
        print(f"setup: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
