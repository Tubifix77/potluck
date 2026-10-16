"""The release firmware (M20): the image a packaged Potluck carries, so a PC with no ESP-IDF can set boards up.

    python -m potluck.release <out dir> [--repo <a Potluck checkout>] [--rev HEAD]

Builds what `potluck apply` builds for a cluster with no apps -- Potluck at <rev> with the sandbox
(CONFIG_POT_WASM=y, so guests from the app store run on it) -- and copies the files ESP-IDF's flasher_args.json names
into <out dir>/<version>/, with release.json ({"version", "elf_sha256", "extra"}). setup.py finds it beside the
packaged program (tools/build_package.ps1 puts it in dist/Potluck/firmware).
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

from . import apply as ap
from . import setup as st

#: The release image's options: the sandbox, for the app store's guests. Nothing else differs from the defaults.
RELEASE_EXTRA = ["CONFIG_POT_WASM=y"]


def build(out: str | Path, repo: str | Path, rev: str = "HEAD", log=print) -> Path:
    """Build (or reuse from apply's cache) the release image and copy it to <out>/<version>; returns that folder."""
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "release.json"
        p.write_text(json.dumps({"schema": ap.SCHEMA, "system": "release", "boards": [],
                                 "firmware": {"potluck": {"git": str(Path(repo).resolve()), "rev": rev},
                                              "extra": RELEASE_EXTRA}}), encoding="utf-8")
        fw = ap.build_firmware(ap.load_cluster(str(p)), ap.default_cache(), log)
    if fw is None:
        raise st.SetupError("no firmware was built")
    bdir = fw.image.parent
    dest = Path(out) / fw.version
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    files = []
    for _, f in st.flash_parts(bdir):
        rel = f.relative_to(bdir)
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, dest / rel)
        files.append(rel.as_posix())
    shutil.copy2(bdir / "flasher_args.json", dest / "flasher_args.json")
    (dest / "release.json").write_text(json.dumps({"version": fw.version, "elf_sha256": fw.elf_sha.hex(),
                                                   "extra": RELEASE_EXTRA, "files": files}, indent=2) + "\n",
                                       encoding="utf-8")
    st.flash_parts(dest)  # every file flasher_args.json names is there
    log(f"release: firmware {fw.version} in {dest}")
    return dest


def main(argv: list[str] | None = None) -> int:
    ap_ = argparse.ArgumentParser(prog="potluck.release", description=__doc__.split("\n\n")[0])
    ap_.add_argument("out")
    ap_.add_argument("--repo", default=str(Path(__file__).resolve().parents[3]))
    ap_.add_argument("--rev", default="HEAD")
    a = ap_.parse_args(argv)
    try:
        build(a.out, a.repo, a.rev)
    except (ap.ApplyError, st.SetupError) as exc:
        print(f"potluck.release: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
