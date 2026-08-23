# Run everything that can be checked without hardware.
#
#   tools\run_all_tests.ps1            # C++ suite, fixture regen, Python suites
#   tools\run_all_tests.ps1 -Asan      # also run the C++ suite under AddressSanitizer
#   tools\run_all_tests.ps1 -Firmware  # also build the firmware and check the section 6 budget
#   tools\run_all_tests.ps1 -Selftest  # also run the on-target self-test under QEMU (~2 min)
#
# Exits non-zero if anything failed, so it is usable as a gate.

[CmdletBinding()]
param(
    [switch]$Asan,
    [switch]$Firmware,
    [switch]$Selftest
)

$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$results = [System.Collections.Generic.List[object]]::new()
function Record([string]$name, [int]$code) {
    $results.Add([pscustomobject]@{ Name = $name; Code = $code })
    $mark = if ($code -eq 0) { "PASS" } else { "FAIL" }
    Write-Host ""
    Write-Host "==> $mark  $name"
}

Write-Host "### C++ host tests"
& (Join-Path $PSScriptRoot "run_host_tests.ps1")
Record "C++ host tests" $LASTEXITCODE

if ($Asan) {
    Write-Host ""
    Write-Host "### C++ host tests under AddressSanitizer"
    & (Join-Path $PSScriptRoot "run_host_tests.ps1") -Asan -BuildDir "build/tests-asan"
    Record "C++ host tests (ASan)" $LASTEXITCODE
}

# Regenerate the fixture the Python tests read, so a serial-format change shows up
# as a Python failure here rather than as a surprise on the bench.
$exe = Get-ChildItem "build/tests" -Filter "pot_tests*" -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in ".exe", "" } | Select-Object -First 1
if ($exe) {
    Write-Host ""
    Write-Host "### regenerating fixtures"
    $null = New-Item -ItemType Directory -Force -Path "host/potluck/tests/fixtures"

    # The serial-format fixture: small, stable, checked in.
    & $exe.FullName --emit-fixtures "host/potluck/tests/fixtures"
    Record "serial format fixture" $LASTEXITCODE

    # The differential fuzz corpus: ~4 MB, deterministic, NOT checked in. Regenerated
    # here so test_differential.py has something to replay. It is byte-identical on
    # every run because the C++ fuzzer uses fixed seeds.
    & $exe.FullName --emit-fuzz-corpus "host/potluck/tests/fixtures"
    Record "differential fuzz corpus" $LASTEXITCODE

    & $exe.FullName --emit-serial-corpus "host/potluck/tests/fixtures"
    Record "serial framing corpus" $LASTEXITCODE

    # READ/WRITE/REPLY across every value type, quality and error code, so the host's second
    # implementation of §5.2's namespace payloads has to agree with the firmware's rather than only
    # with itself.
    & $exe.FullName --emit-ns-corpus "host/potluck/tests/fixtures"
    Record "namespace wire corpus" $LASTEXITCODE
} else {
    Write-Host "WARNING: test binary not found; Python tests will use whatever fixtures exist"
}

$python = if (Get-Command python -ErrorAction SilentlyContinue) { "python" } else { "py" }
foreach ($t in @("test_frame.py", "test_records.py", "test_differential.py", "test_paths.py", "test_sys_paths.py", "test_serial.py", "test_serial_diff.py", "test_value.py", "test_ns_diff.py", "test_bridge.py", "test_replay.py", "test_transport.py", "test_manifest.py", "test_locality.py")) {
    Write-Host ""
    Write-Host "### Python: $t"
    Push-Location "host/potluck"
    try {
        & $python (Join-Path "tests" $t)
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    Record "Python $t" $code
}

# The strict GCC warning set over the portable core. MSVC has always been the only compiler to see
# this code; -Wconversion, -Wsign-conversion, -Wshadow, -Wcast-qual and -Wold-style-cast had never
# run against a GCC frontend until this gate existed.
if (Get-Command bash -ErrorAction SilentlyContinue) {
    Write-Host ""
    Write-Host "### portability gate (strict GCC warnings, portable core)"
    bash tools/check_portability.sh
    Record "portable core strict warnings" $LASTEXITCODE
} else {
    Write-Host "note: bash not found; skipping the portability gate"
}

if ($Firmware) {
    Write-Host ""
    Write-Host "### firmware build and section 6 budget check"
    & (Join-Path $PSScriptRoot "build_firmware.ps1")
    Record "firmware build + budget" $LASTEXITCODE
}

if ($Selftest) {
    # The checks that only the target can make: an unaligned struct cast, soft-float doubles, a task
    # on core 1, the codec path's real stack cost. Off by default because it builds the firmware and
    # boots an emulator, which is a minute or two rather than the seconds everything above takes.
    Write-Host ""
    Write-Host "### on-target self-test under QEMU"
    & (Join-Path $PSScriptRoot "run_selftest.ps1")
    Record "on-target self-test" $LASTEXITCODE
}

Write-Host ""
Write-Host ("=" * 60)
foreach ($r in $results) {
    $mark = if ($r.Code -eq 0) { "PASS" } else { "FAIL ($($r.Code))" }
    Write-Host ("{0,-32} {1}" -f $r.Name, $mark)
}
$failed = @($results | Where-Object { $_.Code -ne 0 }).Count
Write-Host ("=" * 60)
if ($failed -eq 0) {
    Write-Host "ALL PASS"
    exit 0
}
Write-Host "$failed FAILED"
exit 1
