#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs tools/ios/core_smoke.cpp in a Simulator of its own, so a Simulator
# someone is using is never touched, and deletes it afterwards.
#
#   tools/ios/run-simulator-smoke.sh build-ios-sim/bin/mv_ios_core_smoke
set -euo pipefail

binary="${1:?usage: run-simulator-smoke.sh <mv_ios_core_smoke built for the Simulator>}"
read -r runtime devicetype <<<"$(xcrun simctl list runtimes -j | /usr/bin/python3 -I -c '
import json, sys
ios = [r for r in json.load(sys.stdin)["runtimes"] if r["platform"] == "iOS" and r["isAvailable"]][-1]
phones = [d for d in ios["supportedDeviceTypes"] if d["productFamily"] == "iPhone"]
print(ios["identifier"], phones[-1]["identifier"])')"

udid="$(xcrun simctl create "mv-core-smoke-$$" "$devicetype" "$runtime")"
trap 'xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true; xcrun simctl delete "$udid" >/dev/null 2>&1 || true' EXIT
xcrun simctl bootstatus "$udid" -b >/dev/null
xcrun simctl spawn "$udid" "$binary"
