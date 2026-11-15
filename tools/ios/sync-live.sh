#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A new catalogue reaching a running app (owner, 2026-11-14: search "doesn't seem to be updating
# anymore"), live: the Mac's own publisher, a loopback service, the real app.
#
#   tools/ios/sync-live.sh <Local search data folder> <indexed folder> <Simulator UDID> <MediaViewer.app> <out folder>
#
# 1. Copies the indexed folder's stills and short clips onto a disk image of its own (the "NAS"),
#    and two copies of the index moved to point there: the whole folder, and its photos only.
#    The real index is only read.
# 2. Publishes the photos-only copy (generation 1) with the desktop engine's phone_publish.
# 3. Serves the image with `driftwire serve --test-loopback` on 127.0.0.1 and runs
#    UITests/LiveSyncTests: the app pairs, copies, and searches for clips (none yet).
# 4. When the test says so, publishes the whole copy (generation 2) with the same key; the test
#    brings the app back to the foreground and checks the search on screen answers from it.
# Needs build/ (mv_mobile_parity_desktop, mv_driftwire_cli_test) and a Debug MediaViewer.app built
# for the Simulator ad hoc signed (CODE_SIGN_IDENTITY=- CODE_SIGNING_REQUIRED=NO). The image is
# detached on every way out. MV_SYNC_QUERY: the words searched (default "mountain is:video").
set -euo pipefail
data_src="${1:?data}"; folder="${2:?folder}"; udid="${3:?udid}"; app="${4:?app}"; out="${5:?out}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
addons="$(dirname "$(dirname "$data_src")")"
pack="$(ls -d "$addons/AI"/0.* | sort -V | tail -1)"
places="$(ls -d "$addons/Places"/0.* | sort -V | tail -1)/data/places.mvgeo"
desk="$root/build/bin/mv_mobile_parity_desktop"; cli="$root/build/bin/mv_driftwire_cli_test"
port=18464
mkdir -p "$out"; out="$(cd "$out" && pwd -P)"
mnt="$out/mnt"
stop_own_service() {
  [[ -f $out/serve.pid ]] || return 0
  local pid; pid="$(cat "$out/serve.pid")"
  if ps -p "$pid" -o command= 2>/dev/null | grep -q "mv_driftwire_cli_test serve"; then kill "$pid" 2>/dev/null || true; fi
  rm -f "$out/serve.pid"
}
finish() {
  stop_own_service
  [[ -n ${publisher_pid:-} ]] && kill "$publisher_pid" 2>/dev/null || true
  mount | grep -qF " on $mnt (" || return 0
  hdiutil detach -quiet "$mnt" 2>/dev/null || { sleep 2; hdiutil detach -force -quiet "$mnt" 2>/dev/null; } || true
  if mount | grep -qF " on $mnt ("; then
    echo "COULD NOT DETACH $mnt: it holds copies of the files; detach it by hand (hdiutil detach -force $mnt)" >&2
  fi
}
trap finish EXIT
trap 'exit 143' TERM INT HUP

# ---- 1. the "NAS" and the two copies of the index -------------------------------------------
[[ -f $out/nas.sparseimage ]] || hdiutil create -quiet -size 16g -type SPARSE -fs APFS -volname MVSyncNAS "$out/nas"
mkdir -p "$mnt"; mount | grep -qF " on $mnt (" || hdiutil attach -quiet -nobrowse -mountpoint "$mnt" "$out/nas.sparseimage"
rm -rf "$out/data" "$out/data1" "$mnt/.mediaviewer" "$out/work"; mkdir -p "$out/data"
sqlite3 "file:$data_src/index.db?mode=ro" "VACUUM INTO '$out/data/index.db'"
[[ -f $data_src/faces.db ]] && sqlite3 "file:$data_src/faces.db?mode=ro" "VACUUM INTO '$out/data/faces.db'"
cp "$data_src/settings.json" "$out/data/"; [[ -f $data_src/labels.f32 ]] && cp "$data_src/labels.f32" "$out/data/"
# The Core ML cache, cloned (APFS: no copy is made), so the Mac's engine compiles no tower again.
if [[ -d $data_src/cache/coreml ]]; then mkdir -p "$out/work/pack-data/cache"; cp -cR "$data_src/cache/coreml" "$out/work/pack-data/cache/coreml"; fi
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
cp -R "$out/data" "$out/data1"
python3 -I - "$out/data1/index.db" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1]); c.execute("PRAGMA foreign_keys=ON")
n = c.execute("DELETE FROM assets WHERE kind=2").rowcount
c.commit(); c.execute("VACUUM")
print(f"generation 1: {n} clips left out")
PY
printf '%s\n' "${MV_SYNC_QUERY:-mountain is:video}" > "$out/queries.txt"

