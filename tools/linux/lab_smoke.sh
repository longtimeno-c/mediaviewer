#!/usr/bin/env bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Linux PR 1's headless smoke: the present lab under a headless Weston with
# Mesa's software rasteriser. It proves plumbing, never pacing. Headless
# Weston answers every commit with flags 0 and seq 0 and repaints at about
# 26 ms against an advertised 16.67, so the lab must run, report, label its
# source weaker and fail the gate; a pass here would be the bug.
#
#   tools/linux/lab_smoke.sh BUILD/bin/mediaviewer_lab BUILD/bin/frametime \
#     [BUILD/libmv_egl_swap_fault.so]
#
# With the third argument (tests/linux/egl_swap_fault.cpp) it also fails one
# eglSwapBuffers mid-soak and checks the lab rebuilds and goes on presenting.
# MV_WESTON_RENDERER (gl | pixman, default gl) and MV_SMOKE_SECONDS (default
# 10) tune it. Exit 0 when every check holds, 1 when one fails, 77 (ctest's
# skip) when there is no weston.
set -euo pipefail

lab=${1:?usage: lab_smoke.sh MEDIAVIEWER_LAB FRAMETIME}
frametime=${2:?usage: lab_smoke.sh MEDIAVIEWER_LAB FRAMETIME}
swap_fault=${3:-}
renderer=${MV_WESTON_RENDERER:-gl}
seconds=${MV_SMOKE_SECONDS:-10}

if ! command -v weston >/dev/null 2>&1; then
  echo "lab_smoke: no weston; skipped"
  exit 77
fi

runtime=$(mktemp -d)
chmod 700 "$runtime"
socket="mv-smoke-$$"
weston_pid=
cleanup() {
  if [ -n "$weston_pid" ]; then kill "$weston_pid" 2>/dev/null || true; wait "$weston_pid" 2>/dev/null || true; fi
  rm -rf "$runtime"
}
trap cleanup EXIT

XDG_RUNTIME_DIR="$runtime" weston --backend=headless --renderer="$renderer" \
  --socket="$socket" --idle-time=0 --no-config >"$runtime/weston.log" 2>&1 &
weston_pid=$!
for _ in $(seq 1 300); do
  [ -S "$runtime/$socket" ] && break
  if ! kill -0 "$weston_pid" 2>/dev/null; then
    echo "lab_smoke: weston exited"; cat "$runtime/weston.log"; exit 1
  fi
  sleep 0.1
done
if [ ! -S "$runtime/$socket" ]; then
  echo "lab_smoke: weston did not open its socket"; cat "$runtime/weston.log"; exit 1
fi

export XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY="$socket"
status=0
fail() { echo "FAIL: $*"; status=1; }

# frametime runs the animated and the idle soak and parses both reports with
# frametime::parse: 2 means a report was missing, malformed or the lab could
# not run; 0 would mean headless Weston passed Linux PR 1.
set +e
"$frametime" --seconds "$seconds" --lab "$lab" --baseline "$runtime/no-baseline.json" \
  >"$runtime/frametime.log" 2>&1
code=$?
set -e
sed -e '/^libEGL warning/d' -e '/^MESA: error/d' -e '/^$/d' "$runtime/frametime.log"
[ "$code" -eq 1 ] || fail "frametime exited $code; expected 1 (ran, reported, gate failed)"

dir=$(dirname "$frametime")
python3 -I - "$dir/frametime-report.json" "$dir/frametime-idle-report.json" "$seconds" <<'EOF' || status=1
import json, sys
animated = json.load(open(sys.argv[1]))
idle = json.load(open(sys.argv[2]))
seconds = float(sys.argv[3])
problems = []
def check(ok, what):
    if not ok:
        problems.append(what)
check(animated["static"] is False, "animated report is not the animated soak")
check(animated["frames"] > 0, "animated soak presented nothing")
check(animated["measurement_complete"] is True, "animated soak did not complete")
check(animated["drop_source"] == "clock intervals (weaker)",
      "animated source is %r, not the weaker label" % animated["drop_source"])
