# Fetches the official SDL3 development package (MSVC) into ..\third_party\SDL3
# Usage:  powershell -ExecutionPolicy Bypass -File fetch_sdl3.ps1 [version]
param([string]$Version = "3.4.18")

$ErrorActionPreference = "Stop"
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$portdir = Split-Path -Parent $here
$dest    = Join-Path $portdir "third_party\SDL3"
$zip     = Join-Path $env:TEMP "SDL3-devel-$Version-VC.zip"
$url     = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/SDL3-devel-$Version-VC.zip"

if (Test-Path (Join-Path $dest "include\SDL3\SDL.h")) {
    Write-Host "SDL3 already present in $dest"
    exit 0
}

Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip
Write-Host "Extracting to $dest"
$tmp = Join-Path $env:TEMP "sdl3-extract"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive -Path $zip -DestinationPath $tmp -Force

$inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Recurse -Force (Join-Path $inner.FullName "*") $dest
Remove-Item -Recurse -Force $tmp
Remove-Item -Force $zip

Write-Host "SDL3 ready in $dest"
Get-ChildItem (Join-Path $dest "lib\x64") | Select-Object Name
