"""potctl -- talk to a node over the frame link. ARCHITECTURE.md M1/M2, section 4 rule 2.

    python -m potluck.ctl --tcp 127.0.0.1:5555 ls
    python -m potluck.ctl --tcp 127.0.0.1:5555 read potluck://lab/node-1001/sys/uptime
    python -m potluck.ctl --port COM7 read sys/heap-free --node 1001
    python -m potluck.ctl --tcp :5555 write test/setpoint 3.25 --type f32 --node 1001
    python -m potluck.ctl --tcp :5555 watch sys/heap-free --node 1001 --interval 1

Every read prints section 4 rule 2's whole tuple. There is no flag to print just the number, and
that is not an omission -- a tool that can be asked for a bare value is a tool whose output will end
up in a script that has lost the age.

Section 7.1 forbids anything above the bridge owning a serial port, so this drives a Bridge rather
than opening a port itself. At M2 the Bridge is in-process; when potluck-agent arrives at M8 it becomes
a socket and nothing here changes.
"""

from __future__ import annotations

import argparse
import sys
import time

from . import frame as fr
from .bridge import Bridge, BridgeError, RequestTimeout
from .capture import CaptureWriter
from .live import Monitor
from .paths import path_hash
from .sys_paths import describe, sys_paths_for
from .value import NsError, Value, ValueType

#: Shorthand -> the canonical path, so `read sys/uptime --node 1001` works. The full path is what
#: goes on the wire; the shorthand only exists at the keyboard.
_TYPE_BUILDERS = {
    "bool": lambda s: Value.of_bool(s.strip().lower() in ("1", "true", "yes", "on")),
    "i32": lambda s: Value.of_i32(int(s, 0)),
    "u32": lambda s: Value.of_u32(int(s, 0)),
    "f32": lambda s: Value.of_f32(float(s)),
    "i64": lambda s: Value.of_i64(int(s, 0)),
    "u64": lambda s: Value.of_u64(int(s, 0)),
    "f64": lambda s: Value.of_f64(float(s)),
    "bytes": lambda s: Value.of_bytes(bytes.fromhex(s.removeprefix("0x"))),
}


