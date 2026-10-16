"""esptool.exe -- Espressif's flasher (GPLv2+), unmodified, as a program of its own (see packaging/potluck.spec)."""
import sys

from esptool import _main

if __name__ == "__main__":
    sys.exit(_main())
