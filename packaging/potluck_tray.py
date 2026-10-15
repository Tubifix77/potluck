"""potluck-tray.exe -- the windowed tray icon a login starts (see potluck/tray.py)."""
import sys

from potluck.app import tray_main

if __name__ == "__main__":
    sys.exit(tray_main())
