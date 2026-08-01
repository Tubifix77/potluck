# Run potluck-capture from anywhere in the repository.
#
#   tools\capture.ps1 --list-ports
#   tools\capture.ps1 --port COM7 --capture captures\node1.jsonl --interval 60
#   tools\capture.ps1 --replay captures\node1.jsonl
#
# All arguments are passed through to `python -m potluck`. See
# host/potluck/README.md for the full set.
#
# Two conveniences, both because a bench machine is not a development machine:
#   * runs the module from its own directory, so no install step is needed;
#   * prefers ESP-IDF's Python if the system one lacks pyserial, since a machine
#     set up to build the firmware already has pyserial in that environment.

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args = @()
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$pkgDir = Join-Path $repo "host\potluck"

if (-not (Test-Path (Join-Path $pkgDir "potluck\__main__.py"))) {
    throw "potluck not found under $pkgDir"
}

# Reading a live port needs pyserial; a file or a replay does not.
$needsSerial = ($Args -contains "--port") -or ($Args -contains "--list-ports")

function Test-Python([string]$exe) {
    if (-not $exe) { return $false }
    try {
        & $exe -c "import serial" 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } catch { return $false }
}

$python = "python"
if ($needsSerial -and -not (Test-Python "python")) {
    # ESP-IDF's environment ships pyserial. Prefer the newest one present.
    $idfPy = Get-ChildItem (Join-Path $env:USERPROFILE ".espressif\python_env\*\Scripts\python.exe") `
        -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($idfPy -and (Test-Python $idfPy.FullName)) {
        Write-Host "# using ESP-IDF's Python for pyserial: $($idfPy.FullName)"
        $python = $idfPy.FullName
    } else {
        Write-Host "# note: pyserial not found. Install it with 'pip install pyserial',"
        Write-Host "#       or read a saved log with --file instead of --port."
    }
}

# Relative paths in the arguments are the user's, relative to where they ran this
# from -- so resolve them before changing directory to run the module.
$resolved = @()
foreach ($a in $Args) {
    if ($a -like "-*" -or -not ($a -match '[\\/]')) {
        $resolved += $a
    } else {
        $full = Join-Path (Get-Location) $a
        $resolved += ([System.IO.Path]::GetFullPath($full))
    }
}

Push-Location $pkgDir
try {
    & $python -m potluck @resolved
    exit $LASTEXITCODE
} finally {
    Pop-Location
}
