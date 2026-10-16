#!/usr/bin/env python3
"""The store's keys, its signed list, and an app's potluck-app.json (M20; potluck.store says what each is).

    python tools/store_feed.py keygen <key file>                 # an Ed25519 key: the store key, or an app author's
    python tools/store_feed.py sign <feed.json> --key <key file> # writes feed.sig beside it
    python tools/store_feed.py app <module.wasm> --key <author key file> --name <name> --outputs "out:f32 ..." \\
        [--input name:type[:unit] ...] [--description "..."] [--fuel N] [--period-ms N] --out <potluck-app.json>

A key file holds the secret as hex on one line, and keygen prints the public half. Keep the store key and every
author key out of version control (keys/ is git-ignored): the store key's public half is built into Potluck
(potluck.store.FEED_KEY), and an author's is pinned by every cell that installs its apps -- losing either means a
new key that every cell must accept again.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "host" / "potluck"))

from potluck import ed25519_ref as ed  # noqa: E402
from potluck import guest as gu  # noqa: E402
from potluck import store as so  # noqa: E402


def read_secret(path: str) -> bytes:
    sk = bytes.fromhex(Path(path).read_text(encoding="ascii").strip())
    if len(sk) != 32:
        raise SystemExit(f"store_feed: {path} does not hold a 32-byte secret in hex")
    return sk


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="store_feed", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("out")
    s = sub.add_parser("sign")
    s.add_argument("feed")
    s.add_argument("--key", required=True)
    a = sub.add_parser("app")
    a.add_argument("module")
    a.add_argument("--key", required=True)
    a.add_argument("--name", required=True)
    a.add_argument("--outputs", required=True)
    a.add_argument("--input", action="append", default=[])
    a.add_argument("--description", default="")
    a.add_argument("--fuel", type=int, default=100_000)
    a.add_argument("--period-ms", type=int, default=1000)
    a.add_argument("--out", required=True)
    o = ap.parse_args(argv)

    if o.cmd == "keygen":
        p = Path(o.out)
        if p.exists():
            raise SystemExit(f"store_feed: {p} exists -- a key is never written over")
        sk = secrets.token_bytes(32)
        fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="ascii") as f:
            f.write(sk.hex() + "\n")
        print(ed.public_key(sk).hex())
        return 0
    if o.cmd == "sign":
        raw = Path(o.feed).read_bytes()
        so.parse_feed(json.loads(raw))  # never sign a list the hub would refuse
        sig = ed.sign(read_secret(o.key), so.feed_message(raw))
        Path(o.feed).with_name("feed.sig").write_text(sig.hex() + "\n", encoding="ascii")
        print(f"signed {o.feed} ({len(raw)} B)")
        return 0
    module = Path(o.module).read_bytes()
    sk = read_secret(o.key)
    inputs = []
    for spec in o.input:
        parts = spec.split(":")
        inputs.append({"name": parts[0], "type": parts[1] if len(parts) > 1 else "f32",
                       **({"unit": parts[2]} if len(parts) > 2 else {})})
    doc = {"type": so.APP_TYPE, "name": o.name, "description": o.description, "author_key": ed.public_key(sk).hex(),
           "module": Path(o.module).name, "sha256": hashlib.sha256(module).hexdigest(),
           "sig": ed.sign(sk, gu.signed_message(module)).hex(), "inputs": inputs, "outputs": o.outputs,
           "fuel": o.fuel, "period_ms": o.period_ms}
    app = so.parse_app(doc)
    so.verify_module(app, module)  # what every cell will check
    Path(o.out).write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {o.out}: {o.name}, module {Path(o.module).name} beside it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
