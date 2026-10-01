# Start, check or stop a long unattended capture across every attached board.
#
#   tools\soak.ps1 -Start                       # every CH343 port it can find
#   tools\soak.ps1 -Start -Ports COM3,COM4      # or name them
#   tools\soak.ps1 -Status                      # are they alive, are the files growing
#   tools\soak.ps1 -Stop
#
# WHY A LAUNCHER RATHER THAN A COMMAND LINE
#
# §13-M0's acceptance is a 24-hour soak, and three things about that duration need handling once
# rather than remembered three times at midnight:
#
#   * **The capture must outlive whatever started it.** A process launched from a terminal -- or from
#     an agent session -- dies when that closes, and a soak that dies at hour three is discovered at
#     hour twenty-four. These are detached with Start-Process, so nothing it was started from matters
#     afterwards.
#   * **The console baud is 115200, not the 921600 default.** `python -m potluck` defaults to the
#     frame-link rate, which is the other UART. Getting this wrong yields a file full of nothing and
#     no error.
#   * **Every board is captured, not one.** Each node reports its own view of every link, so N boards
#     give N independent views of the same cell and A's opinion of B can be checked against B's
#     opinion of A. One console would be cheaper and much weaker evidence.
#
# The capture itself already handles the rest: capture.py flushes every line (a soak that ends in a
# power cut must not leave a buffered, truncated file), opens in append mode, and source.py retries a
# port that disappears rather than exiting. None of that needed building -- it was written that way.

[CmdletBinding()]
param(
    [switch]$Start,
    [switch]$Stop,
    [switch]$Status,
    [string[]]$Ports,
    [string]$Label = (Get-Date -Format "yyyy-MM-dd"),
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$pkgDir = Join-Path $repo "host\potluck"
$capDir = Join-Path $repo "captures"
$stateFile = Join-Path $capDir "soak-running.json"

function Get-BoardPorts {
    # The boards enumerate as CH343; the CP2102 adapter and anything else must not be swept up.
    Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match "CH343" } |
        ForEach-Object { if ($_.FriendlyName -match "\((COM\d+)\)") { $Matches[1] } } |
        Sort-Object
}

function Get-IdfPython {
    # pyserial lives in ESP-IDF's environment on this machine, not the system Python.
    $env:IDF_PATH = "D:\esp\esp-idf"
    & "D:\esp\esp-idf\export.ps1" *> $null
    return (Get-Command python).Source
}

if ($Stop) {
    if (-not (Test-Path $stateFile)) { Write-Host "no soak recorded as running"; exit 0 }
    $state = Get-Content $stateFile -Raw | ConvertFrom-Json
    foreach ($p in $state.processes) {
        Stop-ScheduledTask -TaskName $p.task -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $p.task -Confirm:$false -ErrorAction SilentlyContinue
        Write-Host "stopped and removed $($p.task) ($($p.port))"
    }
    Remove-Item $stateFile -Force
    exit 0
}

if ($Status) {
    if (-not (Test-Path $stateFile)) { Write-Host "no soak recorded as running"; exit 1 }
    $state = Get-Content $stateFile -Raw | ConvertFrom-Json
    $started = [datetime]$state.started
    $elapsed = (Get-Date) - $started
    Write-Host ("started {0}  --  running {1:dd}d {1:hh}h {1:mm}m" -f $started, $elapsed)
    $bad = 0
    foreach ($p in $state.processes) {
        $t = Get-ScheduledTask -TaskName $p.task -ErrorAction SilentlyContinue
        $alive = $t -and $t.State -eq "Running"
        $size = if (Test-Path $p.file) { (Get-Item $p.file).Length } else { 0 }
        $age = if (Test-Path $p.file) { ((Get-Date) - (Get-Item $p.file).LastWriteTime).TotalSeconds } else { 9999 }
        # Judge on the FILE, not on the task. A task reported as Running whose file stopped growing is
        # the failure that matters, because it looks healthy from outside and produces nothing.
        $verdict = if (-not $alive) { "NOT RUNNING" } elseif ($age -gt 120) { "STALLED ($([int]$age)s since write)" } else { "ok" }
        if ($verdict -ne "ok") { $bad++ }
        Write-Host ("  {0,-6} {1,-22} {2,12:N0} B  {3}" -f $p.port, $p.task, $size, $verdict)
    }
    exit $(if ($bad -eq 0) { 0 } else { 1 })
}

if (-not $Start) {
    Write-Host "usage: tools\soak.ps1 -Start | -Status | -Stop"
    exit 2
}

if (Test-Path $stateFile) {
    throw "a soak is already recorded as running; use -Status, or -Stop it first"
}

if (-not $Ports) { $Ports = Get-BoardPorts }
if (-not $Ports) { throw "no CH343 ports found - is anything plugged in?" }

$null = New-Item -ItemType Directory -Force -Path $capDir
$python = Get-IdfPython
Write-Host "python: $python"
Write-Host "ports : $($Ports -join ', ')"

$procs = @()
foreach ($port in $Ports) {
    $file = Join-Path $capDir "soak-$Label-$port.jsonl"
    $argv = "-m potluck --port $port --baud $Baud --capture `"$file`" --quiet"
    $taskName = "potluck-soak-$port"

    # A SCHEDULED TASK, not Start-Process. This is the second attempt and the reason matters.
    #
    # The first version used Start-Process and announced itself as detached. It was detached from the
    # *shell* -- the launching PowerShell exited and the captures kept running, which looked like
    # proof. It was not detached from the *application*: Start-Process leaves the child inside the
    # parent's Windows job object, so when the Claude desktop app restarted itself at 22:43 on
    # 2026-10-01 it took all three captures down with it, 3 h 27 m into a 24-hour run. The PC never
    # slept, never rebooted, and the boards stayed enumerated throughout.
    #
    # Task Scheduler spawns from a service instead, outside any application's job object. Nothing a
    # GUI does can reach it.
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    $action = New-ScheduledTaskAction -Execute $python -Argument $argv -WorkingDirectory $pkgDir
    $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddSeconds(15)
    # ExecutionTimeLimit 0 = no limit. The default is three days, which would silently truncate a
    # long soak; StartWhenAvailable and the idle settings stop Windows helping in other ways.
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
                    -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero) `
                    -DontStopOnIdleEnd -MultipleInstances IgnoreNew
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
                           -Settings $settings -Description "Potluck soak capture on $port" | Out-Null
    $procs += [pscustomobject]@{ port = $port; task = $taskName; file = $file }
    Write-Host ("  {0} -> scheduled task {1}  {2}" -f $port, $taskName, $file)
}

[pscustomobject]@{
    started   = (Get-Date).ToString("o")
    label     = $Label
    baud      = $Baud
    processes = $procs
} | ConvertTo-Json -Depth 4 | Set-Content -Path $stateFile -Encoding ascii

Write-Host ""
Write-Host "soak started as scheduled tasks. Closing this shell, this session, or the"
Write-Host "Claude app will NOT stop it - Task Scheduler runs them from a service."
Write-Host "  check:  tools\soak.ps1 -Status"
Write-Host "  stop :  tools\soak.ps1 -Stop"
