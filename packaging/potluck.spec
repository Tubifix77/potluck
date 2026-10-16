# PyInstaller spec for the packaged Potluck (M20): one folder, two programs -- potluck.exe (the command line: hub,
# apply, setup, keystore, ...) and potluck-tray.exe (windowed: the tray icon) -- sharing one copy of everything; and
# esptool.exe, a program of its own (GPLv2+, never imported by the Apache-2.0 hub), which tools/build_package.ps1 puts
# in Potluck/esptool/ with its licence and where its source is.
#
#   <build-env python> -m PyInstaller packaging/potluck.spec --distpath dist --workpath build/pyinstaller
#
# Run from the repository root (tools/build_package.ps1 does). The dashboard's files go in beside the hub's code, where
# hub.py looks for them (potluck/dashboard); esptool's flasher stubs go into esptool.exe.

import os
import sys

from PyInstaller.utils.hooks import collect_data_files, collect_submodules

ROOT = os.path.dirname(SPECPATH)  # noqa: F821 -- PyInstaller defines SPECPATH
HOST = os.path.join(ROOT, "host", "potluck")
sys.path.insert(0, HOST)  # so collect_submodules sees the potluck package (it imports it to list them)

hidden = collect_submodules("potluck") + collect_submodules("serial")
datas = [(os.path.join(HOST, "potluck", "dashboard"), "potluck/dashboard")]
excludes = ["tkinter", "unittest", "pydoc_data", "esptool", "espefuse", "espsecure"]  # esptool runs as its own program

cli = Analysis(["potluck_cli.py"], pathex=[HOST], datas=datas, hiddenimports=hidden, excludes=excludes)
tray = Analysis(["potluck_tray.py"], pathex=[HOST], datas=datas, hiddenimports=hidden, excludes=excludes)

cli_pyz = PYZ(cli.pure)
tray_pyz = PYZ(tray.pure)

cli_exe = EXE(cli_pyz, cli.scripts, [], exclude_binaries=True, name="potluck", console=True)
tray_exe = EXE(tray_pyz, tray.scripts, [], exclude_binaries=True, name="potluck-tray", console=False)

COLLECT(cli_exe, cli.binaries, cli.datas, tray_exe, tray.binaries, tray.datas, name="Potluck")

# esptool: one file, built from its own entry point (esptool = esptool.__init__:_main), nothing of Potluck's inside.
esp = Analysis(["esptool_main.py"], hiddenimports=collect_submodules("esptool"), datas=collect_data_files("esptool"),
               excludes=["tkinter", "unittest", "pydoc_data", "potluck"])
EXE(PYZ(esp.pure), esp.scripts, esp.binaries, esp.datas, [], name="esptool", console=True)
