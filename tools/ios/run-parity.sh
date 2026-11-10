#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# docs/plans/mobile.md §9: the phone's engine in a Simulator of its own against the desktop's
# engine, on one generation, row for row. Numbers printed are Simulator numbers, not phone numbers.
#
#   tools/ios/run-parity.sh <gen dir> <library dir> <pack dir> <add-ons dir> <desktop binary> <sim binary> <query files...> -- <work dir>
#
# Keeps nothing it did not make: the Simulator is created here and deleted on the way out.
set -euo pipefail

gen="${1:?}"; library="${2:?}"; pack="${3:?}"; addons="${4:?}"; desktop="${5:?}"; sim="${6:?}"
shift 6
queries=()
while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do queries+=("$1"); shift; done
shift
work="${1:?}"
cat "${queries[@]}" > "$work/queries.txt"
for run in desktop-run desktop-run2; do
  mkdir -p "$work/$run"
  ln -sfn "$(dirname "$pack")/data/fetched" "$work/$run/fetched"
done
kinds=3  # photos and clips: what a phone shows (the plan's "the same rows")

udid="$(xcrun simctl create "mv-parity-$$" "$(xcrun simctl list devicetypes -j | /usr/bin/python3 -I -c 'import json,sys; print([d["identifier"] for d in json.load(sys.stdin)["devicetypes"] if "iPhone" in d["name"] and "Pro" in d["name"]][-1])')" "$(xcrun simctl list runtimes -j | /usr/bin/python3 -I -c 'import json,sys; print([r["identifier"] for r in json.load(sys.stdin)["runtimes"] if r["platform"]=="iOS" and r["isAvailable"]][-1])')")"
trap 'xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true; xcrun simctl delete "$udid" >/dev/null 2>&1 || true' EXIT
xcrun simctl bootstatus "$udid" -b >/dev/null

echo "== desktop engine"
"$desktop" search "$gen" "$pack" "$addons" "$work/desktop-run" "$kinds" "$work/queries.txt" "$work/desktop.jsonl"
echo "== desktop engine, second run (ties and map order)"
"$desktop" search "$gen" "$pack" "$addons" "$work/desktop-run2" "$kinds" "$work/queries.txt" "$work/desktop2.jsonl" || true
echo "== the phone's engine, in the Simulator"
xcrun simctl spawn "$udid" "$sim" "$gen" "$library" "$work/phone-work" "$kinds" "$work/queries.txt" "$work/phone.jsonl"
echo "== compare"
/usr/bin/python3 -I "$(dirname "$0")/compare_parity.py" "$work/desktop.jsonl" "$work/phone.jsonl"
