#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Extend's margin sharpness and seams (docs/design/29 "Extend", 2026-11-05).

usage: extend_metrics.py <dir> <photo_w> <photo_h> <left> <top> <right> <bottom> <variant>...
Reads <dir>/<variant>.rgb (the extended picture, from mv_import_tests
"[.extend-compare]") and prints, per variant:
  lapvar   variance of the 3x3 Laplacian of luma over margin pixels (8 px
           away from the photo and the picture's edge): sharpness
  ref      the same over the photo's own border band of the same depth
  hf       mean |luma - 5x5 box blur| in the margin (fine detail)
  edge     mean |step| across the photo/margin line over mean |step| one
           and two pixels further out (1.0: no visible line)
  seam     along each margin, the column (row) mean |d luma/d along|
           profile's worst peak over its local median (tile seams, fringes)
  chroma   mean |chroma gradient| in the margin (colour fringes)
and writes <variant>.png.
"""
import sys
import numpy as np
from PIL import Image

d = sys.argv[1]
pw, ph, L, T, R, B = map(int, sys.argv[2:8])
W, H = pw + L + R, ph + T + B


def load(name):
    a = np.fromfile(f"{d}/{name}.rgb", dtype=np.uint8).reshape(H, W, 3).astype(np.float64)
    Image.fromarray(a.astype(np.uint8)).save(f"{d}/{name}.png")
    return a


def luma(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def lap(y):
    o = np.zeros_like(y)
    o[1:-1, 1:-1] = 4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:]
    return o


def box5(y):
    p = np.pad(y, 2, mode="edge")
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[5:, 5:] - c[:-5, 5:] - c[5:, :-5] + c[:-5, :-5]) / 25.0


margin = np.ones((H, W), bool)
margin[T:T + ph, L:L + pw] = False
inner = np.zeros((H, W), bool)
k = 8
inner[k:H - k, k:W - k] = True
far = np.ones((H, W), bool)
far[max(T - k, 0):T + ph + k, max(L - k, 0):L + pw + k] = False
mm = margin & inner & far
band = np.zeros((H, W), bool)
band[T:T + ph, L:L + pw] = True
band[T + max(T, 1):T + ph - max(B, 1), L + max(L, 1):L + pw - max(R, 1)] = False
band &= inner


def stats(a):
    y = luma(a)
    lp = lap(y)
    hf = np.abs(y - box5(y))
    out = {"lapvar": lp[mm].var(), "ref": lp[band].var(), "hf": hf[mm].mean(), "hf_ref": hf[band].mean()}
    steps, beyond = [], []
    if L:
        steps.append(np.abs(y[T:T + ph, L] - y[T:T + ph, L - 1]).mean())
        beyond.append(np.abs(y[T:T + ph, L - 2] - y[T:T + ph, L - 3]).mean())
    if R:
        x = L + pw
        steps.append(np.abs(y[T:T + ph, x] - y[T:T + ph, x - 1]).mean())
        beyond.append(np.abs(y[T:T + ph, x + 2] - y[T:T + ph, x + 1]).mean())
    if T:
        steps.append(np.abs(y[T, L:L + pw] - y[T - 1, L:L + pw]).mean())
        beyond.append(np.abs(y[T - 2, L:L + pw] - y[T - 3, L:L + pw]).mean())
    if B:
        yy = T + ph
        steps.append(np.abs(y[yy, L:L + pw] - y[yy - 1, L:L + pw]).mean())
        beyond.append(np.abs(y[yy + 2, L:L + pw] - y[yy + 1, L:L + pw]).mean())
    out["edge"] = np.mean(steps) / max(np.mean(beyond), 1e-6)
    peaks = []

    def profile(g):  # g: 1-D mean |d along| profile
        med = np.array([np.median(g[max(0, i - 32):i + 33]) for i in range(len(g))])
        return (g / np.maximum(med, 1e-3)).max()

    gx = np.abs(np.diff(y, axis=1))
    gy = np.abs(np.diff(y, axis=0))
    if T > 2 * k:
        peaks.append(profile(gx[k:T - k, k:W - k].mean(0)))
    if B > 2 * k:
        peaks.append(profile(gx[H - B + k:H - k, k:W - k].mean(0)))
    if L > 2 * k:
        peaks.append(profile(gy[k:H - k, k:L - k].mean(1)))
    if R > 2 * k:
        peaks.append(profile(gy[k:H - k, W - R + k:W - k].mean(1)))
    out["seam"] = max(peaks) if peaks else float("nan")
    cb = a[..., 2] - y
    cr = a[..., 0] - y
    cg = np.abs(np.diff(cb, axis=1)) + np.abs(np.diff(cr, axis=1))
    out["chroma"] = cg[mm[:, 1:]].mean()
    return out


for v in sys.argv[8:]:
    s = stats(load(v))
    print(f"{v:8s} lapvar {s['lapvar']:8.1f} (photo {s['ref']:7.1f})  hf {s['hf']:5.2f} (photo {s['hf_ref']:5.2f})"
          f"  edge {s['edge']:5.2f}  seam {s['seam']:5.2f}  chroma {s['chroma']:5.2f}")
