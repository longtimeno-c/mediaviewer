#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""#403: a labelled set of photos with text in them, for mv_ai_tests "[.ocr-fixture]".

Writes camera-sized JPEGs to OUT with labels.tsv (file <TAB> phrase): signs,
posters, printed shirts and phone screenshots, each holding one phrase over a
noisy scene, some rotated, small or split over two lines. "suited and booted"
is the reporter's case. Synthetic on purpose: no one's photos in the repo.

    python3 tools/ai/ocr-fixture.py OUT [--count 40] [--seed 403]

Needs Pillow. Fonts: the platform's (macOS /System/Library/Fonts, Windows
C:\\Windows\\Fonts), else Pillow's default bitmap font (much worse to read).
"""
import argparse
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

PHRASES = [
    "suited and booted", "open all night", "fresh bread daily", "no parking here", "mind the gap",
    "happy birthday anna", "welcome to lisbon", "keep calm and carry on", "best coffee in town",
    "closed on sundays", "pull to open", "exit only", "danger high voltage", "fish and chips",
    "live music tonight", "free wifi inside", "sale fifty percent off", "please ring the bell",
    "beware of the dog", "train to brighton", "platform four", "gate twelve boarding", "hand wash only",
    "made in portugal", "summer festival", "the north face", "just do it", "good vibes only",
    "staff only beyond this point", "fire assembly point", "meeting at noon", "battery low",
    "message from mum", "your order has shipped", "flight delayed", "rooftop bar", "ice cream parlour",
    "vintage records", "bike hire", "tickets sold here",
]

FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Georgia Bold.ttf",
    "/System/Library/Fonts/Supplemental/Impact.ttf",
    "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    r"C:\Windows\Fonts\arialbd.ttf",
    r"C:\Windows\Fonts\arial.ttf",
    r"C:\Windows\Fonts\georgiab.ttf",
    r"C:\Windows\Fonts\impact.ttf",
    r"C:\Windows\Fonts\courbd.ttf",
]


def fonts():
    found = [f for f in FONT_CANDIDATES if os.path.exists(f)]
    return found


def font(path, size):
    if path:
        return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def scene(rng, w, h):
    """A noisy 'photo': a two-colour gradient, blobs and grain."""
    a = tuple(rng.randrange(30, 220) for _ in range(3))
    b = tuple(rng.randrange(30, 220) for _ in range(3))
    grad = Image.linear_gradient("L").resize((w, h))
    img = Image.composite(Image.new("RGB", (w, h), a), Image.new("RGB", (w, h), b), grad)
    d = ImageDraw.Draw(img)
    for _ in range(25):
        r = rng.randrange(w // 30, w // 6)
        x, y = rng.randrange(w), rng.randrange(h)
        d.ellipse([x - r, y - r, x + r, y + r], fill=tuple(rng.randrange(0, 255) for _ in range(3)))
    img = img.filter(ImageFilter.GaussianBlur(radius=w / 300))
    noise = Image.effect_noise((w, h), 18).convert("RGB")
    return Image.blend(img, noise, 0.08)


def split(phrase, rng):
    words = phrase.split()
    if len(words) >= 3 and rng.random() < 0.5:
        k = rng.randrange(1, len(words))
        return [" ".join(words[:k]), " ".join(words[k:])]
    return [phrase]


def place_text(rng, base, lines, fpath, height_frac, fg, panel, angle, region=None):
    w, h = base.size
    rx, ry, rw, rh = region if region else (0, 0, w, h)
    size = max(10, int(h * height_frac))
    f = font(fpath, size)
    widths = [f.getbbox(l)[2] for l in lines]
    tw, th = max(widths) + size, int(len(lines) * size * 1.35) + size
    layer = Image.new("RGBA", (tw, th), panel + (255,) if panel else (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i, l in enumerate(lines):
        d.text((size // 2, size // 2 + int(i * size * 1.35)), l, font=f, fill=fg + (255,))
    layer = layer.rotate(angle, expand=True, resample=Image.BICUBIC)
    x = rx + rng.randrange(0, max(1, rw - layer.size[0]))
    y = ry + rng.randrange(0, max(1, rh - layer.size[1]))
    base.paste(layer, (x, y), layer)
    return base


def make(rng, kind, phrase, fpaths):
    if kind == "screenshot":
        w, h = 1170, 2532
        img = Image.new("RGB", (w, h), (248, 248, 250))
        d = ImageDraw.Draw(img)
        small = font(rng.choice(fpaths) if fpaths else None, 34)
        for i in range(30):
            y = 120 + i * 78
            d.rectangle([40, y, w - 40, y + 60], fill=(235, 236, 240))
            d.text((60, y + 10), rng.choice(PHRASES).upper()[::-1].lower(), font=small, fill=(150, 150, 160))
        return place_text(rng, img, [phrase], rng.choice(fpaths) if fpaths else None, 0.017, (20, 20, 25),
                          (255, 255, 255), 0)
    w, h = (4032, 3024) if rng.random() < 0.5 else (3024, 4032)
    img = scene(rng, w // 2, h // 2).resize((w, h), Image.BILINEAR)
    lines = split(phrase, rng)
    fp = rng.choice(fpaths) if fpaths else None
    if kind == "sign":
        return place_text(rng, img, lines, fp, rng.uniform(0.04, 0.08), (255, 255, 255),
                          tuple(rng.randrange(0, 90) for _ in range(3)), rng.uniform(-5, 5))
    if kind == "poster":
        return place_text(rng, img, lines, fp, rng.uniform(0.025, 0.045), (15, 15, 15),
                          (240, 230, 200), rng.uniform(-3, 3))
    # shirt: smaller, no panel of its own, on a fabric-coloured patch, tilted
    patch = tuple(rng.randrange(20, 120) for _ in range(3))
    d = ImageDraw.Draw(img)
    pw, ph = w // 2, h // 3
    px, py = rng.randrange(0, w - pw), rng.randrange(0, h - ph)
    d.rectangle([px, py, px + pw, py + ph], fill=patch)
    return place_text(rng, img, lines, fp, rng.uniform(0.015, 0.03), (235, 235, 235), None, rng.uniform(-10, 10),
                      (px, py, pw, ph))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--count", type=int, default=40)
    ap.add_argument("--seed", type=int, default=403)
    a = ap.parse_args()
    rng = random.Random(a.seed)
    os.makedirs(a.out, exist_ok=True)
    fpaths = fonts()
    if not fpaths:
        print("warning: no TrueType font found; Pillow's bitmap font reads badly", file=sys.stderr)
    kinds = ["sign", "poster", "shirt", "screenshot"]
    phrases = PHRASES[: a.count] if a.count <= len(PHRASES) else [rng.choice(PHRASES) for _ in range(a.count)]
    with open(os.path.join(a.out, "labels.tsv"), "w", encoding="utf-8") as labels:
        for i, phrase in enumerate(phrases):
            kind = kinds[i % len(kinds)]
            img = make(rng, kind, phrase, fpaths)
            name = f"{i:03d}_{kind}.jpg"
            img.save(os.path.join(a.out, name), "JPEG", quality=88)
            labels.write(f"{name}\t{phrase}\n")
    print(f"wrote {len(phrases)} photos to {a.out}")


if __name__ == "__main__":
    main()
