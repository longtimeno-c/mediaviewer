#!/usr/bin/env python3
"""Re-measure everything behind the README's performance charts, then redraw them.

    python tools/perf/regenerate.py --list            # what runs here, and what is missing
    python tools/perf/regenerate.py                   # full run into docs/perf + docs/img
    python tools/perf/regenerate.py --out perf-run    # staged run; published data untouched
    python tools/perf/regenerate.py --quick --out x   # a few seconds per soak: smoke, not data
    python tools/perf/regenerate.py --only first-pixel,bench

Each stage runs one of the app's own harnesses (the lab's soaks, frametime, the headless
decode bench) and writes the report make-charts.py reads. Nothing is typed in.

The published charts are Windows numbers (README "Speed you can measure"). On Windows every
stage can run. The macOS lab has the pacing, first-pixel, browse and video soaks but not the
scripted pan or A/V soaks, and the app comparison is Windows Photos and Media Player, so a Mac
run renders the charts it has and skips the rest. A full run takes about 15 minutes, most of
it the 60 and 120 second soaks, which need a quiet machine and a visible display: windows
open, so do not use the machine while it runs.

Inputs: the RAW + HEIC samples (tools/testmedia/fetch-raw.ps1, fetch-heif.ps1) and the
video corpus (tools/testmedia/generate.sh). A stage whose inputs are missing is skipped and
named, never faked. Exit status is non-zero if a stage that could run failed.
"""
import argparse
import datetime
import json
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
MEDIA = ROOT / "tools" / "testmedia"
WINDOWS = sys.platform == "win32"
MAC = sys.platform == "darwin"
EXE = ".exe" if WINDOWS else ""

STILLS = ["raw/sony_ilce7rm3.arw", "raw/canon_eos7dmk2.cr2", "raw/canon_eosr6.cr3",
          "raw/nikon_d7500.nef", "raw/pentax_k50.dng", "heif/libheif-example.heic"]
PAN_STILL = "raw/sony_ilce7rm3.arw"
AV_CLIP = "soak_31min_1080p_hevc_aac.mp4"  # 1080p HEVC + AAC: the video and A/V charts
COMPARE_CLIPS = ["av_transport.mp4", "hevc_4k_8bit_bt709.mp4"]


def default_bin():
    for cand in (ROOT / "build" / "bin" / "Release", ROOT / "build" / "bin",
                 ROOT / "build-darwin" / "bin"):
        if (cand / f"mediaviewer_lab{EXE}").exists():
            return cand
    return ROOT / "build" / ("bin/Release" if WINDOWS else "bin")


class Stage:
    """One harness run. `plan` returns the commands; `needs` the files it cannot run without."""

    # machine.json describes the machine behind the timing charts; a stage whose numbers do not
    # depend on the machine (search accuracy) records its own and leaves machine.json alone.
    timing = True

    def __init__(self, name, what, charts, windows_only=False):
        self.name, self.what, self.charts, self.windows_only = name, what, charts, windows_only

    def needs(self, ctx):
        return []

    def plan(self, ctx):
        return []

    def after(self, ctx):
        pass

    def outputs(self, ctx):
        """Reports this stage must (re)write; a stage that leaves one stale has failed."""
        return []

    def missing(self, ctx):
        if self.windows_only and not ctx.windows:
            return ["Windows (this harness has no macOS host yet)"]
        return [str(p) for p in self.needs(ctx) if not pathlib.Path(p).exists()]