def canonical(path: str, node_id: int | None) -> str:
    """Expand a shorthand like `sys/uptime` into the full path for `node_id`."""
    if path.startswith("potluck://"):
        return path
    if node_id is None:
        raise SystemExit(
            f"'{path}' is a shorthand; give --node <hex id> or use the full potluck:// path"
        )
    return f"potluck://lab/node-{node_id:04x}/{path.lstrip('/')}"


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="potctl",
        description="Read and write a Potluck node's namespace over the frame link.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    link = p.add_mutually_exclusive_group(required=True)
    link.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyUSB0")
    link.add_argument("--tcp", help="host:port of an emulated UART, e.g. 127.0.0.1:5555")

    p.add_argument("--baud", type=int, default=921600)
    p.add_argument("--node", type=lambda s: int(s, 16), default=None,
                   help="node id in hex, for expanding shorthand paths and as the READ destination")
    p.add_argument("--timeout", type=float, default=1.0, help="per-request timeout in seconds")
    p.add_argument("--hello-timeout", type=float, default=3.0)
    p.add_argument("--capture", help="tee every frame to a section 7.6 capture file")
    p.add_argument("--no-heartbeat", action="store_true",
                   help="do not heartbeat; the node will mark this host Dead after its death window")
    p.add_argument("--verbose", "-v", action="store_true", help="log frames and link events")

    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("ls", help="read every built-in resource of --node")
    s.add_argument("--node-of", type=lambda s: int(s, 16), default=None,
                   help="list this node's built-ins instead of --node's")

    s = sub.add_parser("read", help="read one resource")
    s.add_argument("path")

    s = sub.add_parser("write", help="write one resource")
    s.add_argument("path")
    s.add_argument("value")
    s.add_argument("--type", default="f32", choices=sorted(_TYPE_BUILDERS),
                   help="the wire type to send (default f32)")

    s = sub.add_parser("watch", help="read one resource repeatedly")
    s.add_argument("path")
    s.add_argument("--interval", type=float, default=1.0)
    s.add_argument("--count", type=int, default=0, help="stop after this many reads (0 = forever)")

    s = sub.add_parser("soak", help="read every built-in resource in a loop, for a duration")
    s.add_argument("--seconds", type=float, default=600.0,
                   help="how long to keep sweeping (default 600, M2's ten minutes)")
    s.add_argument("--interval", type=float, default=1.0, help="pause between sweeps")

    sub.add_parser("info", help="say hello and report what the node announced")

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    capture = CaptureWriter(args.capture) if args.capture else None

    # A Monitor is kept whenever a capture is being written, so the session can state the namespace
    # state it observed and the capture can be checked against it afterwards. That comparison is
    # §13-M2's acceptance test; without it a capture is a file nobody has ever verified is sufficient.
    monitor = Monitor() if capture is not None else None

    def log(msg: str) -> None:
        print(f"# {msg}", file=sys.stderr, flush=True)

    def show_frame(f, direction: str) -> None:
        print(f"  {direction} {f}", file=sys.stderr, flush=True)

    def fold(f, direction: str) -> None:
        if monitor is not None and direction == "rx" and f.opcode == fr.Op.REPLY:
            monitor.on_reply_frame(f.src, f.payload)
        if args.verbose:
            show_frame(f, direction)

    bridge = Bridge.open(
        port=args.port,
        tcp=args.tcp,
        baud=args.baud,
        capture=capture,
        heartbeat=not args.no_heartbeat,
        on_log=log,
        on_frame=fold if (monitor is not None or args.verbose) else None,
    )

    try:
        bridge.start()
        print(f"# {bridge.describe()}", file=sys.stderr)
        ack = bridge.hello(timeout=args.hello_timeout)
        if ack is None:
            print(
                f"# no answer from {bridge.transport.description} in {args.hello_timeout:.1f} s.\n"
                f"#   rx {bridge.stats.rx_frames} frames, {bridge.stats.rx_bytes} bytes, "
                f"{bridge.stats.bad_frames} bad.\n"
                f"# A node that is powered and running answers HELLO immediately. Nothing at all "
                f"means the link, not the namespace.",
                file=sys.stderr,
            )
            return 3
        node_id = args.node if args.node is not None else bridge.peer_node_id
        print(f"# node 0x{node_id:04x}, boot epoch {ack.boot_epoch}, "
              f"death window {ack.hb_period_ms} ms x {ack.hb_miss_limit} = "
              f"{ack.hb_period_ms * ack.hb_miss_limit} ms", file=sys.stderr)

        if args.cmd == "info":
            return cmd_info(bridge)
        if args.cmd == "ls":
            return cmd_ls(bridge, args.node_of if args.node_of is not None else node_id, args)
        if args.cmd == "read":
            return cmd_read(bridge, canonical(args.path, node_id), args)
        if args.cmd == "write":
            return cmd_write(bridge, canonical(args.path, node_id), args)
        if args.cmd == "watch":
            return cmd_watch(bridge, canonical(args.path, node_id), args)
        if args.cmd == "soak":
            return cmd_soak(bridge, node_id, args)
        return 2
    finally:
        try:
            bridge.bye()
        except BridgeError:
            pass
        bridge.close()
        if capture is not None:
            path = capture.path
            capture.close()
            # The digest of what this session actually observed, and the command that checks the
            # capture reproduces it. §13-M2's acceptance test, printed rather than left to be
            # reconstructed by whoever finds the file later.
            if monitor is not None and path is not None:
                digest = monitor.namespace_digest()
                print(f"# capture -> {path}  ({len(monitor.ns)} namespace entries observed)",
                      file=sys.stderr)
                print(f"# verify:  python -m potluck --replay {path} --expect-digest {digest}",
                      file=sys.stderr)


def cmd_info(bridge: Bridge) -> int:
    hb = bridge.last_heartbeat
    print(bridge.describe())
    if hb is None:
        print("no heartbeat received yet")
    else:
        pdr = hb.tx_pdr
        print(f"heartbeat #{hb.hb_seq}: uptime {hb.uptime_ms} ms, epoch {hb.boot_epoch}, "
              f"free DRAM {hb.free_dram_kib} KiB, ESP-NOW v{hb.espnow_version}")
        # "unmeasured" is printed as such. A default of 1.0 here would be a fabricated measurement.
        print(f"  tx pdr {'unmeasured' if pdr is None else f'{pdr * 100:.2f}%'}, "
              f"rtt min {hb.rtt_min_us if hb.rtt_min_us is not None else 'unmeasured'} us")
    print(f"link: {bridge.stats.as_dict()}")
    return 0


