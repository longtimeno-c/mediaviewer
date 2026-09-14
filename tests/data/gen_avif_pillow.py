"""AVIF fixtures ffmpeg cannot express: irot/imir, embedded ICC, animated
with explicit durations and loop count. Pillow's own AVIF plugin (libavif).
Dev tool only. usage: gen_avif_pillow.py <outdir> <display_p3.icc>
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

OUT = Path(sys.argv[1])
ICC = Path(sys.argv[2]).read_bytes()
OUT.mkdir(parents=True, exist_ok=True)

W, H = 64, 32
a = np.zeros((H, W, 3), np.uint8)
a[:, : W // 2] = (255, 0, 0)
a[:, W // 2 :] = (0, 0, 255)
a[:8, :8] = (0, 255, 0)
pattern = Image.fromarray(a)

# Rotated: EXIF orientation 6 -> Pillow writes irot (and imir for 2/4/5/7).
exif = pattern.getexif()
exif[0x0112] = 6
pattern.save(OUT / "irot.avif", exif=exif.tobytes(), quality=100, subsampling="4:4:4")

# Mirrored + rotated: orientation 5 -> irot + imir.
exif5 = pattern.getexif()
exif5[0x0112] = 5
pattern.save(OUT / "irot_imir.avif", exif=exif5.tobytes(), quality=100, subsampling="4:4:4")

# Saturated red, Display P3 ICC.
red = np.zeros((16, 16, 3), np.uint8)
red[:, :] = (255, 0, 0)
Image.fromarray(red).save(OUT / "p3_icc.avif", icc_profile=ICC, quality=100, subsampling="4:4:4")

# Animated: 4 frames, durations 40/100/200/0 ms, loop 3 plays.
frames = []
for i in range(4):
    f = np.zeros((16, 32, 3), np.uint8)
    f[:, :] = (255, 0, 0)
    f[:, i * 8 : i * 8 + 8] = (0, 0, 255)
    frames.append(Image.fromarray(f))
frames[0].save(OUT / "anim.avif", save_all=True, append_images=frames[1:],
               duration=[40, 100, 200, 0], loop=3, quality=90, subsampling="4:4:4")

for p in sorted(OUT.glob("*.avif")):
    b = p.read_bytes()
    print(p.name, len(b), "irot" if b"irot" in b else "", "imir" if b"imir" in b else "",
          "prof" if b"prof" in b else "", "avis" if b"avis" in b else "")
