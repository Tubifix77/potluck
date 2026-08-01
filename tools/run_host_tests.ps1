# Configure, build and run the host tests for the portable half of Potluck.
#
# Finds a toolchain rather than assuming one: the ESP-IDF install already ships CMake and Ninja, so
# a machine set up to build the firmware can run the tests with no further installation. MSVC is
# located through vswhere; on a machine with gcc or clang on PATH those are used instead.
#
#   tools\run_host_tests.ps1               # configure, build, run
#   tools\run_host_tests.ps1 -Asan         # same, with AddressSanitizer
#   tools\run_host_tests.ps1 -Filter fuzz  # run one suite
#   tools\run_host_tests.ps1 -Clean        # start from a fresh build directory

[CmdletBinding()]
param(
    [switch]$Asan,
    [switch]$Clean,
    [string]$Filter = "",
    [string]$BuildDir = "build/tests"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# --- CMake and Ninja ---------------------------------------------------------------------------
# Prefer whatever is already on PATH; fall back to the copies the ESP-IDF tools install ships.
function Find-Tool([string]$name, [string[]]$fallbackGlobs) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($g in $fallbackGlobs) {
        $hit = Get-ChildItem $g -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$espressif = Join-Path $env:USERPROFILE ".espressif\tools"
$cmakeExe = Find-Tool "cmake" @(
    "$espressif\cmake\*\bin\cmake.exe",
    "C:\Program Files*\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)
$ninjaExe = Find-Tool "ninja" @(
    "$espressif\ninja\*\ninja.exe",
    "C:\Program Files*\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
)

if (-not $cmakeExe) { throw "cmake not found. Install CMake, or run ESP-IDF's install.ps1 which ships one." }
Write-Host "cmake : $cmakeExe"
if ($ninjaExe) { Write-Host "ninja : $ninjaExe" }

# --- Compiler ----------------------------------------------------------------------------------
# On Windows the usual case is MSVC, which needs its environment imported before cl.exe resolves.
$haveCompiler = (Get-Command cl -ErrorAction SilentlyContinue) -or
                (Get-Command gcc -ErrorAction SilentlyContinue) -or
                (Get-Command clang -ErrorAction SilentlyContinue)

if (-not $haveCompiler -and $IsWindows -ne $false) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1
        if ($vsPath) {
            $devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"
            if (Test-Path $devShell) {
                Write-Host "msvc  : $vsPath"
                # Launch-VsDevShell changes directory unless told not to, and prints a banner.
                & $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation *> $null
                Set-Location $repo
            }
        }
    }
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue) -and
    -not (Get-Command gcc -ErrorAction SilentlyContinue) -and
    -not (Get-Command clang -ErrorAction SilentlyContinue)) {
    throw "No C++ compiler found (looked for cl, gcc, clang)."
}

# Put the located cmake/ninja ahead of anything else so the build uses the pair we reported.
$env:PATH = "$(Split-Path $cmakeExe)" + $(if ($ninjaExe) { ";$(Split-Path $ninjaExe)" } else { "" }) + ";$env:PATH"

# --- Configure, build, run -----------------------------------------------------------------------
if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

$generator = if ($ninjaExe) { @("-G", "Ninja") } else { @() }
$asanFlag = if ($Asan) { "-DPOT_ASAN=ON" } else { "-DPOT_ASAN=OFF" }

& $cmakeExe -S tests -B $BuildDir @generator -DCMAKE_BUILD_TYPE=Debug $asanFlag
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

& $cmakeExe --build $BuildDir --config Debug
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$exe = Get-ChildItem "$BuildDir" -Filter "pot_tests*" -Recurse -File |
    Where-Object { $_.Extension -in ".exe", "" } | Select-Object -First 1
if (-not $exe) { throw "test binary not found under $BuildDir" }

Write-Host ""
& $exe.FullName $Filter
exit $LASTEXITCODE
