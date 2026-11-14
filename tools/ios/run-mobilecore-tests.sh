#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# MobileCore's tests (src.ios/MobilePackage/Tests) in a Simulator of their own, over the C++ core
# built for the Simulator. Deletes the Simulator afterwards.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
export PATH="/opt/homebrew/bin:$HOME/Library/Python/3.9/lib/python/site-packages/cmake/data/bin:$HOME/Library/Python/3.9/bin:$PATH"
PLATFORM_NAME=iphonesimulator "$root/tools/ios/build-core.sh"
ninja -C "$root/build-ios-iphonesimulator" mv_mobile_test_fixture
udid="$(xcrun simctl create "mv-mobilecore-$$" "$(xcrun simctl list devicetypes -j | /usr/bin/python3 -I -c 'import json,sys; print([d["identifier"] for d in json.load(sys.stdin)["devicetypes"] if "iPhone" in d["name"] and "Pro" in d["name"]][-1])')" "$(xcrun simctl list runtimes -j | /usr/bin/python3 -I -c 'import json,sys; print([r["identifier"] for r in json.load(sys.stdin)["runtimes"] if r["platform"]=="iOS" and r["isAvailable"]][-1])')")"
trap 'xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true; xcrun simctl delete "$udid" >/dev/null 2>&1 || true' EXIT
cd "$root/src.ios/MobilePackage"
xcodebuild test -scheme MobilePackage-Package -destination "platform=iOS Simulator,id=$udid" \
  -derivedDataPath "$root/build-xcode-mobilecore" -only-testing:MobileCoreTests CODE_SIGNING_ALLOWED=NO "$@"
