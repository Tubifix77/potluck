"""M19: stub cells on the PC -- the launcher for pot_stubs, and its transport.

    python -m potluck.stubs --port COM6 --package <the tree's signed package> [--ca-key ../../keys/ca.key]
        [--ids ../../keys/stubs] [--out <file.jsonl>] [--duration S] [--sleep <hex id>@<s>+<for s>]

The bench has three boards (ARCHITECTURE section 13, M19's accept): they are one real base cell, and pot_stubs
(host/native/pot_stubs.cpp) hosts the rest of the tree -- the package's boards with ids F000..FDFF ("stub": true in
the cluster file) -- as real pot::Node objects, joined to the cell through the cable. This module makes each stub an
identity under the cell's CA the first time (keys/stubs/stub-<id>.id: a fresh Ed25519 seed and a node certificate,
the enrolment a board gets by cable), compiles the package's image as `apply` did, and carries frames between the
cable (or potluck-hub's relay, found by itself as potluck.hostnode does) and pot_stubs.

The cabled board must relay (`POT! relay 1` on its console, as for M10's host node): the stubs reach the rest of the
cell through it.

--sleep F101@120+90: stub F101's radio off 120 s in, for 90 s -- a stub foreman killed, and brought back.
--watch F301:cell-bench/warmest: stub F301 reads that path every two seconds (a "watch" line with owner, quality,
value, unit and age). --refer F101:8160:node-7368/hw/die_temp: stub F101 asks for that path as a referral through
8160 (the real cell's foreman), then watches it.
"""

from __future__ import annotations

import argparse
import json
import os
import secrets
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from . import deploy as dp
from . import serial_framing as sf
from .manifest import parse
from .signing import load_package, read_key, verify_package
from .transport import SerialTransport, open_transport

_BUILD = os.path.join(os.path.dirname(__file__), "..", "..", "..", "build")
#: the optimised build when there is one (tools/run_host_tests.ps1 -Release -BuildDir build/release -Target pot_stubs):
#: nine stubs checking Ed25519 signatures in a Debug build fell behind their own heartbeats.
DEFAULT_EXE = os.path.join(_BUILD, "release" if os.path.exists(os.path.join(_BUILD, "release", "pot_stubs.exe")) else "tests",
                           "pot_stubs.exe" if os.name == "nt" else "pot_stubs")
STUB_FIRST, STUB_LAST = 0xF000, 0xFDFF


def stub_ids(m) -> list[int]:
    """The package's stub boards: those with a position actor and an id in the stub range."""
    return sorted({a.pin for a in m.actors if a.module == "builtin:position" and a.pin is not None
                   and STUB_FIRST <= a.pin <= STUB_LAST})


