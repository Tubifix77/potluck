"""The release firmware (M20): the image a packaged Potluck carries, so a PC with no ESP-IDF can set boards up.

    python -m potluck.release <out dir> --from <cluster file>    # the release: what apply builds for that cell
    python -m potluck.release <out dir> [--repo <checkout>] [--rev HEAD]   # Potluck and the sandbox only

M20 (ARCHITECTURE section 13, review item 8): the release image is core + sandbox + present + wifi + pocket + both
built-in apps (passive-sensor, the extender). The built-ins come from their own repositories, which this public one
does not name, so the release is built from a cluster file that does (the owner's, e.g. keys/desk-m14-hub.json):
exactly the image `potluck apply` builds for it, no package. Without --from, Potluck and the sandbox only
(CONFIG_POT_WASM=y), and it says so. Either way the files ESP-IDF's flasher_args.json names go into
<out dir>/<version>/ with release.json ({"version", "elf_sha256", "extra", "apps", "files"}); setup.py finds them
beside the packaged program (tools/build_package.ps1 puts them in dist/Potluck/firmware).
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


def build(out: str | Path, repo: str | Path, rev: str = "HEAD", log=print, cluster: str | Path | None = None) -> Path:
    """Build (or reuse from apply's cache) the release image and copy it to <out>/<version>; returns that folder."""
    if cluster is not None:
        c = ap.load_cluster(str(cluster))
        if c.firmware is None or "image" in c.firmware:
            raise st.SetupError(f"{cluster}: its \"firmware\" must name sources to build, not an image")
        if "CONFIG_POT_WASM=y" not in c.extra:
            raise st.SetupError(f"{cluster}: a release carries the sandbox -- add CONFIG_POT_WASM=y to firmware.extra")
        fw = ap.build_firmware(c, ap.default_cache(), log)
        extra, apps = sorted(c.extra), sorted(a.name for a in c.apps if a.components) + (["extender"] if c.extender else [])
    else:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "release.json"
            p.write_text(json.dumps({"schema": ap.SCHEMA, "system": "release", "boards": [],
                                     "firmware": {"potluck": {"git": str(Path(repo).resolve()), "rev": rev},
                                                  "extra": RELEASE_EXTRA}}), encoding="utf-8")
            fw = ap.build_firmware(ap.load_cluster(str(p)), ap.default_cache(), log)
        extra, apps = RELEASE_EXTRA, []
        log("release: Potluck and the sandbox only -- no built-in apps (build with --from <a cluster file that names them>)")
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
    from .pins import from_build
    (dest / "release.json").write_text(json.dumps({"version": fw.version, "elf_sha256": fw.elf_sha.hex(),
                                                   "extra": extra, "apps": apps, "files": files,
                                                   "pins": from_build(bdir)}, indent=2) + "\n",
                                       encoding="utf-8")
    st.flash_parts(dest)  # every file flasher_args.json names is there
    log(f"release: firmware {fw.version} in {dest}")
    return dest


def main(argv: list[str] | None = None) -> int:
    ap_ = argparse.ArgumentParser(prog="potluck.release", description=__doc__.split("\n\n")[0])
    ap_.add_argument("out")
    ap_.add_argument("--repo", default=str(Path(__file__).resolve().parents[3]))
    ap_.add_argument("--rev", default="HEAD")
    ap_.add_argument("--from", dest="cluster", help="build the image this cluster file's apply builds (the built-ins)")
    a = ap_.parse_args(argv)
    try:
        build(a.out, a.repo, a.rev, cluster=a.cluster)
    except (ap.ApplyError, st.SetupError) as exc:
        print(f"potluck.release: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
