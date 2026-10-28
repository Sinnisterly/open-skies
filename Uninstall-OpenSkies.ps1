param([string]$Game = "E:\SteamLibrary\steamapps\common\No Man's Sky")
$ErrorActionPreference = 'Stop'
$bin = Join-Path $Game 'Binaries'
if (Get-Process NMS -ErrorAction SilentlyContinue) { throw "Close No Man's Sky first" }
Remove-Item (Join-Path $bin 'version.dll') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $bin 'version.dll.bak')) {
    Move-Item (Join-Path $bin 'version.dll.bak') (Join-Path $bin 'version.dll')
    Write-Host "Restored the previous version.dll"
}
Write-Host "Open Skies removed. Settings and logs (OpenSkies.*) were left in $bin."
