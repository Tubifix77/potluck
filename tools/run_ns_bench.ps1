# Build and run the namespace lookup microbenchmark (tests/bench_namespace.cpp) -- ARCHITECTURE.md section 15,
# question 2. MSVC /O2 x64, the compiler the host tests use (located as tools\run_host_tests.ps1 does). The
# timings are host figures; the probe counts are machine-independent.
#
#   tools\run_ns_bench.ps1            # build into build\ns-bench and run

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $vsPath) { throw "MSVC not found (vswhere)." }
    & (Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation *> $null
}

$out = Join-Path $repo "build\ns-bench"
New-Item -ItemType Directory -Force $out | Out-Null
$ns = Join-Path $repo "firmware\components\pot_ns"
Push-Location $out
try {
    & cl /nologo /std:c++17 /O2 /DNDEBUG /EHsc /I "$ns\include" /I (Join-Path $repo "firmware\components\pot_frame\include") `
        (Join-Path $repo "tests\bench_namespace.cpp") "$ns\src\namespace.cpp" "$ns\src\value.cpp" `
        /Fe:bench_namespace.exe
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    & .\bench_namespace.exe
} finally {
    Pop-Location
}
