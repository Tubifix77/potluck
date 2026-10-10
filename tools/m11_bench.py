"""M11's refusal checks, over the cable and through the board on it to a target.

    python tools/m11_bench.py --port COM6 --target 8160 refuse --key keys/deploy.key --bcert keys/deploy.bcert

Each check sends something a node must refuse, then reads the target's status to show nothing
changed. Small images -- the first 4 KB of a real image, whose header ESP-IDF accepts -- so a check
whose refusal comes at COMMIT, after the hash, costs seconds rather than a whole transfer:

  downgrade        BEGIN at the target's floor                         -> downgrade, at BEGIN
  foreign_ca       a deploy certificate from another CA               -> bad_signature, at COMMIT
  wrong_counter    signed for counter N, BEGIN claims N+1             -> bad_signature, at COMMIT
  flipped_byte     a byte changed after signing                       -> bad_signature, at COMMIT
  not_an_image     a truncated image, correctly signed                -> bad_image (esp_ota_end refuses it)
  unsigned         a trailer of zeros                                  -> bad_signature, at COMMIT
  noise            4 KB of noise, correctly signed                    -> store_failed, at the first CHUNK
                   (esp_ota_write checks the image header there)
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "host", "potluck"))

from potluck import ed25519_ref as ed  # noqa: E402
from potluck import enrol as en  # noqa: E402
from potluck import fw  # noqa: E402
from potluck.bridge import Bridge  # noqa: E402
from potluck.signing import KeyPair, read_key  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--target", default="8160")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("refuse")
    r.add_argument("--key", required=True)
    r.add_argument("--bcert", required=True)
    r.add_argument("--image", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware",
                                                    "build", "potluck_m0.bin"))
    a = ap.parse_args()
    t = int(a.target, 16)

    key = read_key(a.key)
    bcert = bytes.fromhex(open(a.bcert, encoding="ascii").read().strip())
    head = open(a.image, "rb").read()[:4096]
    noise = os.urandom(4096)

    with Bridge.open(port=a.port) as b:
        b.hello(timeout=3.0)
        before = fw.status(b, t)
        print(f"before: {before}")
        n = before["floor"] + 1
        fails = 0

        def check(name: str, img: bytes, counter: int, trailer: bytes, want: str) -> None:
            nonlocal fails
            got = fw.send_image(b, t, img, counter, trailer)
            ok = got["status"] == want
            fails += 0 if ok else 1
            print(f"  {'ok  ' if ok else 'FAIL'} {name:14s} -> {got['status']} (want {want})")

        check("downgrade", head, before["floor"], fw.fw_trailer(head, before["floor"], key.secret, bcert),
              "downgrade")
        other_sk = os.urandom(32)
        other_ca = KeyPair("ed25519", "ca", "other-ca", ed.public_key(other_sk), other_sk)
        dsk = os.urandom(32)
        foreign = en.build_cert(other_ca, 0, ed.public_key(dsk), issued=1790000000, role=en.ROLE_DEPLOY)
        check("foreign_ca", head, n, fw.fw_trailer(head, n, dsk, foreign), "bad_signature")
        check("wrong_counter", head, n + 1, fw.fw_trailer(head, n, key.secret, bcert), "bad_signature")
        flipped = bytearray(head)
        flipped[1000] ^= 1
        check("flipped_byte", bytes(flipped), n, fw.fw_trailer(head, n, key.secret, bcert), "bad_signature")
        check("not_an_image", head, n, fw.fw_trailer(head, n, key.secret, bcert), "bad_image")
        check("unsigned", head, n, bytes(fw.TRAILER_LEN), "bad_signature")
        check("noise", noise, n, fw.fw_trailer(noise, n, key.secret, bcert), "store_failed")

        after = fw.status(b, t)
        print(f"after:  {after}")
        same = all(after[k] == before[k] for k in ("running", "floor", "state", "version"))
        print(f"running, floor, state and version unchanged: {same}")
        return 0 if fails == 0 and same else 1


if __name__ == "__main__":
    sys.exit(main())
