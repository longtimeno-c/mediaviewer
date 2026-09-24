#!/usr/bin/env python3
"""Render the README performance charts (docs/img/*.svg) from the measured data in docs/perf/.

No dependencies beyond the standard library. Every number on a chart is read from a
report the lab or frametime wrote; nothing here is typed in. Re-run after re-measuring:

    python tools/perf/make-charts.py
"""
import csv
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
PERF = ROOT / "docs" / "perf"
IMG = ROOT / "docs" / "img"

BG, FG, MUTED, GRID = "#14181f", "#e6e9ef", "#8b94a5", "#2a303b"
ACCENT, WARN, GOOD = "#4c9aff", "#ff6b6b", "#3ddc97"
FONT = "font-family=\"Segoe UI, Helvetica, Arial, sans-serif\""


def svg(w, h, body, title):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img" '
            f'aria-label="{title}"><rect width="{w}" height="{h}" rx="10" fill="{BG}"/>'
            f'<text x="24" y="34" {FONT} font-size="17" font-weight="600" fill="{FG}">{title}</text>{body}</svg>')


def text(x, y, s, size=12, fill=MUTED, anchor="start", weight="400"):
    return (f'<text x="{x}" y="{y}" {FONT} font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
            f'font-weight="{weight}">{s}</text>')


def pacing():
    anim = json.loads((PERF / "frametime-animated.json").read_text())
    pan = json.loads((PERF / "pan-soak-42mp-arw.json").read_text())
    refresh = anim["refresh_interval_ms"]
    rows = [
        (f"Animated sweep, {anim['frames']} frames", anim),
        (f"Pan a 42 MP RAW, {pan['frames']} frames", pan),
    ]
    w, h, left, right, top = 760, 300, 210, 40, 84
    scale_max = 2.2 * refresh
    px = lambda v: left + (w - left - right) * v / scale_max
    body = []
    for t in (0, 10, 20, 30):
        body.append(f'<line x1="{px(t)}" y1="{top - 10}" x2="{px(t)}" y2="{h - 64}" stroke="{GRID}"/>')
        body.append(text(px(t), h - 46, f"{t} ms", 11, anchor="middle"))
    body.append(f'<line x1="{px(refresh)}" y1="{top - 14}" x2="{px(refresh)}" y2="{h - 64}" stroke="{ACCENT}" '
                f'stroke-dasharray="4 4"/>')
    body.append(text(px(refresh) + 6, top - 18, f"one refresh, {refresh:.2f} ms", 11, ACCENT))
    body.append(f'<line x1="{px(2 * refresh)}" y1="{top - 14}" x2="{px(2 * refresh)}" y2="{h - 64}" stroke="{WARN}" '
                f'stroke-dasharray="4 4"/>')
    body.append(text(px(2 * refresh) - 6, top - 18, "2x: build fails", 11, WARN, "end"))
    y = top
    for label, r in rows:
        body.append(text(left - 12, y + 40, label, 12, FG, "end"))
        for i, (name, key) in enumerate((("p50", "p50_ms"), ("p99", "p99_ms"), ("max", "max_ms"))):
            v = r[key]
            yy = y + i * 20
            body.append(f'<rect x="{left}" y="{yy}" width="{px(v) - left:.1f}" height="14" rx="3" fill="{ACCENT}" '
                        f'opacity="{1.0 - 0.22 * i:.2f}"/>')
            body.append(text(px(v) + 6, yy + 11, f"{name} {v:.2f} ms", 11, FG))
        y += 92
    body.append(text(24, h - 16, f"{anim['dropped_frames'] + pan['dropped_frames']} dropped frames across both runs. "
                     "Source: DXGI frame statistics on a 60 Hz display.", 11))
    return svg(w, h, "".join(body), "Frame pacing: 60 s soaks")


