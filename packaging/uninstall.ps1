# Remove the packaged Potluck installed by install.ps1: the program folder and its two shortcuts. The cell itself --
# keys, cluster files, logs under %LOCALAPPDATA%\potluck -- is left alone: it is the owner's, and a lost CA key means
# enrolling every board again.
$ErrorActionPreference = "Stop"
$dest = Join-Path $env:LOCALAPPDATA "Programs\Potluck"
Get-Process potluck-tray, potluck -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$dest*" } | Stop-Process
foreach ($dir in @([Environment]::GetFolderPath("Programs"), [Environment]::GetFolderPath("Startup"))) {
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $dir "Potluck.lnk")
}
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Write-Host "Potluck removed; your cell's files in $env:LOCALAPPDATA\potluck are kept."
