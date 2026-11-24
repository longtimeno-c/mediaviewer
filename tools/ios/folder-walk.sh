#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   tools/ios/folder-walk.sh <src.ios> <Simulator UDID> <DerivedData> <label> <runs>
#
# Late frames per folder change with no test attached: builds the fixtures app, launches it with
# -fixtureWalk 3 (into a folder, into its first folder, back twice, three times, 1.5 s apart) and
# reads the frame meter's figures for the walk from Documents/walk.txt. EXTRA=... adds build settings.
set -u
cd "$1"
xcodebuild -project MediaViewerMobile.xcodeproj -scheme MediaViewerFixtures -configuration Debug -destination "platform=iOS Simulator,id=$2" -derivedDataPath "$3" CODE_SIGNING_ALLOWED=NO ${EXTRA:-} build 2>&1 | grep -E "error:|BUILD FAILED" && exit 1
xcrun simctl install "$2" "$3/Build/Products/Debug-iphonesimulator/MediaViewerFixtures.app"
bundle=io.github.longtimeno-c.mediaviewer.mobile.fixtures
for i in $(seq 1 $5); do
  docs="$(xcrun simctl get_app_container "$2" $bundle data)/Documents"; rm -f "$docs/walk.txt"
  xcrun simctl launch --terminate-running-process "$2" $bundle -fixtureState library -fixtureChrome NO -fixtureFrameMeter YES -fixtureWalk 3 >/dev/null
  for t in $(seq 1 90); do [ -f "$docs/walk.txt" ] && break; sleep 1; done
  python3 - "$docs/walk.txt" "$4" <<'PY'
import sys,re
t=open(sys.argv[1]).read(); ch=int(re.search(r'changes=(\d+)',t).group(1))
def v(line,k): return float(re.search(k+r'=([\d.]+)',line).group(1))
b=[l for l in t.splitlines() if l.startswith('before')][0]; a=[l for l in t.splitlines() if l.startswith('after')][0]
h=v(a,'hitches')-v(b,'hitches'); ms=v(a,'hitchMs')-v(b,'hitchMs')
print(f"{sys.argv[2]} WALK changes={ch} hitches={h:.0f} hitchMs={ms:.0f} perChange={h/ch:.2f} msPerChange={ms/ch:.1f}")
PY
done
xcrun simctl terminate "$2" $bundle 2>/dev/null
