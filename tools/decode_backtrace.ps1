# Decode a panic backtrace from a captured console log.
#
#   tools\decode_backtrace.ps1                                  # build\qemu-console.log, QEMU mirror ELF
#   tools\decode_backtrace.ps1 -Log build\board.log -Elf D:\esp\potluck-fw-esp32s3\build\potluck_m0.elf
#
# WHY THIS EXISTS, AND THE MISTAKE IT PREVENTS
#
# A backtrace is a list of addresses. Decoding it against an ELF built from *different* source gives
# a plausible-looking answer that is entirely wrong -- during M0 debugging a stale ELF pointed the
# blame at mbedtls PSA crypto init, a function the firmware never calls. Nothing in the output said
# so; the addresses simply landed inside whatever symbol happened to occupy them.
#
# ESP-IDF prints `ELF file SHA256: <9 hex>` immediately before "Rebooting...", precisely so this can
# be checked. This script checks it and REFUSES to decode on a mismatch. A refusal to answer is worth
# more than a confident wrong answer.
#
# addr2line attributes an address to the nearest preceding symbol, which for a PC just past the end
# of a function names its neighbour. So each frame is resolved twice -- addr2line for file:line, and
# the symbol table for the enclosing symbol -- and both are shown. When they disagree, believe the
# symbol table.

