#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage, pack, sign and sideload the Enhance add-on for a local validation run
(docs/plans/enhance.md E1).

    enhance-sideload.py --build <build dir> --key <dev key hex file> --addons <add-ons folder> \
        [--models tests/data/enhance/fixture-models.json] [--upstream remote|bundle] [--cache DIR] \
        [--platform win-x64|macos] [--version 1.0.0]

Stages the catalogue with enhance-models.py (by default the fixture, whose
files are real Hugging Face pins already verified for Local search; with
--upstream remote, the default, the add-on itself downloads the model you
choose), lays it beside the native add-on built in <build>/addons/enhance,
then packs, signs and unpacks it into <addons>/<folder>/<version>/ with
ai-sideload.py: exactly what the app's own installer leaves. --upstream
bundle puts the weights in the pack instead (offline), fetched once into
--cache (default <build>/addons/.model-cache, ai-models.py's own).

As with ai-sideload.py, only a build configured with
-DMV_ADDON_DEV_PUBLIC_KEY=<the key's public half> trusts it; point that build
at the folder with MV_DEV_ADDONS_DIR. A release build refuses it.
"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def load(name: str, file: str):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ai_sideload = load("ai_sideload", "ai-sideload.py")
enhance_models = load("enhance_models", "enhance-models.py")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--addons", required=True)
    ap.add_argument("--models", default=str(ROOT / "tests/data/enhance/fixture-models.json"))
    ap.add_argument("--upstream", choices=["bundle", "remote"], default="remote")
    ap.add_argument("--cache")
    ap.add_argument("--platform", default="macos" if sys.platform == "darwin" else "win-x64")
    ap.add_argument("--version", default="1.0.0")
    a = ap.parse_args(argv)
    addons = Path(a.addons)
    addons.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix=".mv-enhance-stage-", dir=str(addons.parent)))
    try:
        cache = a.cache or str(Path(a.build) / "addons" / ".model-cache")
        if enhance_models.main(["stage", "--models", a.models, "--out", str(work / "enhance"),
                                "--upstream", a.upstream, "--cache", cache]) != 0:
            return 1
        return ai_sideload.main(["--build", a.build, "--models", str(work), "--key", a.key, "--addons", a.addons,
                                 "--platform", a.platform, "--pieces", "enhance", "--version", a.version])
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
