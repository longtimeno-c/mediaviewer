#!/bin/bash
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The iPhone app's sample library (docs/plans/mobile.md §11.1), made again from its sources:
#
#   tools/ios/sample-library.sh <work folder> <generation> [--upload]
#
# 1. Downloads every picture and clip in tools/ios/sample-library/sources.json from Wikimedia
#    Commons, checking each SHA-1 and that Commons still says CC0 or public domain.
# 2. Makes the library folder: JPEGs at 2048 px, H.264 clips at 1280 px (sample_library.py make).
# 3. Puts it on a disk image of its own, so no path of this Mac reaches the catalogue, and runs the
#    desktop's real indexer and phone_publish over it (mv_mobile_parity_desktop sample-publish):
#    the AI pack's CLIP ViT-B/32, signed with the sample library's own key, never the owner's.
# 4. Flattens the catalogue and the originals into <work>/release, one file per path, as the app
#    reads them (src/mobile/sample.h), and signs files.json: the size and SHA-256 of every file the
#    catalogue's own signature does not cover (the JPEG-512 pack, the originals).
# 5. --upload: puts <work>/release on the GitHub release `sample-library` (gh), current.json last,
#    so a phone never sees a generation before all of its files are there.
#
# <generation> must be above the one on the release now (the phone refuses anything older).
# Needs build/ (mv_mobile_parity_desktop), the AI and Places add-ons installed, ffmpeg (Homebrew),
# and the signing key: MV_SAMPLE_KEY, by default ~/Dev/.mv-toolchain/devkey/sample-library-publisher.key.
set -euo pipefail
work="${1:?work folder}"; generation="${2:?generation}"; upload="${3:-}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
key="${MV_SAMPLE_KEY:-$HOME/Dev/.mv-toolchain/devkey/sample-library-publisher.key}"
addons="$HOME/Library/Application Support/MediaViewer/Add-ons"
pack="$(ls -d "$addons/AI"/0.* | sort -V | tail -1)"
places="$(ls -d "$addons/Places"/0.* | sort -V | tail -1)/data/places.mvgeo"
desk="$root/build/bin/mv_mobile_parity_desktop"
library_id="$(sed -n 's/.*kLibraryID = "\([^"]*\)".*/\1/p' "$root/src/mobile/sample.h")"
want_key="$(sed -n 's/.*kPublisherKey = "\([^"]*\)".*/\1/p' "$root/src/mobile/sample.h")"
sub="Sample library"
[[ -f $key ]] || { echo "no signing key at $key" >&2; exit 1; }
mkdir -p "$work"; work="$(cd "$work" && pwd -P)"
mnt="$work/mnt"

finish() {
  mount | grep -qF " on $mnt (" || return 0
  hdiutil detach -quiet "$mnt" 2>/dev/null || hdiutil detach -force -quiet "$mnt" 2>/dev/null || true
}
trap finish EXIT
trap 'exit 143' TERM INT HUP

# ---- 1, 2. sources and the library folder --------------------------------------------------
python3 -I "$root/tools/ios/sample_library.py" fetch "$root/tools/ios/sample-library/sources.json" "$work/downloads"
rm -rf "$work/content"
python3 -I "$root/tools/ios/sample_library.py" make "$root/tools/ios/sample-library/sources.json" "$work/downloads" "$work/content"

# ---- 3. its own volume, indexed and published ----------------------------------------------
finish
rm -f "$work/sample.sparseimage"
hdiutil create -quiet -size 2g -type SPARSE -fs APFS -volname "MediaViewer Sample" "$work/sample"
mkdir -p "$mnt"
hdiutil attach -quiet -nobrowse -mountpoint "$mnt" "$work/sample.sparseimage"
mkdir -p "$mnt/$sub"
cp -pR "$work/content/." "$mnt/$sub/"
rm -rf "$work/engine"
"$desk" sample-publish "$mnt/$sub" "$pack" "$addons" "$places" "$work/engine" "$mnt" "$key" "$library_id" "$generation" \
  | tee "$work/publish.txt"
got_key="$(sed -n 's/^publisher_key=//p' "$work/publish.txt")"
[[ $got_key == "$want_key" ]] || { echo "signed with $got_key, but the app pins $want_key" >&2; exit 1; }
# Nothing of this Mac in what a phone reads: no home folder anywhere, and no user name in what
# the publisher wrote. (models/ holds the AI pack's own files, pinned by hash: CLIP's vocabulary
# has thousands of first names in it.)
leaks="$( { grep -rlaF -e "$HOME" "$mnt/.mediaviewer"; grep -rlaF -e "$(id -un)" --exclude-dir=models "$mnt/.mediaviewer"; } || true)"
if [[ -n $leaks ]]; then echo "the catalogue names this Mac's folders or user:" >&2; echo "$leaks" >&2; exit 1; fi

# ---- 4. the static folder -------------------------------------------------------------------
python3 -I "$root/tools/ios/sample_library.py" flatten "$mnt" "$sub" "$work/release"
"$desk" sample-sign "$work/release/files.json" "$key" "$work/release/files.sig"

# ---- 5. the release --------------------------------------------------------------------------
if [[ $upload == --upload ]]; then
  repo=longtimeno-c/mediaviewer tag=sample-library
  gh release view "$tag" --repo "$repo" >/dev/null 2>&1 || gh release create "$tag" --repo "$repo" --prerelease \
    --title "Sample library for the iPhone app" \
    --notes "The iPhone app's sample library (code SAMPLE): CC0 and public-domain pictures from Wikimedia Commons, listed with their authors in tools/ios/sample-library/SOURCES.md. Made by tools/ios/sample-library.sh."
  # The files first, then the list that vouches for them, then current.json.
  ( cd "$work/release" && ls | grep -vxE 'c0c0c0c0c0c0c0c0__current.json|files.json|files.sig' | xargs gh release upload "$tag" --repo "$repo" --clobber )
  gh release upload "$tag" --repo "$repo" --clobber "$work/release/files.json" "$work/release/files.sig"
  gh release upload "$tag" --repo "$repo" --clobber "$work/release/c0c0c0c0c0c0c0c0__current.json"
  echo "uploaded generation $generation"
fi
