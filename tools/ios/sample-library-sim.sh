#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sample library in the Simulator, as App Review meets it (docs/plans/mobile.md §11.1):
#
#   tools/ios/sample-library-sim.sh <work folder of sample-library.sh> <Simulator UDID> <MediaViewer.app> <out folder>
#
# Serves <work>/release on 127.0.0.1 with ranges, as GitHub's CDN answers, and points a Debug build
# at it (MV_SAMPLE_BASE); nothing else is reachable from the app's sample transport. Then runs
# UITests/SampleLibraryTests: a wrong code, SAMPLE, the copy, the grid, six searches, a photo, a
# clip, Forget. Afterwards checks that each search's first answers are pictures tagged for it in
# sources.json, that Forget left nothing of the library in the app's container, and that no request
# carried anything but a fixed user agent, the range and the usual transport headers.
# MV_SAMPLE_FILE=1 serves it as a file:// folder instead of over loopback HTTP.
set -euo pipefail
work="${1:?work}"; udid="${2:?udid}"; app="${3:?app}"; out="${4:?out}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
bundle=io.github.longtimeno-c.mediaviewer.mobile
port=18470
mkdir -p "$out"; out="$(cd "$out" && pwd -P)"; work="$(cd "$work" && pwd -P)"
[[ -f $work/release/files.sig ]] || { echo "no $work/release: run tools/ios/sample-library.sh first" >&2; exit 2; }

if [[ ${MV_SAMPLE_FILE:-} == 1 ]]; then
  base="file://$work/release/"
else
  rm -f "$out/headers.jsonl"
  MV_SAMPLE_HEADERS="$out/headers.jsonl" python3 -I "$root/tools/ios/sample_library.py" serve "$work/release" "$port" > "$out/serve.log" 2>&1 &
  server=$!
  trap 'kill $server 2>/dev/null || true' EXIT
  base="http://127.0.0.1:$port/"
  sleep 1
fi

xcrun simctl terminate "$udid" "$bundle" 2>/dev/null || true
xcrun simctl uninstall "$udid" "$bundle" 2>/dev/null || true
xcrun simctl keychain "$udid" reset
xcrun simctl install "$udid" "$app"
cd "$root/src.ios"
TEST_RUNNER_MV_SAMPLE_BASE="$base" TEST_RUNNER_MV_SAMPLE_OUT="$out" \
  xcodebuild test -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -destination "platform=iOS Simulator,id=$udid" \
  -derivedDataPath "$out/dd-tests" CODE_SIGNING_ALLOWED=NO -only-testing:MediaViewerUITests/SampleLibraryTests \
  | tee "$out/xcodebuild.txt" | grep -E 'error:|Test Case|\*\* TEST' || true

data="$(xcrun simctl get_app_container "$udid" "$bundle" data)"
python3 -I - "$root/tools/ios/sample-library/sources.json" "$work/release" "$out/answers.json" "$data" "$out/headers.jsonl" <<'PY'
import json, os, sqlite3, sys
sources, release, answers, data, headers = sys.argv[1:6]
tags = {}
for e in json.load(open(sources))["items"]:
    t = set(e["tags"]) | {w for w in e["title"].lower().replace(",", " ").split()}
    tags[e["file"]] = t
gen = sorted(n for n in os.listdir(release) if n.endswith("__catalog.db"))[-1]
db = sqlite3.connect(f"file:{release}/{gen}?mode=ro", uri=True)
path = {str(i): p.split("/", 2)[2] for i, p in db.execute("SELECT id, path FROM assets")}
# What each query's first answers must be about: any of these words in the picture's tags or title.
want = {"dog on a beach": {"dog"}, "waterfall": {"waterfall", "falls", "creek", "stream"},
        "bridge at night": {"bridge"}, "breakfast": {"food", "breakfast", "pancakes", "coffee"},
        "snowy mountain": {"mountain", "snow", "snow-capped", "peak", "peaks"},
        "lake reflection": {"lake", "reflection", "mirrored"}}
failed = 0
for a in json.load(open(answers)):
    files = [path.get(t, "?") for t in a["tiles"]]
    top = files[:3]
    ok = bool(top) and all(want[a["query"]] & tags.get(f, set()) for f in top)
    failed += not ok
    print(("ok  " if ok else "BAD ") + a["query"].ljust(16), " | ".join(top))
lib = os.path.join(data, "Library/Application Support/library")
left = [os.path.join(r, f) for r, _, fs in os.walk(lib) for f in fs] if os.path.isdir(lib) else []
thumbs = os.path.join(data, "Library/Caches/thumbs")
left += [os.path.join(r, f) for r, _, fs in os.walk(thumbs) for f in fs] if os.path.isdir(thumbs) else []
print("after Forget:", "nothing left" if not left else f"{len(left)} files left, e.g. {left[0]}")
# Nothing about the person in a request: a fixed user agent, any language, no cookies.
sent = [json.loads(l) for l in open(headers)] if os.path.exists(headers) else []
told = {(k.lower(), v) for h in sent for k, v in h.items() if k.lower() not in ("host", "range", "accept", "accept-encoding", "connection", "cache-control", "pragma")}
odd = told - {("user-agent", "MediaViewer"), ("accept-language", "*")}
print(f"headers over {len(sent)} requests:", sorted(told) if not odd else f"UNEXPECTED {sorted(odd)}")
sys.exit(1 if failed or left or odd or (os.path.exists(headers) and not sent) else 0)
PY
