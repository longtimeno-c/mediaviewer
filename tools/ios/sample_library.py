#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sample library's content and its static folder (docs/plans/mobile.md §11.1). Driven by
# tools/ios/sample-library.sh; each step can run alone.
#
#   sample_library.py fetch   <sources.json> <downloads>        originals, SHA-1 and licence checked
#   sample_library.py make    <sources.json> <downloads> <dir>  the library folder: JPEGs and H.264 clips
#   sample_library.py flatten <volume> <sub> <release>          the published catalogue + originals, flat
#   sample_library.py serve   <release> <port>                  a loopback server with ranges, for tests
#   sample_library.py table   <sources.json>                    SOURCES.md: every file, its author and licence
#
# Licences: only CC0 and public domain pass, checked against Wikimedia Commons each time. The flat
# names follow src/mobile/sample.cpp flat_name exactly.
import hashlib
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from datetime import datetime, timezone

AGENT = "MediaViewerSampleBuilder/1.0 (https://github.com/longtimeno-c/mediaviewer)"
ALLOWED = {"CC0", "Public domain"}
CATALOGUE_ROOT = "c0c0c0c0c0c0c0c0"  # roots.json ids: 16 lowercase hex digits, as the service's
MEDIA_ROOT = "a0a0a0a0a0a0a0a0"
PLAIN = re.compile(r"^[A-Za-z0-9._ -]+$")


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except Exception:
            if attempt == 3:
                raise
            time.sleep(2 + attempt * 3)


def items(sources):
    return json.load(open(sources, encoding="utf-8"))["items"]


def licences(entries):
    """Each file's licence as Commons states it now."""
    out = {}
    titles = ["File:" + e["title"] for e in entries]
    for i in range(0, len(titles), 40):
        q = urllib.parse.urlencode({"action": "query", "format": "json", "titles": "|".join(titles[i:i + 40]),
                                    "prop": "imageinfo", "iiprop": "extmetadata", "iiextmetadatafilter": "LicenseShortName"})
        pages = json.loads(get("https://commons.wikimedia.org/w/api.php?" + q))["query"]
        norm = {n["to"]: n["from"] for n in pages.get("normalized", [])}
        for p in pages["pages"].values():
            meta = p.get("imageinfo", [{}])[0].get("extmetadata", {})
            value = re.sub(r"<[^>]+>", "", meta.get("LicenseShortName", {}).get("value", "")).strip()
            out[norm.get(p["title"], p["title"])[5:]] = value
    return out


def fetch(sources, downloads):
    entries = items(sources)
    os.makedirs(downloads, exist_ok=True)
    stated = licences(entries)
    bad = [f'{e["title"]}: {stated.get(e["title"], "missing")}' for e in entries if stated.get(e["title"]) not in ALLOWED]
    if bad:
        sys.exit("licence not CC0 or public domain:\n  " + "\n  ".join(bad))
    for e in entries:
        path = os.path.join(downloads, e["sha1"] + os.path.splitext(e["original"])[1].lower())
        if os.path.exists(path) and hashlib.sha1(open(path, "rb").read()).hexdigest() == e["sha1"]:
            continue
        data = get(e["original"])
        if hashlib.sha1(data).hexdigest() != e["sha1"]:
            sys.exit(f'{e["original"]}: SHA-1 does not match sources.json (the file on Commons changed)')
        open(path, "wb").write(data)
        time.sleep(0.5)  # Commons asks clients to go gently
    print(f"{len(entries)} originals, licences checked")


def when(text):
    """The picture's date for the file's time: the grid's order when a file has no EXIF date."""
    m = re.match(r"(\d{4})-(\d{2})-(\d{2})(?:[ T](\d{2}):(\d{2})(?::(\d{2}))?)?", text or "")
    if m:
        y, mo, d, h, mi, s = (int(x) if x else 12 if i == 3 else 0 for i, x in enumerate(m.groups()))
        return datetime(y, mo, d, h, mi, s, tzinfo=timezone.utc).timestamp()
    m = re.search(r"(\d{1,2}) (\w+) (\d{4})", text or "")
    if m:
        return datetime.strptime(" ".join(m.groups()), "%d %B %Y").replace(hour=12, tzinfo=timezone.utc).timestamp()
    m = re.match(r"(\d{4})", text or "")
    return datetime(int(m.group(1)) if m else 2020, 6, 1, 12, tzinfo=timezone.utc).timestamp()


