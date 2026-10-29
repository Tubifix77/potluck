# Tell the installed tray which cell its hub runs (M20). Run it yourself, from a normal PowerShell:
#   powershell -ExecutionPolicy Bypass -File packaging\use-cell.ps1 -Cluster <cluster file> [-Listen 0.0.0.0:8443] [-CopyFrom <folder>]
# It writes %LOCALAPPDATA%\potluck\tray.json, so a hub the tray starts (at login, when none runs) uses that cluster file;
# -Listen also serves the dashboard on the house network, with the TLS files in %LOCALAPPDATA%\potluck\hub-tls.
# -CopyFrom copies a hub's TLS files and logins (hub-tls\, hub-users.json -- Home Assistant's pairing is in it) from
# another potluck folder, such as the one a Claude session's hub used; files already here are never overwritten.
param(
    [Parameter(Mandatory = $true)][string]$Cluster,
    [string]$Listen = "",
    [string]$CopyFrom = ""
)
$ErrorActionPreference = "Stop"
$dst = Join-Path $env:LOCALAPPDATA "potluck"
New-Item -ItemType Directory -Force $dst | Out-Null
$Cluster = (Resolve-Path $Cluster).Path

if ($CopyFrom) {
    foreach ($item in @("hub-tls", "hub-users.json")) {
        $from = Join-Path $CopyFrom $item
        $to = Join-Path $dst $item
        if (-not (Test-Path $from)) { Write-Host "not in $CopyFrom : $item"; continue }
        if (Test-Path $to) { Write-Host "kept (already here): $to"; continue }
        Copy-Item -Recurse $from $to
        Write-Host "copied: $item"
    }
}

$cfg = [ordered]@{ cluster = $Cluster }
$cfgFile = Join-Path $dst "tray.json"
if (Test-Path $cfgFile) {  # keep the Potluck Desk's entry (install.ps1 -Desk) when the cell changes
    $old = [IO.File]::ReadAllText($cfgFile).TrimStart([char]0xFEFF) | ConvertFrom-Json
    if ($old.desk) { $cfg.desk = $old.desk }
}
if ($Listen) {
    $tls = Join-Path $dst "hub-tls"
    foreach ($f in @("cert.pem", "key.pem")) {
        if (-not (Test-Path (Join-Path $tls $f))) { throw "no $f in $tls -- give -CopyFrom, or make them first" }
    }
    $cfg.hub_args = @("--listen", $Listen, "--tls-cert", (Join-Path $tls "cert.pem"), "--tls-key", (Join-Path $tls "key.pem"))
}
# no BOM: Windows PowerShell's Set-Content -Encoding UTF8 writes one, and the tray read the file as no cell (2026-10-16)
# -Depth 10: tray.json's "programs" nest a command list four levels down, which -Depth 3 wrote as "System.Object[]".
[IO.File]::WriteAllText((Join-Path $dst "tray.json"), ($cfg | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding $false))
Write-Host "the tray's hub will run $Cluster$(if ($Listen) { " and serve the dashboard on $Listen" })"
