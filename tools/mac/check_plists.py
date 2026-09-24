#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Policy check for the macOS bundle metadata (PR 20). Runs anywhere; nothing
here needs a Mac.

  - The app's document types and the Quick Look extension's supported types
    are the same list, and it covers every still format_family the codec
    probes (src/codec/format.h), the D5 still set.
  - Every document type is LSHandlerRank Alternate: listed in Open With,
    never a silent default-app hijack (plan/15 PR 20).
  - The extension is sandboxed and asks for nothing else.
  - The Sparkle keys cmake/darwin-app.cmake injects require a signed feed and
    a verified archive, use HTTPS, and never send a system profile (rule 6).
"""
from __future__ import annotations

import plistlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PACKAGING = ROOT / "packaging" / "macos"

# format_family (src/codec/format.h) -> the UTIs that cover it. A new still
# format must add a row here, and the check fails until the plists list it.
FAMILY_UTIS = {
    "jpeg": {"public.jpeg"},
    "png": {"public.png"},
    "bmp": {"com.microsoft.bmp"},
    "gif": {"com.compuserve.gif"},
    "webp": {"org.webmproject.webp"},
    "tiff": {"public.tiff"},
    "ico": {"com.microsoft.ico"},
    "heic": {"public.heic", "public.heif"},
    "avif": {"public.avif"},
    "raw": {"public.camera-raw-image"},
}

# The D5 video containers (src/shell/media_kind.h). Video has its own document
# type; Quick Look thumbnails stay stills-only.
VIDEO_UTIS = {"public.mpeg-4", "com.apple.m4v-video", "com.apple.quicktime-movie",
              "org.matroska.mkv", "org.webmproject.webm", "public.avi",
              "public.mpeg-2-transport-stream"}


def configure(template: Path, extra: dict[str, str] | None = None) -> dict:
    text = template.read_text()
    values = {"MV_MAC_BUNDLE_ID": "io.example.check", "PROJECT_VERSION": "0.0.0",
              "MV_MAC_BUILD_NUMBER": "0", "MV_SPARKLE_PLIST_KEYS": ""}
    values.update(extra or {})
    text = re.sub(r"@([A-Z0-9_]+)@", lambda m: values[m.group(1)], text)
    return plistlib.loads(text.encode())


def format_families() -> list[str]:
    header = (ROOT / "src" / "codec" / "format.h").read_text()
    body = re.search(r"enum class format_family[^{]*\{(.*?)\};", header, re.S).group(1)
    names = re.findall(r"^\s*([a-z0-9_]+)\s*=", body, re.M)
    return [n for n in names if n != "unknown"]


def sparkle_keys() -> str:
    cmake = (ROOT / "cmake" / "darwin-app.cmake").read_text()
    block = re.search(r'set\(MV_SPARKLE_PLIST_KEYS "(.*?)"\)', cmake, re.S).group(1)
    return (block.replace("${MV_SPARKLE_FEED_URL}", "https://example.invalid/appcast.xml")
                 .replace("${MV_SPARKLE_PUBLIC_ED_KEY}", "AAAA"))


def main() -> int:
    problems: list[str] = []

    app = configure(PACKAGING / "Info.plist.in")
    appex = configure(PACKAGING / "QuickLook-Info.plist.in")

    doc_types = app.get("CFBundleDocumentTypes", [])
    app_utis: set[str] = set()
    video_utis: set[str] = set()
    for doc in doc_types:
        (video_utis if doc.get("CFBundleTypeName") == "Video" else app_utis).update(
            doc.get("LSItemContentTypes", []))
        if doc.get("LSHandlerRank") != "Alternate":
            problems.append(f"document type {doc.get('CFBundleTypeName')!r}: LSHandlerRank must be "
                            "Alternate (never a silent default-app hijack)")
        if doc.get("CFBundleTypeRole") != "Viewer":
            problems.append(f"document type {doc.get('CFBundleTypeName')!r}: role must be Viewer "
                            "(v1 never writes an original)")

    ext = appex.get("NSExtension", {})
    if ext.get("NSExtensionPointIdentifier") != "com.apple.quicklook.thumbnail":
        problems.append("extension is not a Quick Look thumbnail extension")
    ql_utis = set(ext.get("NSExtensionAttributes", {}).get("QLSupportedContentTypes", []))

    if app_utis != ql_utis:
        problems.append(f"app and Quick Look types differ: only app {sorted(app_utis - ql_utis)}, "
                        f"only Quick Look {sorted(ql_utis - app_utis)}")

    families = format_families()
    for family in families:
        if family not in FAMILY_UTIS:
            problems.append(f"format_family::{family} has no UTI row in tools/mac/check_plists.py")
            continue
        missing = FAMILY_UTIS[family] - app_utis
        if missing:
            problems.append(f"format_family::{family}: {sorted(missing)} not in the document types")
    known = set().union(*FAMILY_UTIS.values())
    extra = app_utis - known
    if extra:
        problems.append(f"types outside the D5 still set: {sorted(extra)}")

    if video_utis != VIDEO_UTIS:
        problems.append(f"video document type must list exactly the D5 containers: "
                        f"missing {sorted(VIDEO_UTIS - video_utis)}, extra {sorted(video_utis - VIDEO_UTIS)}")

    for name, plist in (("app", app), ("extension", appex)):
        if plist.get("LSMinimumSystemVersion") != "14.0":
            problems.append(f"{name}: LSMinimumSystemVersion must be 14.0 (plan/15 floor)")
    if appex.get("CFBundleIdentifier") != app.get("CFBundleIdentifier") + ".thumbnails":
        problems.append("extension bundle id must be <app id>.thumbnails")

    entitlements = plistlib.loads((PACKAGING / "QuickLook.entitlements").read_bytes())
    if entitlements != {"com.apple.security.app-sandbox": True}:
        problems.append(f"extension entitlements must be the sandbox alone, got {entitlements}")

    sparkled = configure(PACKAGING / "Info.plist.in", {"MV_SPARKLE_PLIST_KEYS": sparkle_keys()})
    want = {"SURequireSignedFeed": True, "SUVerifyUpdateBeforeExtraction": True,
            "SUEnableSystemProfiling": False, "SUScheduledCheckInterval": 21600}
    for key, value in want.items():
        if sparkled.get(key) != value:
            problems.append(f"Sparkle key {key} must be {value!r}, got {sparkled.get(key)!r}")
    if not str(sparkled.get("SUFeedURL", "")).startswith("https://"):
        problems.append("SUFeedURL must be https")

    for p in problems:
        print(f"check_plists: {p}", file=sys.stderr)
    if not problems:
        print(f"check_plists: ok ({len(app_utis)} types, {len(families)} still families)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