def cmd_ls(bridge: Bridge, node_id: int, args) -> int:
    paths = sys_paths_for(node_id)
    width = max(len(describe(p)) for p in paths)
    failures = 0
    for p in paths:
        label = describe(p)
        try:
            r = bridge.read(p, timeout=args.timeout)
        except RequestTimeout:
            print(f"{label:<{width}}  TIMEOUT")
            failures += 1
            continue
        print(f"{label:<{width}}  {r.format()}")
        if not r.usable():
            failures += 1
    print(f"\n{len(paths) - failures}/{len(paths)} answered with a usable value")
    return 0 if failures == 0 else 1


def cmd_read(bridge: Bridge, path: str, args) -> int:
    try:
        r = bridge.read(path, timeout=args.timeout)
    except RequestTimeout as exc:
        print(f"TIMEOUT: {exc}", file=sys.stderr)
        return 4
    print(f"{path}")
    print(f"  {r.format()}")
    print(f"  hash 0x{path_hash(path):08x}")
    return 0 if r.usable() else 1


def cmd_write(bridge: Bridge, path: str, args) -> int:
    value = _TYPE_BUILDERS[args.type](args.value)
    try:
        rep = bridge.write(path, value, timeout=args.timeout)
    except RequestTimeout as exc:
        print(f"TIMEOUT: {exc}", file=sys.stderr)
        return 4
    if not rep.ok:
        # The node's own word for the refusal, not a generic failure.
        print(f"REFUSED: {NsError.name_of(rep.status)}", file=sys.stderr)
        return 5
    print(f"{path}  <- {value.format()}  ({ValueType.name_of(value.type)})")
    print(f"  now: {rep.reading().format()}")
    return 0


def cmd_watch(bridge: Bridge, path: str, args) -> int:
    n = 0
    try:
        while args.count == 0 or n < args.count:
            try:
                r = bridge.read(path, timeout=args.timeout)
                print(f"{time.strftime('%H:%M:%S')}  {r.format()}", flush=True)
            except RequestTimeout:
                print(f"{time.strftime('%H:%M:%S')}  TIMEOUT", flush=True)
            n += 1
            if args.count == 0 or n < args.count:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\n# interrupted", file=sys.stderr)
    return 0


def cmd_soak(bridge: Bridge, node_id: int, args) -> int:
    """Sweep the whole namespace repeatedly, in one connection.

    `watch` holds one path, so a capture taken through it replays a namespace of exactly one entry,
    and "byte-identical" over one entry is a weak thing to claim. This sweeps every built-in
    instead, so a long session exercises the whole namespace without needing a second connection --
    which matters, because QEMU's socket serial accepts exactly one per VM lifetime.
    """
    paths = sys_paths_for(node_id)
    width = max(len(describe(p)) for p in paths)
    deadline = time.monotonic() + args.seconds
    sweeps = reads = timeouts = unusable = 0
    try:
        while time.monotonic() < deadline:
            for p in paths:
                try:
                    r = bridge.read(p, timeout=args.timeout)
                except RequestTimeout:
                    timeouts += 1
                    continue
                reads += 1
                if not r.usable():
                    unusable += 1
            sweeps += 1
            print(f"{time.strftime('%H:%M:%S')}  sweep {sweeps}: {reads} read, "
                  f"{timeouts} timed out, {unusable} unusable", flush=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print("# interrupted", file=sys.stderr)

    # End by showing the tuples rather than only counting them, and let the exit code speak for the
    # final sweep -- the state the capture's digest is taken over.
    failures = 0
    print("")
    print(f"after {sweeps} sweeps, {reads} reads, {timeouts} timeouts:")
    for p in paths:
        label = describe(p)
        try:
            r = bridge.read(p, timeout=args.timeout)
        except RequestTimeout:
            print(f"{label:<{width}}  TIMEOUT")
            failures += 1
            continue
        print(f"{label:<{width}}  {r.format()}")
        if not r.usable():
            failures += 1
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
