"""potluck-capture -- read a Potluck M0 node's serial stream, record it, and report.

    python -m potluck --port COM7                     # live table
    python -m potluck --port COM7 --capture soak.jsonl
    python -m potluck --file node1.log --once         # re-read a saved log
    python -m potluck --replay soak.jsonl            # re-read a capture
    python -m potluck --list-ports

Ctrl-C prints the summary and exits 0: interrupting a soak is how a soak ends,
not a failure, and the summary is the deliverable.
"""

from __future__ import annotations

import argparse
import signal
import sys
import time
from pathlib import Path

from . import frame as fr
from .capture import CaptureWriter, read_capture
from .live import Monitor
from .records import KNOWN_KINDS, FrameRecord, LogLine, Record, RecordStream
from .source import FileSource, SerialSource, StdinSource, list_serial_ports


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="potluck-capture",
        description="Read, record and summarise a Potluck M0 node's serial stream.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    src = p.add_mutually_exclusive_group()
    src.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyUSB0")
    src.add_argument("--file", help="read a saved log file instead of a port")
    src.add_argument("--replay", help="re-read a capture file written by --capture")
    src.add_argument("--stdin", action="store_true", help="read lines from stdin")
    src.add_argument("--list-ports", action="store_true", help="list serial ports and exit")

    p.add_argument("--baud", type=int, default=921600,
                   help="serial baud rate (default 921600, matching sdkconfig.defaults)")
    p.add_argument("--capture", help="write a section 7.6 capture file here")
    p.add_argument("--compress", action="store_true", help="gzip the capture file")
    p.add_argument("--rotate-mb", type=int, default=64, help="rotate the capture at this size")
    p.add_argument("--interval", type=float, default=10.0,
                   help="seconds between live table redraws (default 10)")
    p.add_argument("--once", action="store_true",
                   help="stop at end of input rather than following it")
    p.add_argument("--quiet", action="store_true", help="suppress the live table")
    p.add_argument("--show-frames", action="store_true",
                   help="print each teed frame as it arrives (needs CONFIG_POT_FRAME_TEE)")
    p.add_argument("--show-logs", action="store_true", help="print the node's own log lines")
    p.add_argument("--duration", type=float, default=0.0,
                   help="stop after this many seconds (0 = until interrupted)")
    p.add_argument("--expect-digest", metavar="SHA256",
                   help="fail (exit 6) unless the namespace-state digest matches this; "
                        "this is section 13-M2's acceptance test, made checkable")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.list_ports:
        ports = list_serial_ports()
        if not ports:
            print("no serial ports found (or pyserial is not installed: pip install pyserial)")
            return 1
        for dev, desc in ports:
            print(f"{dev}\t{desc}")
        return 0

    if args.replay:
        return replay(args)

    if args.port:
        source = SerialSource(args.port, args.baud, reconnect=not args.once)
    elif args.file:
        source = FileSource(args.file, follow=not args.once)
    elif args.stdin:
        source = StdinSource()
    else:
        build_parser().print_help()
        return 2

    monitor = Monitor()
    stream = RecordStream()
    writer = None
    if args.capture:
        writer = CaptureWriter(
            args.capture,
            rotate_bytes=args.rotate_mb * 1024 * 1024,
            compress=args.compress,
        )

    # Ctrl-C has to reach the summary, so it flips a flag rather than raising out
    # of the middle of a write. A soak interrupted at hour 23 must still report.
    stopping = {"now": False}

    def on_sigint(_sig, _frm):
        stopping["now"] = True

    signal.signal(signal.SIGINT, on_sigint)

    print(f"# potluck-capture reading {source.description}")
    if writer is not None:
        print(f"# capture -> {writer.path}")
    print(f"# summary on Ctrl-C{'; --duration ' + str(args.duration) + ' s' if args.duration else ''}")
    print()

    started = time.time()
    next_draw = started + args.interval
    try:
        for line in source.lines():
            host_ts = time.time()
            item = stream.feed(line)

            if isinstance(item, Record):
                out = monitor.on_record(item.kind, item.data, host_ts=host_ts)
                if out:
                    print(out, flush=True)
                if writer is not None:
                    writer.write_record(item.kind, item.data, host_ts=host_ts)

            elif isinstance(item, FrameRecord):
                monitor.frames_seen += 1
                # Folded in live by exactly the same route replay uses, so the two digests are
                # comparable by construction rather than by coincidence.
                if item.decoded is not None and item.decoded.opcode == fr.Op.REPLY:
                    monitor.on_reply_frame(item.decoded.src, item.decoded.payload)
                if writer is not None:
                    writer.write_frame(
                        node_id=None,
                        direction=item.direction,
                        raw=item.raw,
                        peer_mac=item.peer_mac,
                        rssi=item.rssi,
                        node_ts_us=item.node_ts_us,
                        host_ts=host_ts,
                    )
                if args.show_frames:
                    desc = str(item.decoded) if item.decoded else f"UNPARSEABLE ({item.error})"
                    print(f"  {item.direction} {item.peer_mac} rssi={item.rssi:4d}  {desc}",
                          flush=True)

            elif isinstance(item, LogLine):
                if writer is not None:
                    writer.write_log(item.text, host_ts=host_ts)
                if args.show_logs:
                    print(f"  | {item.text}", flush=True)

            now = time.time()
            if not args.quiet and now >= next_draw:
                elapsed = now - started
                print(f"\n--- {elapsed / 3600:.2f} h elapsed, {stream.stats.lines} lines, "
                      f"{monitor.frames_seen} frames ---")
                print(monitor.table(), flush=True)
                next_draw = now + args.interval

            if stopping["now"]:
                print("\n# interrupted", flush=True)
                break
            if args.duration and (now - started) >= args.duration:
                print(f"\n# reached --duration {args.duration} s", flush=True)
                break
    finally:
        source.close()
        if writer is not None:
            writer.close()

    monitor.print_summary()
    s = stream.stats
    print(f"parse: {s.lines} lines, {s.records} records, {s.frames} frames, "
          f"{s.logs} log lines, {s.bad_json} unparseable lines, {s.bad_frames} bad frames")
    if s.unknown_kinds:
        # A record type this tool does not know is a firmware/host version skew,
        # and it should be visible rather than silently ignored.
        print(f"warning: unrecognised record types (firmware newer than this tool?): "
              f"{dict(s.unknown_kinds)}")
    if writer is not None and writer.path:
        print(f"capture: {writer.written_lines} lines in {writer.path.parent}")
        print(f"replay it with:  python -m potluck --replay {writer.path} "
              f"--expect-digest {monitor.namespace_digest()}")
    return report_digest(monitor, args)


