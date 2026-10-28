param(
    [string]$Game = "E:\SteamLibrary\steamapps\common\No Man's Sky",
    [string]$Dll = "$PSScriptRoot\build\Release\version.dll"
)
$ErrorActionPreference = 'Stop'
$target = Join-Path $Game 'Binaries\version.dll'
if (-not (Test-Path $Dll)) { throw "Build first: $Dll not found" }
if (Get-Process NMS -ErrorAction SilentlyContinue) { throw "Close No Man's Sky first" }

# Keep any version.dll that is not ours (ours is over 1 MB, other proxies are small).
if ((Test-Path $target) -and -not (Test-Path "$target.bak") -and (Get-Item $target).Length -lt 1MB) {
    Copy-Item $target "$target.bak"
    Write-Host "Backed up existing version.dll to version.dll.bak"
}
Copy-Item $Dll $target -Force

Write-Host "Installed Open Skies to $target"
Write-Host "Start the game and press F8."
