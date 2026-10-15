"""The packaged Potluck (M20): one program for the hub's PC, no Python or ESP-IDF needed.

    potluck hub [...]        the resident hub (python -m potluck.hub)
    potluck tray             the tray icon (also its own windowed program, potluck-tray)
    potluck apply <cell>     potluck apply -- deploy what the cluster file says (needs a firmware build or image)
    potluck setup ports      the boards on USB, and the setup wizard's other steps
    potluck keystore ...     the owner's keys: seal, check, backup, restore
    potluck ctl ...          potctl: read and write the namespace

Each command is the module of the same name, run as `python -m potluck.<module>` would run it.
"""

from __future__ import annotations

import importlib
import sys

COMMANDS = {"hub": "hub", "tray": "tray", "apply": "apply", "setup": "setup", "keystore": "keystore", "ctl": "ctl",
            "enrol": "enrol", "fw": "fw", "hubauth": "hubauth"}


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] not in COMMANDS:
        print(__doc__.split("\n\n")[1])
        return 2
    module = importlib.import_module(f"potluck.{COMMANDS[argv[0]]}")
    return int(module.main(argv[1:]) or 0)


def tray_main() -> int:
    """potluck-tray.exe: the windowed program a login starts -- a hub if none runs, then the icon."""
    from potluck import tray
    return tray.main(["--start-hub"])


if __name__ == "__main__":
    sys.exit(main())
