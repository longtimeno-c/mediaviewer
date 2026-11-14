#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every screen on the smallest phone at the largest text size (UITests/SmallScreenTests):
#   tools/ios/small-screen.sh <out folder> [device type]
# Creates its own Simulator (iPhone SE (3rd generation) where the runtime has it, else the newest
# "e" model) and deletes it afterwards. Screenshots land in <out>.
set -euo pipefail
out="${1:?out}"; mkdir -p "$out"; out="$(cd "$out" && pwd)"
root="$(cd "$(dirname "$0")/../.." && pwd)"
runtime="$(xcrun simctl list runtimes -j | /usr/bin/python3 -I -c 'import json,sys; print([r["identifier"] for r in json.load(sys.stdin)["runtimes"] if r["platform"]=="iOS" and r["isAvailable"]][-1])')"
types=("${2:-com.apple.CoreSimulator.SimDeviceType.iPhone-SE-3rd-generation}" com.apple.CoreSimulator.SimDeviceType.iPhone-17e com.apple.CoreSimulator.SimDeviceType.iPhone-16e)
udid=""
for t in "${types[@]}"; do udid="$(xcrun simctl create "mv-small-$$" "$t" "$runtime" 2>/dev/null)" && { echo "device: $t"; break; }; done
[[ -n $udid ]] || { echo "no small device type for $runtime"; exit 2; }
trap 'xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true; xcrun simctl delete "$udid" >/dev/null 2>&1 || true' EXIT
xcrun simctl boot "$udid"; xcrun simctl bootstatus "$udid" -b >/dev/null
xcrun simctl ui "$udid" content_size accessibility-extra-extra-extra-large
cd "$root/src.ios"
TEST_RUNNER_MV_SMALL_OUT="$out" xcodebuild test -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures \
  -destination "platform=iOS Simulator,id=$udid" -derivedDataPath "$root/build-xcode-small" CODE_SIGNING_ALLOWED=NO \
  -only-testing:MediaViewerUITests/SmallScreenTests 2>&1 | grep -E 'error:|Test Case|\*\* TEST|SmallScreen'
