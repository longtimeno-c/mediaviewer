#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the MediaViewer app mark (plan/13-updates-and-telemetry.md, "Icon").

One mark, one .ico, used everywhere the OS shows the app:

    assets/icon/mediaviewer.ico      16 20 24 32 40 48 64 256 (256 is PNG inside)
    assets/icon/mediaviewer-256.png  the same mark, for About

The mark is a camera dump in one tile: a rounded photo tile with a mountain
ridge (photos) and a play triangle where the sun would be (video).

Standard library only, byte-for-byte reproducible:

    python tools/make-icon.py            # write both files
    python tools/make-icon.py --check    # fail if the committed files differ

Small sizes are hand-placed on the pixel grid rather than downscaled from the
256 master: at 16 px a downscale turns the ridge into mush and the triangle
into a blob. Geometry is given in pixels of the target size; vertical and
horizontal edges sit on pixel boundaries so they stay crisp.
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "assets" / "icon"
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)

# Tile gradient (top -> bottom) and the white of the content. Chosen to read on
# both the dark and the light taskbar; the canvas grey (33,35,42) would vanish.
TILE_TOP = (66, 160, 246)
TILE_BOTTOM = (40, 92, 214)
INK = (250, 251, 255)

# Hand-tuned geometry per size, in target pixels.
#   tile:     (x0, y0, x1, y1, corner radius)
#   frame:    inset of the photo inside the tile; the ridge is clipped to it so
#             the tile edge survives on a light taskbar
#   ridge:    polygon (clipped by the frame) — left peak, valley, right peak
#   play:     triangle (x_left, y_top, y_bottom, x_tip)
GEOMETRY: dict[int, dict] = {
    16: dict(tile=(0, 0, 16, 16, 3), frame=1,
             ridge=[(0, 16), (0, 13), (5, 8), (8, 11), (10, 9), (16, 15), (16, 16)],
             play=(9, 2, 8, 13.5)),
    20: dict(tile=(1, 1, 19, 19, 3.5), frame=1,
             ridge=[(1, 19), (1, 15), (7, 9), (10, 12), (12.5, 9.5), (19, 16), (19, 19)],
             play=(11, 3, 9, 16)),
    24: dict(tile=(1, 1, 23, 23, 4), frame=2,
             ridge=[(1, 23), (1, 18), (8, 11), (12, 15), (15, 12), (23, 20), (23, 23)],
             play=(13, 4, 11, 19)),
    32: dict(tile=(1, 1, 31, 31, 6), frame=2,
             ridge=[(1, 31), (1, 24), (11, 14), (16, 19), (20, 15), (31, 26), (31, 31)],
             play=(18, 5, 14, 25.5)),
}


def master(size: int) -> dict:
    """The 256 design, scaled to `size` and snapped where it matters."""
    s = size / 256.0

    def p(v: float) -> float:
        return round(v * s * 2) / 2  # half-pixel snap keeps diagonals balanced

    def e(v: float) -> float:
        return float(round(v * s))  # axis-aligned edges on whole pixels

    return dict(
        tile=(e(12), e(12), e(244), e(244), p(52)),
        frame=e(14),
        ridge=[(e(12), e(244)), (e(12), p(194)), (p(90), p(108)), (p(134), p(156)),
               (p(166), p(128)), (e(244), p(200)), (e(244), e(244))],
        play=(e(144), e(40), e(116), p(210)),
    )


def rounded_rect(x0, y0, x1, y1, r, steps=24):
    pts = []
    corners = ((x1 - r, y0 + r, -90), (x1 - r, y1 - r, 0),
               (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180))
    for cx, cy, start in corners:
        for i in range(steps + 1):
            a = math.radians(start + 90 * i / steps)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def coverage(poly, size, sub=16):
    """Even-odd polygon coverage per pixel: `sub` sample rows, exact spans."""
    cov = [0.0] * (size * size)
    n = len(poly)
    w = 1.0 / sub
    for py in range(size):
        row = py * size
        for k in range(sub):
            y = py + (k + 0.5) * w
            xs = []
            for i in range(n):
                ax, ay = poly[i]
                bx, by = poly[(i + 1) % n]
                if (ay <= y < by) or (by <= y < ay):
                    xs.append(ax + (y - ay) * (bx - ax) / (by - ay))
            xs.sort()
            for j in range(0, len(xs) - 1, 2):
                a = max(0.0, xs[j])
                b = min(float(size), xs[j + 1])
                if b <= a:
                    continue
                for px in range(int(a), min(size, int(math.ceil(b)))):
                    o = min(b, px + 1) - max(a, px)
                    if o > 0:
                        cov[row + px] += o * w
    return cov


def render(size: int) -> bytes:
    """Straight-alpha RGBA, top-down."""
    g = GEOMETRY.get(size) or master(size)
    tile = coverage(rounded_rect(*g["tile"]), size)
    x0, y0, x1, y1, r = g["tile"]
    f = g["frame"]
    frame = coverage(rounded_rect(x0 + f, y0 + f, x1 - f, y1 - f, max(0.5, r - f)), size)
    ridge = [min(a, b) for a, b in zip(coverage(g["ridge"], size), frame)]
    xl, yt, yb, xt = g["play"]
    play = coverage([(xl, yt), (xt, (yt + yb) / 2), (xl, yb)], size)
    out = bytearray(size * size * 4)
    for py in range(size):
        t = min(1.0, max(0.0, (py + 0.5 - y0) / max(1.0, y1 - y0)))
        base = [TILE_TOP[c] + (TILE_BOTTOM[c] - TILE_TOP[c]) * t for c in range(3)]
        for px in range(size):
            i = py * size + px
            tc = min(1.0, tile[i])
            if tc <= 0.0:
                continue
            ink = min(tc, min(1.0, ridge[i] + play[i]))
            o = i * 4
            for c in range(3):
                v = (base[c] * (tc - ink) + INK[c] * ink) / tc
                out[o + c] = max(0, min(255, int(v + 0.5)))
            out[o + 3] = max(0, min(255, int(tc * 255 + 0.5)))
    return bytes(out)


def png(size: int, rgba: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = b"".join(b"\x00" + rgba[y * size * 4:(y + 1) * size * 4] for y in range(size))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def dib(size: int, rgba: bytes) -> bytes:
    """32-bpp BITMAPINFOHEADER image with an AND mask, bottom-up BGRA."""
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            o = (y * size + x) * 4
            pixels += bytes((rgba[o + 2], rgba[o + 1], rgba[o], rgba[o + 3]))
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        row = bytearray(stride)
        for x in range(size):
            if rgba[(y * size + x) * 4 + 3] == 0:
                row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    return header + bytes(pixels) + bytes(mask)


def ico(images: dict[int, bytes]) -> bytes:
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for size in sorted(images):
        blob = png(size, images[size]) if size >= 256 else dib(size, images[size])
        dim = 0 if size >= 256 else size
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    return struct.pack("<HHH", 0, 1, len(images)) + b"".join(entries) + b"".join(blobs)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the committed icon files are out of date")
    args = ap.parse_args()

    images = {s: render(s) for s in ICO_SIZES}
    outputs = {
        OUT_DIR / "mediaviewer.ico": ico(images),
        OUT_DIR / "mediaviewer-256.png": png(256, images[256]),
    }
    if args.check:
        stale = [p for p, data in outputs.items() if not p.exists() or p.read_bytes() != data]
        for p in stale:
            print(f"out of date: {p.relative_to(ROOT)}", file=sys.stderr)
        return 1 if stale else 0
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for p, data in outputs.items():
        p.write_bytes(data)
        print(f"wrote {p.relative_to(ROOT)} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
