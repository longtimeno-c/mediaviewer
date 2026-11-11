#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# docs/plans/mobile.md §9, live: the Mac's own publisher, a loopback service, the real app.
#
#   tools/ios/search-live.sh <Local search data folder> <indexed folder> <Simulator UDID> <out folder>
#
# 1. Copies the indexed folder's stills and short clips onto a disk image of its own (the "NAS"),
#    and a copy of the index, moved to point there. The real index is only read.
# 2. Runs the desktop engine's phone_publish (mobile_parity_desktop.mm publish-engine) with the
#    installed AI pack, into the image, then asks that engine every query: the Mac's answers.
# 3. Serves the image with `driftwire serve --test-loopback` on 127.0.0.1 (never the public DHT),
#    pairs the app in the Simulator, copies, and runs each query on the search screen
#    (UITests/LiveSearchTests).
# 4. Compares the tiles the phone showed with the Mac's first rows (compare_live.py).
# Needs build/ (mv_mobile_parity_desktop, mv_driftwire_cli_test) and <out>/MediaViewer.app, built
# for the Simulator ad hoc signed (CODE_SIGN_IDENTITY=- CODE_SIGNING_REQUIRED=NO): an unsigned app
# has no keychain, so pairing cannot store its device key.
# MV_LIVE_REUSE=1 keeps a finished publish (<out>/mac.jsonl) and starts at step 3.
# MV_LIVE_QUERIES and MV_LIVE_MAC: another query list and the Mac's answers to it (answer-engine).
set -euo pipefail
data_src="${1:?data}"; folder="${2:?folder}"; udid="${3:?udid}"; out="${4:?out}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
addons="$(dirname "$(dirname "$data_src")")"
pack="$(ls -d "$addons/AI"/0.* | sort -V | tail -1)"
places="$(ls -d "$addons/Places"/0.* | sort -V | tail -1)/data/places.mvgeo"
desk="$root/build/bin/mv_mobile_parity_desktop"; cli="$root/build/bin/mv_driftwire_cli_test"
port=18462
mkdir -p "$out"; out="$(cd "$out" && pwd)"
mnt="$out/mnt"

# ---- 1. the "NAS" and the index pointing at it ----------------------------------------------
if [[ ! -d $mnt/Photos && -f $out/nas.sparseimage ]]; then mkdir -p "$mnt"; hdiutil attach -quiet -nobrowse -mountpoint "$mnt" "$out/nas.sparseimage"; fi
if [[ ${MV_LIVE_REUSE:-} == 1 && -s $out/mac.jsonl ]]; then echo "reusing the publish in $out"; else
if [[ ! -d $mnt/Photos ]]; then
  [[ -f $out/nas.sparseimage ]] || hdiutil create -quiet -size 16g -type SPARSE -fs APFS -volname MVTestNAS "$out/nas"
  mkdir -p "$mnt"; hdiutil attach -quiet -nobrowse -mountpoint "$mnt" "$out/nas.sparseimage"
fi
rm -rf "$out/data"; mkdir -p "$out/data"
sqlite3 "file:$data_src/index.db?mode=ro" "VACUUM INTO '$out/data/index.db'"
[[ -f $data_src/faces.db ]] && sqlite3 "file:$data_src/faces.db?mode=ro" "VACUUM INTO '$out/data/faces.db'"
cp "$data_src/settings.json" "$out/data/"; [[ -f $data_src/labels.f32 ]] && cp "$data_src/labels.f32" "$out/data/"
# The towers on the CPU, as the phone runs them: no Core ML compile of the picture tower (minutes,
# gigabytes) for a run that embeds no picture. Quality High, as Auto picks on an Apple silicon Mac:
# Auto on the CPU picks Fast, whose Whisper wrote no transcript here.
python3 -I - "$out/data/settings.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1])); s["compute"] = 1; s["quality"] = 2; json.dump(s, open(sys.argv[1], "w"))
PY
python3 -I - "$out/data/index.db" "$folder" "$mnt/Photos/$(basename "$folder")" <<'PY'
import os, shutil, sqlite3, sys
db, src, dst = sys.argv[1:4]
c = sqlite3.connect(db); c.execute("PRAGMA foreign_keys=ON")
rid = c.execute("SELECT id FROM roots WHERE path=?", (src,)).fetchone()[0]
keep = c.execute("SELECT id, path FROM assets WHERE root_id=? AND (kind=1 OR (kind=2 AND size<60000000))", (rid,)).fetchall()
c.execute("DELETE FROM roots WHERE id<>?", (rid,))
c.execute("CREATE TEMP TABLE keep(id INTEGER PRIMARY KEY)"); c.executemany("INSERT INTO keep VALUES(?)", [(i,) for i, _ in keep])
c.execute("DELETE FROM assets WHERE id NOT IN (SELECT id FROM keep)")
n = len(src)
c.execute("UPDATE roots SET path=? WHERE id=?", (dst, rid))
c.execute("UPDATE assets SET path=?||substr(path,?) WHERE root_id=?", (dst, n + 1, rid))
c.execute("UPDATE assets SET motion=?||substr(motion,?) WHERE motion LIKE ?", (dst, n + 1, src + "/%"))
c.commit()
copied = 0
for _, p in keep:
    to = dst + p[n:]
    if not os.path.exists(to):
        os.makedirs(os.path.dirname(to), exist_ok=True); shutil.copy2(p, to); copied += 1
