#!/usr/bin/env python3
"""Add, remove or list the programs the Potluck tray keeps running (tray.json's "programs", potluck.tray.ProgramKeeper).

    python tools\\tray_program.py list
    python tools\\tray_program.py add <name> [--cwd <folder>] [--stop-code <n> ...] -- <program> [args ...]
    python tools\\tray_program.py remove <name>

Run by the owner: an entry makes the tray start the program at every login and keep it running, stopped when the tray
quits. It edits %LOCALAPPDATA%\\potluck\\tray.json in place (keeping everything else there, a copy beside it as
tray.json.bak), and the tray picks the change up within a few seconds -- no restart. `--file` names another tray.json
(for tests). A program that exits with one of its --stop-code values is not restarted until its entry changes.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path


def default_file() -> Path:
    return Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "potluck" / "tray.json"


def load(p: Path) -> dict:
    if not p.exists():
        return {}
    return json.loads(p.read_text(encoding="utf-8-sig"))  # Windows PowerShell writes a BOM


def save(p: Path, cfg: dict) -> None:
    if p.exists():
        shutil.copy(p, p.with_name(p.name + ".bak"))
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    cmd_tail: list[str] = []
    if "--" in argv:
        i = argv.index("--")
        argv, cmd_tail = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser(prog="tray_program")
    ap.add_argument("--file", type=Path, default=None)
    sub = ap.add_subparsers(dest="what", required=True)
    sub.add_parser("list")
    a = sub.add_parser("add")
    a.add_argument("name")
    a.add_argument("--cwd")
    a.add_argument("--stop-code", type=int, action="append", default=[])
    r = sub.add_parser("remove")
    r.add_argument("name")
    args = ap.parse_args(argv)
    p = args.file or default_file()
    cfg = load(p)
    programs = [x for x in cfg.get("programs") or [] if isinstance(x, dict)]
    if args.what == "list":
        for x in programs:
            print(f"{x.get('name')}: {' '.join(x.get('cmd') or [])}  (in {x.get('cwd')}, stop codes {x.get('stop_codes', [])})")
        if not programs:
            print("no programs in", p)
        return 0
    if args.what == "add":
        if not cmd_tail:
            print("give the program after --, e.g. add phone -- C:\\Python314\\pythonw.exe svc.py")
            return 2
        if args.cwd and not Path(args.cwd).is_dir():
            print(f"no folder {args.cwd}")
            return 2
        entry = {"name": args.name, "cmd": cmd_tail, "stop_codes": args.stop_code}
        if args.cwd:
            entry["cwd"] = args.cwd
        programs = [x for x in programs if x.get("name") != args.name] + [entry]
        cfg["programs"] = programs
        save(p, cfg)
        print(f"{args.name} added to {p}: the tray starts it within a few seconds and at every login")
        return 0
    kept = [x for x in programs if x.get("name") != args.name]
    if len(kept) == len(programs):
        print(f"no program named {args.name} in {p}")
        return 1
    cfg["programs"] = kept
    save(p, cfg)
    print(f"{args.name} removed from {p}: the tray stops it within a few seconds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
