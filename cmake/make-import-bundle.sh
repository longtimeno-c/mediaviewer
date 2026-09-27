#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Wraps an add-on chrome's SwiftPM dynamic library into a loadable bundle
# (plan/18 "Mac chrome": loaded with NSBundle, reached through its principal
# class). Milestone G's Import.bundle, and Milestone H's AI.bundle (plan/17).
#
# Usage: make-import-bundle.sh <swift build path> <bundle path> <version>
#            [<product> <executable> <bundle identifier> <principal class>]
#
# The four optional arguments default to Import's (ImportChrome, Import,
# org.mediaviewer.addon.import, MVImportChrome), so the three-argument call
# darwin.cmake has always made produces exactly the same Import.bundle.
# The Swift build layout differs between toolchains (see collect-swift-chrome.sh),
# so the dylib is found, not assumed.
set -eu
build="$1"
bundle="$2"
version="$3"
product="${4:-ImportChrome}"
exe="${5:-Import}"
identifier="${6:-org.mediaviewer.addon.import}"
principal="${7:-MVImportChrome}"
# Not the DWARF companion in lib<product>.dylib.dSYM, which has the same
# name: install_name_tool rejects it ("string table not at the end of the
# file"), and it is not a loadable library.
lib=$(find "$build" -name "lib${product}.dylib" -type f -not -path '*.dSYM/*' | head -n 1)
if [ -z "$lib" ]; then
  echo "make-import-bundle: lib${product}.dylib not found under $build" >&2
  exit 1
fi
rm -rf "$bundle"
mkdir -p "$bundle/Contents/MacOS"
cp "$lib" "$bundle/Contents/MacOS/$exe"
install_name_tool -id "@rpath/$exe" "$bundle/Contents/MacOS/$exe"
cat > "$bundle/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>${identifier}</string>
  <key>CFBundleName</key><string>${exe}</string>
  <key>CFBundleExecutable</key><string>${exe}</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>${version}</string>
  <key>CFBundleVersion</key><string>${version}</string>
  <key>NSPrincipalClass</key><string>${principal}</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
</dict>
</plist>
PLIST
