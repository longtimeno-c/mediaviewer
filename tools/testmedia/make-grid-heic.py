# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make a phone-shaped HEIC for the perf bench: a 4032x3024 grid of 512x512
HEVC tiles (8x6, cropped), a 320x240 thumbnail item, and a Display P3 ICC --
the layout an iPhone writes. libheif's example.heic is one small tile, so the
bench could not see grid decode or thumbnail-first at camera size.

    python tools/testmedia/make-grid-heic.py            # tools/testmedia/heif/grid-12mp.heic
    python tools/testmedia/make-grid-heic.py --fixture  # tests/data/heif/grid_thumb*.heic

The bench picture is libheif's example.heic (fetch-heif.ps1 first), upscaled
with noise so the tiles cost what a photo costs (~2.8 MB, like a 12 MP iPhone
file). Generated, never committed (tools/testmedia/ is gitignored, docs/design/09).

--fixture writes the same layout at 120x90 (2x2 tiles of 64, a 32x24
thumbnail) from ffmpeg's testsrc2, so it is licence-clean and lives in git:
grid_thumb.heic, and grid_thumb_square.heic whose 24x24 thumbnail is not the
image's shape (first pixel must refuse it).

Requirements (dev tool only, never shipped): an ffmpeg on PATH with libx265.
Making test bytes with x265 on a developer box is fine; it never enters
vcpkg.json or the product (docs/design/11, as make-seeds.py).
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(HERE, "heif", "libheif-example.heic")
P3_FIXTURE = os.path.join(REPO, "tests", "data", "heif", "p3_icc.heic")


class Spec:
    def __init__(self, out, w, h, tile, thumb_w, thumb_h, crf, source, noise):
        self.out, self.w, self.h, self.tile = out, w, h, tile
        self.cols, self.rows = (w + tile - 1) // tile, (h + tile - 1) // tile
        self.thumb_w, self.thumb_h, self.crf = thumb_w, thumb_h, crf
        self.source, self.noise = source, noise


BENCH = Spec(os.path.join(HERE, "heif", "grid-12mp.heic"), 4032, 3024, 512, 320, 240, "14",
             ["-i", SRC], 9)
TESTSRC = ["-f", "lavfi", "-i", "testsrc2=size=480x360:rate=1"]
FIXTURES = [
    Spec(os.path.join(REPO, "tests", "data", "heif", "grid_thumb.heic"), 120, 90, 64, 32, 24, "30",
         TESTSRC, 0),
    Spec(os.path.join(REPO, "tests", "data", "heif", "grid_thumb_square.heic"), 120, 90, 64, 24, 24,
         "30", TESTSRC, 0),
]


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def encode(spec, vf, frames, out_mp4):
    # All-intra, full range, BT.601 matrix: what a phone's tiles are.
    ffmpeg(*spec.source, "-vf", vf, "-frames:v", str(frames), "-c:v", "libx265", "-crf", spec.crf,
           "-preset", "slow", "-pix_fmt", "yuv420p", "-color_range", "pc",
           "-colorspace", "smpte170m", "-color_primaries", "bt709", "-color_trc", "iec61966-2-1",
           "-x265-params", "keyint=1:log-level=error", "-tag:v", "hvc1", out_mp4)


# --- reading the MP4 ffmpeg wrote --------------------------------------------

