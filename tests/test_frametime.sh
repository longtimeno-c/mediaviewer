#!/usr/bin/env bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# The Linux frametime harness against synthetic reports (tests/test_frametime.ps1's
# twin): a passing pair passes and becomes the baseline byte for byte; every
# failing shape fails and leaves the baseline alone.
set -euo pipefail
harness=${1:?usage: test_frametime.sh FRAMETIME FIXTURE OUTPUT_ROOT}
fixture=${2:?usage: test_frametime.sh FRAMETIME FIXTURE OUTPUT_ROOT}
root=${3:?usage: test_frametime.sh FRAMETIME FIXTURE OUTPUT_ROOT}
mkdir -p "$root"
dir=$(mktemp -d "$root/run.XXXXXX")
trap 'rm -rf "$dir"' EXIT
cp "$harness" "$dir/frametime"
baseline="$dir/baseline.json"
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-fixture}  # the harness refuses without one

MV_FRAMETIME_FIXTURE_MODE=pass "$dir/frametime" --lab "$fixture" --baseline "$baseline" --update-baseline >/dev/null \
  || { echo "a valid pair of reports did not pass"; exit 1; }
cmp -s "$baseline" "$dir/frametime-report.json" || { echo "the baseline differs from the gated report"; exit 1; }
saved=$(cat "$baseline")

for mode in drop short unknown-refresh fast gap busy-idle idle-present child-error child-fail \
            malformed regression weak dxgi; do
  set +e
  MV_FRAMETIME_FIXTURE_MODE=$mode "$dir/frametime" --lab "$fixture" --baseline "$baseline" \
    --update-baseline >/dev/null 2>&1
  code=$?
  set -e
  [ "$code" -ne 0 ] || { echo "the failing $mode run passed"; exit 1; }
  [ "$(cat "$baseline")" = "$saved" ] || { echo "the failing $mode run changed the baseline"; exit 1; }
done

set +e
env -u WAYLAND_DISPLAY MV_FRAMETIME_FIXTURE_MODE=pass "$dir/frametime" --lab "$fixture" >/dev/null 2>&1
code=$?
set -e
[ "$code" -eq 2 ] || { echo "without WAYLAND_DISPLAY the harness exited $code, not 2"; exit 1; }
echo "frametime: success, report identity, failure gates, child exits and baseline protection passed"
