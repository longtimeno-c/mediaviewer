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
# MV_LIVE_REUSE=1 keeps a finished publish (<out>/mac.jsonl) and starts at step 3; the image then
# stays attached when the run ends. Without it the image is detached on the way out.
# MV_LIVE_QUERIES and MV_LIVE_MAC: another query list and the Mac's answers to it (answer-engine).
# MV_LIVE_SOUND=<folder>: sound search's model (tools/ios/clap_text_phone.py's output) served on
# 127.0.0.1 for a Debug app, which downloads it and searches with it; the Mac side always hears.
set -euo pipefail
data_src="${1:?data}"; folder="${2:?folder}"; udid="${3:?udid}"; out="${4:?out}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
addons="$(dirname "$(dirname "$data_src")")"
pack="$(ls -d "$addons/AI"/0.* | sort -V | tail -1)"
places="$(ls -d "$addons/Places"/0.* | sort -V | tail -1)/data/places.mvgeo"
desk="$root/build/bin/mv_mobile_parity_desktop"; cli="$root/build/bin/mv_driftwire_cli_test"
port=18462
# The physical path: mount reports /private/tmp, not /tmp, and the checks below compare with it.
mkdir -p "$out"; out="$(cd "$out" && pwd -P)"
mnt="$out/mnt"
# This script's own service only: the pid in serve.pid, and only while it is still our
# `driftwire serve` (a reused pid is someone else's process).
stop_own_service() {
  if [[ -f $out/sound-serve.pid ]]; then
    local spid; spid="$(cat "$out/sound-serve.pid")"
    if ps -p "$spid" -o command= 2>/dev/null | grep -q "http.server 18463"; then kill "$spid" 2>/dev/null || true; fi
    rm -f "$out/sound-serve.pid"
  fi
  [[ -f $out/serve.pid ]] || return 0
  local pid; pid="$(cat "$out/serve.pid")"
  if ps -p "$pid" -o command= 2>/dev/null | grep -q "mv_driftwire_cli_test serve"; then kill "$pid" 2>/dev/null || true; fi
  rm -f "$out/serve.pid"
}
finish() {
  stop_own_service
  # The image holds copies of the owner's files: detached on every way out, unless the next run
  # reuses it (MV_LIVE_REUSE=1). A file still open in it (the service just stopped) can refuse
  # the first try; the second forces it, and a third failure is said, not hidden.
  [[ ${MV_LIVE_REUSE:-} == 1 ]] && return 0
  mount | grep -qF " on $mnt (" || return 0
  hdiutil detach -quiet "$mnt" 2>/dev/null || { sleep 2; hdiutil detach -force -quiet "$mnt" 2>/dev/null; } || true
  # Checked, not assumed: the image holds copies of the files.
  if mount | grep -qF " on $mnt ("; then
    echo "COULD NOT DETACH $mnt: it holds copies of the files; detach it by hand (hdiutil detach -force $mnt)" >&2
  fi
}
trap finish EXIT
trap 'exit 143' TERM INT HUP  # a signal ends the script through finish too

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
# The owner's own compute and quality, unless MV_LIVE_CPU=1 (towers on the CPU at quality High: no
# Core ML compile). The Core ML cache is cloned from the real data folder (APFS: no copy is made),
# so Auto does not compile the picture tower again for minutes.
if [[ ${MV_LIVE_CPU:-} == 1 ]]; then
python3 -I - "$out/data/settings.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1])); s["compute"] = 1; s["quality"] = 2; json.dump(s, open(sys.argv[1], "w"))
PY
fi
if [[ -d $data_src/cache/coreml && ! -d $out/coreml-cache ]]; then cp -cR "$data_src/cache/coreml" "$out/coreml-cache"; fi
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
if [[ -d $out/coreml-cache ]]; then mkdir -p "$out/work/pack-data/cache"; cp -cR "$out/coreml-cache" "$out/work/pack-data/cache/coreml"; fi
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
# Only this script's own service from an earlier run, never whatever else holds the port.
stop_own_service; sleep 1
"$cli" serve --data "$out/serve" --test-loopback > "$out/serve.log" 2>&1 & echo $! > "$out/serve.pid"
sound_base=""
if [[ -n ${MV_LIVE_SOUND:-} ]]; then
  # Plain files on loopback; the app asks from byte 0 and checks every file against its pin.
  python3 -I -m http.server 18463 --bind 127.0.0.1 --directory "$MV_LIVE_SOUND" > "$out/sound-serve.log" 2>&1 &
  echo $! > "$out/sound-serve.pid"
  sound_base="http://127.0.0.1:18463/"
fi
sleep 1

bundle=io.github.longtimeno-c.mediaviewer.mobile
xcrun simctl terminate "$udid" "$bundle" 2>/dev/null || true
xcrun simctl uninstall "$udid" "$bundle" 2>/dev/null || true
xcrun simctl keychain "$udid" reset
xcrun simctl install "$udid" "$out/MediaViewer.app"
mkdir -p "$out/shots"
cd "$root/src.ios"
TEST_RUNNER_MV_LIVE_SOUND_BASE="$sound_base" TEST_RUNNER_MV_LIVE_PAYLOAD="$payload" TEST_RUNNER_MV_LIVE_QUERIES="${MV_LIVE_QUERIES:-$out/work/queries.txt}" TEST_RUNNER_MV_LIVE_OUT="$out/shots" \
  xcodebuild test -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -destination "platform=iOS Simulator,id=$udid" \
  -derivedDataPath "$out/dd-tests" CODE_SIGNING_ALLOWED=NO -only-testing:MediaViewerUITests/LiveSearchTests | grep -E 'error:|Test Case|\*\* TEST'

# ---- 4. compare -----------------------------------------------------------------------------
data="$(xcrun simctl get_app_container "$udid" "$bundle" data)"
lib="$data/Library/Application Support/library"
gen="$lib/gen/$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["dir"])' "$lib/installed.json")"
python3 -I "$root/tools/ios/compare_live.py" "${MV_LIVE_MAC:-$out/mac.jsonl}" "$out/shots/phone-answers.json" "$gen/catalog.db" \
  "$mnt/Photos/$(basename "$folder")" | tee "$out/compare.txt"
