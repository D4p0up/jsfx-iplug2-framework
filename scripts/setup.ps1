# One-time setup (Windows / PowerShell). See setup.sh for what it does.
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$IPLUG2_URL = "https://github.com/iPlug2/iPlug2.git"
$IPLUG2_SHA = "d54f69050f517e43b941d88c2a170f0a840b9ee4"   # 2026-08-19
$YSFX_URL   = "https://github.com/JoepVanlier/ysfx.git"
$YSFX_SHA   = "5c3452fee62583aa3d1b7e877d0c758c4024af89"   # 2026-08-19

if (-not (Test-Path .git)) { git init -b main }

function Add-Pinned($url, $path, $sha) {
  if (-not (Test-Path "$path/.git")) {
    git submodule add --depth 1 $url $path
    if ($LASTEXITCODE -ne 0) { git submodule update --init --depth 1 $path }
  }
  git -C $path fetch --quiet --depth 1 origin $sha
  git -C $path checkout --quiet $sha
  git add $path
}

Add-Pinned $IPLUG2_URL "iPlug2" $IPLUG2_SHA
Add-Pinned $YSFX_URL "third_party/ysfx" $YSFX_SHA
git -C third_party/ysfx submodule update --init --depth 1 thirdparty/dr_libs

if (-not (Test-Path "iPlug2/Dependencies/IPlug/VST3_SDK/pluginterfaces")) {
  Push-Location iPlug2/Dependencies/IPlug
  bash ./download-vst3-sdk.sh    # Git Bash ships with Git for Windows
  Pop-Location
}

Write-Host ""
Write-Host "Ready. Next: cmake --preset windows ; cmake --build --preset windows"
