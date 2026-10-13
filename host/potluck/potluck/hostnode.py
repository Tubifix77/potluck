"""M10: the PC as a placement node -- the launcher for pot_hostnode, and its transport.

    python -m potluck.hostnode --port COM6 --package ../../keys/<m>.pkg.json \\
        --identity ../../keys/host-00fe.id [--ca ../../keys/ca.pub] [--exe <pot_hostnode>] [--out <file.jsonl>]
        [--duration <s>] [--hub [host:port]]

pot_hostnode is a Potluck node built from the boards' own C++ sources (host/native/pot_hostnode.cpp):
the same Node, reconciler and actors, so a portable actor runs here from the same source it runs from
on a board. It has no transport. This module owns the serial port and the cable's COBS/CRC framing
(potluck.serial_framing, the same as potluck.bridge), and carries raw Potluck Frames between the cable
and the node's stdin/stdout. The node's statistics (stderr, JSON lines) are kept in --out in the
capture format, with host_ts, so a report reads them beside the boards' consoles.

The package is the one the boards run: its signature is checked against the CA, and its image compiled
here exactly as for a deploy. Only one program may hold the port: not potctl or potluck-agent at the
same time -- unless potluck-hub holds it (M14): then this connects to the hub's relay instead (found by itself, or
--hub [host:port]), and the hub carries its frames unchanged while potctl, the agent and apply run beside it.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import subprocess
import sys
import tempfile
import threading
import time

from . import deploy as dp
from . import serial_framing as sf
from .manifest import parse
from .signing import load_package, read_key, verify_package
from .transport import SerialTransport, open_transport

DEFAULT_EXE = os.path.join(os.path.dirname(__file__), "..", "..", "..", "build", "tests",
                           "pot_hostnode.exe" if os.name == "nt" else "pot_hostnode")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="potluck.hostnode")
    ap.add_argument("--port", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--identity", required=True)
    ap.add_argument("--ca", default=os.path.join("..", "..", "keys", "ca.pub"))
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--out")
    ap.add_argument("--duration", type=float, default=0.0, help="stop after this many seconds (0: run until killed)")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--hub", nargs="?", const="auto", default=None,
                    help="through potluck-hub's relay (host:port; default: the running hub's) -- found by itself too")
    a = ap.parse_args(argv)
    from .hub import hub_endpoint

    if a.hub not in (None, "auto"):
        relay = a.hub  # named
    else:
        relay = hub_endpoint(None if a.hub == "auto" else a.port)  # the hub holding our port (or, asked for, any)
    if a.hub == "auto" and relay is None:
        print("hostnode: --hub given and no hub is running")
        return 2

    doc = load_package(a.package)
    verify_package(doc, read_key(a.ca).public)
    m = parse(doc["manifest"])
    img = dp.compile_image(m, int(doc["rollback_counter"]))
    with tempfile.NamedTemporaryFile(prefix="pot_host_", suffix=".img", delete=False) as f:
        f.write(img)
        img_path = f.name

    child = subprocess.Popen([os.path.abspath(a.exe), "--identity", a.identity, "--image", img_path],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    if relay is not None and not os.environ.get("POTLUCK_NO_HUB"):
        # Behind the hub the node must number its requests below 0x8000; an executable built before that would lose
        # its replies to the hub after ~32k requests to one peer (found by review), so it is refused here.
        caps = subprocess.run([os.path.abspath(a.exe), "--caps"], capture_output=True, text=True, timeout=10)
        try:
            ok = caps.returncode == 0 and json.loads(caps.stdout.strip() or "{}").get("msg_id_max", 0xFFFF) < 0x8000
        except ValueError:
            ok = False
        if not ok:
            print(f"hostnode: {a.exe} predates the hub (no --caps): rebuild it (tools/run_host_tests.ps1), or stop "
                  f"the hub and use the cable directly")
            child.terminate()
            os.unlink(img_path)
            return 2
        port = open_transport(tcp=relay)  # the hub's relay speaks the cable's framing
        print(f"# through potluck-hub at {relay}", flush=True)
    else:
        port = SerialTransport(a.port, a.baud)
    out = open(a.out, "a", encoding="utf-8") if a.out else None
    stop = threading.Event()
    counts = {"cable_rx": 0, "cable_tx": 0, "crc_bad": 0}

    def cable_to_node() -> None:
        r = sf.SerialReassembler()
        while not stop.is_set():
            chunk = port.read(4096)
            if not chunk:
                continue
            for frame in r.feed(chunk):
                counts["cable_rx"] += 1
                try:
                    child.stdin.write(struct.pack("<H", len(frame)) + frame)
                    child.stdin.flush()
                except OSError:
                    stop.set()
                    return
            counts["crc_bad"] = r.bad_crc

    def node_to_cable() -> None:
        while not stop.is_set():
            hdr = child.stdout.read(2)
            if len(hdr) < 2:
                stop.set()
                return
            (n,) = struct.unpack("<H", hdr)
            frame = child.stdout.read(n)
            port.write(sf.write_serial_frame(frame))
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
                d["rec"] = d.pop("t", "?")
                d["host_ts"] = time.time()
                out.write(json.dumps(d) + "\n")
                out.flush()
            if not line.startswith('{"t":"rec"') and not line.startswith('{"t":"host"'):
                print(line, flush=True)
        stop.set()

    threads = [threading.Thread(target=f, daemon=True) for f in (cable_to_node, node_to_cable, stats)]
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