def boxes(buf, start, end):
    i = start
    while i + 8 <= end:
        size, kind = struct.unpack(">I4s", buf[i:i + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", buf[i + 8:i + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - i
        yield kind.decode("latin1"), i, i + hdr, i + size
        i += size


def find(buf, path, start=0, end=None):
    end = len(buf) if end is None else end
    head, *rest = path
    for kind, _, body, stop in boxes(buf, start, end):
        if kind == head:
            return (body, stop) if not rest else find(buf, rest, body, stop)
    raise SystemExit(f"no {head} box")


def read_samples(mp4):
    """(hvcC box bytes, [sample bytes]) of the one video track."""
    buf = open(mp4, "rb").read()
    stbl = find(buf, ["moov", "trak", "mdia", "minf", "stbl"])
    body, stop = find(buf, ["stsd"], *stbl)
    entry = body + 8  # version/flags + entry_count
    # hvc1 sample entry: 8 header + 78 visual sample entry fields, then child boxes.
    esize = struct.unpack(">I", buf[entry:entry + 4])[0]
    hvcc = None
    for kind, b0, _, b1 in boxes(buf, entry + 8 + 78, entry + esize):
        if kind == "hvcC":
            hvcc = buf[b0:b1]
    if hvcc is None:
        raise SystemExit("no hvcC")
    b, _ = find(buf, ["stsz"], *stbl)
    fixed, count = struct.unpack(">II", buf[b + 4:b + 12])
    sizes = [fixed] * count if fixed else list(struct.unpack(f">{count}I", buf[b + 12:b + 12 + 4 * count]))
    try:
        b, _ = find(buf, ["stco"], *stbl)
        n = struct.unpack(">I", buf[b + 4:b + 8])[0]
        chunks = list(struct.unpack(f">{n}I", buf[b + 8:b + 8 + 4 * n]))
    except SystemExit:
        b, _ = find(buf, ["co64"], *stbl)
        n = struct.unpack(">I", buf[b + 4:b + 8])[0]
        chunks = list(struct.unpack(f">{n}Q", buf[b + 8:b + 8 + 8 * n]))
    b, _ = find(buf, ["stsc"], *stbl)
    n = struct.unpack(">I", buf[b + 4:b + 8])[0]
    runs = [struct.unpack(">III", buf[b + 8 + 12 * k:b + 20 + 12 * k]) for k in range(n)]
    samples, s = [], 0
    for c in range(len(chunks)):
        per = next(r[1] for r in reversed(runs) if r[0] <= c + 1)
        off = chunks[c]
        for _ in range(per):
            if s >= count:
                break
            samples.append(buf[off:off + sizes[s]])
            off += sizes[s]
            s += 1
    return hvcc, samples


# --- writing the HEIF ----------------------------------------------------------

def box(kind, payload):
    return struct.pack(">I4s", 8 + len(payload), kind.encode()) + payload


def fullbox(kind, version, flags, payload):
    return box(kind, struct.pack(">I", (version << 24) | flags) + payload)


def p3_icc():
    d = open(P3_FIXTURE, "rb").read()
    i = d.find(b"colrprof")
    size = struct.unpack(">I", d[i - 4:i])[0]
    return d[i + 8:i - 4 + size]


def build(spec, tile_hvcc, tiles, thumb_hvcc, thumb, icc):
    grid_id, thumb_id = 1, 2
    tile_ids = list(range(3, 3 + len(tiles)))
    grid_data = struct.pack(">BBBBHH", 0, 0, spec.rows - 1, spec.cols - 1, spec.w, spec.h)
    items = [(grid_id, "grid", grid_data), (thumb_id, "hvc1", thumb)] + \
            [(tid, "hvc1", t) for tid, t in zip(tile_ids, tiles)]

    def meta(mdat_payload_start):
        hdlr = fullbox("hdlr", 0, 0, struct.pack(">I4s12x", 0, b"pict") + b"\0")
        pitm = fullbox("pitm", 0, 0, struct.pack(">H", grid_id))
        infes = b"".join(fullbox("infe", 2, 1 if iid in tile_ids else 0,
                                 struct.pack(">HH4s", iid, 0, kind.encode()) + b"\0")
                         for iid, kind, _ in items)
        iinf = fullbox("iinf", 0, 0, struct.pack(">H", len(items)) + infes)
        dimg = box("dimg", struct.pack(">HH", grid_id, len(tile_ids)) +
                   b"".join(struct.pack(">H", t) for t in tile_ids))
        thmb = box("thmb", struct.pack(">HHH", thumb_id, 1, grid_id))
        iref = fullbox("iref", 0, 0, dimg + thmb)
        ispe = lambda w, h: fullbox("ispe", 0, 0, struct.pack(">II", w, h))
        colr = box("colr", b"prof" + icc)
        ipco = box("ipco", tile_hvcc + ispe(spec.tile, spec.tile) + thumb_hvcc +
                   ispe(spec.thumb_w, spec.thumb_h) + ispe(spec.w, spec.h) + colr)
        # Property indices are 1-based; the top bit marks essential.
        assoc = {grid_id: [5, 6], thumb_id: [0x80 | 3, 4, 6]}
        for t in tile_ids:
            assoc[t] = [0x80 | 1, 2]
        ipma_body = struct.pack(">I", len(items)) + b"".join(
            struct.pack(">HB", iid, len(assoc[iid])) + bytes(assoc[iid]) for iid, _, _ in items)
        ipma = fullbox("ipma", 0, 0, ipma_body)
        iprp = box("iprp", ipco + ipma)
        off = mdat_payload_start
        iloc_items = b""
        for iid, _, data in items:
            iloc_items += struct.pack(">HHHII", iid, 0, 1, off, len(data))
            off += len(data)
        iloc = fullbox("iloc", 0, 0, struct.pack(">BBH", 0x44, 0x00, len(items)) + iloc_items)
        return fullbox("meta", 0, 0, hdlr + pitm + iinf + iref + iprp + iloc)

    ftyp = box("ftyp", b"heic" + struct.pack(">I", 0) + b"mif1heic")
    probe = meta(0)
    start = len(ftyp) + len(probe) + 8
    payload = b"".join(d for _, _, d in items)
    return ftyp + meta(start) + box("mdat", payload)


def make(spec):
    with tempfile.TemporaryDirectory() as tmp:
        tiles_mp4 = os.path.join(tmp, "tiles.mp4")
        thumb_mp4 = os.path.join(tmp, "thumb.mp4")
        noise = f"noise=alls={spec.noise}:allf=t," if spec.noise else ""
        picture = (f"scale={spec.w}:{spec.h}:flags=lanczos,{noise}"
                   f"pad={spec.cols * spec.tile}:{spec.rows * spec.tile}:0:0,"
                   f"untile={spec.cols}x{spec.rows}")
        encode(spec, picture, spec.cols * spec.rows, tiles_mp4)
        encode(spec, f"scale={spec.thumb_w}:{spec.thumb_h}:flags=lanczos", 1, thumb_mp4)
        tile_hvcc, tiles = read_samples(tiles_mp4)
        thumb_hvcc, thumb = read_samples(thumb_mp4)
    if len(tiles) != spec.cols * spec.rows:
        raise SystemExit(f"expected {spec.cols * spec.rows} tiles, got {len(tiles)}")
    data = build(spec, tile_hvcc, tiles, thumb_hvcc, thumb[0], p3_icc())
    with open(spec.out, "wb") as f:
        f.write(data)
    print(f"wrote {spec.out}: {spec.w}x{spec.h}, {spec.cols}x{spec.rows} tiles, "
          f"thumbnail {spec.thumb_w}x{spec.thumb_h}, {len(data) / 1e6:.2f} MB")


def main():
    if "--fixture" in sys.argv[1:]:
        for spec in FIXTURES:
            make(spec)
        return 0
    if not os.path.exists(SRC):
        raise SystemExit(f"missing {SRC}: run tools/testmedia/fetch-heif.ps1 first")
    make(BENCH)
    return 0


if __name__ == "__main__":
    sys.exit(main())
