#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn docs/perf/compare/{presentmon.csv,runs.json} into summary.json and docs/img/compare-*.svg.

    python tools/perf/compare-analyse.py

Pan and playback runs are cut out of the PresentMon capture by QPC timestamp. Open runs are the
screen-settle times measured by compare.ps1. Nothing is typed in by hand.
"""
import csv
import json
import pathlib
import statistics

ROOT = pathlib.Path(__file__).resolve().parents[2]
DIR = ROOT / "docs" / "perf" / "compare"
IMG = ROOT / "docs" / "img"
LABEL = {"mv": "MediaViewer", "photos": "Windows Photos", "wmp": "Media Player"}
COLOUR = {"mv": "#4c9aff", "photos": "#ff9f43", "wmp": "#c56cf0"}
PROC = {"mediaviewer_lab.exe": "mv", "Photos.exe": "photos", "Microsoft.Media.Player.exe": "wmp"}
BG, FG, MUTED, GRID = "#14181f", "#e6e9ef", "#8b94a5", "#2a303b"
FONT = 'font-family="Segoe UI, Helvetica, Arial, sans-serif"'


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def load():
    meta = json.loads((DIR / "runs.json").read_text(encoding="utf-8-sig"))
    freq = meta["qpc_freq"]
    rows = []
    with (DIR / "presentmon.csv").open(encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            try:
                rows.append((PROC.get(r["Application"]), int(r["QPCTime"]), float(r["MsBetweenPresents"]),
                             float(r["MsBetweenDisplayChange"]) if r["MsBetweenDisplayChange"] != "NA" else None))
            except (KeyError, ValueError):
                pass
    return meta["runs"], freq, rows


def window(rows, app, a, b):
    return [r for r in rows if r[0] == app and a <= r[1] <= b]


def summarise(runs, freq, rows):
    out = {"open": {}, "pan": {}, "play": {}}
    for run in runs:
        key = (run["app"], run["file"])
        if run["kind"] == "open":
            out["open"].setdefault(key, []).append(run["first_pixel_ms"])
            continue
        w = window(rows, run["app"], run["qpc_start"], run["qpc_end"])
        secs = (run["qpc_end"] - run["qpc_start"]) / freq
        gaps = [r[2] for r in w[1:]]
        disp = [r[3] for r in w if r[3] is not None]
        out[run["kind"]].setdefault(key, []).append({
            "seconds": secs, "presents": len(w), "fps": len(w) / secs if secs else 0,
            "p50_ms": pct(gaps, 50) if gaps else None, "p99_ms": pct(gaps, 99) if gaps else None,
            "max_ms": max(gaps) if gaps else None,
            "over_2_refresh": sum(1 for g in gaps if g > 33.4),
            "display_p99_ms": pct(disp, 99) if disp else None})
    return out


def text(x, y, s, size=12, fill=MUTED, anchor="start", weight="400"):
    return f'<text x="{x}" y="{y}" {FONT} font-size="{size}" fill="{fill}" text-anchor="{anchor}" font-weight="{weight}">{s}</text>'


def grouped(title, groups, series, unit, note, fname, ref=None, ymax=None):
    """groups: list of (label, {app: value or None}); one bar per app in each group."""
    w, left, top, bar, gap = 760, 200, 88, 15, 30
    h = top + len(groups) * (len(series) * (bar + 4) + gap) + 46
    vmax = ymax or max((v for _, d in groups for v in d.values() if v), default=1) * 1.12
    px = lambda v: left + (w - left - 90) * v / vmax
    body = [f'<rect width="{w}" height="{h}" rx="10" fill="{BG}"/>', text(24, 34, title, 17, FG, weight="600")]
    x = left
    for a in series:
        body.append(f'<rect x="{x}" y="50" width="12" height="12" rx="2" fill="{COLOUR[a]}"/>' + text(x + 18, 61, LABEL[a], 12, FG))
        x += 150
    step = max(1, round(vmax / 5, -1 if vmax > 50 else 0)) if vmax > 5 else 1
    t = 0
    while t <= vmax:
        body.append(f'<line x1="{px(t)}" y1="{top - 10}" x2="{px(t)}" y2="{h - 40}" stroke="{GRID}"/>' + text(px(t), h - 24, f"{t:g}", 11, anchor="middle"))
        t += step
    if ref:
        body.append(f'<line x1="{px(ref[0])}" y1="{top - 14}" x2="{px(ref[0])}" y2="{h - 40}" stroke="#ff6b6b" stroke-dasharray="4 4"/>' + text(px(ref[0]) + 5, top - 16, ref[1], 11, "#ff6b6b"))
    y = top
    for label, d in groups:
        body.append(text(left - 12, y + len(series) * (bar + 4) / 2 + 2, label, 12, FG, "end"))
        for a in series:
            v = d.get(a)
            if v is None:
                body.append(text(left + 6, y + 12, "did not open / no data", 11, "#ff6b6b"))
            else:
                body.append(f'<rect x="{left}" y="{y}" width="{max(px(v) - left, 2):.1f}" height="{bar}" rx="3" fill="{COLOUR[a]}"/>' + text(px(v) + 6, y + 12, f"{v:.1f} {unit}", 11, FG))
            y += bar + 4
        y += gap
    body.append(text(24, h - 6, note, 11))
    (IMG / fname).write_text(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img" aria-label="{title}">{"".join(body)}</svg>', encoding="utf-8")


def med(vals):
    vals = [v for v in vals if v is not None]
    return statistics.median(vals) if vals else None


def main():
    runs, freq, rows = load()
    s = summarise(runs, freq, rows)
    (DIR / "summary.json").write_text(json.dumps({f"{k}|{a}|{f}": v for k, d in s.items() for (a, f), v in d.items()}, indent=1))
    files = sorted({f for (_, f) in s["open"]})
    grouped("Double-click to picture on screen", [(f, {a: med(s["open"].get((a, f), [])) for a in ("mv", "photos")}) for f in files],
            ("mv", "photos"), "ms", "Median of repeated cold launches. Screen-capture timing, about one refresh either way. Same files, same machine.",
            "compare-open.svg")
    pans = sorted({f for (_, f) in s["pan"]})
    grouped("Panning a zoomed-in photo: 99th-percentile frame gap", [(f, {a: med([r["p99_ms"] for r in s["pan"].get((a, f), [])]) for a in ("mv", "photos")}) for f in pans],
            ("mv", "photos"), "ms", "Same scripted mouse drag on each app, PresentMon. Lower is smoother. 16.7 ms is one refresh of this 60 Hz display.", "compare-pan.svg", ref=(16.7, "one refresh"))
    grouped("Panning: frames presented per second", [(f, {a: med([r["fps"] for r in s["pan"].get((a, f), [])]) for a in ("mv", "photos")}) for f in pans],
            ("mv", "photos"), "fps", "Same drag. Fewer frames while you drag feels choppier.", "compare-pan-fps.svg", ref=(60, "60 Hz"))
    clips = sorted({f for (_, f) in s["play"]})
    grouped("Video playback: 99th-percentile frame gap", [(f, {a: med([r["p99_ms"] for r in s["play"].get((a, f), [])]) for a in ("mv", "wmp")}) for f in clips],
            ("mv", "wmp"), "ms", "Same clips, PresentMon. A steady 30 fps clip sits near 33 ms. A 60 fps clip sits near 16.7 ms.", "compare-video.svg")
    print(json.dumps({k: {f"{a}|{f}": v for (a, f), v in d.items()} for k, d in s.items()}, indent=1)[:6000])


if __name__ == "__main__":
    main()
