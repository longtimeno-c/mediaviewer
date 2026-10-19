#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Issue #486: launch MediaViewer repeatedly with --boot-report and summarise the boot
timeline (src/core/boot_timeline.h) per stage. The macOS twin of tools/perf/boot.ps1, with
the same report shape; it runs on Windows too. With two --exe values the runs alternate
(base, new, base, new), as docs/DEVELOPMENT "Performance suite" asks of a perf claim.

  caffeinate -d -i python3 tools/perf/boot.py --exe base/build/bin/mediaviewer_lab \\
      --exe build/bin/mediaviewer_lab --runs 10 --open tools/testmedia/raw/x.arw --out boot.json

The first launch of each build is a warm-up and is discarded (the published runs are warm
cache). Writes every run and the per-stage medians to --out.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time


def boot(exe, report, open_path):
    argv = [exe, "--boot-report", report]
    if open_path:
        argv.append(open_path)
    if os.path.exists(report):
        os.remove(report)
    try:
        code = subprocess.run(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=60).returncode
    except subprocess.TimeoutExpired:
        sys.exit(f"no report after 60 s: {exe}")
    if not os.path.exists(report):
        sys.exit(f"exit {code} without a report: {exe}")
    with open(report, encoding="utf-8") as f:
        return json.load(f)["stages_ms"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", action="append", required=True, help="a build's mediaviewer_lab (repeat to alternate)")
    ap.add_argument("--runs", type=int, default=8)
    ap.add_argument("--open", default="", help="a file to open on each launch")
    ap.add_argument("--out", default="boot.json")
    ap.add_argument("--pause-ms", type=int, default=1500)
    a = ap.parse_args()
    exes = [os.path.abspath(e) for e in a.exe]
    open_path = os.path.abspath(a.open) if a.open else ""
    pause = a.pause_ms / 1000.0
    results = {e: [] for e in exes}
    with tempfile.TemporaryDirectory(prefix="mv-boot-") as tmp:
        report = os.path.join(tmp, "boot.json")
        for e in exes:  # warm-up, discarded
            boot(e, report, open_path)
            time.sleep(pause)
        for r in range(a.runs):
            for i, e in enumerate(exes):
                s = boot(e, report, open_path)
                results[e].append(s)
                ready = max(s["interactive"], s["first_frame"], s["full_resolution"])
                print(f"run {r} build {i}  interactive={s['interactive']:.0f} ms  ready={ready:.0f} ms", flush=True)
                time.sleep(pause)

    names = list(results[exes[0]][0].keys())
    summary = {"open": os.path.basename(open_path), "runs": a.runs, "builds": []}
    for e in exes:
        stages = {}
        for n in names:
            vals = [run[n] for run in results[e] if run[n] >= 0]
            stages[n] = {
                "median": round(statistics.median(vals), 1) if vals else -1,
                "min": round(min(vals), 1) if vals else -1,
                "max": round(max(vals), 1) if vals else -1,
            }
        summary["builds"].append({"exe": e, "median_ms": stages, "runs": results[e]})
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    print()
    print("stage                 " + "".join(f"build {i}".rjust(20) for i in range(len(exes))))
    for n in names:
        line = n.ljust(22)
        for b in summary["builds"]:
            m = b["median_ms"][n]
            line += f"{m['median']:>8} [{m['min']:.0f}-{m['max']:.0f}]".rjust(20)
        print(line)


if __name__ == "__main__":
    main()
