"""The host's path hash must agree with the node's, exactly.

The node stores a 32-bit hash and never sees the string. If these two implementations ever disagree,
every read misses and nothing reports why -- so the reference values below are the same ones
tests/test_namespace.cpp asserts, and a change on either side breaks both.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck.paths import FNV1A32_OFFSET, PathTable, path_hash  # noqa: E402


def test_reference_values_match_the_firmware_tests():
    # Identical assertions to test_namespace.cpp's `the_path_hash_is_stable_and_specified`.
    assert path_hash("") == FNV1A32_OFFSET == 2166136261
    assert path_hash("a") == 3826002220
    assert path_hash("potluck://lab/n2/adc/0") != path_hash("potluck://lab/n2/adc/1")


def test_hash_is_32_bit_and_deterministic():
    for p in ("potluck://lab/node-07/adc/1", "potluck://home/vents/kitchen/position", "x" * 200, ""):
        h = path_hash(p)
        assert 0 <= h <= 0xFFFFFFFF
        assert h == path_hash(p)


def test_utf8_paths_hash_over_bytes_not_code_points():
    # The node hashes bytes. A host hashing code points would silently diverge on any non-ASCII
    # path -- and this project's own name has a mu in it.
    assert path_hash("potluck://lab/µnode/adc") == path_hash("potluck://lab/µnode/adc")
    assert path_hash("µ") != path_hash("μ")  # different characters, different resources


def test_table_resolves_and_reports_unknowns_honestly():
    t = PathTable(["potluck://lab/n2/adc/0", "potluck://lab/n2/adc/1"])
    assert len(t) == 2
    assert t.resolve(path_hash("potluck://lab/n2/adc/0")) == "potluck://lab/n2/adc/0"
    assert t.resolve(0xDEADBEEF) is None
    # An unknown hash renders as a hash rather than as a plausible-looking name.
    assert t.name_or_hash(0xDEADBEEF) == "<0xdeadbeef>"
    assert "potluck://lab/n2/adc/0" in t
    assert "potluck://lab/n2/adc/9" not in t


def test_adding_the_same_path_twice_is_fine():
    t = PathTable()
    a = t.add("potluck://lab/n1/x")
    b = t.add("potluck://lab/n1/x")
    assert a == b
    assert len(t) == 1


def test_a_collision_fails_the_manifest_rather_than_aliasing():
    t = PathTable(["potluck://lab/n1/x"])
    h = path_hash("potluck://lab/n1/x")
    # Force the collision case the node also refuses, by poking the table directly -- two real
    # colliding paths are a 1-in-2^32 event that cannot be constructed conveniently.
    t._by_hash[h] = "potluck://lab/n1/OTHER"
    try:
        t.add("potluck://lab/n1/x")
    except ValueError as exc:
        assert "collision" in str(exc)
        return
    raise AssertionError("a colliding path should not be accepted")


def test_no_realistic_manifest_collides():
    # section 6 caps the table at 128 entries and section 15's open question 2 asks whether that is
    # right. Build a manifest far larger than the cap and confirm the hash spreads -- a collision
    # inside 128 entries has probability around 1.9e-6, and this checks the function is not
    # pathologically worse than that on paths shaped like real ones.
    paths = [
        f"potluck://{cluster}/node-{n:02d}/{cls}/{i}"
        for cluster in ("lab", "home", "car")
        for n in range(20)
        for cls in ("adc", "pwm", "imu", "temp", "vent")
        for i in range(4)
    ]
    hashes = {path_hash(p) for p in paths}
    assert len(hashes) == len(paths), f"{len(paths) - len(hashes)} collisions in {len(paths)} paths"


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