check(animated["meets_pr1_gate"] is False, "headless Weston passed the gate")
check(animated["displayed_presents"] == 0, "flags-0 feedback counted as displayed")
check(animated["statistics_unavailable_frames"] > 0, "no frame was left unjudged")
check(animated["elapsed_seconds"] >= seconds * 0.95, "animated soak was short")
check(idle["static"] is True, "idle report is not the static soak")
check(idle["idle_presents"] == 0, "the idle soak presented %d frames" % idle["idle_presents"])
check(idle["idle_input_events"] == 0, "the idle soak saw input")
check(0.0 <= idle["idle_cpu_percent"] < 1.0,
      "the idle soak used %.3f %% of a core" % idle["idle_cpu_percent"])
check(idle["meets_pr1_gate"] is False or seconds >= 60.0, "a short idle soak passed")
for p in problems:
    print("FAIL:", p)
print("animated: %d frames, mean %.2f ms, p99 %.2f ms, source %s; idle: %d presents, %.4f %% CPU"
      % (animated["frames"], animated["mean_ms"], animated["p99_ms"], animated["drop_source"],
         idle["idle_presents"], idle["idle_cpu_percent"]))
sys.exit(1 if problems else 0)
EOF

# A swap that fails as a lost surface does, half a second into the measured
# window: the lab rebuilds the device and goes on presenting. The request that
# swap never committed must be forgotten, or the loop waits for ever on a frame
# callback nothing will send. The rate to expect is the animated soak's.
if [ -n "$swap_fault" ]; then
  read -r rate fail_at < <(python3 -I -c '
import json, sys
r = json.load(open(sys.argv[1]))
rate = r["frames"] / r["elapsed_seconds"] if r["elapsed_seconds"] > 0 else 0.0
print("%.3f %d" % (rate, max(2, int(rate * 1.5))))' "$dir/frametime-report.json")
  set +e
  MV_FAIL_SWAP_AT="$fail_at" LD_PRELOAD="$swap_fault" \
    ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}verify_asan_link_order=0" \
    "$lab" --soak 3 --json "$runtime/swap-fault.json" >"$runtime/swap-fault.log" 2>&1
  code=$?
  set -e
  [ "$code" -eq 0 ] || { fail "the swap-fault soak exited $code; expected 0"; cat "$runtime/swap-fault.log"; }
  grep -q "egl_swap_fault: failing swap $fail_at" "$runtime/swap-fault.log" ||
    fail "the swap-fault module never failed a swap (not preloaded?)"
  python3 -I - "$runtime/swap-fault.json" "$rate" <<'EOF' || status=1
import json, sys
r = json.load(open(sys.argv[1]))
rate = float(sys.argv[2])
expected = rate * r["elapsed_seconds"]
problems = []
if r["frames"] < expected * 0.5:
    problems.append("after a failed swap the lab presented %d frames of about %.0f"
                    % (r["frames"], expected))
if r["measurement_complete"] is not False:
    problems.append("a soak with a device rebuild counted as complete")
for p in problems:
    print("FAIL:", p)
print("swap fault: %d frames over %.1f s after one failed swap (about %.0f expected)"
      % (r["frames"], r["elapsed_seconds"], expected))
sys.exit(1 if problems else 0)
EOF
fi

# The lab refuses to run without a Wayland session rather than guessing.
set +e
env -u WAYLAND_DISPLAY "$lab" --soak 1 >/dev/null 2>&1
code=$?
set -e
[ "$code" -eq 2 ] || fail "the lab without WAYLAND_DISPLAY exited $code; expected 2"
set +e
"$lab" --soak 0 >/dev/null 2>&1
code=$?
set -e
[ "$code" -eq 2 ] || fail "--soak 0 exited $code; expected 2"

if [ "$status" -eq 0 ]; then echo "lab_smoke ($renderer): PASS"; fi
exit "$status"
