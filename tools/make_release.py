#!/usr/bin/env python3
"""Make a Potluck release (M20): what HACS updates from, and what the installer is downloaded from.

    python tools/make_release.py 0.2.0             # bump, commit, tag -- local only; prints what --publish would do
    python tools/make_release.py 0.2.0 --publish   # also push the tag and create the GitHub release

HACS takes an integration's version from the newest GitHub *release* ("Just publishing tags is not enough, you need to
publish releases"; with none, the default branch's newest commit -- every commit then looks like an update). So a
release is: custom_components/potluck/manifest.json's "version" set to X.Y.Z, a commit, an annotated tag vX.Y.Z, and a
GitHub release of that tag. The packaged Potluck (tools/build_package.ps1's dist/Potluck, with its release firmware)
is attached as Potluck-X.Y.Z-windows.zip when it has been built from this very commit.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "custom_components" / "potluck" / "manifest.json"
VERSION = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def parse(v: str) -> tuple[int, int, int]:
    m = VERSION.match(v)
    if not m:
        raise SystemExit(f"make_release: '{v}' is not X.Y.Z")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, check=True, capture_output=True, text=True).stdout.strip()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="make_release", description=__doc__.split("\n\n")[0])
    ap.add_argument("version")
    ap.add_argument("--publish", action="store_true", help="push the tag and create the GitHub release")
    a = ap.parse_args(argv)
    new = parse(a.version)
    tag = f"v{a.version}"
    raw = MANIFEST.read_bytes()
    doc = json.loads(raw)
    old = parse(doc["version"])
    if new <= old:
        raise SystemExit(f"make_release: {a.version} is not newer than the integration's {doc['version']}")
    if git("status", "--porcelain", "--untracked-files=no"):
        raise SystemExit("make_release: commit or stash your changes first -- a release is exactly one commit")
    if git("rev-parse", "--abbrev-ref", "HEAD") != "main":
        raise SystemExit("make_release: releases are made from main")
    if git("tag", "--list", tag):
        raise SystemExit(f"make_release: {tag} exists already")

    # The version, written in place: the file keeps its own layout and line endings.
    text = raw.decode("utf-8")
    text, n = re.subn(r'("version":\s*")[^"]*(")', rf"\g<1>{a.version}\g<2>", text, count=1)
    if n != 1:
        raise SystemExit(f"make_release: no \"version\" in {MANIFEST}")
    MANIFEST.write_bytes(text.encode("utf-8"))
    git("add", str(MANIFEST))
    git("commit", "-q", "-m", f"Release {tag}")
    git("tag", "-a", tag, "-m", f"Potluck {tag}")
    head = git("rev-parse", "--short=7", "HEAD")
    print(f"make_release: {tag} at {head} (manifest {doc['version']} -> {a.version})")

    assets = []
    dist = ROOT / "dist" / "Potluck"
    if (dist / "potluck.exe").exists():
        rels = sorted((dist / "firmware").glob("*/release.json")) if (dist / "firmware").is_dir() else []
        built_from = json.loads(rels[-1].read_text(encoding="utf-8"))["version"].split("-")[0] if rels else None
        # The firmware's version names the last commit that changed the firmware, not this one: attach only a
        # package whose firmware is the tree this release has.
        fw_now = git("log", "-1", "--format=%h", "--abbrev=7", "HEAD", "--", "firmware", "tools/build_firmware.ps1")
        if built_from == fw_now:
            z = shutil.make_archive(str(ROOT / "dist" / f"Potluck-{a.version}-windows"), "zip", dist.parent, "Potluck")
            assets.append(z)
        else:
            print(f"make_release: dist/Potluck is not built from this tree (firmware {built_from}, tree {fw_now}) -- "
                  "not attached; run tools/build_package.ps1 and attach it with `gh release upload`")
    cmds = [["git", "push", "origin", "main", tag],
            ["gh", "release", "create", tag, "--repo", "Tubifix77/potluck", "--title", f"Potluck {tag}",
             "--generate-notes", "--verify-tag", *assets]]
    if not a.publish:
        print("make_release: local only. To publish:")
        for c in cmds:
            print("  " + " ".join(c))
        return 0
    for c in cmds:
        subprocess.run(c, cwd=ROOT, check=True)
    print(f"make_release: published {tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
