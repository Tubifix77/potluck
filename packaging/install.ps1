# Install the packaged Potluck for the current user (M20). Run it yourself, from a normal PowerShell:
#   powershell -ExecutionPolicy Bypass -File packaging\install.ps1
# It copies dist\Potluck to %LOCALAPPDATA%\Programs\Potluck, adds "Potluck" to the Start menu, and starts the tray icon at
# login (it starts a hub when none runs). Nothing needs administrator rights; uninstall with packaging\uninstall.ps1.
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
Start-Process (Join-Path $dest "potluck-tray.exe") -WorkingDirectory $dest
Write-Host "Potluck installed in $dest; the tray icon starts now and at every login."