class Context:
    def __init__(self, args):
        self.windows = WINDOWS if args.platform == "auto" else args.platform == "windows"
        self.bin = pathlib.Path(args.bin).resolve() if args.bin else default_bin()
        self.out = pathlib.Path(args.out).resolve() if args.out else ROOT / "docs" / "perf"
        self.img = pathlib.Path(args.img).resolve() if args.img else (
            ROOT / "docs" / "img" if not args.out else self.out / "img")
        self.quick = args.quick
        exe = ".exe" if self.windows else ""
        self.lab = self.bin / f"mediaviewer_lab{exe}"
        self.frametime = self.bin / f"frametime{exe}"
        self.tests = self.bin / f"mv_tests{exe}"
        self.ai_tests = self.bin / f"mv_ai_tests{exe}"

    def secs(self, full):
        return max(3, full // 12) if self.quick else full


def lab_soak(ctx, seconds, report, media, static=False, extra=()):
    cmd = [str(ctx.lab), "--soak", str(seconds), "--json", str(report)]
    if static:
        cmd.append("--static")
    return cmd + list(extra) + [str(media)]


class Pacing(Stage):
    def __init__(self):
        super().__init__("pacing", "60 s animated + idle soaks (frametime)", ["perf-pacing.svg"])

    def needs(self, ctx):
        return [ctx.frametime, ctx.lab]

    def plan(self, ctx):
        return [[str(ctx.frametime), "--seconds", str(ctx.secs(60)), "--lab", str(ctx.lab)]]

    def after(self, ctx):
        # frametime writes beside itself; the charts read them from the perf directory.
        for src, dst in (("frametime-report.json", "frametime-animated.json"),
                         ("frametime-idle-report.json", "frametime-idle.json")):
            if (ctx.bin / src).exists():
                shutil.copyfile(ctx.bin / src, ctx.out / dst)

    def outputs(self, ctx):
        return [ctx.out / "frametime-animated.json", ctx.out / "frametime-idle.json"]


class Pan(Stage):
    def __init__(self):
        super().__init__("pan", "60 s scripted pan over a 42 MP RAW", ["perf-pacing.svg"],
                         windows_only=True)

    def needs(self, ctx):
        return [ctx.lab, MEDIA / PAN_STILL]

    def plan(self, ctx):
        return [lab_soak(ctx, ctx.secs(60), ctx.out / "pan-soak-42mp-arw.json", MEDIA / PAN_STILL,
                         extra=["--pan-soak"])]

    def outputs(self, ctx):
        return [ctx.out / "pan-soak-42mp-arw.json"]


class FirstPixel(Stage):
    def __init__(self):
        super().__init__("first-pixel", "6 s static soak per camera file", ["perf-first-pixel.svg"])

    def needs(self, ctx):
        return [ctx.lab] + [MEDIA / s for s in STILLS]

    def plan(self, ctx):
        cmds = []
        for s in STILLS:
            f = MEDIA / s
            # One untimed open first so the file cache is warm, as the published runs were.
            cmds.append(lab_soak(ctx, 2, ctx.out / "first-pixel" / f"{f.name}.warm.json", f, static=True))
            cmds.append(lab_soak(ctx, 6, ctx.out / "first-pixel" / f"{f.name}.json", f, static=True))
        return cmds

    def after(self, ctx):
        for p in (ctx.out / "first-pixel").glob("*.warm.json"):
            p.unlink()

    def outputs(self, ctx):
        return [ctx.out / "first-pixel" / f"{pathlib.Path(s).name}.json" for s in STILLS]


class Browse(Stage):
    def __init__(self):
        super().__init__("browse", "arrow through the RAW folder, and jumps", ["perf-browse.svg"])

    def needs(self, ctx):
        return [ctx.lab, MEDIA / "raw"]

    def plan(self, ctx):
        return [[str(ctx.lab), "--browse-soak", "--json", str(ctx.out / "browse.json"), str(MEDIA / "raw")]]

    def outputs(self, ctx):
        return [ctx.out / "browse.json"]


class Video(Stage):
    def __init__(self):
        super().__init__("video", "60 s of 1080p HEVC on screen", ["perf-video.svg"])

    def needs(self, ctx):
        return [ctx.lab, MEDIA / AV_CLIP]

    def plan(self, ctx):
        # The lab also writes <report>.video.json (selection counters, A/V error).
        return [lab_soak(ctx, ctx.secs(60), ctx.out / "investigation" / "video-native-60s.json",
                         MEDIA / AV_CLIP)]

    def outputs(self, ctx):
        r = ctx.out / "investigation" / "video-native-60s.json"
        return [r, r.with_name(r.name + ".video.json")]


class AvSync(Stage):
    def __init__(self):
        super().__init__("av-sync", "120 s A/V error trace", ["perf-av-sync.svg"], windows_only=True)

    def needs(self, ctx):
        return [ctx.lab, MEDIA / AV_CLIP]

    def plan(self, ctx):
        return [[str(ctx.lab), "--av-soak", str(ctx.secs(120)), "--csv",
                 str(ctx.out / "investigation" / "av-after-120s.csv"), str(MEDIA / AV_CLIP)]]

    def outputs(self, ctx):
        return [ctx.out / "investigation" / "av-after-120s.csv"]


class Compare(Stage):
    def __init__(self):
        super().__init__("compare", "MediaViewer vs Windows Photos / Media Player, timed from the screen",
                         ["compare-open.svg", "compare-pan.svg", "compare-video.svg"], windows_only=True)

    def needs(self, ctx):
        # compare-apps.ps1 starts build\bin\Release\mediaviewer_lab.exe.
        return [ROOT / "build" / "bin" / "Release" / "mediaviewer_lab.exe"] + \
               [MEDIA / s for s in (PAN_STILL, "raw/nikon_d7500.nef", "heif/libheif-example.heic")] + \
               [MEDIA / c for c in COMPARE_CLIPS]

    def plan(self, ctx):
        reps, secs = ("1", "4") if ctx.quick else ("2", "12")
        return [["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                 str(ROOT / "tools" / "perf" / "compare-screen.ps1"), "-OpenReps", reps,
                 "-PanSeconds", secs, "-PlaySeconds", secs, "-OutDir", str(ctx.out / "compare")]]

    def outputs(self, ctx):
        return [ctx.out / "compare" / "screen.json"]


class Bench(Stage):
    def __init__(self):
        super().__init__("bench", "headless decode / colour / folder timings (mv_tests [.perf-bench])", [])

    def needs(self, ctx):
        return [ctx.tests]

    def plan(self, ctx):
        return [[str(ctx.tests), "[.perf-bench]"]]

    def env(self, ctx):
        return {"MV_BENCH_JSON": str(ctx.out / "bench.json")}

    def outputs(self, ctx):
        return [ctx.out / "bench.json"]


class Search(Stage):
    """Local search accuracy: mv_ai_tests "[.calibration]" over a labelled photo set at library
    sizes (plan/17, issue #85). The per-query JSONL (MBs) goes to a temporary folder; search.json
    keeps the default Precision level's summary per tower and size."""

    timing = False
    SIZES = (1000, 5000, 10000, 25000)

    def __init__(self):
        super().__init__("search", "Local search accuracy at library sizes (mv_ai_tests [.calibration])",
                         ["search-accuracy.svg"])
        self.raw = None

    def needs(self, ctx):
        pack, evald = os.environ.get("MV_AI_PACK_DIR", ""), os.environ.get("MV_AI_EVAL_DIR", "")
        return [ctx.ai_tests, pack or "MV_AI_PACK_DIR (the AI pack)",
                (pathlib.Path(evald) / "labels.json") if evald else "MV_AI_EVAL_DIR (labelled photos)"]

    def plan(self, ctx):
        return [[str(ctx.ai_tests), "[.calibration]"]]

    def env(self, ctx):
        self.raw = pathlib.Path(tempfile.mkdtemp(prefix="mv-search-"))
        sizes = self.SIZES[:1] if ctx.quick else self.SIZES
        return {"MV_AI_CALIBRATION_SIZES": ",".join(map(str, sizes)),
                "MV_AI_CALIBRATION_OUT": str(self.raw / "{tower}.jsonl")}

    def after(self, ctx):
        if not self.raw:
            return
        rows = [json.loads(line) for f in sorted(self.raw.glob("*.jsonl")) for line in f.open()]
        shutil.rmtree(self.raw, ignore_errors=True)
        if not rows:
            return
        towers = {}
        for (tower, size) in sorted({(r["tower"], r["size"]) for r in rows}):
            rs = [r for r in rows if r["tower"] == tower and r["size"] == size and r["level"] == 2]
            kind = lambda *k: [r for r in rs if r["kind"] in k]
            caps, junk = kind("caption"), kind("nonsense_tune", "nonsense_heldout")
            cats = [r for r in kind("category") if "helicopter" not in r["text"]]
            lab = kind("labelled")
            answered = [r["rows"] for r in junk if r["passed"]]
            towers.setdefault(tower, {})[str(size)] = {
                "captions": len(caps),
                "captions_found": sum(r["passed"] for r in caps),
                "captions_own_photo": sum(r["found_own"] for r in caps),
                "nonsense": len(junk),
                "nonsense_answered": len(answered),
                "nonsense_rows_median": statistics.median(answered) if answered else 0,
                "category_precision": round(sum(r["relevant"] for r in cats) / max(1, sum(r["rows"] for r in cats)), 3),
                "category_recall": round(sum(r["relevant"] for r in cats) / max(1, sum(r["pool"] for r in cats)), 3),
                "labelled_p5_mean": round(statistics.mean(r["p5"] for r in lab), 3),
                "labelled_p5_min": min(r["p5"] for r in lab),
                "absent_subject_rows": [r["rows"] for r in kind("category") if "helicopter" in r["text"]],
            }
        report = {"precision_level": 2, "eval": os.environ.get("MV_AI_EVAL_NAME", "labelled photo set"),
                  "machine": machine_info(ctx), "towers": towers}
        (ctx.out / "search.json").write_text(json.dumps(report, indent=2) + "\n")

    def outputs(self, ctx):
        return [ctx.out / "search.json"]


STAGES = [Pacing(), Pan(), FirstPixel(), Browse(), Video(), AvSync(), Compare(), Bench(), Search()]


def machine_info(ctx):
    info = {
        "date": datetime.date.today().isoformat(),
        "os": f"{platform.system()} {platform.release()}",
        "machine": platform.machine(),
        "cpu": platform.processor() or platform.machine(),
        "quick": ctx.quick,
    }
    try:
        info["commit"] = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT,
                                        capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        pass
    if MAC:
        try:
            info["cpu"] = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                         capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    return info


def keep_display_awake():
    """A sleeping display stops the display link / DXGI presents and voids every soak."""
    if MAC:
        try:
            return subprocess.Popen(["caffeinate", "-d", "-i", "-w", str(os.getpid())])
        except OSError:
            return None
    if WINDOWS:
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED, ES_DISPLAY_REQUIRED = 0x80000000, 0x1, 0x2
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("--bin", help="directory holding mediaviewer_lab, frametime, mv_tests")
    ap.add_argument("--out", help="report directory (default docs/perf; charts then go to <out>/img)")
    ap.add_argument("--img", help="chart directory (default docs/img, or <out>/img with --out)")
    ap.add_argument("--only", help="comma-separated stage names")
    ap.add_argument("--skip", help="comma-separated stage names")
    ap.add_argument("--quick", action="store_true", help="short soaks: checks the suite, not the product")
    ap.add_argument("--list", action="store_true", help="show the stages and what each is missing")
    ap.add_argument("--dry-run", action="store_true", help="print the commands only")
    ap.add_argument("--charts-only", action="store_true", help="redraw from the reports already there")
    ap.add_argument("--platform", choices=("auto", "windows", "mac"), default="auto",
                    help=argparse.SUPPRESS)  # tests plan the Windows run on any machine
    args = ap.parse_args(argv)
    ctx = Context(args)

    if args.quick and not args.out and not args.dry_run and not args.list:
        ap.error("--quick writes short, non-representative soaks; give --out so docs/perf is not overwritten")

    names = {s.name for s in STAGES}
    only = set(args.only.split(",")) if args.only else names
    skip = set(args.skip.split(",")) if args.skip else set()
    unknown = (only | skip) - names
    if unknown:
        ap.error(f"unknown stage(s): {', '.join(sorted(unknown))}; see --list")
    stages = [] if args.charts_only else [s for s in STAGES if s.name in only and s.name not in skip]

    if args.list:
        for s in STAGES:
            gone = s.missing(ctx)
            state = "ready" if not gone else "missing " + "; ".join(gone)
            print(f"{s.name:12} {s.what}\n{'':12} -> {', '.join(s.charts) or 'bench.json'}  [{state}]")
        return 0

    awake = None
    if not args.dry_run:
        ctx.out.mkdir(parents=True, exist_ok=True)
        if stages:
            awake = keep_display_awake()
    failures, skipped = [], []
    for s in stages:
        gone = s.missing(ctx)
        if gone and not args.dry_run:
            skipped.append((s.name, gone))
            print(f"== {s.name}: skipped, missing {'; '.join(gone)}")
            continue
        print(f"== {s.name}: {s.what}" + (f"  (here: missing {'; '.join(gone)})" if gone else ""))
        if not args.dry_run:
            for out in s.outputs(ctx):
                out.parent.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, **(s.env(ctx) if hasattr(s, "env") else {}))
        started = time.time()
        for cmd in s.plan(ctx):
            print("   $", " ".join(cmd))
            if args.dry_run:
                continue
            t0 = time.monotonic()
            # A failed gate still writes its report (the chart shows the failure), and a
            # short A/V soak exits 4 by design, so the exit code is reported, not judged.
            code = subprocess.run(cmd, cwd=ROOT, env=env).returncode
            print(f"   exit {code} after {time.monotonic() - t0:.1f} s")
        if args.dry_run:
            continue
        s.after(ctx)
        stale = [p for p in s.outputs(ctx) if not p.exists() or p.stat().st_mtime < started - 1]
        if stale:
            failures.append((s.name, "did not write " + ", ".join(str(p) for p in stale)))

    if awake:
        awake.terminate()
    if args.dry_run:
        return 0
    if any(s.timing for s in stages):
        (ctx.out / "machine.json").write_text(json.dumps(machine_info(ctx), indent=2) + "\n")
    charts = subprocess.run([sys.executable, str(ROOT / "tools" / "perf" / "make-charts.py"),
                             "--perf", str(ctx.out), "--img", str(ctx.img)], cwd=ROOT)
    for name, gone in skipped:
        print(f"skipped {name}: {'; '.join(gone)}")
    for name, why in failures:
        print(f"FAILED {name}: {why}", file=sys.stderr)
    if args.quick:
        print("--quick: these reports are a smoke test of the suite; do not publish them.")
    return 1 if failures or charts.returncode else 0


if __name__ == "__main__":
    sys.exit(main())