def make(sources, downloads, out):
    ffmpeg = "/opt/homebrew/bin/ffmpeg"
    for e in items(sources):
        src = os.path.join(downloads, e["sha1"] + os.path.splitext(e["original"])[1].lower())
        dst = os.path.join(out, e["file"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not os.path.exists(dst):
            if e["kind"] == "photo":
                # 2048 px on the long edge: a sharp full screen on any phone, and a small download.
                subprocess.run(["/usr/bin/sips", "-Z", "2048", "-s", "format", "jpeg", "-s", "formatOptions", "82",
                                src, "--out", dst], check=True, capture_output=True)
            else:
                # H.264 from the OS's own encoder (no x264), 1280 px, moov first so a range reader
                # can start playing at once. No audio: the sources' sound is wind and water.
                subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", src, "-t", str(e["seconds"]),
                                "-vf", "scale='min(1280,iw)':-2", "-c:v", "h264_videotoolbox", "-b:v", "3M",
                                "-pix_fmt", "yuv420p", "-tag:v", "avc1", "-an", "-map_metadata", "-1",
                                "-movflags", "+faststart", dst], check=True)
        t = when(e["date"])
        os.utime(dst, (t, t))
    total = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(out) for f in fs)
    print(f"library folder: {total / 1e6:.1f} MB")


def flat_name(root, rel):
    """src/mobile/sample.cpp flat_name: '/' as "__", ' ' as '_', nothing else outside [A-Za-z0-9._-]."""
    parts = rel.split("/")
    if not rel or any(p in ("", ".", "..") or not PLAIN.match(p) for p in parts):
        raise SystemExit(f"{rel}: not a name the phone can ask for")
    return root + "__" + "__".join(p.replace(" ", "_") for p in parts)


def flatten(volume, sub, release):
    catalogue = os.path.join(volume, ".mediaviewer")
    gens = sorted(os.listdir(os.path.join(catalogue, "gen")))
    manifest = json.load(open(os.path.join(catalogue, "gen", gens[-1], "manifest.json")))
    shares = {r["share"] for r in manifest["roots"]}
    subs = {r["sub"] for r in manifest["roots"]}
    if len(shares) != 1 or subs != {sub}:
        sys.exit(f"expected one root at {sub!r}, the manifest says {manifest['roots']}")
    os.makedirs(release, exist_ok=True)
    for f in os.listdir(release):
        os.remove(os.path.join(release, f))
    names = {}

    def add(root, base, rel):
        name = flat_name(root, rel)
        if name in names:
            sys.exit(f"{rel} and {names[name]} have the same flat name")
        names[name] = rel
        shutil.copy2(os.path.join(base, rel), os.path.join(release, name))

    for r, _, files in os.walk(catalogue):
        for f in files:
            rel = os.path.relpath(os.path.join(r, f), catalogue)
            if rel != "publisher.lease" and not f.endswith(".part") and ".part/" not in rel:
                add(CATALOGUE_ROOT, catalogue, rel)
    media = os.path.join(volume, sub)
    for r, _, files in os.walk(media):
        for f in files:
            if not f.startswith("."):
                add(MEDIA_ROOT, volume, os.path.relpath(os.path.join(r, f), volume))
    json.dump({"roots": [{"id": CATALOGUE_ROOT, "label": "Catalogue"}, {"id": MEDIA_ROOT, "label": shares.pop()}]},
              open(os.path.join(release, "roots.json"), "w"))
    sizes = {"catalogue": 0, "media": 0}
    for name in os.listdir(release):
        sizes["catalogue" if name.startswith(CATALOGUE_ROOT) else "media"] += os.path.getsize(os.path.join(release, name))
    print(f"release: {len(names) + 1} files; the phone copies {sizes['catalogue'] / 1e6:.1f} MB of catalogue "
          f"(tower included) once; originals {sizes['media'] / 1e6:.1f} MB, read on demand")


class RangeHandler(http.server.SimpleHTTPRequestHandler):
    """GET with one Range, as GitHub's release CDN answers. Tests only, loopback only."""

    def do_GET(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        start, end = 0, size - 1
        m = re.match(r"bytes=(\d+)-(\d*)$", self.headers.get("Range", ""))
        if m:
            start = int(m.group(1))
            end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
            if start >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.end_headers()
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)


def table(sources):
    print("# The sample library's pictures and clips\n")
    print("Every file in the iPhone app's sample library (docs/plans/mobile.md §11.1), from Wikimedia Commons,")
    print("each CC0 or in the public domain, chosen with no identifiable face. `sources.json` pins each")
    print("original by SHA-1; `tools/ios/sample-library.sh` checks the licence again on every run.")
    print("Generated by `python3 tools/ios/sample_library.py table tools/ios/sample-library/sources.json`.\n")
    print("| File in the library | Original | Author | Licence |")
    print("|---|---|---|---|")
    for e in items(sources):
        title = e["title"].replace("|", "\\|")
        print(f'| {e["file"]} | [{title}]({e["page"]}) | {e["author"].replace("|", "/")} | {e["licence"]} |')


def serve(release, port):
    os.chdir(release)
    http.server.ThreadingHTTPServer(("127.0.0.1", int(port)), RangeHandler).serve_forever()


if __name__ == "__main__":
    commands = {"fetch": fetch, "make": make, "flatten": flatten, "serve": serve, "table": table}
    if len(sys.argv) < 2 or sys.argv[1] not in commands:
        sys.exit(__doc__ or "fetch | make | flatten | serve")
    commands[sys.argv[1]](*sys.argv[2:])
