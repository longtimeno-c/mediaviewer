#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The phone against `driftwire serve` on loopback (docs/plans/mobile.md M3, M4, M6–M8):
#   tools/ios/link-e2e.sh <Simulator UDID> <folder for this run>
# Builds nothing; `build/`, `build-ios-sim/` and the app must exist. Never contacts the public DHT.
set -euo pipefail
udid="${1:?udid}"; out="${2:?folder}"; root="$(cd "$(dirname "$0")/../.." && pwd)"
pub="$root/build/bin/mv_publish_test_library"; cli="$root/build/bin/mv_driftwire_cli_test"
app="$out/MediaViewer.app"
[[ -d $app ]] || { echo "copy MediaViewer.app to $app"; exit 2; }
bundle=io.github.longtimeno-c.mediaviewer.mobile

python3 -I - "$out" <<'PY' || exit 1
import os,sys
os.makedirs(sys.argv[1]+"/photos/2025",exist_ok=True)
PY
cd "$out/photos/2025"
if [[ ! -f clip.mp4 ]]; then
  for i in 1 2 3 4 5 6; do /opt/homebrew/bin/ffmpeg -y -loglevel error -f lavfi -i "testsrc2=size=3000x2000:duration=0.04:rate=25" -frames:v 1 -update 1 "img_$i.jpg"; done
  /opt/homebrew/bin/ffmpeg -y -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=30 -f lavfi -i sine=frequency=440:duration=30 -c:v h264_videotoolbox -b:v 2M -pix_fmt yuv420p -c:a aac -movflags +faststart clip.mp4
fi
touch -t 202601010000 img_*.jpg; touch clip.mp4
cd "$out"
rm -rf nas work data shots serve.log; mkdir -p nas work shots data; chmod 700 data
mkfile -n 700m work/model.bin  # sparse, so a copy is long enough to interrupt on loopback
MV_E2E_MODEL="$out/work/model.bin" "$pub" photos nas work 1 > publish.txt; cat publish.txt
publisher="$(grep publisher_key= publish.txt | cut -d= -f2)"

cat > data/serve.json <<JSON
{"port":18442,"name":"E2E NAS","roots":[{"id":"a1a1a1a1a1a1a1a1","label":"Photos","path":"$out/photos"},{"id":"c2c2c2c2c2c2c2c2","label":"Catalogue","path":"$out/nas/.mediaviewer"}]}
JSON
chmod 600 data/serve.json
"$cli" serve --data data --test-loopback init > init.txt
code="$("$cli" serve --data data --test-loopback pair-code)"
identity="$(awk '/identity/{print $2}' init.txt)"; access="$(awk '/access-key/{print $2}' init.txt)"
payload="$(python3 -I - "$publisher" "$identity" "$access" "$code" <<'PY'
import sys,json,base64,time
p,i,a,c=sys.argv[1:5]
j={"v":1,"library_id":"e2e-library-1","library_name":"E2E Library","publisher_key":p,
 "service":{"local":"127.0.0.1:18442","public":"localhost:18442","identity":i},"access_key":a,"code":c,"expires":int(time.time())+900}
print("mediaviewer://pair#"+base64.urlsafe_b64encode(json.dumps(j).encode()).decode().rstrip("="))
PY
)"
# A service left over from an aborted run would keep the port with other keys.
stale="$(lsof -ti tcp:18442 || true)"; if [[ -n $stale ]]; then kill $stale; sleep 1; fi
trap 'kill "$(cat "$out/serve.pid")" 2>/dev/null || true' EXIT
start_serve() { "$cli" serve --data data --test-loopback > serve.log 2>&1 & echo $! > serve.pid; sleep 1; }
start_serve

xcrun simctl uninstall "$udid" "$bundle" 2>/dev/null || true
xcrun simctl keychain "$udid" reset
xcrun simctl install "$udid" "$app"
data_dir() { xcrun simctl get_app_container "$udid" "$bundle" data; }

# Kills the service once a staged copy is well under way, brings it back: the test presses Try again.
(
  while :; do
    d="$(data_dir 2>/dev/null || true)"
    n="$(find "$d/Library/Application Support/library" -name '*.part' -size +20000k 2>/dev/null | head -1 || true)"
    if [[ -n $n ]]; then kill "$(cat serve.pid)"; echo "killed the service mid-copy at $(stat -f %z "$n") bytes" > interrupted.txt; sleep 2; "$cli" serve --data data --test-loopback > serve.log 2>&1 & echo $! > serve.pid; break; fi
    sleep 0.1
  done
) &
watcher=$!

cd "$root/src.ios"
TEST_RUNNER_MV_E2E_PAYLOAD="$payload" TEST_RUNNER_MV_E2E_OUT="$out/shots" xcodebuild test \
  -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -destination "platform=iOS Simulator,id=$udid" \
  -derivedDataPath "$out/dd-tests" CODE_SIGN_IDENTITY=- CODE_SIGNING_REQUIRED=NO \
  -only-testing:MediaViewerUITests/LinkEndToEndTests | tail -25 || echo "UI TESTS FAILED"
kill "$watcher" 2>/dev/null || true
cd "$out"
cat interrupted.txt || echo "NOT INTERRUPTED"

d="$(data_dir)"
inst="$d/Library/Application Support/library/installed.json"
gen_now() { python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['generation'])" "$inst"; }
echo "installed after first copy: generation $(gen_now)"
echo "--- integrity: every installed file against the signed manifest"
genname="$(python3 -I -c "import json,sys;print(json.load(open(sys.argv[1]))['dir'])" "$inst")"
python3 -I - "$d/Library/Application Support/library/gen/$genname" <<'PY'
import json,hashlib,sys,os
d=sys.argv[1]; m=json.load(open(d+"/manifest.json"))
bad=0
for f in m["files"]:
  h=hashlib.sha256(open(os.path.join(d,f["name"]),"rb").read()).hexdigest()
  if h!=f["sha256"]: bad+=1; print("MISMATCH",f["name"])
print(len(m["files"]),"files checked,",bad,"mismatched")
PY
relaunch() { xcrun simctl terminate "$udid" "$bundle" 2>/dev/null || true; xcrun simctl launch "$udid" "$bundle" >/dev/null; sleep 8; }

echo "--- a newer generation installs"
cp nas/.mediaviewer/current.json current-gen1.json
"$pub" photos nas work 2 >/dev/null
relaunch; echo "generation: $(gen_now)"
echo "--- a lower generation, correctly signed, is refused"
cp nas/.mediaviewer/current.json current-gen2.json
cp current-gen1.json nas/.mediaviewer/current.json
relaunch; echo "generation stays: $(gen_now)"
cp current-gen2.json nas/.mediaviewer/current.json
echo "--- a wrong signature is refused"
"$pub" photos nas work 3 wrong-key >/dev/null
relaunch; echo "generation stays: $(gen_now)"
xcrun simctl io "$udid" screenshot shots/10-wrong-signature.png >/dev/null 2>&1
kill "$(cat serve.pid)" 2>/dev/null || true
echo finished