def replay(args) -> int:
    """Re-read a capture file and produce the same summary.

    This is what makes a soak re-analysable after the fact -- section 7.6's whole
    argument. It also means the summary code path is exercised without hardware.
    """
    monitor = Monitor()
    path = Path(args.replay)
    print(f"# replaying {path}")

    frames = 0
    first_ts = None
    last_ts = None
    for obj in read_capture(path):
        kind = obj.get("rec")
        host_ts = obj.get("host_ts")
        if isinstance(host_ts, (int, float)):
            first_ts = host_ts if first_ts is None else first_ts
            last_ts = host_ts
        if kind == "frame":
            frames += 1
            # Frames used to be counted and thrown away, which meant a capture taken by potctl -- all
            # frames, no console -- replayed to an empty namespace. §13-M2 wants namespace state out
            # of a replay, so REPLY frames are decoded and folded in.
            replay_frame(monitor, obj)
            continue
        if kind in KNOWN_KINDS:
            data = {k: v for k, v in obj.items() if k not in ("rec", "host_ts")}
            out = monitor.on_record(kind, data, host_ts=host_ts)
            if out and not args.quiet:
                print(out)

    monitor.frames_seen = frames
    if first_ts is not None and last_ts is not None:
        monitor.started = first_ts
        # The summary reports elapsed time from `started` against time.time(), which
        # is wrong for a replay, so anchor it to the capture's own span.
        monitor.started = time.time() - (last_ts - first_ts)
    monitor.print_summary()
    return report_digest(monitor, args)


def replay_frame(monitor: Monitor, obj: dict) -> None:
    """Decode one captured frame and fold what it says about the namespace into `monitor`."""
    raw_hex = obj.get("raw", "")
    if not isinstance(raw_hex, str) or not raw_hex:
        return
    try:
        raw = bytes.fromhex(raw_hex)
        f = fr.parse(raw)
    except (ValueError, fr.FrameError):
        return
    if f.opcode == fr.Op.REPLY:
        monitor.on_reply_frame(f.src, f.payload)


def report_digest(monitor: Monitor, args) -> int:
    """Print the namespace-state digest, and compare it if one was expected.

    §13-M2's acceptance is "a captured 10-minute session replays and produces byte-identical namespace
    state". A digest makes that checkable by a machine rather than by eye, and `--expect-digest` makes
    it a gate that can fail.
    """
    digest = monitor.namespace_digest()
    entries = len(monitor.ns)
    print(f"\nnamespace state: {entries} entr{'y' if entries == 1 else 'ies'}, "
          f"sha256 {digest}")

    expected = getattr(args, "expect_digest", None)
    if not expected:
        return 0
    if expected.lower() == digest:
        print("digest matches: the replay reproduced byte-identical namespace state")
        return 0
    print(f"DIGEST MISMATCH\n  expected {expected.lower()}\n  got      {digest}", file=sys.stderr)
    if entries == 0:
        print("  the replayed namespace is empty; the capture carried no `ns` records and no REPLY "
              "frames", file=sys.stderr)
    return 6


if __name__ == "__main__":
    sys.exit(main())
