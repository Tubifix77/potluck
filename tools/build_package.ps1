# Build the packaged Potluck (M20): dist\Potluck\ with potluck.exe and potluck-tray.exe -- no Python or ESP-IDF needed
# where it runs. Needs the packaging environment (PyInstaller and the hub's packages) at
# %LOCALAPPDATA%\potluck\build-env, made once with:
#   py -3.14 -m venv $env:LOCALAPPDATA\potluck\build-env
#   & $env:LOCALAPPDATA\potluck\build-env\Scripts\python.exe -m pip install pyinstaller cryptography==46.0.7 pyserial==3.5 esptool==5.3.1
# Then install it for the current user with packaging\install.ps1.
# It also puts the release firmware in dist\Potluck\firmware (potluck.release: what apply builds for a cell with no apps,
# with the sandbox), so a PC with no ESP-IDF can set its first boards up; building it needs ESP-IDF here, once.
# -NoFirmware leaves it out.
param([switch]$NoFirmware)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$py = Join-Path $env:LOCALAPPDATA "potluck\build-env\Scripts\python.exe"
if (-not (Test-Path $py)) { throw "no packaging environment at $py -- see the top of this script" }
Push-Location $root
try {
    & $py -m PyInstaller packaging\potluck.spec --distpath dist --workpath build\pyinstaller --noconfirm
    if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }
    & dist\Potluck\potluck.exe setup ports | Out-Null  # the frozen program starts and finds its modules
    if ($LASTEXITCODE -ne 0) { throw "the built program does not start" }
    if (-not $NoFirmware) {
        $env:PYTHONPATH = Join-Path $root "host\potluck"
        & $py -m potluck.release dist\Potluck\firmware --repo $root
        if ($LASTEXITCODE -ne 0) { throw "the release firmware was not built" }
    }
    Write-Host "built: $root\dist\Potluck (potluck.exe, potluck-tray.exe$(if (-not $NoFirmware) { ', firmware' }))"
} finally { Pop-Location }
