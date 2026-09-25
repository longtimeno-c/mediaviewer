#!/bin/sh
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Wraps src.swift/ImportChrome's dynamic library into Import.bundle
# (plan/18 "Mac chrome": loaded with NSBundle, principal class MVImportChrome).
# Usage: make-import-bundle.sh <swift build path> <bundle path> <version>
# The Swift build layout differs between toolchains (see collect-swift-chrome.sh),
# so the dylib is found, not assumed.
set -eu
build="$1"
bundle="$2"
version="$3"
# Not the DWARF companion in libImportChrome.dylib.dSYM, which has the same
# name: install_name_tool rejects it ("string table not at the end of the
# file"), and it is not a loadable library.
lib=$(find "$build" -name 'libImportChrome.dylib' -type f -not -path '*.dSYM/*' | head -n 1)
if [ -z "$lib" ]; then
  echo "make-import-bundle: libImportChrome.dylib not found under $build" >&2
  exit 1
fi
rm -rf "$bundle"
mkdir -p "$bundle/Contents/MacOS"
cp "$lib" "$bundle/Contents/MacOS/Import"
install_name_tool -id "@rpath/Import" "$bundle/Contents/MacOS/Import"
cat > "$bundle/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>org.mediaviewer.addon.import</string>
  <key>CFBundleName</key><string>Import</string>
  <key>CFBundleExecutable</key><string>Import</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>${version}</string>
  <key>CFBundleVersion</key><string>${version}</string>
  <key>NSPrincipalClass</key><string>MVImportChrome</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
</dict>
</plist>
PLIST
