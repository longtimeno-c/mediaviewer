#!/bin/sh
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# Issue #72 spike: builds tools/ai/photos_spike_mac.mm with the add-on's Photos
# source into a throwaway, ad-hoc-signed .app (PhotoKit's permission prompt
# needs a bundle with NSPhotoLibraryUsageDescription), runs it and prints the
# report. The first run shows the system Photos prompt for "MV Photos Spike";
# Allow it. Counts and timings only: no identifier, name or path is printed.
#
#   tools/ai/photos-spike.sh [--stills 1000] [--videos 20] [--edge 448] [--threads 2]
set -eu
repo=$(cd "$(dirname "$0")/../.." && pwd)
work=${TMPDIR:-/tmp}/mv-photos-spike
app="$work/MV Photos Spike.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>io.github.longtimeno-c.mediaviewer.photos-spike</string>
  <key>CFBundleExecutable</key><string>spike</string>
  <key>CFBundleName</key><string>MV Photos Spike</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSUIElement</key><true/>
  <key>NSPhotoLibraryUsageDescription</key>
  <string>Measures how fast MediaViewer's Local search could read your Photos library. Nothing is changed or sent anywhere.</string>
</dict></plist>
PLIST
xcrun clang++ -std=c++20 -O2 -fobjc-arc -fno-rtti -Wall -Wextra -Werror \
  -I "$repo/src" -I "$repo/src/abi/include" \
  "$repo/tools/ai/photos_spike_mac.mm" "$repo/src/addons/ai/photos_mac.mm" \
  -framework Foundation -framework AppKit -framework Photos -framework AVFoundation \
  -o "$app/Contents/MacOS/spike"
codesign --force --sign - "$app"
out="$work/report.json"
rm -f "$out"
open -W -n "$app" --args --out "$out" "$@"
cat "$out"
