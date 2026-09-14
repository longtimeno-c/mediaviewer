"""Generate tiny licence-clean HEIC fixtures for tests/data/heif/.

Dev tool only: pillow-heif (and its bundled x265 encoder) run on the developer
machine to MAKE fixtures. Nothing here is a build dependency; the app decodes
with libheif + libde265 only.
"""
import math
import struct
import sys
from pathlib import Path

import numpy as np
import pillow_heif
from PIL import Image

pillow_heif.register_heif_opener()
OUT = Path(sys.argv[1])
OUT.mkdir(parents=True, exist_ok=True)

W, H = 64, 32


def pattern8() -> np.ndarray:
    """Left half red, right half blue, top-left 8x8 green: orientation-asymmetric."""
    a = np.zeros((H, W, 3), np.uint8)
    a[:, : W // 2] = (255, 0, 0)
    a[:, W // 2 :] = (0, 0, 255)
    a[:8, :8] = (0, 255, 0)
    return a


def s15(v: float) -> bytes:
    return struct.pack(">i", int(round(v * 65536)))


def xyz_tag(x, y, z) -> bytes:
    return b"XYZ " + b"\0" * 4 + s15(x) + s15(y) + s15(z)


def mluc(text: str) -> bytes:
    s = text.encode("utf-16-be")
    body = b"mluc" + b"\0" * 4 + struct.pack(">II", 1, 12) + b"enUS" + struct.pack(">II", len(s), 28) + s
    return body


def para_srgb() -> bytes:
    g, a, b, c, d = 2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045
    return b"para" + b"\0" * 4 + struct.pack(">HH", 3, 0) + b"".join(s15(v) for v in (g, a, b, c, d))


def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def mat_inv(m):
    (a, b, c), (d, e, f), (g, h, i) = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]


def icc_rgb(desc: str, prim, white=(0.3127, 0.3290)) -> bytes:
    """ICC v4 matrix/shaper, sRGB TRC. Written independently of the C++ synth so
    the nclx and ICC P3 fixtures cross-check each other."""
    def xyz(xy):
        x, y = xy
        return [x / y, 1.0, (1 - x - y) / y]

    cols = [xyz(p) for p in prim]
    m = [[cols[j][i] for j in range(3)] for i in range(3)]
    wv = xyz(white)
    s = [sum(mat_inv(m)[i][k] * wv[k] for k in range(3)) for i in range(3)]
    m = [[m[i][j] * s[j] for j in range(3)] for i in range(3)]
    brad = [[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]]
    d50 = [0.9642, 1.0, 0.8249]
    cw = [sum(brad[i][k] * wv[k] for k in range(3)) for i in range(3)]
    cd = [sum(brad[i][k] * d50[k] for k in range(3)) for i in range(3)]
    diag = [[cd[i] / cw[i] if i == j else 0 for j in range(3)] for i in range(3)]
    chad = mat_mul(mat_inv(brad), mat_mul(diag, brad))
    md50 = mat_mul(chad, m)
    tags = [
        (b"desc", mluc(desc)),
        (b"cprt", mluc("CC0")),
        (b"wtpt", xyz_tag(*d50)),
        (b"rXYZ", xyz_tag(md50[0][0], md50[1][0], md50[2][0])),
        (b"gXYZ", xyz_tag(md50[0][1], md50[1][1], md50[2][1])),
        (b"bXYZ", xyz_tag(md50[0][2], md50[1][2], md50[2][2])),
        (b"rTRC", para_srgb()),
        (b"gTRC", para_srgb()),
        (b"bTRC", para_srgb()),
        (b"chad", b"sf32" + b"\0" * 4 + b"".join(s15(chad[i][j]) for i in range(3) for j in range(3))),
    ]
    table_len = 4 + 12 * len(tags)
    off = 128 + table_len
    entries, data = b"", b""
    for sig, body in tags:
        while len(body) % 4:
            body += b"\0"
        entries += sig + struct.pack(">II", off + len(data), len(body))
        data += body
    size = 128 + table_len + len(data)
    hdr = struct.pack(">I", size) + b"\0" * 4 + struct.pack(">I", 0x04300000) + b"mntrRGB XYZ "
    hdr += b"\0" * 12 + b"acsp" + b"\0" * 24 + struct.pack(">I", 0) + s15(0.9642) + s15(1.0) + s15(0.8249)
    hdr += b"\0" * (128 - len(hdr))
    return hdr + struct.pack(">I", len(tags)) + entries + data


P3 = [(0.680, 0.320), (0.265, 0.690), (0.150, 0.060)]
(OUT / "display_p3.icc").write_bytes(icc_rgb("Display P3 (test)", P3))

common = dict(quality=-1, chroma=444)  # lossless x265, 4:4:4: exact pixels

# 1. 8-bit sRGB pattern, nclx sRGB (libheif default colr).
Image.fromarray(pattern8()).save(OUT / "srgb_8bit.heic", **common)

# 2. Saturated red with an embedded Display P3 ICC (prof).
red = np.zeros((16, 16, 3), np.uint8)
red[:, :] = (255, 0, 0)
im = Image.fromarray(red)
im.info["icc_profile"] = icc_rgb("Display P3 (test)", P3)
im.save(OUT / "p3_icc.heic", icc_profile=im.info["icc_profile"], **common)

# 3. Same red, nclx P3 (primaries 12, sRGB transfer 13), no ICC.
Image.fromarray(red).save(
    OUT / "p3_nclx.heic", color_primaries=12, transfer_characteristic=13,
    matrix_coefficients=6, full_range_flag=1, **common)

# 4. Rotated: EXIF orientation 6 -> pillow-heif writes irot and resets EXIF.
rot = Image.fromarray(pattern8())
exif = rot.getexif()
exif[0x0112] = 6
rot.save(OUT / "irot.heic", exif=exif.tobytes(), **common)

# 5. 10-bit gradient (SDR, sRGB).
g = np.zeros((H, W, 3), np.uint16)
for x in range(W):
    g[:, x, 0] = int(round(x / (W - 1) * 65535))
    g[:, x, 1] = 32768
    g[:, x, 2] = 65535 - int(round(x / (W - 1) * 65535))
hf = pillow_heif.from_bytes(mode="RGB;16", size=(W, H), data=g.tobytes())
hf.save(OUT / "gradient_10bit.heic", **common)

# 6. PQ BT.2020 10-bit: left half 203-nit white, right half 1000-nit white.
def pq_oetf(nits: float) -> float:
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    y = (nits / 10000.0) ** m1
    return ((c1 + c2 * y) / (1 + c3 * y)) ** m2


pq = np.zeros((16, 16, 3), np.uint16)
pq[:, :8] = int(round(pq_oetf(203.0) * 65535))
pq[:, 8:] = int(round(pq_oetf(1000.0) * 65535))
hf = pillow_heif.from_bytes(mode="RGB;16", size=(16, 16), data=pq.tobytes())
hf.save(OUT / "pq_10bit.heic", color_primaries=9, transfer_characteristic=16,
        matrix_coefficients=9, full_range_flag=1, **common)

# 7. iPhone-shaped: lossy 4:2:0, YCbCr matrix, Display P3 ICC, irot (EXIF 6).
#    This is the file the OS (WIC) path is allowed to take; the test compares
#    WIC and libheif pixels on it.
ip = np.zeros((48, 64, 3), np.uint8)
for y in range(48):
    for x in range(64):
        ip[y, x] = (int(x * 255 / 63), int(y * 255 / 47), 128)
ip[:8, :8] = (0, 255, 0)
ipim = Image.fromarray(ip)
ipexif = ipim.getexif()
ipexif[0x0112] = 6
ipim.save(OUT / "iphone_like.heic", quality=92, chroma=420, exif=ipexif.tobytes(),
          icc_profile=icc_rgb("Display P3 (test)", P3))

# PNG with the P3 ICC for the AVIF generator (ffmpeg carries iCCP to colr prof).
Image.fromarray(red).save(OUT.parent / "p3_red.png", icc_profile=icc_rgb("Display P3 (test)", P3))

for p in sorted(OUT.glob("*")):
    print(p.name, p.stat().st_size, b"irot" in p.read_bytes(), b"prof" in p.read_bytes(), b"nclx" in p.read_bytes())
