#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Typing a search, recorded and traced, so two builds can be compared on the same Simulator:
#
#   tools/ios/search-smoothness.sh <Simulator UDID> <MediaViewer.app> <out folder> [template]
#
# Installs the app over the one there (its library stays), runs UITests/SearchSmoothnessTests, and
# while it types records the screen (<out>/typing.mp4) and an Instruments trace of the app
# (<out>/trace.trace; template "Animation Hitches" unless named, e.g. "+SwiftUI,Hangs" in the
# Simulator, where the hitch instruments do not run; "none" for no trace). Also writes the time from
# each query's last key to its answer (<out>/latency.txt) and the app's memory after typing
# (<out>/footprint.txt). The sample
# library comes from the GitHub release unless MV_SAMPLE_BASE names another. Build the tests once
# into <out>/../dd-tests with MV_BUILD_TESTS=1; later runs reuse them.
set -euo pipefail
udid="${1:?udid}"; app="${2:?app}"; out="${3:?out}"; template="${4:-Animation Hitches}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
bundle=io.github.longtimeno-c.mediaviewer.mobile
mkdir -p "$out"; out="$(cd "$out" && pwd -P)"
dd="$(dirname "$out")/dd-tests"
rm -f "$out/ready" "$out/done"; rm -rf "$out/trace.trace" "$out/typing.mp4"

xcrun simctl terminate "$udid" "$bundle" 2>/dev/null || true
xcrun simctl install "$udid" "$app"
cd "$root/src.ios"
if [[ ${MV_BUILD_TESTS:-} == 1 || ! -d $dd/Build/Products ]]; then
  xcodebuild build-for-testing -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures \
    -destination "platform=iOS Simulator,id=$udid" -derivedDataPath "$dd" CODE_SIGNING_ALLOWED=NO > "$out/build-tests.txt" 2>&1
fi
TEST_RUNNER_MV_SEARCH_SMOOTHNESS=1 TEST_RUNNER_MV_SEARCH_OUT="$out" TEST_RUNNER_MV_SAMPLE_BASE="${MV_SAMPLE_BASE:-}" \
  xcodebuild test-without-building -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures \
  -destination "platform=iOS Simulator,id=$udid" -derivedDataPath "$dd" \
  -only-testing:MediaViewerUITests/SearchSmoothnessTests > "$out/xcodebuild.txt" 2>&1 &
tests=$!

for _ in $(seq 1 1200); do [[ -f $out/ready ]] && break; kill -0 $tests 2>/dev/null || break; sleep 0.5; done
[[ -f $out/ready ]] || { echo "the test never got ready: $out/xcodebuild.txt" >&2; exit 1; }
xcrun simctl io "$udid" recordVideo --codec=h264 --force "$out/typing.mp4" > "$out/record.log" 2>&1 &
video=$!
# A template, or "+Name,Name" for single instruments: the Simulator has no Hitches instrument, and
# a template that carries one (Animation Hitches, SwiftUI) saves a malformed trace there.
what=(--template "$template")
if [[ $template == +* ]]; then
  what=(); IFS=, read -ra names <<< "${template#+}"
  for name in "${names[@]}"; do what+=(--instrument "$name"); done
fi
trace=
if [[ $template != none ]]; then  # "none": the recording, latency and memory alone
  xcrun xctrace record "${what[@]}" --device "$udid" --attach MediaViewer \
    --output "$out/trace.trace" --time-limit 60s > "$out/xctrace.txt" 2>&1 &
  trace=$!
fi
for _ in $(seq 1 1200); do [[ -f $out/done ]] && break; kill -0 $tests 2>/dev/null || break; sleep 0.5; done
# The app's memory once the typing is done (footprint's phys_footprint, as Xcode's gauge shows).
pid="$(pgrep -f 'MediaViewer.app/MediaViewer$' | head -1 || true)"
[[ -n $pid ]] && footprint -p "$pid" > "$out/footprint.txt" 2>&1 || true
# simctl's recorder, not the xcrun in front of it, must get the signal to write the movie out.
pkill -INT -f "simctl io $udid recordVideo" 2>/dev/null || true
kill -INT $trace 2>/dev/null || true
wait $video 2>/dev/null || true
[[ -z $trace ]] || wait $trace 2>/dev/null || true
wait $tests || { grep -E 'error|failed' "$out/xcodebuild.txt" | head; exit 1; }
echo "$out/typing.mp4 $out/trace.trace"
