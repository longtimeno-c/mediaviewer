# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the fuzz seeds and the hand-crafted broken files (PR 7, plan/09).

    python tools/testmedia/make-seeds.py            # writes tests/data/seeds and tests/data/broken
    python tools/testmedia/make-seeds.py --check    # regenerate into a temp dir and list differences

Every file is synthesised here from code: no camera, no downloaded sample, so
the whole set is licence-clean and can live in git (the real corpus cannot —
plan/09). Output is small on purpose (target < 1 MB for everything): seeds are
starting points for libFuzzer and the broken-corpus test, not a format corpus.

Requirements (dev tool only, never shipped):
  * Pillow >= 11.3 with AVIF support (JPEG, PNG, APNG, BMP, GIF, WebP, AVIF).
  * pillow-heif for HEIC. Install it into a throwaway venv, not the system
    Python:  python -m venv .venv-seeds && .venv-seeds/Scripts/pip install pillow pillow-heif
    pillow-heif's wheel carries an x265 encoder. That is fine for making a few
    test bytes on a developer box; it never enters vcpkg.json or the product
    (plan/11 forbids bundling a software HEVC encoder, not reading its output).
    Without pillow-heif the HEIC seeds are skipped with a warning.
TIFF (tiled, 16-bit), DNG and ICO are written by hand below so their structure
is exact and documented.
"""
from __future__ import annotations

import argparse
import io
import math
import struct
import sys
import tempfile
import zlib
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------------------
# Pixel sources — deterministic gradients with some structure, so encoders
# produce non-trivial entropy-coded data (a flat image fuzzes badly).
# ---------------------------------------------------------------------------
def gradient_rgb(w: int, h: int, phase: int = 0) -> Image.Image:
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (
                (x * 255 // max(1, w - 1) + phase) & 0xFF,
                (y * 255 // max(1, h - 1)) & 0xFF,
                ((x ^ y) * 16 + phase * 3) & 0xFF,
            )
    return img


def gradient_rgba(w: int, h: int, phase: int = 0) -> Image.Image:
    img = gradient_rgb(w, h, phase).convert("RGBA")
    px = img.load()
    for y in range(h):
        for x in range(w):
            r, g, b, _ = px[x, y]
            px[x, y] = (r, g, b, 255 if (x + y) % 5 else 96)
    return img


def rgb16_rows(w: int, h: int) -> bytes:
    """Big-endian 16-bit RGB samples."""
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += struct.pack(">HHH", x * 65535 // max(1, w - 1), y * 65535 // max(1, h - 1),
                               ((x * 7 + y * 13) * 977) & 0xFFFF)
    return bytes(out)


def srgb_icc() -> bytes:
    from PIL import ImageCms
    return ImageCms.ImageCmsProfile(ImageCms.createProfile("sRGB")).tobytes()


def save(img: Image.Image, fmt: str, **kw) -> bytes:
    buf = io.BytesIO()
    img.save(buf, fmt, **kw)
    return buf.getvalue()


# ---------------------------------------------------------------------------
# PNG
# ---------------------------------------------------------------------------
def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png_rgb16(w: int, h: int) -> bytes:
    raw = rgb16_rows(w, h)
    stride = w * 6
    scan = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) +
            png_chunk(b"IDAT", zlib.compress(scan, 9)) + png_chunk(b"IEND", b""))


def png_header_only(w: int, h: int, idat: bytes) -> bytes:
    """A PNG whose IHDR claims w x h but whose IDAT is a few bytes."""
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr) + png_chunk(b"IDAT", idat) +
            png_chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# TIFF (hand-built, little-endian). Entries: (tag, type, values)
#   type 3 SHORT, 4 LONG, 5 RATIONAL, 1 BYTE, 2 ASCII, 7 UNDEFINED, 16 LONG8 (unused)
# ---------------------------------------------------------------------------
TYPE_SIZE = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 7: 1, 10: 8, 12: 8}


class TiffWriter:
    """Minimal classic-TIFF builder: IFDs with out-of-line values and blobs."""

    def __init__(self) -> None:
        self.buf = bytearray(b"II*\x00\x00\x00\x00\x00")

    def align(self) -> None:
        if len(self.buf) % 2:
            self.buf += b"\x00"

    def blob(self, data: bytes) -> int:
        self.align()
        off = len(self.buf)
        self.buf += data
        return off

    @staticmethod
    def pack_values(typ: int, values) -> bytes:
        if typ in (1, 7):
            return bytes(values)
        if typ == 2:
            return values if isinstance(values, bytes) else values.encode() + b"\x00"
        if typ == 3:
            return b"".join(struct.pack("<H", v) for v in values)
        if typ == 4:
            return b"".join(struct.pack("<I", v) for v in values)
        if typ in (5, 10):
            return b"".join(struct.pack("<II", n, d) for n, d in values)
        if typ == 12:
            return b"".join(struct.pack("<d", v) for v in values)
        raise ValueError(typ)

    def ifd(self, entries, next_ifd: int = 0) -> tuple[int, dict[int, int]]:
        """Writes one IFD; returns (offset, {tag: offset of its 4-byte value field})."""
        entries = sorted(entries, key=lambda e: e[0])
        payloads = []
        for tag, typ, values in entries:
            data = self.pack_values(typ, values)
            count = len(data) // TYPE_SIZE[typ]
            payloads.append((tag, typ, count, data))
        # out-of-line data first
        inline = {}
        for i, (tag, typ, count, data) in enumerate(payloads):
            if len(data) > 4:
                inline[i] = struct.pack("<I", self.blob(data))
            else:
                inline[i] = data.ljust(4, b"\x00")
        self.align()
        off = len(self.buf)
        self.buf += struct.pack("<H", len(payloads))
        value_fields = {}
        for i, (tag, typ, count, _data) in enumerate(payloads):
            value_fields[tag] = len(self.buf) + 8
            self.buf += struct.pack("<HHI", tag, typ, count) + inline[i]
        self.buf += struct.pack("<I", next_ifd)
        return off, value_fields

    def set_first_ifd(self, off: int) -> None:
        self.buf[4:8] = struct.pack("<I", off)

    def patch_u32(self, at: int, value: int) -> None:
        self.buf[at:at + 4] = struct.pack("<I", value)


def tiff_rgb(w: int, h: int, pixels: bytes, bits: int = 8, tiled: bool = False,
             tile: int = 16) -> bytes:
    """Uncompressed chunky RGB, strips (one per 8 rows) or tiles."""
    t = TiffWriter()
    spp = 3
    bps = bits // 8
    entries = [
        (256, 4, [w]), (257, 4, [h]), (258, 3, [bits] * 3), (259, 3, [1]),
        (262, 3, [2]), (277, 3, [spp]), (284, 3, [1]),
        (282, 5, [(72, 1)]), (283, 5, [(72, 1)]), (296, 3, [2]),
    ]
    row = w * spp * bps
    if not tiled:
        rps = 8
        offsets, counts = [], []
        for y0 in range(0, h, rps):
            chunk = pixels[y0 * row:min(h, y0 + rps) * row]
            offsets.append(t.blob(chunk))
            counts.append(len(chunk))
        entries += [(278, 4, [rps]), (273, 4, offsets), (279, 4, counts)]
    else:
        across, down = math.ceil(w / tile), math.ceil(h / tile)
        offsets, counts = [], []
        px = bps * spp
        for ty in range(down):
            for tx in range(across):
                tb = bytearray(tile * tile * px)
                for y in range(tile):
                    sy = ty * tile + y
                    if sy >= h:
                        break
                    for x in range(tile):
                        sx = tx * tile + x
                        if sx >= w:
                            break
                        s = (sy * w + sx) * px
                        tb[(y * tile + x) * px:(y * tile + x + 1) * px] = pixels[s:s + px]
                offsets.append(t.blob(bytes(tb)))
                counts.append(len(tb))
        entries += [(322, 4, [tile]), (323, 4, [tile]), (324, 4, offsets), (325, 4, counts)]
    off, _ = t.ifd(entries)
    t.set_first_ifd(off)
    return bytes(t.buf)


def rgb16_le(w: int, h: int) -> bytes:
    be = rgb16_rows(w, h)
    return b"".join(be[i + 1:i + 2] + be[i:i + 1] for i in range(0, len(be), 2))


def dng(w: int = 32, h: int = 24) -> bytes:
    """A tiny, structurally real DNG 1.4: IFD0 is an 8-bit RGB thumbnail
    (NewSubfileType=1), SubIFD 0 the uncompressed 16-bit RGGB Bayer mosaic.
    Enough for LibRaw's identify + unpack; the colour matrices are identity-ish
    sRGB-like numbers, not a real camera's."""
    t = TiffWriter()
    # Bayer RGGB from a gradient, 12-bit values stored in 16-bit samples.
    cfa = bytearray()
    for y in range(h):
        for x in range(w):
            r = x * 4095 // (w - 1)
            g = y * 4095 // (h - 1)
            b = ((x ^ y) * 131) & 0xFFF
            v = (r if (y % 2 == 0 and x % 2 == 0) else b if (y % 2 == 1 and x % 2 == 1) else g)
            cfa += struct.pack("<H", v)
    raw_off = t.blob(bytes(cfa))
    tw, th = w // 2, h // 2
    thumb = gradient_rgb(tw, th).tobytes()
    thumb_off = t.blob(thumb)
    sub, _ = t.ifd([
        (254, 4, [0]), (256, 4, [w]), (257, 4, [h]), (258, 3, [16]), (259, 3, [1]),
        (262, 3, [32803]), (273, 4, [raw_off]), (277, 3, [1]), (278, 4, [h]),
        (279, 4, [len(cfa)]), (284, 3, [1]),
        (33421, 3, [2, 2]), (33422, 1, [0, 1, 1, 2]),          # CFARepeatPatternDim, CFAPattern
        (50710, 1, [0, 1, 2]), (50711, 3, [1]),                 # CFAPlaneColor, CFALayout
        (50714, 3, [0]), (50717, 3, [4095]),                    # BlackLevel, WhiteLevel
        (50829, 4, [0, 0, h, w]),                               # ActiveArea
    ])
    cm = [(1, 1), (0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (0, 1), (0, 1), (1, 1)]
    ifd0, fields = t.ifd([
        (254, 4, [1]), (256, 4, [tw]), (257, 4, [th]), (258, 3, [8, 8, 8]), (259, 3, [1]),
        (262, 3, [2]), (271, 2, "MediaViewer"), (272, 2, "Seed DNG"), (273, 4, [thumb_off]),
        (274, 3, [1]), (277, 3, [3]), (278, 4, [th]), (279, 4, [len(thumb)]), (284, 3, [1]),
        (330, 4, [sub]),                                        # SubIFDs
        (50706, 1, [1, 4, 0, 0]), (50707, 1, [1, 1, 0, 0]),     # DNGVersion, BackwardVersion
        (50708, 2, "MediaViewer Seed"),                         # UniqueCameraModel
        (50721, 10, [(1, 1), (0, 1), (0, 1), (0, 1), (1, 1), (0, 1), (0, 1), (0, 1), (1, 1)]),
        (50728, 5, [(1, 1), (1, 1), (1, 1)]),                   # AsShotNeutral
        (50778, 3, [21]),                                       # CalibrationIlluminant1 = D65
    ])
    del cm, fields
    t.set_first_ifd(ifd0)
    return bytes(t.buf)


# ---------------------------------------------------------------------------
# ICO (hand-built): one 16x16 32-bpp BMP entry, one 32x32 PNG entry.
# ---------------------------------------------------------------------------
def ico_bmp_png() -> bytes:
    small = gradient_rgba(16, 16)
    # BITMAPINFOHEADER with doubled height (XOR + AND masks), bottom-up BGRA.
    bgra = bytearray()
    px = small.load()
    for y in reversed(range(16)):
        for x in range(16):
            r, g, b, a = px[x, y]
            bgra += bytes((b, g, r, a))
    mask_row = ((16 + 31) // 32) * 4
    dib = struct.pack("<IiiHHIIiiII", 40, 16, 32, 1, 32, 0, len(bgra) + mask_row * 16, 0, 0, 0, 0)
    bmp_entry = dib + bytes(bgra) + b"\x00" * (mask_row * 16)
    png_entry = save(gradient_rgba(32, 32, 40), "PNG", optimize=True)
    header = struct.pack("<HHH", 0, 1, 2)
    first = 6 + 16 * 2
    d1 = struct.pack("<BBBBHHII", 16, 16, 0, 0, 1, 32, len(bmp_entry), first)
    d2 = struct.pack("<BBBBHHII", 32, 32, 0, 0, 1, 32, len(png_entry), first + len(bmp_entry))
    return header + d1 + d2 + bmp_entry + png_entry


# ---------------------------------------------------------------------------
# GIF with many frames (hand-built; Pillow dedupes identical frames).
# ---------------------------------------------------------------------------
def gif_many_frames(frames: int, screen_w: int, screen_h: int) -> bytes:
    out = bytearray(b"GIF89a" + struct.pack("<HHBBB", screen_w, screen_h, 0x80, 0, 0))
    out += bytes((0, 0, 0, 255, 255, 255))  # 2-colour global table
    out += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"
    # 1x1 frame at (0,0): LZW min code size 2, codes clear(4) 0 eoi(5) -> 0x44 0x01
    frame = b"\x21\xf9\x04\x00\x00\x00\x00\x00" + b"\x2c" + struct.pack("<HHHHB", 0, 0, 1, 1, 0) + b"\x02\x02\x44\x01\x00"
    out += frame * frames
    out += b"\x3b"
    return bytes(out)


# ---------------------------------------------------------------------------
def build(out_root: Path) -> list[str]:
    seeds = out_root / "seeds"
    broken = out_root / "broken"
    warnings: list[str] = []
    files: dict[str, bytes] = {}

    rgb = gradient_rgb(48, 32)
    rgba = gradient_rgba(48, 32)

    # JPEG
    files["jpeg/baseline.jpg"] = save(rgb, "JPEG", quality=85)
    files["jpeg/progressive.jpg"] = save(rgb, "JPEG", quality=85, progressive=True)
    files["jpeg/icc.jpg"] = save(rgb, "JPEG", quality=85, icc_profile=srgb_icc())
    files["jpeg/gray_420_restart.jpg"] = save(rgb.convert("L"), "JPEG", quality=70)
    files["jpeg/subsampling_444.jpg"] = save(gradient_rgb(17, 13), "JPEG", quality=90, subsampling=0)

    # PNG
    files["png/rgba8.png"] = save(rgba, "PNG", optimize=True)
    files["png/palette.png"] = save(rgb.quantize(16), "PNG", optimize=True)
    files["png/gray16.png"] = save(Image.frombytes("I;16", (32, 24), bytes((i * 37) & 0xFF for i in range(32 * 24 * 2))), "PNG")
    files["png/rgb16.png"] = png_rgb16(24, 16)
    apng_frames = [gradient_rgba(24, 16, p) for p in (0, 60, 120)]
    files["png/apng.png"] = save(apng_frames[0], "PNG", save_all=True, append_images=apng_frames[1:],
                                 duration=[40, 80, 120], loop=0, disposal=[0, 1, 2], blend=[0, 1, 0])

    # BMP
    files["bmp/rgb24.bmp"] = save(gradient_rgb(19, 11), "BMP")
    files["bmp/rgba32.bmp"] = save(gradient_rgba(16, 9), "BMP")

    # GIF
    gif_frames = [gradient_rgb(24, 16, p).quantize(32) for p in (0, 50, 100, 150)]
    files["gif/animated.gif"] = save(gif_frames[0], "GIF", save_all=True, append_images=gif_frames[1:],
                                     duration=[30, 60, 90, 120], loop=0, disposal=2)
    files["gif/still.gif"] = save(gradient_rgb(20, 20).quantize(64), "GIF")

    # WebP
    files["webp/lossy.webp"] = save(rgb, "WEBP", quality=60, method=4)
    files["webp/lossless.webp"] = save(rgba, "WEBP", lossless=True)
    webp_frames = [gradient_rgba(24, 16, p) for p in (0, 70, 140)]
    files["webp/animated.webp"] = save(webp_frames[0], "WEBP", save_all=True, append_images=webp_frames[1:],
                                       duration=[50, 100, 150], loop=0, lossless=True)

    # TIFF
    files["tiff/strip_rgb8.tif"] = tiff_rgb(24, 20, gradient_rgb(24, 20).tobytes())
    files["tiff/strip_lzw.tif"] = save(gradient_rgb(24, 20), "TIFF", compression="tiff_lzw")
    files["tiff/strip_deflate_rgba.tif"] = save(gradient_rgba(20, 12), "TIFF", compression="tiff_adobe_deflate")
    files["tiff/tiled_rgb8.tif"] = tiff_rgb(40, 24, gradient_rgb(40, 24).tobytes(), tiled=True, tile=16)
    files["tiff/strip_rgb16.tif"] = tiff_rgb(16, 12, rgb16_le(16, 12), bits=16)
    files["tiff/multipage.tif"] = save(gradient_rgb(12, 12), "TIFF", save_all=True,
                                       append_images=[gradient_rgb(12, 12, 90)], compression="packbits")

    # ICO
    files["ico/bmp_and_png.ico"] = ico_bmp_png()

    # AVIF (Pillow's own plugin)
    try:
        files["avif/still.avif"] = save(rgb, "AVIF", quality=50, speed=8)
        av_frames = [gradient_rgb(24, 16, p) for p in (0, 80, 160)]
        files["avif/animated.avif"] = save(av_frames[0], "AVIF", save_all=True, append_images=av_frames[1:],
                                           duration=[100, 100, 100], quality=40, speed=8)
    except Exception as e:  # noqa: BLE001 — tool, report and carry on
        warnings.append(f"AVIF seeds skipped: {e}")

    # HEIC
    try:
        import pillow_heif
        heif = pillow_heif.from_pillow(rgb)
        buf = io.BytesIO()
        heif.save(buf, quality=50)
        files["heic/8bit.heic"] = buf.getvalue()
        hi = pillow_heif.from_bytes(mode="RGB;16", size=(24, 16), data=b"".join(
            struct.pack("<HHH", (x * 65535 // 23), (y * 65535 // 15), 32768) for y in range(16) for x in range(24)))
        buf = io.BytesIO()
        hi.save(buf, quality=50)
        files["heic/10bit.heic"] = buf.getvalue()
    except ImportError:
        warnings.append("HEIC seeds skipped: pillow-heif not installed (see module docstring)")

    # RAW
    files["raw/tiny.dng"] = dng()

    # --- Hand-crafted nasties (tests/data/broken) --------------------------
    tiny_idat = zlib.compress(b"\x00" * 64)
    nasty: dict[str, bytes] = {
        "png_60000x60000_tiny_idat.png": png_header_only(60000, 60000, tiny_idat),
        "png_16000x16000_tiny_idat.png": png_header_only(16000, 16000, tiny_idat),
        "png_65535x1_truncated_idat.png": png_header_only(65535, 1, tiny_idat[:5]),
        "tiff_60000x60000_one_lzw_strip.tif": bytes(_tiff_bomb(60000, 60000)),
        "tiff_16000x16000_strip_past_eof.tif": bytes(_tiff_bomb(16000, 16000, strip_len=0xFFFFFF00)),
        "tiff_ifd_loop.tif": _tiff_ifd_loop(),
        "gif_20000_frames_1x1.gif": gif_many_frames(20000, 1, 1),
        "gif_300_frames_4096x4096_screen.gif": gif_many_frames(300, 4096, 4096),
        "ico_overlapping_entries.ico": _ico_overlap(),
        "ico_offset_past_eof.ico": _ico_past_eof(),
        "bmp_65535x65535_no_pixels.bmp": _bmp_claim(65535, 65535),
        "bmp_16000x16000_no_pixels.bmp": _bmp_claim(16000, 16000),
        "jpeg_sof_65500x65500.jpg": _jpeg_sof_patch(files["jpeg/baseline.jpg"], 65500, 65500),
        "webp_vp8x_16383x16383_canvas.webp": _webp_vp8x_canvas(files["webp/animated.webp"], 16383, 16383),
        "dng_cfa_strip_past_eof.dng": _dng_strip_past_eof(files["raw/tiny.dng"]),
    }
    if "heic/8bit.heic" in files:
        nasty["heic_ispe_65535x65535.heic"] = _ispe_patch(files["heic/8bit.heic"], 65535, 65535)
    if "avif/still.avif" in files:
        nasty["avif_ispe_65535x65535.avif"] = _ispe_patch(files["avif/still.avif"], 65535, 65535)

    for rel, data in files.items():
        if data is None:
            continue
        p = seeds / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    broken.mkdir(parents=True, exist_ok=True)
    for name, data in nasty.items():
        (broken / name).write_bytes(data)
    return warnings


def _tiff_bomb(w: int, h: int, strip_len: int | None = None) -> bytearray:
    t = TiffWriter()
    strip = t.blob(b"\x80\x00\x00\x00")  # a few LZW-ish bytes
    entries = [(256, 4, [w]), (257, 4, [h]), (258, 3, [8, 8, 8]), (259, 3, [5]), (262, 3, [2]),
               (273, 4, [strip]), (277, 3, [3]), (278, 4, [h]), (279, 4, [strip_len or 4]), (284, 3, [1])]
    off, _ = t.ifd(entries)
    t.set_first_ifd(off)
    return t.buf


def _tiff_ifd_loop() -> bytes:
    t = TiffWriter()
    strip = t.blob(gradient_rgb(4, 4).tobytes())
    entries = [(256, 4, [4]), (257, 4, [4]), (258, 3, [8, 8, 8]), (259, 3, [1]), (262, 3, [2]),
               (273, 4, [strip]), (277, 3, [3]), (278, 4, [4]), (279, 4, [48]), (284, 3, [1])]
    off, _ = t.ifd(entries)
    t.set_first_ifd(off)
    # next-IFD pointer -> itself
    count = struct.unpack_from("<H", t.buf, off)[0]
    t.patch_u32(off + 2 + 12 * count, off)
    return bytes(t.buf)


def _ico_overlap() -> bytes:
    good = ico_bmp_png()
    b = bytearray(good)
    # Both entries point at the directory itself (offset 6) with a huge size.
    for i in range(2):
        struct.pack_into("<II", b, 6 + 16 * i + 8, 0x7FFFFFFF, 6)
    return bytes(b)


def _ico_past_eof() -> bytes:
    b = bytearray(ico_bmp_png())
    struct.pack_into("<II", b, 6 + 8, 4096, len(b) - 8)
    return bytes(b)


def _bmp_claim(w: int, h: int) -> bytes:
    return b"BM" + struct.pack("<IHHI", 54, 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, 0, 0, 0, 0, 0)


def _jpeg_sof_patch(jpg: bytes, w: int, h: int) -> bytes:
    b = bytearray(jpg)
    i = 2
    while i + 4 <= len(b):
        if b[i] != 0xFF:
            break
        marker = b[i + 1]
        seg = struct.unpack_from(">H", b, i + 2)[0]
        if marker in (0xC0, 0xC1, 0xC2):
            struct.pack_into(">HH", b, i + 5, h, w)
            return bytes(b[:i + 2 + seg + 64])  # header plus a sliver of scan
        i += 2 + seg
    raise RuntimeError("no SOF")


def _webp_vp8x_canvas(webp: bytes, w: int, h: int) -> bytes:
    b = bytearray(webp)
    assert b[12:16] == b"VP8X"
    b[24:27] = (w - 1).to_bytes(3, "little")
    b[27:30] = (h - 1).to_bytes(3, "little")
    return bytes(b)


def _ispe_patch(data: bytes, w: int, h: int) -> bytes:
    b = bytearray(data)
    i = b.find(b"ispe")
    if i < 0:
        raise RuntimeError("no ispe box")
    struct.pack_into(">II", b, i + 8, w, h)  # type(4) version/flags(4) width height
    return bytes(b)


def _dng_strip_past_eof(data: bytes) -> bytes:
    b = bytearray(data)
    # Find the SubIFD's StripByteCounts (tag 279 type LONG count 1) holding the CFA length.
    cfa_len = 32 * 24 * 2
    needle = struct.pack("<HHII", 279, 4, 1, cfa_len)
    i = b.find(needle)
    if i < 0:
        raise RuntimeError("no CFA StripByteCounts")
    struct.pack_into("<I", b, i + 8, 0x7FFFFFF0)
    return bytes(b)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=REPO / "tests" / "data")
    ap.add_argument("--check", action="store_true", help="generate into a temp dir and compare")
    args = ap.parse_args()

    if args.check:
        with tempfile.TemporaryDirectory() as tmp:
            warnings = build(Path(tmp))
            diffs = 0
            for p in sorted(Path(tmp).rglob("*")):
                if p.is_file():
                    rel = p.relative_to(tmp)
                    committed = args.out / rel
                    if not committed.exists() or committed.read_bytes() != p.read_bytes():
                        print(f"differs: {rel}")
                        diffs += 1
            for w in warnings:
                print(f"warning: {w}", file=sys.stderr)
            print(f"{diffs} file(s) differ (encoder versions change bytes; that alone is not a bug)")
            return 0

    warnings = build(args.out)
    total = sum(p.stat().st_size for p in args.out.rglob("*") if p.is_file())
    count = sum(1 for p in args.out.rglob("*") if p.is_file())
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    print(f"wrote {count} files, {total} bytes under {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
