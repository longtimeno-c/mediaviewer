#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A local stand-in for the Synology, for trying the iPhone app by hand in the Simulator:
#   tools/ios/demo-service.sh <folder> [desktop build dir]     start, print the pairing link
#   tools/ios/demo-service.sh <folder> stop                    stop the service
# DEMO_LIBRARY=<id> publishes under another library id (pair one library over another).
# DEMO_FILE_MODE=1 leaves only library.json on the share: the phone opens it in file mode.
#
# Makes a small sample library (generated pictures and two clips), publishes a signed catalogue
# with the test publisher, and runs `driftwire serve --test-loopback` on 127.0.0.1:18452. Writes the
# pairing link to <folder>/pair-link.txt and as a QR picture to <folder>/pair-code.png; add that to
# the Simulator's Photos (`xcrun simctl addmedia booted <folder>/pair-code.png`) to pair through
# "Choose a picture", or open the link (`xcrun simctl openurl booted "$(cat <folder>/pair-link.txt)"`).
# Needs the desktop build's bin/mv_publish_test_library and bin/mv_driftwire_cli_test. Never
# contacts the public DHT. The pairing code is single-use and lasts 15 minutes.
set -euo pipefail
out="${1:?folder}"; root="$(cd "$(dirname "$0")/../.." && pwd)"
port=18452
mkdir -p "$out"; out="$(cd "$out" && pwd)"
# It deletes and remakes its own folders inside <folder>, so it only works in a folder it made.
if [[ ! -f $out/.mv-demo && -n "$(ls -A "$out")" ]]; then
  echo "$out is not empty and is not a demo folder this script made; choose a new folder"; exit 2
fi
touch "$out/.mv-demo"

stop() {
  if [[ -f $out/serve.pid ]]; then kill "$(cat "$out/serve.pid")" 2>/dev/null || true; rm -f "$out/serve.pid"; fi
}
if [[ ${2:-} == stop ]]; then stop; echo "stopped"; exit 0; fi

build="${2:-$root/build}"
pub="$build/bin/mv_publish_test_library"; cli="$build/bin/mv_driftwire_cli_test"
[[ -x $pub && -x $cli ]] || { echo "needs $pub and $cli (the desktop build)"; exit 2; }
stop

# The sample library: two years of pictures in year folders, newest first, and two clips. Needs
# Pillow (pip3 install --user pillow), which lives in the user's site-packages, so no -I here.
python3 - "$out/photos" <<'PY'
import os, random, sys, time
from PIL import Image, ImageDraw
root = sys.argv[1]
random.seed(7)
skies = [((36, 52, 98), (236, 150, 98)), ((70, 120, 180), (190, 220, 240)), ((20, 24, 48), (60, 70, 120)),
         ((150, 190, 220), (240, 236, 220)), ((90, 60, 120), (250, 180, 120))]