[CmdletBinding()]
param(
    [string]$Log = "",
    [string]$Elf = "D:\esp\potluck-fw-qemu\build\potluck_m0.elf",
    [switch]$SkipShaCheck
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ($Log -eq "") { $Log = Join-Path $repo "build\qemu-console.log" }

if (-not (Test-Path $Log)) { throw "no console log at $Log" }
if (-not (Test-Path $Elf)) { throw "no ELF at $Elf" }

$toolRoot = "C:\Users\tuebo\.espressif\tools\xtensa-esp-elf"
$a2l = Get-ChildItem $toolRoot -Recurse -Filter "xtensa-esp32s3-elf-addr2line.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
$nm = Get-ChildItem $toolRoot -Recurse -Filter "xtensa-esp32s3-elf-nm.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $a2l) { throw "xtensa-esp32s3-elf-addr2line not found under $toolRoot" }

$text = Get-Content $Log -Raw

# --- gate: does this ELF belong to this log? ------------------------------------------------------
# @() so a single match stays an array. Indexing a bare string yields its first *character*, which
# printed "sha256 f..." and made a correct check look broken.
$logShas = @([regex]::Matches($text, 'ELF file SHA256:\s*([0-9a-f]+)') |
    ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
$elfSha = (Get-FileHash $Elf -Algorithm SHA256).Hash.ToLower()

if ($logShas.Count -eq 0) {
    Write-Host "log carries no 'ELF file SHA256' line - cannot verify the ELF matches." -ForegroundColor Yellow
} else {
    $ok = $false
    foreach ($s in $logShas) { if ($elfSha.StartsWith($s)) { $ok = $true } }
    if ($ok) {
        Write-Host "ELF matches the log (sha256 $($logShas[0])...)" -ForegroundColor Green
    } else {
        Write-Host "ELF SHA MISMATCH" -ForegroundColor Red
        Write-Host "  log says : $($logShas -join ', ')"
        Write-Host "  elf is   : $($elfSha.Substring(0,9))"
        Write-Host ""
        Write-Host "The ELF was rebuilt after this log was captured. Every address below would be"
        Write-Host "decoded against the wrong binary. Re-run the capture, or pass -SkipShaCheck if"
        Write-Host "you know what you are doing."
        if (-not $SkipShaCheck) { exit 2 }
        Write-Host "-SkipShaCheck given; results below are UNTRUSTWORTHY." -ForegroundColor Red
    }
}

# --- symbol table, for enclosing-symbol lookup ----------------------------------------------------
$symAddrs = New-Object System.Collections.Generic.List[UInt32]
$symNames = New-Object System.Collections.Generic.List[String]
if ($nm) {
    foreach ($line in (& $nm.FullName -C -n --defined-only $Elf 2>$null)) {
        if ($line -match '^([0-9a-fA-F]{8})\s+(\S)\s+(.+)$') {
            $symAddrs.Add([Convert]::ToUInt32($Matches[1], 16))
            $symNames.Add($Matches[3])
        }
    }
}
function Enclosing([UInt32]$addr) {
    if ($symAddrs.Count -eq 0) { return "?" }
    $lo = 0; $hi = $symAddrs.Count - 1; $best = -1
    while ($lo -le $hi) {
        $mid = [int](($lo + $hi) / 2)
        if ($symAddrs[$mid] -le $addr) { $best = $mid; $lo = $mid + 1 } else { $hi = $mid - 1 }
    }
    if ($best -lt 0) { return "?" }
    return "$($symNames[$best]) +0x$(('{0:x}' -f ($addr - $symAddrs[$best])))"
}

# --- the panics -----------------------------------------------------------------------------------
$panics = [regex]::Matches($text, "Guru Meditation Error:\s*(.+)")
Write-Host ""
Write-Host "$($panics.Count) panic(s) in $Log"

$excvaddr = [regex]::Matches($text, 'EXCVADDR:\s*(0x[0-9a-fA-F]+)') |
    ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
$pcs = [regex]::Matches($text, 'PC\s+:\s*(0x[0-9a-fA-F]+)') |
    ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
if ($panics.Count -gt 0) {
    Write-Host "  reason   : $($panics[0].Groups[1].Value.Trim())"
    Write-Host "  PC       : $($pcs -join ', ')"
    Write-Host "  EXCVADDR : $($excvaddr -join ', ')   <- the address the faulting access used"
}

$resets = [regex]::Matches($text, 'rst:(0x[0-9a-f]+)\s*\(([A-Z_0-9]+)\)') |
    ForEach-Object { $_.Groups[2].Value }
if ($resets.Count -gt 0) {
    Write-Host "  resets   : $(($resets | Group-Object | ForEach-Object { "$($_.Count)x $($_.Name)" }) -join ', ')"
}

# Distinct backtraces only: a boot loop repeats the same one, and printing it nine times hides
# whether there is in fact more than one failure.
$traces = [regex]::Matches($text, 'Backtrace:((?:\s*0x[0-9a-fA-F]+:0x[0-9a-fA-F]+)+)') |
    ForEach-Object { $_.Groups[1].Value.Trim() } | Select-Object -Unique

if ($traces.Count -eq 0) {
    Write-Host ""
    Write-Host "no 'Backtrace:' line in the log."
    Write-Host "If the panic printed no backtrace, CONFIG_ESP_SYSTEM_PANIC is probably not"
    Write-Host "PRINT_REBOOT, or the log was truncated mid-panic."
    exit 0
}

$n = 0
foreach ($t in $traces) {
    $n++
    Write-Host ""
    Write-Host "=== backtrace $n of $($traces.Count) ==="
    $frames = [regex]::Matches($t, '(0x[0-9a-fA-F]+):0x[0-9a-fA-F]+') |
        ForEach-Object { $_.Groups[1].Value }
    $decoded = & $a2l.FullName -pfiaC -e $Elf @frames 2>$null
    $i = 0
    foreach ($d in $decoded) {
        # addr2line emits one line per frame, plus extra "(inlined by)" lines that carry no address.
        if ($d -match '^(0x[0-9a-fA-F]+):') {
            $addr = [Convert]::ToUInt32($Matches[1], 16)
            Write-Host ("  #{0}  {1}" -f $i, $d)
            Write-Host ("      in symbol: {0}" -f (Enclosing $addr)) -ForegroundColor DarkGray
            $i++
        } else {
            Write-Host ("      {0}" -f $d.Trim())
        }
    }
}
Write-Host ""