def first_pixel():
    names = {
        "sony_ilce7rm3.arw": "Sony ARW  42 MP",
        "canon_eos7dmk2.cr2": "Canon CR2  20 MP",
        "canon_eosr6.cr3": "Canon CR3  20 MP",
        "nikon_d7500.nef": "Nikon NEF  21 MP",
        "pentax_k50.dng": "Pentax DNG  16 MP",
        "libheif-example.heic": "HEIC  (bundled decoder)",
    }
    data = []
    for f, label in names.items():
        r = json.loads((PERF / "first-pixel" / f"{f}.json").read_text())
        data.append((label, r["still_first_pixel_s"] * 1000, r["still_full_s"] * 1000))
    data.sort(key=lambda d: d[1])
    w, h, left, right, top = 760, 84 + 44 * len(data) + 40, 190, 60, 84
    vmax = max(d[2] for d in data)
    px = lambda v: left + (w - left - right) * v / vmax
    body = [f'<rect x="{left}" y="52" width="12" height="12" rx="2" fill="{GOOD}"/>',
            text(left + 18, 63, "first pixel on screen", 12, FG),
            f'<rect x="{left + 170}" y="52" width="12" height="12" rx="2" fill="{MUTED}"/>',
            text(left + 188, 63, "full-resolution decode replaces it", 12, FG)]
    for t in range(0, int(vmax) + 1, 500):
        body.append(f'<line x1="{px(t)}" y1="{top - 8}" x2="{px(t)}" y2="{h - 44}" stroke="{GRID}"/>')
        body.append(text(px(t), h - 28, f"{t} ms", 11, anchor="middle"))
    for i, (label, first, full) in enumerate(data):
        y = top + i * 44
        body.append(text(left - 12, y + 20, label, 12, FG, "end"))
        body.append(f'<rect x="{left}" y="{y}" width="{max(px(first) - left, 2):.1f}" height="14" rx="3" fill="{GOOD}"/>')
        body.append(text(px(first) + 6, y + 11, f"{first:.0f} ms", 11, FG))
        if full - first > 1:
            body.append(f'<rect x="{left}" y="{y + 18}" width="{px(full) - left:.1f}" height="10" rx="3" fill="{MUTED}" '
                        f'opacity="0.55"/>')
            body.append(text(px(full) + 6, y + 27, f"{full:.0f} ms", 11))
    body.append(text(24, h - 8, "6 s runs (file cache warm), CC0 samples from raw.pixls.us and libheif. Embedded preview first, "
                     "full decode refines in place.", 11))
    return svg(w, h, "".join(body), "Time to first pixel, straight from camera files")


def _pct(values, p):
    if not values:
        return None
    s = sorted(values)
    if len(s) == 1:
        return s[0]
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def _ms(v):
    return f"{v:.1f} ms" if v < 10 else f"{v:.0f} ms"


def browse():
    report = json.loads((PERF / "browse.json").read_text())
    steps = [s for s in report["steps"] if not s.get("timed_out")]
    warm = [s for s in steps if s["kind"] == "warm" and s["cached"]]
    warm_miss = [s for s in steps if s["kind"] == "warm" and not s["cached"]]
    cold = [s for s in steps if s["kind"] == "cold"]
    groups = []
    refresh = next((s["refresh_ms"] for s in steps if s.get("refresh_ms")), 16.7)
    if warm:
        groups.append(("Arrow to the next photo. It was already decoded.", warm, refresh, True))
    if warm_miss:
        groups.append(("Arrow to the next photo. It still had to be decoded.", warm_miss,
                       max(s["present_ms"] for s in warm_miss) * 1.25, False))
    if cold:
        groups.append(("Jump from the first photo, past the two decoded neighbours.", cold,
                       max(s["present_ms"] for s in cold) * 1.25, False))
    if not groups:
        raise SystemExit("browse.json has no completed steps")
    row_h = 32
    w, left, right = 760, 230, 90
    h = 70 + sum(52 + row_h * len(rows) for _, rows, _, _ in groups) + 48
    body = []
    y = 58
    for label, rows, vmax, against_refresh in groups:
        px = lambda v, vmax=vmax: left + (w - left - right) * min(v, vmax) / vmax
        body.append(text(24, y, label, 13, FG, weight="600"))
        y += 16
        if against_refresh:
            body.append(f'<line x1="{px(refresh)}" y1="{y}" x2="{px(refresh)}" y2="{y + row_h * len(rows) - 8}" '
                        f'stroke="{ACCENT}" stroke-dasharray="4 4"/>')
            body.append(text(px(refresh), y - 4, "one refresh", 11, ACCENT, "middle"))
        for s in rows:
            short = s["name"]
            if len(short) > 26:
                short = short[:23] + "..."
            body.append(text(left - 12, y + 13, short, 12, FG, "end"))
            body.append(f'<rect x="{left}" y="{y}" width="{max(px(s["present_ms"]) - left, 2):.1f}" height="16" '
                        f'rx="3" fill="{GOOD if s["cached"] else ACCENT}"/>')
            body.append(text(max(px(s["present_ms"]) + 6, left + 8), y + 12, _ms(s["present_ms"]), 11, FG))
            y += row_h
        y += 18
    dwell = report.get("dwell_s", 3)
    body.append(text(24, h - 28,
                     f"Each arrow waits {dwell:.0f} s on the current photo first, so the next one can be decoded ahead.",
                     11))
    body.append(text(24, h - 12,
                     "Time until the frame is handed to the display. A cached photo is on screen at the next refresh.",
                     11))
    return svg(w, h, "".join(body), "Moving through the folder")


