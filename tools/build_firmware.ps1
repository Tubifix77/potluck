# Build the M0 firmware, and check its static DRAM against ARCHITECTURE.md §6's 64 KB core cap.
#
#   tools\build_firmware.ps1                  # mirror, build, size report, budget check
#   tools\build_firmware.ps1 -Clean           # full rebuild
#   tools\build_firmware.ps1 -Flash -Port COM7
#   tools\build_firmware.ps1 -Monitor -Port COM7
#
# ---------------------------------------------------------------------------------------------
# THE ASCII MIRROR: now conditional, and normally not used at all
# ---------------------------------------------------------------------------------------------
# ESP-IDF v6.0.2 cannot build from a path containing a non-ASCII character on a CP1252 Windows. Two
# independent failures, both measured on 2026-08-01 while the project was still called dμOS:
#
#   * Xtensa GCC. The path used U+03BC GREEK SMALL LETTER MU. argv reached the compiler as the CP1252
#     byte 0xB5, and the CRT converted that back to U+00B5 MICRO SIGN — a *different* character — so
#     cc1.exe reported "No such file or directory" for a file that plainly existed. A round-trip
#     mismatch, not an unrepresentable character: best-fit mapping turns both mu code points into 0xB5.
#
#   * esp-idf-kconfig. Tested with U+00B5, which the round-trip argument above predicts should work.
#     It does not: kconfig generation died with
#     FileNotFoundError: '.../dÂµOS-probe/build/kconfigs.in' — UTF-8 bytes read back as CP1252.
#
# Also tried and rejected then: a directory junction with an ASCII name, and a subst virtual drive.
# CMake resolves both back to the real path.
#
# THE RENAME TO POTLUCK REMOVED THE CAUSE. An all-ASCII repository path builds directly, so the mirror
# is skipped unless the path actually contains a non-ASCII character. The code is kept rather than
# deleted for two reasons: it costs one `if`, and it means the project still builds for anyone who
# clones it under a path with an accent in their user name — which is the same bug wearing a different
# hat. Pass -ForceMirror to exercise the path deliberately.
#
# When the mirror is used it is one-way: sources are copied out, build artefacts stay in the mirror,
# and only the size reports are copied back. Never edit the mirror.

[CmdletBinding()]
param(
    # esp32s3 is the default: the fleet is 7 x ESP32-S3-DevKitC-1 N16R8. Classic esp32 is still
    # supported and still builds, because §6's budget is written against it and it is useful to be
    # able to compare the two.
    [ValidateSet("esp32s3", "esp32")]
    [string]$Target = "esp32s3",
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port = "",
    [string]$IdfPath = "D:\esp\esp-idf",
    [string]$MirrorDir = "",
    # Build through the ASCII mirror even when the repository path does not need it.
    [switch]$ForceMirror
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repo "firmware"

# One mirror per target, so switching targets does not force a full rebuild each time.
if (-not $MirrorDir) { $MirrorDir = "D:\esp\potluck-fw-$Target" }

if (-not (Test-Path (Join-Path $IdfPath "export.ps1"))) {
    throw "ESP-IDF not found at $IdfPath. Pass -IdfPath, or see M0-RUNBOOK.md for the install."
}

# --- mirror, only if the path forces it ------------------------------------------------------
# Test the *whole* path, not just the repository folder's name: a non-ASCII character anywhere above
# it (a user name with an accent, say) breaks the build exactly the same way.
$needsMirror = $ForceMirror -or ($src -match '[^\x00-\x7F]')
if ($needsMirror) {
    if (-not $ForceMirror) {
        Write-Host "path contains a non-ASCII character; building through an ASCII mirror" -ForegroundColor Yellow
    }
    # /MIR keeps the mirror exactly in step with the repository, including deletions, so a source file
    # removed here cannot linger there and quietly keep building. The build directory is excluded from
    # both the copy and the purge, which is what makes incremental builds work.
    Write-Host "mirroring $src -> $MirrorDir"
    $null = New-Item -ItemType Directory -Force -Path $MirrorDir
    robocopy $src $MirrorDir /MIR /XD build /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed with $LASTEXITCODE" }
    $buildRoot = $MirrorDir
} else {
    Write-Host "building in place at $src (all-ASCII path, no mirror needed)"
    $buildRoot = $src
}

# --- ESP-IDF environment --------------------------------------------------------------------
$env:IDF_PATH = $IdfPath
& (Join-Path $IdfPath "export.ps1") *> $null
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "idf.py not on PATH after running export.ps1"
}

