#!/usr/bin/env bash
# One-time setup after creating the repository from these files:
#   - adds iPlug2 and ysfx as submodules, pinned to tested commits
#   - fetches ysfx's dr_libs (its only third-party code we compile)
#   - downloads the VST3 SDK into iPlug2
# Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")/.."

IPLUG2_URL=https://github.com/iPlug2/iPlug2.git
IPLUG2_SHA=d54f69050f517e43b941d88c2a170f0a840b9ee4   # 2026-08-19
YSFX_URL=https://github.com/JoepVanlier/ysfx.git
YSFX_SHA=5c3452fee62583aa3d1b7e877d0c758c4024af89     # 2026-08-19

[ -d .git ] || git init -b main

add_pinned() { # url path sha
  if [ ! -e "$2/.git" ]; then
    git submodule add --depth 1 "$1" "$2" || git submodule update --init --depth 1 "$2"
  fi
  git -C "$2" fetch --quiet --depth 1 origin "$3"
  git -C "$2" checkout --quiet "$3"
  git add "$2"
}

add_pinned "$IPLUG2_URL" iPlug2 "$IPLUG2_SHA"
add_pinned "$YSFX_URL" third_party/ysfx "$YSFX_SHA"
git -C third_party/ysfx submodule update --init --depth 1 thirdparty/dr_libs

if [ ! -d iPlug2/Dependencies/IPlug/VST3_SDK/pluginterfaces ]; then
  (cd iPlug2/Dependencies/IPlug && ./download-vst3-sdk.sh)
fi

echo
echo "Ready. Next:"
echo "  macOS  : cmake --preset macos   && cmake --build --preset macos"
echo "  Windows: cmake --preset windows && cmake --build --preset windows"
echo "  then commit: git add -A && git commit -m 'JSFX plugin framework'"