def make_identities(ids: list[int], folder: Path, ca_key: str) -> list[int]:
    """An enrolled identity per stub that has none yet (pot_hostnode's file format). Returns the ids made."""
    from . import ed25519_ref as ed
    from .enrol import build_cert

    folder.mkdir(parents=True, exist_ok=True)
    made = []
    ca = None
    for n in ids:
        p = folder / f"stub-{n:04x}.id"
        if p.exists():
            continue
        if ca is None:
            ca = read_key(ca_key)
        seed = secrets.token_bytes(32)
        cert = build_cert(ca, n, ed.public_key(seed))
        p.write_text(f"seed {seed.hex()}\nca {ca.public.hex()}\ncert {cert.hex()}\n", encoding="ascii")
        made.append(n)
    return made


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="potluck.stubs")
    ap.add_argument("--port", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--ca", default=os.path.join("..", "..", "keys", "ca.pub"))
    ap.add_argument("--ca-key", default=os.path.join("..", "..", "keys", "ca.key"))
    ap.add_argument("--ids", default=os.path.join("..", "..", "keys", "stubs"))
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out")
    ap.add_argument("--duration", type=float, default=0.0)
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--sleep", action="append", default=[], help="<hex id>@<seconds>+<seconds>: a stub's radio off")
    ap.add_argument("--hub", nargs="?", const="auto", default=None)
    ap.add_argument("--watch", action="append", default=[], help="<hex stub>:<path>")
    ap.add_argument("--refer", action="append", default=[], help="<hex stub>:<hex via>:<path>")
    a = ap.parse_args(argv)
    from .hub import hub_endpoint

    relay = a.hub if a.hub not in (None, "auto") else hub_endpoint(None if a.hub == "auto" else a.port)
    doc = load_package(a.package)
    verify_package(doc, read_key(a.ca).public)
    m = parse(doc["manifest"])
    ids = stub_ids(m)
    if not ids:
        print("stubs: the package has no stub boards (ids F000..FDFF)")
        return 2
    made = make_identities(ids, Path(a.ids), a.ca_key)
    if made:
        print(f"# enrolled {len(made)} stub identit{'y' if len(made) == 1 else 'ies'} under the cell's CA: "
              + ", ".join(f"{n:04x}" for n in made), flush=True)
    img = dp.compile_image(m, int(doc["rollback_counter"]))
    with tempfile.NamedTemporaryFile(prefix="pot_stubs_", suffix=".img", delete=False) as f:
        f.write(img)
        img_path = f.name
    child = subprocess.Popen([os.path.abspath(a.exe), "--image", img_path, "--ids", os.path.abspath(a.ids)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    stdin_lock = threading.Lock()

    def control(text: str) -> None:
        b = text.encode()
        try:
            with stdin_lock:
                child.stdin.write(struct.pack("<H", 0x8000 | len(b)) + b)
                child.stdin.flush()
        except OSError:
            pass

    if relay is not None and not os.environ.get("POTLUCK_NO_HUB"):
        port = open_transport(tcp=relay)
        print(f"# through potluck-hub at {relay}", flush=True)
    else:
        port = SerialTransport(a.port, a.baud)
    out = open(a.out, "a", encoding="utf-8") if a.out else None
    stop = threading.Event()
    counts = {"cable_rx": 0, "cable_tx": 0}

    def cable_to_stubs() -> None:
        r = sf.SerialReassembler()
        while not stop.is_set():
            chunk = port.read(4096)
            if not chunk:
                if getattr(port, "closed_by_peer", False):
                    print("# the link closed (the hub stopped?): stopping", flush=True)
                    stop.set()
                    return
                continue
            for frame in r.feed(chunk):
                counts["cable_rx"] += 1
                try:
                    with stdin_lock:
                        child.stdin.write(struct.pack("<H", len(frame)) + frame)
                        child.stdin.flush()
                except OSError:
                    stop.set()
                    return

    def stubs_to_cable() -> None:
        while not stop.is_set():
            hdr = child.stdout.read(2)
            if len(hdr) < 2:
                stop.set()
                return
            (n,) = struct.unpack("<H", hdr)
            port.write(sf.write_serial_frame(child.stdout.read(n)))
            counts["cable_tx"] += 1

    def stats() -> None:
        for raw in iter(child.stderr.readline, b""):
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            if out is not None and line.startswith("{"):
                try:
                    d = json.loads(line)
                except ValueError:
                    continue
                d["host_ts"] = time.time()
                out.write(json.dumps(d) + "\n")
                out.flush()
            if not line.startswith('{"t":"stub"'):
                print(line, flush=True)
        stop.set()

    def sleeper(spec: str) -> None:
        who, _, when = spec.partition("@")
        at, _, dur = when.partition("+")
        if stop.wait(float(at)):
            return
        control(f"sleep {who} 1")
        print(f"# stub {who} asleep", flush=True)
        if dur and not stop.wait(float(dur)):
            control(f"sleep {who} 0")
            print(f"# stub {who} awake", flush=True)

    from .paths import path_hash

    def full(p: str) -> str:
        return p if p.startswith("potluck://") else f"potluck://{m.system}/{p.lstrip('/')}"

    def asks() -> None:
        if stop.wait(20.0):  # once the stubs have found the cell
            return
        for w in a.watch:
            who, _, p = w.partition(":")
            control(f"watch {who} {path_hash(full(p))}")
            print(f"# stub {who} watches {full(p)} ({path_hash(full(p))})", flush=True)
        for r in a.refer:
            who, via, p = r.split(":", 2)
            control(f"refer {who} {via} {path_hash(full(p))}")
            print(f"# stub {who} asks {via} for {full(p)} as a referral ({path_hash(full(p))})", flush=True)

    threads = [threading.Thread(target=f, daemon=True) for f in (cable_to_stubs, stubs_to_cable, stats, asks)]
    threads += [threading.Thread(target=sleeper, args=(s,), daemon=True) for s in a.sleep]
    for t in threads:
        t.start()
    end = time.time() + a.duration if a.duration > 0 else None
    try:
        while not stop.is_set() and (end is None or time.time() < end):
            time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    stop.set()
    child.terminate()
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
    port.close()
    if out is not None:
        out.close()
    os.unlink(img_path)
    print(f"# cable frames in {counts['cable_rx']}, out {counts['cable_tx']}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