Push-Location $buildRoot
try {
    if ($Clean -and (Test-Path "build")) {
        Remove-Item -Recurse -Force "build"
    }
    # Re-run set-target if the mirror was last built for a different chip; sdkconfig is regenerated
    # from sdkconfig.defaults plus sdkconfig.defaults.<target>, so the S3 overrides are picked up.
    $needTarget = -not (Test-Path "build\CMakeCache.txt")
    if (-not $needTarget) {
        $cached = Select-String -Path "build\CMakeCache.txt" -Pattern "^IDF_TARGET:STRING=(.*)$" |
            ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
        if ($cached -ne $Target) { $needTarget = $true }
    }
    if ($needTarget) {
        Write-Host "setting target to $Target"
        Remove-Item -Force "sdkconfig" -ErrorAction SilentlyContinue
        idf.py set-target $Target
        if ($LASTEXITCODE -ne 0) { throw "idf.py set-target $Target failed" }
    }

    idf.py build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed" }

    # --- size reports -------------------------------------------------------------------------
    # Per target, so an S3 report never overwrites an esp32 one and the two stay comparable.
    $sizeDir = Join-Path $repo "firmware\size\$Target"
    $null = New-Item -ItemType Directory -Force -Path $sizeDir

    idf.py size            2>&1 | Tee-Object -FilePath (Join-Path $sizeDir "size.txt")           | Out-Null
    idf.py size-components 2>&1 | Tee-Object -FilePath (Join-Path $sizeDir "size-components.txt") | Out-Null

    # Structured output for the budget checker. The format is json2, not json: esp-idf-size renamed
    # it, and the version ESP-IDF v6.0.2 ships rejects `json`. Going straight to the module rather
    # than through idf.py keeps the ninja progress chatter out of the JSON file.
    $jsonPath = Join-Path $sizeDir "size-components.json"
    $mapFile = Join-Path $buildRoot "build\potluck_m0.map"
    $sizePy = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
    if (-not (Test-Path $sizePy)) { $sizePy = "python" }
    if (Test-Path $mapFile) {
        & $sizePy -m esp_idf_size --archives --format json2 $mapFile 2>$null |
            Set-Content -Path $jsonPath -Encoding utf8
    }
    if (-not (Test-Path $jsonPath) -or (Get-Item $jsonPath).Length -lt 100) {
        Remove-Item $jsonPath -ErrorAction SilentlyContinue
        Write-Host "note: json2 size report unavailable; the checker will parse the text table"
    }

    Write-Host ""
    Get-Content (Join-Path $sizeDir "size.txt") | Select-Object -Last 25

    # --- §6 budget gate -----------------------------------------------------------------------
    Write-Host ""
    python (Join-Path $repo "tools\check_size_budget.py") $sizeDir
    $budgetExit = $LASTEXITCODE

    if ($Flash) {
        if (-not $Port) { throw "-Flash needs -Port COMx" }
        idf.py -p $Port flash
        if ($LASTEXITCODE -ne 0) { throw "flash failed" }
    }
    if ($Monitor) {
        if (-not $Port) { throw "-Monitor needs -Port COMx" }
        idf.py -p $Port monitor
    }

    exit $budgetExit
} finally {
    Pop-Location
}
