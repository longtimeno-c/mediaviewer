#!/bin/sh
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# ai-bench over the Mac's Photos library (issue #72): wraps <build>/bin/ai-bench
# in a throwaway .app (PhotoKit's prompt needs NSPhotoLibraryUsageDescription),
# runs it with `open` so the permission is the bench's own, and prints its
# output. The first run shows the Photos prompt for "MV AI Bench".
#
#   tools/ai-bench/photos-bench.sh <build dir> --addons <dev add-ons> [--query "a dog"]... [--timeout 3600]
set -eu
build=$1
shift
work=${TMPDIR:-/tmp}/mv-ai-bench-photos
app="$work/MV AI Bench.app"
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleIdentifier</key><string>io.github.longtimeno-c.mediaviewer.ai-bench</string>
  <key>CFBundleExecutable</key><string>ai-bench</string>
  <key>CFBundleName</key><string>MV AI Bench</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSUIElement</key><true/>
  <key>NSPhotoLibraryUsageDescription</key>
  <string>Measures MediaViewer's Local search over your Photos library, on this Mac only. Nothing is changed or sent anywhere.</string>
</dict></plist>
PLIST
cp "$build/bin/ai-bench" "$app/Contents/MacOS/ai-bench"
codesign --force --sign - "$app"
out="$work/out.jsonl"
rm -f "$out"
open -W -n "$app" --env MV_DEV_THUMBS_DIR="$work/thumbs" --stdout "$out" --stderr "$out" --args --photos "$@"
cat "$out"
