# Run the on-target self-test under QEMU and report a verdict.
#
#   tools\run_selftest.ps1
#   tools\run_selftest.ps1 -Seconds 40      # a slow machine, or more checks added
#
# WHAT THIS ADDS OVER tools\run_host_tests.ps1
#
# The host tests compile the portable core for x86-64 and run it there. That proves the logic. It
# cannot prove anything about the target machine: a struct cast onto an unaligned buffer, double
# arithmetic on a part whose FPU is single-precision only, a task landing on core 1, the codec path's
# real stack cost. Those are properties of the running machine, and firmware/main/selftest.cpp checks
# them on it.
#
# It runs under QEMU because the self-test touches no peripheral -- no radio, no GPIO -- so a board is
# not needed to catch a target-specific mistake. What a board is still needed for is every number
# section 13-M0 asks for, and no timing figure the self-test prints is a performance measurement:
# QEMU is not cycle-accurate.
#
# Exit code is the verdict: 0 if every check passed, 1 if any failed, 2 if the run produced no
# verdict at all -- which is a different thing from a failure and is reported as such, because a
# missing result read as a pass is how a broken gate stays green.

[CmdletBinding()]
param(
    [int]$Seconds = 30,
    [string]$IdfPath = "D:\esp\esp-idf",
    [string]$MirrorDir = "D:\esp\potluck-fw-qemu"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

Write-Host "building and running the on-target self-test under QEMU"
& (Join-Path $PSScriptRoot "run_qemu.ps1") -Seconds $Seconds -Extra "CONFIG_POT_SELFTEST=y" `
    -IdfPath $IdfPath -MirrorDir $MirrorDir | Out-Null

# run_qemu.ps1 copies the console back into the repository; read the verdict from there rather than
# from its stdout, so a change to its logging cannot quietly break this gate.
$log = Join-Path $repo "build\qemu-console.log"
if (-not (Test-Path $log)) {
    Write-Host "no console log at $log" -ForegroundColor Red
    exit 2
}

$lines = Get-Content $log
$cases = $lines | Where-Object { $_ -match '"t":"selftest"' }
$end = $lines | Where-Object { $_ -match '"t":"selftest_end"' } | Select-Object -Last 1

Write-Host ""
foreach ($line in $cases) {
    if ($line -match '"case":"([^"]+)","ok":(true|false),"detail":"([^"]*)"') {
        $name, $ok, $detail = $Matches[1], $Matches[2], $Matches[3]
        $mark = if ($ok -eq "true") { "ok  " } else { "FAIL" }
        $colour = if ($ok -eq "true") { "Gray" } else { "Red" }
        $suffix = if ($detail) { "   $detail" } else { "" }
        Write-Host ("  {0} {1}{2}" -f $mark, $name, $suffix) -ForegroundColor $colour
    }
}

if (-not $end) {
    Write-Host ""
    Write-Host "the self-test produced no verdict: it did not reach the end of its run." -ForegroundColor Red
    Write-Host "That is not the same as a failure - look for a panic in $log."
    exit 2
}

if ($end -notmatch '"pass":(\d+),"fail":(\d+)') {
    Write-Host "could not read the verdict from: $end" -ForegroundColor Red
    exit 2
}
$pass, $fail = [int]$Matches[1], [int]$Matches[2]

Write-Host ""
Write-Host ("on-target self-test: {0} passed, {1} failed" -f $pass, $fail) `
    -ForegroundColor $(if ($fail -eq 0) { "Green" } else { "Red" })
if ($pass -eq 0) {
    Write-Host "zero checks ran, which is a broken gate rather than a pass." -ForegroundColor Red
    exit 2
}
Write-Host "  emulated: no timing figure above is a performance measurement (see selftest.cpp)"
exit $(if ($fail -eq 0) { 0 } else { 1 })
