#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""PR 11 macOS crash-reporting verify (plan/10 PR 11, "Verify (macOS, crash
reporting)"): the Mac twin of tools/make-crash-raw.ps1 and
tools/minidump-scan.ps1, in Python like the rest of tools/mac.

  make  -- writes the deliberately-corrupted RAW as a COPY (never the original,
           rule 5), in a folder and under a name a leak cannot hide:
             <out>/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.dng
           With --source: a copy of that file with the ASCII marker
           MV-DELIBERATE-CRASH at 0x1000 (codec/crash_test_hook.cpp looks for
           it in the first 64 KB). Without: two pattern BMPs, the companion
           that decodes and stays in memory and the canary that crashes, and
           the pixel patterns to scan for.
  scan  -- searches a minidump (or any file) for forbidden strings (UTF-8 and
           UTF-16LE, ASCII-case-insensitive) and pixel byte runs. Exit 0 PASS,
           1 FAIL, 2 usage.

  MV_CRASH_TEST=decode MediaViewer.app/Contents/MacOS/MediaViewer <out>/PRIVATE_FOLDER_canary
  (relaunch: the app scrubs the dump on start)
  tools/mac/crash_canary.py scan ~/Library/Application\\ Support/MediaViewer/Crashes/completed/*.dmp \\
      --forbid SECRET_FILENAME_canary_7Q3 --forbid PRIVATE_FOLDER_canary --forbid "$USER" \\
      --pixel-hex C35A177EE1293B94D6F00DB8622FACD971441CCB8EA736F5
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

MARKER = b"MV-DELIBERATE-CRASH"
FOLDER = "PRIVATE_FOLDER_canary"
CANARY = "SECRET_FILENAME_canary_7Q3.dng"
COMPANION = "SECRET_COMPANION_canary.bmp"
# Eight distinctive RGB triples, as the Windows script: the RGBA form (A=255) is
# what a decoder holds.
PALETTE = [0xC3, 0x5A, 0x17, 0x7E, 0xE1, 0x29, 0x3B, 0x94, 0xD6, 0xF0, 0x0D, 0xB8,
           0x62, 0x2F, 0xAC, 0xD9, 0x71, 0x44, 0x1C, 0xCB, 0x8E, 0xA7, 0x36, 0xF5]
SCRUB_MARKER = 0x4353564D  # shell/minidump_scrub.h kScrubMarker


def pattern_bmp(width: int, height: int, with_marker: bool) -> bytes:
    stride = (width * 3 + 3) & ~3
    pix = 54
    size = pix + stride * height
    b = bytearray(size)
    b[0:2] = b"BM"
    struct.pack_into("<IHHI", b, 2, size, 0, 0, pix)
    struct.pack_into("<IiiHHIIiiII", b, 14, 40, width, height, 1, 24, 0, 0, 0, 0, 0, 0)
    for y in range(height):
        row = pix + y * stride
        for x in range(width):
            p = (x % 8) * 3
            b[row + x * 3:row + x * 3 + 3] = bytes((PALETTE[p + 2], PALETTE[p + 1], PALETTE[p]))
    if with_marker:
        b[pix:pix + len(MARKER)] = MARKER
    return bytes(b)


def pixel_patterns() -> tuple[str, str]:
    bgr = "".join(f"{PALETTE[3*i+2]:02X}{PALETTE[3*i+1]:02X}{PALETTE[3*i]:02X}" for i in range(8))
    rgba = "".join(f"{PALETTE[3*i]:02X}{PALETTE[3*i+1]:02X}{PALETTE[3*i+2]:02X}FF" for i in range(8))
    return bgr, rgba


def cmd_make(args: argparse.Namespace) -> int:
    folder = Path(args.out) / FOLDER
    folder.mkdir(parents=True, exist_ok=True)
    out = folder / CANARY
    if args.source:
        src = Path(args.source).resolve()
        if src == out.resolve():
            print("refusing to overwrite the source file")
            return 2
        data = bytearray(src.read_bytes())
        if len(data) < len(MARKER):
            data = bytearray(len(MARKER))
        offset = min(0x1000, max(0, len(data) - len(MARKER)))
        data[offset:offset + len(MARKER)] = MARKER
        out.write_bytes(bytes(data))
        print(f"wrote {out} (copy of source, marker at 0x{offset:X})")
        return 0
    (folder / COMPANION).write_bytes(pattern_bmp(args.width, args.height, False))
    out.write_bytes(pattern_bmp(args.width, args.height, True))
    bgr, rgba = pixel_patterns()
    print(f"wrote {out} and {COMPANION} ({args.width} x {args.height}, BMP bytes)")
    print(f"pixel pattern file BGR   : {bgr}")
    print(f"pixel pattern decoded RGBA: {rgba}")
    print("open the companion with MV_CRASH_TEST=decode, then the canary")
    return 0


def find_all(hay: bytes, needle: bytes, ignore_case: bool) -> list[int]:
    if not needle:
        return []
    if ignore_case:
        hay = hay.lower()
        needle = needle.lower()
    hits, at = [], hay.find(needle)
    while at >= 0:
        hits.append(at)
        at = hay.find(needle, at + max(1, len(needle)))
    return hits


def scan(data: bytes, forbidden: list[str], pixel_hex: list[str]) -> list[tuple[str, int]]:
    """(label, offset) for every hit. ASCII case folding, as the Windows scan."""
    hits: list[tuple[str, int]] = []
    for f in forbidden:
        if not f:
            continue
        for label, enc in ((f"'{f}' (UTF-8)", f.encode("utf-8")), (f"'{f}' (UTF-16LE)", f.encode("utf-16-le"))):
            hits += [(label, o) for o in find_all(data, enc, True)]
    for h in pixel_hex:
        clean = "".join(c for c in h if c in "0123456789abcdefABCDEF")
        if len(clean) < 2 or len(clean) % 2:
            raise ValueError(f"bad hex: {h}")
        hits += [(f"pixel run {clean[:16]}...", o) for o in find_all(data, bytes.fromhex(clean), False)]
    return hits


def cmd_scan(args: argparse.Namespace) -> int:
    failed = False
    for name in args.dump:
        path = Path(name)
        if not path.is_file():
            print(f"no such file: {path}")
            return 2
        data = path.read_bytes()
        if len(data) >= 32 and struct.unpack_from("<I", data, 0)[0] == 0x504D444D:
            streams = struct.unpack_from("<I", data, 8)[0]
            marked = struct.unpack_from("<I", data, 16)[0] == SCRUB_MARKER
            print(f"{path}: minidump, {len(data)} bytes, {streams} streams, scrubbed marker: {marked}")
            if not marked:
                print("  FAIL: the app has not scrubbed this dump yet (relaunch it first)")
                failed = True
        try:
            hits = scan(data, args.forbid, args.pixel_hex)
        except ValueError as e:
            print(e)
            return 2
        for label, offset in hits[: args.max_hits]:
            print(f"  HIT {label} at 0x{offset:X}")
        if hits:
            failed = True
    if failed:
        print("FAIL")
        return 1
    print(f"PASS: {len(args.forbid)} forbidden string(s) x 2 encodings, "
          f"{len(args.pixel_hex)} pixel pattern(s): no hits")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("make", help="write the corrupted-RAW canary (a copy)")
    m.add_argument("--source", help="a real camera file to copy and corrupt")
    m.add_argument("--out", default="/tmp/mv-crash-canary")
    m.add_argument("--width", type=int, default=256)
    m.add_argument("--height", type=int, default=256)
    m.set_defaults(func=cmd_make)
    s = sub.add_parser("scan", help="scan a (scrubbed) minidump for leaks")
    s.add_argument("dump", nargs="+")
    s.add_argument("--forbid", action="append", default=[])
    s.add_argument("--pixel-hex", action="append", default=[])
    s.add_argument("--max-hits", type=int, default=10)
    s.set_defaults(func=cmd_scan)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