# The publish only: the engine goes on to answer every query the library suggests, which this
# test does not need, so it is stopped once the key is printed (the publish is delivered by then).
publish() {  # <data> <out.txt>
  "$desk" publish-engine "$1" "$pack" "$addons" "$places" "$out/work" "$mnt" 3 "$out/queries.txt" /dev/null > "$2" 2>&1 &
  local pid=$!
  until grep -q publisher_key= "$2" 2>/dev/null || ! kill -0 "$pid" 2>/dev/null; do sleep 1; done
  kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
  grep -q publisher_key= "$2" || { echo "publish failed: $(tail -3 "$2")" >&2; return 1; }
  grep '^publish:' "$2"
}

# ---- 2. generation 1 ------------------------------------------------------------------------
publish "$out/data1" "$out/publish1.txt"
publisher="$(grep publisher_key= "$out/publish1.txt" | cut -d= -f2)"
library="$(python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["library_id"])' "$mnt/.mediaviewer/library.json")"
share="$(python3 -I - "$mnt/.mediaviewer" <<'PY'
import json, os, sys
gen = sorted(os.listdir(sys.argv[1] + "/gen"))[-1]
print(json.load(open(f"{sys.argv[1]}/gen/{gen}/manifest.json"))["roots"][0]["share"])
PY
)"

# ---- 3. the service, the app ----------------------------------------------------------------
rm -rf "$out/serve"; mkdir -p "$out/serve"; chmod 700 "$out/serve"
cat > "$out/serve/serve.json" <<JSON
{"port":$port,"name":"Test NAS","roots":[{"id":"a1a1a1a1a1a1a1a1","label":"$share","path":"$mnt"},{"id":"c2c2c2c2c2c2c2c2","label":"Catalogue","path":"$mnt/.mediaviewer"}]}
JSON
chmod 600 "$out/serve/serve.json"
"$cli" serve --data "$out/serve" --test-loopback init --no-passphrase > "$out/init.txt"
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
stop_own_service; sleep 1
"$cli" serve --data "$out/serve" --test-loopback > "$out/serve.log" 2>&1 & echo $! > "$out/serve.pid"
sleep 1

bundle=io.github.longtimeno-c.mediaviewer.mobile
xcrun simctl terminate "$udid" "$bundle" 2>/dev/null || true
xcrun simctl uninstall "$udid" "$bundle" 2>/dev/null || true
xcrun simctl keychain "$udid" reset
xcrun simctl install "$udid" "$app"
mkdir -p "$out/shots"; rm -f "$out/shots/phone-ready" "$out/shots/gen2-published" "$out/shots/result.txt"

# ---- 4. generation 2, once the phone has searched generation 1 -------------------------------
(
  while [[ ! -f $out/shots/phone-ready ]]; do sleep 1; done
  publish "$out/data" "$out/publish2.txt" > /dev/null
  touch "$out/shots/gen2-published"
) &
publisher_pid=$!

cd "$root/src.ios"
TEST_RUNNER_MV_SYNC_PAYLOAD="$payload" TEST_RUNNER_MV_SYNC_OUT="$out/shots" TEST_RUNNER_MV_SYNC_QUERY="${MV_SYNC_QUERY:-}" \
  xcodebuild test -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -destination "platform=iOS Simulator,id=$udid" \
  -derivedDataPath "$out/dd-tests" CODE_SIGNING_ALLOWED=NO -only-testing:MediaViewerUITests/LiveSyncTests \
  | tee "$out/xcodebuild.txt" | grep -E 'error:|LIVE-SYNC|Test Case|\*\* TEST' || true
wait "$publisher_pid" 2>/dev/null || true; publisher_pid=""
ls "$mnt/.mediaviewer/gen"
cat "$out/shots/result.txt" 2>/dev/null || echo "no result"
grep -q '\*\* TEST SUCCEEDED' "$out/xcodebuild.txt"