def drift():
    rows = list(csv.DictReader((PERF / "av-drift-120s.csv").open()))
    t = [float(r["elapsed_s"]) for r in rows]
    p99 = [float(r["error_p99_ms"]) for r in rows]
    p50 = [abs(float(r["error_p50_ms"])) for r in rows]
    w, h, left, right, top, bottom = 760, 320, 60, 30, 70, 56
    ymax = 40.0
    px = lambda v: left + (w - left - right) * v / max(t)
    py = lambda v: h - bottom - (h - top - bottom) * v / ymax
    body = []
    for v in (0, 10, 20, 30, 40):
        body.append(f'<line x1="{left}" y1="{py(v)}" x2="{w - right}" y2="{py(v)}" stroke="{GRID}"/>')
        body.append(text(left - 8, py(v) + 4, f"{v}", 11, anchor="end"))
    for s in range(0, int(max(t)) + 1, 20):
        body.append(text(px(s), h - bottom + 20, f"{s} s", 11, anchor="middle"))
    frame = 1000.0 / 30.0
    body.append(f'<line x1="{left}" y1="{py(frame)}" x2="{w - right}" y2="{py(frame)}" stroke="{WARN}" stroke-dasharray="4 4"/>')
    body.append(text(w - right, py(frame) - 6, "one frame of a 30 fps clip, 33.3 ms", 11, WARN, "end"))
    for series, colour in ((p99, ACCENT), (p50, GOOD)):
        pts = " ".join(f"{px(a):.1f},{py(b):.1f}" for a, b in zip(t, series))
        body.append(f'<polyline points="{pts}" fill="none" stroke="{colour}" stroke-width="2"/>')
    body.append(f'<rect x="{left}" y="46" width="12" height="12" rx="2" fill="{ACCENT}"/>' + text(left + 18, 57, "p99 A/V error", 12, FG))
    body.append(f'<rect x="{left + 130}" y="46" width="12" height="12" rx="2" fill="{GOOD}"/>' + text(left + 148, 57, "median |A/V error|", 12, FG))
    body.append(text(left, h - 10, f"HEVC 720p30 + AAC, WASAPI as master clock. 120 s diagnostic run "
                     f"(the verify soak is 30 min). ms of audio-to-video error.", 11))
    return svg(w, h, "".join(body), "Audio/video sync over 120 s of playback")


if __name__ == "__main__":
    IMG.mkdir(parents=True, exist_ok=True)
    jobs = [("perf-pacing.svg", pacing), ("perf-first-pixel.svg", first_pixel), ("perf-av-sync.svg", drift)]
    if (PERF / "browse.json").exists():
        jobs.append(("perf-browse.svg", browse))
    for name, fn in jobs:
        (IMG / name).write_text(fn(), encoding="utf-8")
        print("wrote", IMG / name)
