"""M11: firmware over the cell -- sign a firmware image, and roll it out one node at a time.

    python -m potluck.fw --port COM6 status [--targets 6300,7368,8160]
    python -m potluck.fw --port COM6 rollout <image.bin> --counter N --key <deploy.key> --bcert <deploy.bcert>
                         [--targets 7368,8160,6300] [--image-for 7368=<other.bin>] [--confirm-timeout 180]

Every request goes to the board the host is cabled to and names its target; that board answers for
itself and passes a request for any other node on to it (fw_rt in firmware/main/m0_main.cpp), so the image is
never stored on the way. A rollout updates one target at a time: it sends the image, commits it (the
target checks the signature and switches its boot slot, then reboots), and waits for the target to
come back running the new counter, confirmed -- its trial passed. A target that comes back on the old
image has rolled back, and the rollout stops there: a bad image never reaches the next node.

The signature (firmware/components/pot_trust, fw_trailer_check) is the deploy key's, over
FW_DOMAIN + counter (u32 LE) + length (u32 LE) + SHA-512(image), behind the deploy key's certificate:
the same trust as a package, under its own domain.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import time

from . import frame as fr

FW_DOMAIN = b"potluck-firmware-v1\0"
TRAILER_LEN = 176
CHUNK_DEFAULT = 1024  # pot/fw.hpp's kFwChunkMax: the cable and ESP-NOW v2 both carry it (M11)
CHUNK_MAX = 1024
STATUS = {0: "ok", 1: "too_large", 2: "downgrade", 3: "not_started", 4: "bad_offset", 5: "malformed",
          6: "busy", 7: "bad_signature", 8: "store_failed", 9: "bad_image", 10: "unreachable", 11: "incomplete"}
STATE = {0: "confirmed", 1: "on_trial", 2: "factory"}


class FwError(Exception):
    pass


def signed_message(counter: int, image_len: int, digest: bytes) -> bytes:
    return FW_DOMAIN + struct.pack("<II", counter, image_len) + digest


def fw_trailer(img: bytes, counter: int, deploy_secret: bytes, deploy_cert: bytes) -> bytes:
    from . import ed25519_ref as ed

    if len(deploy_cert) != 112:
        raise FwError("a deploy certificate is 112 bytes (python -m potluck.enrol --deploy-cert)")
    return deploy_cert + ed.sign(deploy_secret, signed_message(counter, len(img), hashlib.sha512(img).digest()))


def begin_payload(target: int, image_len: int, counter: int, trailer: bytes) -> bytes:
    if len(trailer) != TRAILER_LEN:
        raise FwError("a trailer is 176 bytes")
    return struct.pack("<HII", target, image_len, counter) + trailer


def chunk_payload(target: int, offset: int, data: bytes) -> bytes:
    if len(data) > CHUNK_MAX:
        raise FwError(f"a chunk is at most {CHUNK_MAX} bytes")
    return struct.pack("<HIH", target, offset, len(data)) + data


def target_payload(target: int) -> bytes:
    return struct.pack("<H", target)


def parse_reply(raw: bytes) -> dict:
    if len(raw) != 49:
        raise FwError(f"an FwReply is 49 bytes, got {len(raw)}")
    status, node, received, running, floor, state = struct.unpack_from("<HHIIIB", raw, 0)
    version = raw[17:49].split(b"\0", 1)[0].decode("ascii", "replace")
    return {"status": STATUS.get(status, str(status)), "node": node, "received": received, "running": running,
            "floor": floor, "state": STATE.get(state, str(state)), "version": version}


def _step(b, op: int, payload: bytes, timeout: float = 3.0, tries: int = 3) -> dict:
    from .bridge import RequestTimeout

    for k in range(tries):
        try:
            return parse_reply(b.deploy_step(op, payload, timeout=timeout))
        except RequestTimeout:
            if k == tries - 1:
                raise
    raise FwError("unreachable")


def status(b, target: int) -> dict:
    return _step(b, fr.Op.FW_STATUS, target_payload(target))


def send_image(b, target: int, img: bytes, counter: int, trailer: bytes, chunk: int = CHUNK_DEFAULT,
               progress=None) -> dict:
    r = _step(b, fr.Op.FW_BEGIN, begin_payload(target, len(img), counter, trailer), timeout=5.0)
    if r["status"] != "ok":
        return r
    t0 = time.monotonic()
    for off in range(0, len(img), chunk):
        r = _step(b, fr.Op.FW_CHUNK, chunk_payload(target, off, img[off:off + chunk]))
        if r["status"] != "ok":
            return r
        if progress is not None and (off // chunk) % 200 == 0:
            progress(off, len(img), time.monotonic() - t0)
    # The commit hashes nothing more but checks the signature and the image: allow it the time.
    return _step(b, fr.Op.FW_COMMIT, target_payload(target), timeout=15.0, tries=1)


def wait_confirmed(b, target: int, counter: int, timeout: float) -> dict | None:
    """Poll the target until it runs `counter` confirmed, or comes back on another image (rolled back)."""
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        time.sleep(2.0)
        try:
            last = status(b, target)
        except Exception:
            # Rebooting, or the cell re-forming around it. If the target is the board on the cable,
            # it has forgotten this host with its reboot: announce again.
            try:
                b.hello(timeout=1.0)
            except Exception:  # noqa: BLE001 - the next poll tries again
                pass
            continue
        if last["status"] != "ok" or last["node"] != target:
            continue  # the board passing it on answered for it: the target is out of reach, not rolled back
        if last["running"] == counter and last["state"] == "confirmed":
            return last
        if last["running"] != counter and last["state"] != "on_trial" and time.monotonic() > end - timeout + 20:
            return last  # back, and not on the new image: rolled back
    return last


def _targets(s: str) -> list[int]:
    return [int(x, 16) for x in s.split(",") if x]


def main(argv: list[str] | None = None) -> int:
    from .bridge import Bridge

    ap = argparse.ArgumentParser(prog="potluck.fw")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=921600)
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("status")
    st.add_argument("--targets", default="6300,7368,8160")
    ro = sub.add_parser("rollout")
    ro.add_argument("image")
    ro.add_argument("--counter", type=int, required=True)
    ro.add_argument("--key", required=True)
    ro.add_argument("--bcert", required=True)
    ro.add_argument("--targets", default="7368,8160,6300")
    ro.add_argument("--image-for", action="append", default=[], metavar="NODE=IMAGE",
                    help="a different image for one node (a board built as another variant)")
    ro.add_argument("--chunk", type=int, default=CHUNK_DEFAULT)
    ro.add_argument("--confirm-timeout", type=float, default=180.0)
    a = ap.parse_args(argv)

    with Bridge.open(port=a.port, baud=a.baud) as b:
        b.hello(timeout=3.0)
        if a.cmd == "status":
            for t in _targets(a.targets):
                try:
                    print(f"0x{t:04x}: {status(b, t)}")
                except Exception as exc:  # noqa: BLE001 - report and go on to the next node
                    print(f"0x{t:04x}: {exc}")
            return 0
        from .signing import read_key

        key = read_key(a.key)
        bcert = bytes.fromhex(open(a.bcert, encoding="ascii").read().strip())
        paths = {t: a.image for t in _targets(a.targets)}
        for spec in a.image_for:
            node, _, path = spec.partition("=")
            paths[int(node, 16)] = path
        for t in _targets(a.targets):
            img = open(paths[t], "rb").read()
            trailer = fw_trailer(img, a.counter, key.secret, bcert)
            print(f"0x{t:04x}: image {paths[t]}, {len(img)} B, counter {a.counter}, "
                  f"sha512 {hashlib.sha512(img).hexdigest()[:16]}...")
            before = status(b, t)
            print(f"0x{t:04x}: before {before}")
            t0 = time.monotonic()
            r = send_image(b, t, img, a.counter, trailer, a.chunk,
                           progress=lambda off, n, s: print(f"  {off * 100 // n:3d} %  {s:6.1f} s", flush=True))
            sent = time.monotonic() - t0
            if r["status"] != "ok":
                print(f"0x{t:04x}: transfer stopped after {sent:.1f} s: {r['status']} with {r['received']} B taken "
                      f"-- the target keeps the image it runs; rollout stopped")
                return 1
            print(f"0x{t:04x}: committed after {sent:.1f} s ({len(img) / sent / 1024:.1f} KiB/s)")
            done = wait_confirmed(b, t, a.counter, a.confirm_timeout)
            took = time.monotonic() - t0
            if done is None or done["running"] != a.counter or done["state"] != "confirmed":
                print(f"0x{t:04x}: NOT confirmed after {took:.1f} s: {done} -- rollout stopped")
                return 1
            print(f"0x{t:04x}: confirmed {done['version']} after {took:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
