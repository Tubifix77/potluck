#!/usr/bin/env python3
"""Check the Potluck core's static DRAM against ARCHITECTURE.md section 6's 64 KB cap.

Section 6 budgets the *Potluck core* at 59.4 KB with a hard cap of 64 KB, and M0-BRIEF.md says
"Core above the 64 KB cap = build treated as failing". The figure that must be compared is the
core's, not the whole image's: `idf.py size` reports DRAM for everything linked in, most of which is
ESP-IDF's Wi-Fi stack, FreeRTOS, lwIP and libc. Section 6 accounts for the Wi-Fi stack separately as
a [MEASURE] item and never claimed the 64 KB covered it -- so comparing the image total against
64 KB would fail a build that is comfortably inside its budget, which is worse than not checking.

The gate is therefore: sum the DRAM (.data + .bss) of the Potluck archives only, and compare that to
65536.

Reads `idf.py size-components --format json2` when present, and falls back to parsing the
box-drawing text table. Note that the format is json2, not json: esp-idf-size renamed it, and
`--format json` is silently rejected by the version ESP-IDF v6.0.2 ships.

Exit codes:
    0  within budget
    1  over budget: the build is to be treated as failing
    2  could not determine the figure, which is not a pass
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

CAP_BYTES = 64 * 1024            # section 6: "Budget cap (hard) | 64 KB | build fails above this"
DESIGN_BYTES = int(59.4 * 1024)  # section 6: "Potluck core total | 59.4 KB"

# The archives that are Potluck. `libmain.a` is the M0 application rather than the core proper, but it
# is counted: at M0 it holds the peer table, the histograms, the counters, the event ring and both
# task stacks, which is most of what section 6's table is actually about.
CORE_ARCHIVES = ("libpot_frame.a", "libpot_link.a", "libpot_espnow.a", "libmain.a")

# The memory type that holds static data, by target family.
#
#   classic ESP32 -> "DRAM"   DRAM and IRAM are separate physical banks.
#   ESP32-S3      -> "DIRAM"  one unified internal SRAM that serves as both, so esp-idf-size names
#                             it DIRAM and there is no "DRAM" row at all.
#
# Accepting either is not cosmetic: with only "DRAM" in the list, an S3 report parses to zero
# archives and the check silently reports COULD NOT DETERMINE. Both names are listed so a new
# target that uses a third one fails loudly rather than passing on an empty sum.
STATIC_DATA_MEMORY_TYPES = ("DRAM", "DIRAM")


def from_json2(path: Path) -> tuple[dict[str, int], str] | None:
    """Read per-archive static data memory from `--format json2`.

    Schema: a top-level object keyed by archive path, each value carrying `abbrev_name` and
    `memory_types`, in which the relevant entry's `size` is already .data + .bss.

    Returns (archives, memory_type_name) so the caller can report which row it used.
    """
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, UnicodeDecodeError):
        return None
    if not isinstance(doc, dict):
        return None

    for mem_type in STATIC_DATA_MEMORY_TYPES:
        found: dict[str, int] = {}
        for key, entry in doc.items():
            if not isinstance(entry, dict):
                continue
            name = entry.get("abbrev_name")
            if not isinstance(name, str):
                name = key.replace("\\", "/").rsplit("/", 1)[-1]
            mem = entry.get("memory_types")
            if not isinstance(mem, dict):
                continue
            row = mem.get(mem_type)
            if isinstance(row, dict) and isinstance(row.get("size"), (int, float)):
                found[name] = found.get(name, 0) + int(row["size"])
        if found:
            return found, mem_type
    return None


def from_text(path: Path) -> tuple[dict[str, int], str] | None:
    """Read per-archive static data memory from the box-drawing text table.

    The table is delimited by U+2502 BOX DRAWINGS LIGHT VERTICAL, and its columns have changed
    between esp-idf-size releases and differ by target. So the header row is located by name and
    the column found by its label rather than by position: reading the wrong column would produce
    a confident wrong answer, which is the one outcome worse than no answer.
    """
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None

    def cells(line: str) -> list[str]:
        if "│" not in line:
            return []
        return [c.strip() for c in line.strip().strip("│").split("│")]

    wanted = {t.lower() for t in STATIC_DATA_MEMORY_TYPES}
    data_col: int | None = None
    name_col: int | None = None
    mem_type = "?"
    found: dict[str, int] = {}

    for line in lines:
        c = cells(line)
        if not c:
            continue

        if data_col is None:
            lowered = [x.lower() for x in c]
            hit = next((i for i, x in enumerate(lowered) if x in wanted), None)
            if hit is not None and any("archive" in x for x in lowered):
                data_col = hit
                mem_type = c[hit]
                name_col = next(i for i, x in enumerate(lowered) if "archive" in x)
            continue

        if name_col is None or name_col >= len(c) or data_col >= len(c):
            continue
        name = c[name_col]
        if not name.endswith(".a"):
            continue
        m = re.fullmatch(r"(\d+)", c[data_col])
        if m:
            found[name] = found.get(name, 0) + int(m.group(1))

    return (found, mem_type) if found else None


def main(argv: list[str]) -> int:
    size_dir = Path(argv[1] if len(argv) > 1 else "firmware/size")

    parsed = from_json2(size_dir / "size-components.json")
    source = "size-components.json (json2)"
    if parsed is None:
        parsed = from_text(size_dir / "size-components.txt")
        source = "size-components.txt (text table)"

    archives = None
    mem_type = "?"
    if parsed is not None:
        archives, mem_type = parsed

    if archives is None:
        print("section 6 budget check: COULD NOT DETERMINE")
        print(f"  no readable per-archive size report in {size_dir}")
        print("  looked for size-components.json then size-components.txt")
        print(f"  memory types tried: {', '.join(STATIC_DATA_MEMORY_TYPES)}")
        print("  an undetermined figure is not a pass - see M0-LOG.md")
        return 2

    core = {name: b for name, b in archives.items() if name in CORE_ARCHIVES}
    total = sum(core.values())

    print(f"section 6 budget check  (from {source}, memory type {mem_type})")
    if mem_type.upper() == "DIRAM":
        # Worth saying, because a reader who knows section 6 will be looking for a DRAM row.
        print("  note: this target has one unified internal SRAM serving as both data and")
        print("  instruction memory, so esp-idf-size reports DIRAM and there is no DRAM row.")
        print("  Section 6's 160 KB static-DRAM ceiling is a classic-ESP32 figure and does not")
        print("  apply here; the 64 KB core cap below is a Potluck design limit and still does.")
    print(f"  {'archive':<26}{'static data':>14}")
    for name in CORE_ARCHIVES:
        if name in core:
            print(f"  {name:<26}{core[name]:>12,} B")
        else:
            print(f"  {name:<26}{'absent':>14}")
    print(f"  {'-' * 40}")
    print(f"  {'Potluck core total':<26}{total:>12,} B")
    print(f"  {'section 6 design total':<26}{DESIGN_BYTES:>12,} B  (59.4 KB)")
    print(f"  {'section 6 hard cap':<26}{CAP_BYTES:>12,} B  (64 KB)")

    if not core:
        print("\n  none of the Potluck archives appeared in the report.")
        print("  either the component names changed or the report is of a different project.")
        return 2

    if total > CAP_BYTES:
        print(f"\n  OVER BUDGET by {total - CAP_BYTES:,} B.")
        print("  M0-BRIEF.md: core above the 64 KB cap = build treated as failing.")
        return 1

    print(f"\n  within budget: {total / 1024:.1f} KB used, "
          f"{(CAP_BYTES - total) / 1024:.1f} KB of headroom "
          f"({100 * total / CAP_BYTES:.1f}% of the cap).")
    if total > DESIGN_BYTES:
        print(f"  note: above section 6's 59.4 KB design allocation by {total - DESIGN_BYTES:,} B,")
        print("  which is inside the cap but means the design table is now optimistic.")
    else:
        print("  M0 allocates only part of section 6's table: the namespace, actor blocks, crypto")
        print("  contexts, reassembly buffers and TX ring belong to M1 and later, so the remaining")
        print("  headroom is committed, not free.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
