# Install the packaged Potluck for the current user (M20). Run it yourself, from a normal PowerShell:
#   powershell -ExecutionPolicy Bypass -File packaging\install.ps1 [-Desk <Potluck Desk checkout>]
# -Desk puts "Open Potluck Desk" in the tray's menu (and its Add boards behind "Add boards"): it adds the desk's folder
# and this PC's pythonw.exe to %LOCALAPPDATA%\potluck\tray.json, keeping what is already there.
# It copies dist\Potluck to %LOCALAPPDATA%\Programs\Potluck, adds "Potluck" to the Start menu, and starts the tray icon at
# login (it starts a hub when none runs). Nothing needs administrator rights; uninstall with packaging\uninstall.ps1.
param([string]$Desk = "")
$ErrorActionPreference = "Stop"
$src = Join-Path (Split-Path -Parent $PSScriptRoot) "dist\Potluck"
if (-not (Test-Path (Join-Path $src "potluck-tray.exe"))) { throw "build it first: tools\build_package.ps1" }
$dest = Join-Path $env:LOCALAPPDATA "Programs\Potluck"
# the tray and the hub it keeps (potluck.exe): a running hub holds _internal's DLLs, so the copy would fail
$running = @(Get-Process potluck-tray, potluck, esptool -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dest*" })
$running | Stop-Process -Force
$running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item -Recurse -Force (Join-Path $src "*") $dest

$shell = New-Object -ComObject WScript.Shell
foreach ($dir in @([Environment]::GetFolderPath("Programs"), [Environment]::GetFolderPath("Startup"))) {
    $lnk = $shell.CreateShortcut((Join-Path $dir "Potluck.lnk"))
    $lnk.TargetPath = Join-Path $dest "potluck-tray.exe"
    $lnk.WorkingDirectory = $dest
    $lnk.Description = "Potluck: the cell's state, its dashboard and setup"
    $lnk.Save()
}
if ($Desk) {
    $Desk = (Resolve-Path $Desk).Path
    if (-not (Test-Path (Join-Path $Desk "potluck_desk"))) { throw "no potluck_desk folder in $Desk -- give the desk's checkout" }
    $pyw = $null
    $cmd = Get-Command pythonw.exe -ErrorAction SilentlyContinue
    if ($cmd) { $pyw = $cmd.Source }
    if (-not $pyw) {
        $py = Get-Command py.exe -ErrorAction SilentlyContinue
        if ($py) { $pyw = (& $py.Source -3 -c "import sys, os; print(os.path.join(os.path.dirname(sys.executable), 'pythonw.exe'))").Trim() }
    }
    if (-not $pyw -or -not (Test-Path $pyw)) { throw "no pythonw.exe found for the desk -- install Python 3.14 first" }
    $cfgDir = Join-Path $env:LOCALAPPDATA "potluck"
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    $cfgFile = Join-Path $cfgDir "tray.json"
    $cfg = @{}
    if (Test-Path $cfgFile) {
        $old = [IO.File]::ReadAllText($cfgFile).TrimStart([char]0xFEFF) | ConvertFrom-Json
        foreach ($p in $old.PSObject.Properties) { $cfg[$p.Name] = $p.Value }
    }
    if ($cfg["desk"] -and $cfg["desk"].exe) {
        # the Desk's own installer wrote its exe: an installed desk wins over a checkout (2026-10-19)
        Write-Host "kept the installed Potluck Desk in tray.json ($($cfg["desk"].exe)); -Desk changed nothing"
    } else {
        $cfg["desk"] = [ordered]@{ python = $pyw; cwd = $Desk }
        Write-Host "the tray's menu opens the Potluck Desk in $Desk (with $pyw)"
    }
    # no BOM (the tray reads one, but nothing else needs it)
    [IO.File]::WriteAllText($cfgFile, ($cfg | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
}
Start-Process (Join-Path $dest "potluck-tray.exe") -WorkingDirectory $dest
Write-Host "Potluck installed in $dest; the tray icon starts now and at every login."
