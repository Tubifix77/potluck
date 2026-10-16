# Build the packaged Potluck (M20): dist\Potluck\ with potluck.exe and potluck-tray.exe -- no Python or ESP-IDF needed
# where it runs. Needs the packaging environment (PyInstaller and the hub's packages) at
# %LOCALAPPDATA%\potluck\build-env, made once with:
#   py -3.14 -m venv $env:LOCALAPPDATA\potluck\build-env
#   & $env:LOCALAPPDATA\potluck\build-env\Scripts\python.exe -m pip install pyinstaller cryptography==46.0.7 pyserial==3.5 esptool==5.3.1
# Then install it for the current user with packaging\install.ps1.
# It also puts the release firmware in dist\Potluck\firmware (potluck.release), so a PC with no ESP-IDF can set its first
# boards up; building it needs ESP-IDF here, once. -FromCluster <cluster file> builds the release proper -- the image apply
# builds for that cell, with the built-in apps it names (keys\desk-m14-hub.json on the bench); without it, Potluck and the
# sandbox only. -NoFirmware leaves the firmware out.
param([switch]$NoFirmware, [string]$FromCluster = "")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $env:LOCALAPPDATA "potluck\build-env\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "no packaging environment at $py -- see the top of this script" }
Push-Location $root
try {
    & $py -m PyInstaller packaging\potluck.spec --distpath dist --workpath build\pyinstaller --noconfirm
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }
    # the commit it is built from, which the hub reports as its version (a package has no git to ask)
    $rev = (git -C $root rev-parse --short=7 HEAD).Trim()
    if (git -C $root status --porcelain --untracked-files=no) { $rev = "$rev-dirty" }
    Set-Content -Encoding ascii -NoNewline dist\Potluck\version.txt $rev
    & dist\Potluck\potluck.exe setup ports | Out-Null  # the frozen program starts and finds its modules
    if ($LASTEXITCODE -ne 0) { throw "the built program does not start" }
    # esptool, GPLv2+, as a program of its own beside Potluck's: its licence and where its source is go with it.
    $esp = New-Item -ItemType Directory -Force dist\Potluck\esptool
    Move-Item -Force dist\esptool.exe (Join-Path $esp "esptool.exe")
    $meta = & $py -c "import importlib.metadata as m; d = m.distribution('esptool'); print(d.version); print(d.locate_file(''))"
    $ver, $site = $meta[0], $meta[1]
    Copy-Item (Join-Path $site "esptool-$ver.dist-info\licenses\LICENSE") (Join-Path $esp "LICENSE")
    Set-Content -Encoding ascii (Join-Path $esp "SOURCE.txt") @(
        "esptool $ver, Espressif's flasher, GPLv2+ (LICENSE beside this file).",
        "Source: https://github.com/espressif/esptool/tree/v$ver",
        "Built unmodified into this one-file program by PyInstaller; Potluck runs it as a separate program.")
    & (Join-Path $esp "esptool.exe") version | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "esptool.exe does not start" }
    if (-not $NoFirmware) {
        $env:PYTHONPATH = Join-Path $root "host\potluck"
        if ($FromCluster) { & $py -m potluck.release dist\Potluck\firmware --from (Resolve-Path $FromCluster) }
        else { & $py -m potluck.release dist\Potluck\firmware --repo $root }
        if ($LASTEXITCODE -ne 0) { throw "the release firmware was not built" }
    }
    Write-Host "built: $root\dist\Potluck (potluck.exe, potluck-tray.exe$(if (-not $NoFirmware) { ', firmware' }))"
} finally { Pop-Location }
