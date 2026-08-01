"""Differential: the Python framing must produce the C++ framing's exact bytes.

`pot_tests --emit-serial-corpus host/potluck/tests/fixtures` writes each input alongside the
wire bytes the firmware's encoder produced for it. This re-encodes the same inputs and requires
byte equality, then decodes the firmware's bytes back.

Reference vectors prove the two agree on cases somebody thought of. This proves they agree on 430
they did not -- which is where a COBS group boundary or a CRC seed actually differs.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck.serial_framing import crc16, read_serial_frame, write_serial_frame  # noqa: E402

CORPUS = Path(__file__).parent / "fixtures" / "serial_corpus.jsonl"


def _rows() -> list[dict]:
    if not CORPUS.exists():
        raise AssertionError(
            f"{CORPUS} is missing. Generate it with:\n"
            "  build/tests/pot_tests.exe --emit-serial-corpus host/potluck/tests/fixtures"
        )
    return [json.loads(l) for l in CORPUS.read_text(encoding="utf-8").splitlines() if l.strip()]


def test_the_corpus_covers_the_structural_cases():
    rows = _rows()
    assert len(rows) > 400, f"only {len(rows)} frames"
    lengths = {len(bytes.fromhex(r["in"])) for r in rows}
    # 254 is where COBS starts a new group; 1446 and 1470 are section 5.3's caps.
    for n in (1, 253, 254, 255, 256, 1446, 1470):
        assert n in lengths, f"corpus is missing the {n}-byte case"


def test_both_encoders_produce_identical_bytes():
    bad = []
    for i, r in enumerate(_rows()):
        payload = bytes.fromhex(r["in"])
        want = bytes.fromhex(r["wire"])
        got = write_serial_frame(payload)
        if got != want:
            bad.append(f"[{i}] len={len(payload)}\n  cpp={want.hex()}\n  py ={got.hex()}")
        if len(bad) >= 5:
            break
    if bad:
        raise AssertionError(
            "the two framings disagree -- a link built on this would look dead, not wrong\n"
            + "\n".join(bad)
        )


def test_both_crcs_agree():
    for r in _rows():
        assert crc16(bytes.fromhex(r["in"])) == r["crc"]


def test_python_decodes_what_the_firmware_encoded():
    for i, r in enumerate(_rows()):
        payload = bytes.fromhex(r["in"])
        wire = bytes.fromhex(r["wire"])
        assert wire[-1] == 0x00, f"[{i}] no delimiter"
        assert 0 not in wire[:-1], f"[{i}] a zero survived encoding"
        assert read_serial_frame(wire[:-1]) == payload, f"[{i}] decode mismatch"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"ok   {name}")
            except Exception as exc:  # noqa: BLE001
                failures += 1
                print(f"FAIL {name}: {exc}")
    print(f"\n{failures} failure(s)")
    sys.exit(1 if failures else 0)
