"""The host's built-in resource paths must hash to what the firmware computes.

A node ships only hashes. If these spellings drift from sys_resources.cpp, every resource in a
capture renders as `<0x...>` and nobody finds out why -- so the expected hashes below are computed
the same way the firmware's own test does, from the same strings.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from potluck.paths import path_hash  # noqa: E402
from potluck.sys_paths import (  # noqa: E402
    SYS_LABELS,
    SYS_SUFFIXES,
    describe,
    sys_path,
    sys_paths_for,
    table_for_nodes,
)


def test_the_canonical_path_matches_the_firmwares_format():
    # tests/test_sys_resources.cpp asserts this exact string for node 0x1a2b.
    assert sys_path(0x1A2B, "sys/heap-free") == "potluck://lab/node-1a2b/sys/heap-free"


def test_every_suffix_is_distinct_and_labelled():
    assert len(set(SYS_SUFFIXES)) == len(SYS_SUFFIXES)
    for s in SYS_SUFFIXES:
        assert s in SYS_LABELS, f"{s} has no human label"


def test_a_nodes_builtins_are_six_distinct_hashes():
    paths = sys_paths_for(0x0101)
    assert len(paths) == 6
    assert len({path_hash(p) for p in paths}) == 6


def test_two_nodes_do_not_share_a_resource():
    a = set(sys_paths_for(0x0101))
    b = set(sys_paths_for(0x0202))
    assert not (a & b)
    assert path_hash(sys_path(0x0101, "sys/uptime")) != path_hash(sys_path(0x0202, "sys/uptime"))


def test_a_table_built_from_observed_nodes_resolves_their_resources():
    # The host learns node ids from records it is already reading, so a capture becomes
    # self-describing without anyone supplying a manifest.
    t = table_for_nodes([0x0101, 0x0202])
    assert len(t) == 12
    h = path_hash(sys_path(0x0202, "sys/rssi"))
    assert t.resolve(h) == "potluck://lab/node-0202/sys/rssi"
    # An unknown hash still renders honestly rather than guessing.
    assert t.name_or_hash(0x12345678) == "<0x12345678>"


def test_describe_gives_a_label_for_known_paths_and_the_path_otherwise():
    assert describe("potluck://lab/node-0101/sys/uptime") == "uptime"
    assert describe("potluck://lab/node-0101/adc/0") == "potluck://lab/node-0101/adc/0"


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
