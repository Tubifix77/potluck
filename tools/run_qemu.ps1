# Run the firmware under QEMU — no hardware required.
#
#   tools\run_qemu.ps1                 # boot, print the console, exit after -Seconds
#   tools\run_qemu.ps1 -Seconds 60
#   tools\run_qemu.ps1 -FrameLinkPort 5555   # expose UART1 as a TCP socket for potluck-bridge
#
# WHAT THIS DOES AND DOES NOT PROVE
#
# Espressif maintains a QEMU fork emulating the ESP32-S3's CPU, memory and several peripherals.
# It has no Wi-Fi device -- `qemu-system-xtensa -device help` lists none, and the machine is handed
# an Ethernet device instead -- so esp_wifi_start() enters PHY calibration and never returns. The
# build used here therefore sets CONFIG_POT_RADIO_DISABLE.
#
#   Verifiable here : boot, NVS and the boot epoch, task scheduling, the namespace and §4's read
#                     contract, the serial frame link end to end, static memory figures.
#   Not verifiable  : PDR, RTT, Wi-Fi DRAM, the §6 [MEASURE] item, §8.2's timers against a real
#                     radio -- in short, every number §13-M0 asks for.
#
# Runs from this build report "no_radio":1 on every statistics line, so a capture taken here can
# never be mistaken for the measured soak.

[CmdletBinding()]
param(
    [int]$Seconds = 30,
    [int]$FrameLinkPort = 0,
    [int]$NodeId = 0,
    # Extra Kconfig lines, applied last. For bisecting a hang by turning one feature off at a time:
    #   tools\run_qemu.ps1 -Extra "CONFIG_POT_SERIAL_LINK=n"
    [string[]]$Extra = @(),
    [switch]$Clean,
    [string]$IdfPath = "D:\esp\esp-idf",
    [string]$MirrorDir = "D:\esp\potluck-fw-qemu"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$src = Join-Path $repo "firmware"

# Mirror to an ASCII path, for the same reason tools\build_firmware.ps1 does.
$null = New-Item -ItemType Directory -Force -Path $MirrorDir
robocopy $src $MirrorDir /MIR /XD build /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with $LASTEXITCODE" }

$env:IDF_PATH = $IdfPath
& (Join-Path $IdfPath "export.ps1") *> $null

Push-Location $MirrorDir
try {
    if ($Clean -and (Test-Path "build")) { Remove-Item -Recurse -Force "build" }
    $defaults = "sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.qemu"

    # QEMU's generated efuse image has no MAC, so esp_read_mac() returns all zeros and every
    # emulated node derives the same id. -NodeId writes an extra defaults fragment to pin it, which
    # is what makes running two emulated nodes against each other possible at all.
    $frag_lines = @()
    if ($NodeId -gt 0) {
        $frag_lines += "CONFIG_POT_NODE_ID=$NodeId"
        Write-Host "node id pinned to 0x$('{0:x4}' -f $NodeId)"
    }
    if ($Extra.Count -gt 0) {
        $frag_lines += $Extra
        Write-Host "extra config: $($Extra -join ', ')"
    }
    if ($frag_lines.Count -gt 0) {
        $frag = Join-Path $MirrorDir "sdkconfig.qemu.node"
        Set-Content -Path $frag -Value $frag_lines -Encoding ascii
        $defaults = "$defaults;sdkconfig.qemu.node"
    } else {
        Remove-Item (Join-Path $MirrorDir "sdkconfig.qemu.node") -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path "build\CMakeCache.txt")) {
        Remove-Item -Force "sdkconfig" -ErrorAction SilentlyContinue
        idf.py -D SDKCONFIG_DEFAULTS="$defaults" set-target esp32s3
        if ($LASTEXITCODE -ne 0) { throw "set-target failed" }
    }
    idf.py -D SDKCONFIG_DEFAULTS="$defaults" build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }

    # idf.py qemu builds the flash and efuse images for us, then runs QEMU. Doing the run by hand
    # instead, so the console can be captured to a file and UART1 exposed on a socket.
    idf.py qemu --help *> $null
    $flash = Join-Path $MirrorDir "build\qemu_flash.bin"
    $efuse = Join-Path $MirrorDir "build\qemu_efuse.bin"

    # ALWAYS regenerate the flash image, never reuse an existing one.
    #
    # This was a real bug and an expensive one. The image is a *merge* of the bootloader, partition
    # table and application, produced separately from `idf.py build` -- so keeping it when it already
    # existed meant QEMU booted the previous application while the freshly built ELF sat next to it.
    # Every backtrace decoded against that ELF named the wrong function, with nothing in the output
    # to suggest it. `ELF file SHA256` in the panic is the only signal, which is why
    # tools\decode_backtrace.ps1 now refuses to decode when it disagrees.
    Remove-Item $flash -Force -ErrorAction SilentlyContinue
    $gen = Start-Job -ScriptBlock {
        param($idf, $dir)
        $env:IDF_PATH = $idf
        & (Join-Path $idf "export.ps1") *> $null
        Set-Location $dir
        idf.py qemu 2>&1
    } -ArgumentList $IdfPath, $MirrorDir
    for ($i = 0; $i -lt 120 -and -not (Test-Path $flash); $i++) { Start-Sleep -Milliseconds 500 }
    # The image is written incrementally; give it a moment to finish before QEMU is handed it.
    if (Test-Path $flash) { Start-Sleep -Milliseconds 1500 }
    Stop-Job $gen -ErrorAction SilentlyContinue
    Remove-Job $gen -Force -ErrorAction SilentlyContinue
    Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path $flash)) { throw "qemu_flash.bin was not generated" }

    $qemu = (Get-Command qemu-system-xtensa -ErrorAction SilentlyContinue).Source
    if (-not $qemu) { throw "qemu-system-xtensa not on PATH; run idf_tools.py install qemu-xtensa" }

    # The log lives in the mirror, not the repository: QEMU cannot open a path containing the
    # repository's non-ASCII character either -- the same limitation that forces the mirror in the
    # first place. It is copied back at the end.
    $log = Join-Path $MirrorDir "qemu-console.log"
    Remove-Item $log -ErrorAction SilentlyContinue

    $args = @(
        '-M', 'esp32s3', '-m', '32M',
        '-drive', "file=$flash,if=mtd,format=raw",
        '-drive', "file=$efuse,if=none,format=raw,id=efuse",
        '-global', 'driver=nvram.esp32s3.efuse,property=drive,value=efuse',
        '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true',
        '-nographic',
        '-serial', "file:$log"          # UART0: the console
    )
    if ($FrameLinkPort -gt 0) {
        # UART1: the Potluck Frame link. potluck-bridge connects to it as a TCP client.
        $args += @('-serial', "tcp:127.0.0.1:$FrameLinkPort,server,nowait")
        Write-Host "frame link (UART1) listening on 127.0.0.1:$FrameLinkPort"
    }

    Write-Host "qemu: $qemu"
    Write-Host "console -> $log"
    $p = Start-Process -FilePath $qemu -ArgumentList $args -PassThru -WindowStyle Hidden
    Write-Host "running for $Seconds s (pid $($p.Id))"
    Start-Sleep -Seconds $Seconds
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue

    Write-Host ""
    if (Test-Path $log) {
        $dest = Join-Path $repo "build\qemu-console.log"
        $null = New-Item -ItemType Directory -Force -Path (Split-Path $dest)
        Copy-Item $log $dest -Force
        Get-Content $log
    } else {
        Write-Host "no console output was captured"
    }
} finally {
    Pop-Location
    Get-Process qemu-system-xtensa -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}