lands = [(40, 60, 50), (60, 70, 90), (120, 90, 60), (30, 40, 60), (80, 110, 70)]
now = time.time()
for i in range(48):
    year = 2026 if i < 28 else 2025
    folder = os.path.join(root, str(year))
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, f"IMG_{4100 + i}.JPG")
    if os.path.exists(path):
        continue
    w, h = (4032, 3024) if i % 3 else (3024, 4032)
    top, bottom = skies[i % len(skies)]
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=tuple(int(top[k] + (bottom[k] - top[k]) * t) for k in range(3)))
    sx, sy, r = random.randint(w // 5, 4 * w // 5), random.randint(h // 6, h // 3), w // 12
    d.ellipse([sx - r, sy - r, sx + r, sy + r], fill=(250, 226, 180))
    for layer in range(3):
        base = h * (0.55 + 0.13 * layer)
        pts = [(0, h)] + [(x, base - random.randint(0, int(h * 0.18))) for x in range(0, w + w // 6, w // 6)] + [(w, h)]
        c = lands[(i + layer) % len(lands)]
        d.polygon(pts, fill=tuple(max(0, v - 18 * layer) for v in c))
    img.save(path, quality=88)
    stamp = now - (i * 9 + random.random() * 6) * 86400
    os.utime(path, (stamp, stamp))
PY
# Lower-case .mp4: the test publisher knows a clip by a lower-case extension only.
if [[ ! -f $out/photos/2026/MVI_4200.mp4 ]]; then
  /opt/homebrew/bin/ffmpeg -y -loglevel error -f lavfi -i "gradients=s=1920x1080:d=20:speed=0.02" -f lavfi -i "sine=frequency=330:duration=20" \
    -c:v h264_videotoolbox -b:v 6M -pix_fmt yuv420p -c:a aac -shortest -movflags +faststart "$out/photos/2026/MVI_4200.mp4"
  /opt/homebrew/bin/ffmpeg -y -loglevel error -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=12" \
    -c:v h264_videotoolbox -b:v 4M -pix_fmt yuv420p -movflags +faststart "$out/photos/2025/MVI_4201.mp4"
  touch -t 202610021200 "$out/photos/2026/MVI_4200.mp4"; touch -t 202507141200 "$out/photos/2025/MVI_4201.mp4"
fi

cd "$out"
rm -rf nas work data; mkdir -p nas work data; chmod 700 data
"$pub" photos nas work 1 "library=${DEMO_LIBRARY:-e2e-library-1}" > publish.txt
publisher="$(grep publisher_key= publish.txt | cut -d= -f2)"
library="$(grep library_id= publish.txt | cut -d= -f2)"
# DEMO_FILE_MODE=1: the share as the desktop leaves it before Local search has published anything,
# library.json only, so the phone opens the library in file mode (docs/plans/phone-in-the-app.md §4).
if [[ ${DEMO_FILE_MODE:-} == 1 ]]; then
  find nas/.mediaviewer -mindepth 1 -maxdepth 1 ! -name library.json -exec rm -rf {} +
fi
cat > data/serve.json <<JSON
{"port":$port,"name":"Demo NAS","roots":[{"id":"a1a1a1a1a1a1a1a1","label":"Photos","path":"$out/photos"},{"id":"c2c2c2c2c2c2c2c2","label":"Catalogue","path":"$out/nas/.mediaviewer"}]}
JSON
chmod 600 data/serve.json
"$cli" serve --data data --test-loopback init > init.txt
code="$("$cli" serve --data data --test-loopback pair-code)"
identity="$(awk '/identity/{print $2}' init.txt)"; access="$(awk '/access-key/{print $2}' init.txt)"
python3 -I - "$publisher" "$identity" "$access" "$code" "$port" "$library" > pair-link.txt <<'PY'
import sys, json, base64, time
p, i, a, c, port, library = sys.argv[1:7]
j = {"v": 1, "library_id": library, "library_name": "Home library", "publisher_key": p,
     "service": {"local": f"127.0.0.1:{port}", "public": f"localhost:{port}", "identity": i}, "access_key": a, "code": c,
     "expires": int(time.time()) + 900}
print("mediaviewer://pair#" + base64.urlsafe_b64encode(json.dumps(j).encode()).decode().rstrip("="))
PY

# The link as a QR picture, drawn by Core Image the way MediaViewer draws its own.
cat > work/qr.swift <<'SWIFT'
import CoreImage
import CoreImage.CIFilterBuiltins
import ImageIO
import UniformTypeIdentifiers
let text = try! String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8).trimmingCharacters(in: .whitespacesAndNewlines)
let filter = CIFilter.qrCodeGenerator()
filter.message = Data(text.utf8)
filter.correctionLevel = "M"
let image = filter.outputImage!.transformed(by: CGAffineTransform(scaleX: 12, y: 12))
let padded = image.composited(over: CIImage(color: .white).cropped(to: image.extent.insetBy(dx: -96, dy: -96)))
let context = CIContext()
let cg = context.createCGImage(padded, from: padded.extent)!
let dest = CGImageDestinationCreateWithURL(URL(fileURLWithPath: CommandLine.arguments[2]) as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(dest, cg, nil)
CGImageDestinationFinalize(dest)
SWIFT
xcrun swift work/qr.swift pair-link.txt pair-code.png

# The listener only, not the app connected to it.
stale="$(lsof -ti tcp:$port -sTCP:LISTEN || true)"; if [[ -n $stale ]]; then kill $stale; sleep 1; fi
nohup "$cli" serve --data data --test-loopback > serve.log 2>&1 &
echo $! > serve.pid
sleep 1
echo "service on 127.0.0.1:$port (pid $(cat serve.pid))"
echo "link:    $out/pair-link.txt"
echo "QR code: $out/pair-code.png"
