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
#
# Phone backup (1.1, §10b): Photos is granted and filled with generated pictures (never anyone's own
# photos: solid colours made here), the test backs them up to "Sample NAS on this iPhone", and the
# script checks that folder holds each picture once, whole, before Forget, and nothing after it.
set -euo pipefail
work="${1:?work}"; udid="${2:?udid}"; app="${3:?app}"; out="${4:?out}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
bundle=io.github.longtimeno-c.mediaviewer.mobile
port=18470
mkdir -p "$out"; out="$(cd "$out" && pwd -P)"; work="$(cd "$work" && pwd -P)"
[[ -f $work/release/files.json ]] || { echo "no $work/release: run tools/ios/sample-library.sh first" >&2; exit 2; }

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
# Generated pictures for backup: 12 JPEGs of plain colour, each a few hundred KB of noise-free pixels.
media="$out/generated-photos"; rm -rf "$media"; mkdir -p "$media"
python3 -I - "$media" <<'PY'
import os, struct, sys, zlib, random
out = sys.argv[1]
for i in range(12):
    w, h = 1600, 1200
    r, g, b = (i * 40) % 256, (i * 90) % 256, (i * 150) % 256
    row = b"\x00" + bytes([r, g, b]) * w
    raw = zlib.compress(row * h, 6)
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", raw) + chunk(b"IEND", b"")
    open(os.path.join(out, f"MV_TEST_{i:02d}.png"), "wb").write(png)
PY
for f in "$media"/*.png; do sips -s format jpeg "$f" --out "${f%.png}.jpg" >/dev/null && rm "$f"; done
xcrun simctl addmedia "$udid" "$media"/*.jpg
xcrun simctl privacy "$udid" grant photos "$bundle"
cd "$root/src.ios"
run_tests() {
  TEST_RUNNER_MV_SAMPLE_BASE="$base" TEST_RUNNER_MV_SAMPLE_OUT="$out" \
    xcodebuild test -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -destination "platform=iOS Simulator,id=$udid" \
    -derivedDataPath "$out/dd-tests" CODE_SIGNING_ALLOWED=NO "$@" \
    | tee -a "$out/xcodebuild.txt" | grep -E 'error:|Test Case|\*\* TEST' || true
}
rm -f "$out/xcodebuild.txt"
run_tests -only-testing:MediaViewerUITests/SampleLibraryTests -skip-testing:MediaViewerUITests/SampleLibraryTests/test5_ForgetRemovesIt

data="$(xcrun simctl get_app_container "$udid" "$bundle" data)"
# The sample backup folder: every file whole (each generated picture's bytes appear once), no unfinished copy.
python3 -I - "$data/Library/Application Support/backup/sample-nas" "$media" "$out/backup-line.txt" <<'PY' || backup_failed=1
import hashlib, os, sys
nas, media, line = sys.argv[1:4]
files = {}
parts = 0
for r, ds, fs in os.walk(nas):
    ds[:] = [d for d in ds if d != ".mediaviewer-phone-backup"]
    for f in fs:
        if f.endswith(".mvpart"): parts += 1; continue
        files[os.path.relpath(os.path.join(r, f), nas)] = hashlib.sha256(open(os.path.join(r, f), "rb").read()).hexdigest()
ledger = os.path.isfile(os.path.join(nas, ".mediaviewer-phone-backup", "0000000000000000", "ledger.json"))
print(f"sample backup: {len(files)} files, {parts} unfinished, ledger copy {'there' if ledger else 'MISSING'}")
print("screen said:", open(line).read().strip() if os.path.exists(line) else "(no line)")
dups = len(files) - len(set(files.values()))
print("duplicates:", dups)
sys.exit(1 if not files or parts or dups or not ledger else 0)
PY
run_tests -only-testing:MediaViewerUITests/SampleLibraryTests/test5_ForgetRemovesIt
if [[ -d "$data/Library/Application Support/backup/sample-nas" ]]; then echo "after Forget: the sample backup folder is still there"; backup_failed=1; else echo "after Forget: no sample backup folder"; fi
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
exit "${backup_failed:-0}"