print(f"{len(keep)} assets kept, {copied} files copied")
PY

# ---- 2. the Mac's publisher, and the Mac's answers ------------------------------------------
rm -rf "$out/work" "$mnt/.mediaviewer"
"$desk" publish-engine "$out/data" "$pack" "$addons" "$places" "$out/work" "$mnt" 3 \
  "$root/tools/ios/parity_queries.txt" "$out/mac.jsonl" | tee "$out/publish.txt"
fi
publisher="$(grep publisher_key= "$out/publish.txt" | cut -d= -f2)"
library="$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["library_id"])' "$mnt/.mediaviewer/library.json")"
share="$(python3 -I - "$mnt/.mediaviewer" <<'PY'
import json, os, sys
gen = sorted(os.listdir(sys.argv[1] + "/gen"))[-1]
print(json.load(open(f"{sys.argv[1]}/gen/{gen}/manifest.json"))["roots"][0]["share"])
PY
)"

# ---- 3. the service, the app ---------------------------------------------------------------
rm -rf "$out/serve"; mkdir -p "$out/serve"; chmod 700 "$out/serve"
cat > "$out/serve/serve.json" <<JSON
{"port":$port,"name":"Test NAS","roots":[{"id":"a1a1a1a1a1a1a1a1","label":"$share","path":"$mnt"},{"id":"c2c2c2c2c2c2c2c2","label":"Catalogue","path":"$mnt/.mediaviewer"}]}
JSON
chmod 600 "$out/serve/serve.json"
"$cli" serve --data "$out/serve" --test-loopback init > "$out/init.txt"
code="$("$cli" serve --data "$out/serve" --test-loopback pair-code)"
identity="$(awk '/identity/{print $2}' "$out/init.txt")"; access="$(awk '/access-key/{print $2}' "$out/init.txt")"
payload="$(python3 -I - "$publisher" "$identity" "$access" "$code" "$port" "$library" <<'PY'
import sys, json, base64, time
p, i, a, c, port, library = sys.argv[1:7]
j = {"v": 1, "library_id": library, "library_name": "Test library", "publisher_key": p,
     "service": {"local": f"127.0.0.1:{port}", "public": f"localhost:{port}", "identity": i}, "access_key": a, "code": c,
     "expires": int(time.time()) + 900}
print("mediaviewer://pair#" + base64.urlsafe_b64encode(json.dumps(j).encode()).decode().rstrip("="))
PY
)"
stale="$(lsof -ti tcp:$port -sTCP:LISTEN || true)"; if [[ -n $stale ]]; then kill $stale; sleep 1; fi
"$cli" serve --data "$out/serve" --test-loopback > "$out/serve.log" 2>&1 & echo $! > "$out/serve.pid"
trap 'kill "$(cat "$out/serve.pid")" 2>/dev/null || true' EXIT
sleep 1

bundle=io.github.longtimeno-c.mediaviewer.mobile
xcrun simctl terminate "$udid" "$bundle" 2>/dev/null || true
xcrun simctl uninstall "$udid" "$bundle" 2>/dev/null || true
xcrun simctl keychain "$udid" reset
xcrun simctl install "$udid" "$out/MediaViewer.app"
mkdir -p "$out/shots"
cd "$root/src.ios"
TEST_RUNNER_MV_LIVE_PAYLOAD="$payload" TEST_RUNNER_MV_LIVE_QUERIES="${MV_LIVE_QUERIES:-$out/work/queries.txt}" TEST_RUNNER_MV_LIVE_OUT="$out/shots" \
  xcodebuild test -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -destination "platform=iOS Simulator,id=$udid" \
  -derivedDataPath "$out/dd-tests" CODE_SIGNING_ALLOWED=NO -only-testing:MediaViewerUITests/LiveSearchTests | grep -E 'error:|Test Case|\*\* TEST'

# ---- 4. compare -----------------------------------------------------------------------------
data="$(xcrun simctl get_app_container "$udid" "$bundle" data)"
lib="$data/Library/Application Support/library"
gen="$lib/gen/$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["dir"])' "$lib/installed.json")"
python3 -I "$root/tools/ios/compare_live.py" "${MV_LIVE_MAC:-$out/mac.jsonl}" "$out/shots/phone-answers.json" "$gen/catalog.db" \
  "$mnt/Photos/$(basename "$folder")" | tee "$out/compare.txt"
