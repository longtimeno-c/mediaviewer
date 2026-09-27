#!/usr/bin/env python3
"""Tests for the chart suite (regenerate.py + make-charts.py). Standard library only.

    python tools/perf/test_perf_tools.py        # or: ctest -R perf_tools

They check the suite, not the product: every published chart reproduces byte-for-byte from
the committed reports, every chart's reports are written by some stage, and the planned
commands and partial-run behaviour hold on any machine (no harness is launched).
"""
import contextlib
import importlib.util
import io
import json
import pathlib
import shutil
import sys
import tempfile
import unittest
import xml.dom.minidom

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


charts = load("make_charts", "make-charts.py")
regen = load("regenerate", "regenerate.py")


def quiet(fn, *a):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        return fn(*a), out.getvalue(), err.getvalue()


class ChartsReproduce(unittest.TestCase):
    def test_every_published_chart_rebuilds_identically(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, _, err = quiet(charts.main, ["--img", tmp, "--strict"])
            self.assertEqual(code, 0, err)
            for name in charts.INPUTS:
                made = pathlib.Path(tmp) / name
                self.assertTrue(made.exists(), name)
                xml.dom.minidom.parse(str(made))  # well-formed SVG
                published = ROOT / "docs" / "img" / name
                self.assertEqual(made.read_bytes(), published.read_bytes(),
                                 f"docs/img/{name} is stale: run tools/perf/make-charts.py")

    def test_partial_reports_draw_what_they_can(self):
        with tempfile.TemporaryDirectory() as tmp:
            perf, img = pathlib.Path(tmp) / "perf", pathlib.Path(tmp) / "img"
            shutil.copytree(ROOT / "docs" / "perf" / "first-pixel", perf / "first-pixel")
            code, _, err = quiet(charts.main, ["--perf", str(perf), "--img", str(img)])
            self.assertEqual(code, 0)
            self.assertEqual(sorted(p.name for p in img.iterdir()), ["perf-first-pixel.svg"])
            self.assertIn("skipped perf-pacing.svg", err)
            code, _, _ = quiet(charts.main, ["--perf", str(perf), "--img", str(img), "--strict"])
            self.assertEqual(code, 1)


class SuitePlan(unittest.TestCase):
    def ctx(self, tmp):
        args = regen.argparse.Namespace(platform="windows", bin=str(pathlib.Path(tmp) / "bin"),
                                        out=str(pathlib.Path(tmp) / "out"), img=None, quick=False)
        return regen.Context(args)

    def test_every_chart_input_is_written_by_a_stage(self):
        with tempfile.TemporaryDirectory() as tmp:
            ctx = self.ctx(tmp)
            written = {p.relative_to(ctx.out).as_posix() for s in regen.STAGES for p in s.outputs(ctx)}
            for chart, inputs in charts.INPUTS.items():
                for rel in inputs:
                    self.assertIn(rel, written, f"{chart} reads {rel}, which no stage writes")

    def test_stage_charts_exist(self):
        for s in regen.STAGES:
            for chart in s.charts:
                self.assertIn(chart, charts.INPUTS, s.name)

    def test_windows_plan_runs_each_harness(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, out, _ = quiet(regen.main, ["--dry-run", "--platform", "windows", "--bin",
                                              str(pathlib.Path(tmp) / "bin"), "--out",
                                              str(pathlib.Path(tmp) / "out")])
            self.assertEqual(code, 0)
            for needle in ("frametime.exe --seconds 60", "--pan-soak", "--browse-soak",
                           "--av-soak 120", "compare-screen.ps1", "[.perf-bench]",
                           "--soak 6 --json"):
                self.assertIn(needle, out)
            self.assertFalse((pathlib.Path(tmp) / "out").exists(), "a dry run wrote files")

    def test_quick_never_overwrites_published_reports(self):
        with self.assertRaises(SystemExit):
            quiet(regen.main, ["--quick"])

    def test_unknown_stage_is_an_error(self):
        with self.assertRaises(SystemExit):
            quiet(regen.main, ["--only", "nope", "--dry-run"])

    def test_mac_plan_skips_windows_only_harnesses(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = regen.argparse.Namespace(platform="mac", bin=tmp, out=tmp, img=None, quick=False)
            ctx = regen.Context(args)
            windows_only = {s.name for s in regen.STAGES if s.missing(ctx) and "Windows" in s.missing(ctx)[0]}
            self.assertEqual(windows_only, {"pan", "av-sync", "compare"})


class CommittedReports(unittest.TestCase):
    def test_first_pixel_reports_have_the_fields_charted(self):
        for p in (ROOT / "docs" / "perf" / "first-pixel").glob("*.json"):
            r = json.loads(p.read_text())
            self.assertGreater(r["still_first_pixel_s"], 0, p.name)
            self.assertGreaterEqual(r["still_full_s"], r["still_first_pixel_s"], p.name)


if __name__ == "__main__":
    sys.exit(0 if unittest.main(exit=False, verbosity=1).result.wasSuccessful() else 1)
