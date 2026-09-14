"""Rewrite the first `colr nclx` box of a HEIF/AVIF file in place.

The fixture encoders do not write every CICP code they are asked for: ffmpeg's
avif muxer writes nclx 2/2 whatever -color_primaries/-color_trc say, and
pillow-heif stores transfer 13 when asked for 16 (PQ). The bitstreams carry no
conflicting colour description, so the container box is authoritative, and
patching it produces exactly the file a correct encoder would.

usage: patch_nclx.py <file> <primaries> <transfer> <matrix> <full_range 0|1>
"""
import struct
import sys

path, prim, trc, mat, full = sys.argv[1], *map(int, sys.argv[2:6])
data = bytearray(open(path, "rb").read())
i = data.find(b"colrnclx")
if i < 0:
    sys.exit(f"{path}: no colr nclx box")
data[i + 8 : i + 14] = struct.pack(">HHH", prim, trc, mat)
data[i + 14] = (0x80 if full else 0) | (data[i + 14] & 0x7F)
open(path, "wb").write(data)
print(f"{path}: nclx {prim}/{trc}/{mat} full={full}")
