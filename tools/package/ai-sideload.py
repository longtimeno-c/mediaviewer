#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack, sign and sideload the AI pack for a local validation run (plan/17
"Offline / sideload"; Milestone H).

    ai-sideload.py --build <build dir> --models <staged pieces root> \
        --key <dev key hex file> --addons <add-ons folder> [--platform win-x64|macos] \
        [--pieces ai,ai-audio,ai-faces,ai-cuda] [--version 1.0.0]

For each piece it lays out the files as the release does (the native add-on,
ONNX Runtime and the chrome from <build>/addons/<piece>, the models staged by
ai-models.py under <models>/<piece>), packs and signs them with addon-pack.py,
then unpacks the archive into <addons>/<folder>/<version>/ beside the signed
manifest: exactly what the app's own installer leaves, so the app verifies it
the same way on load.

Only a build configured with -DMV_ADDON_DEV_PUBLIC_KEY=<the key's public half>
trusts it; point that build at the folder with MV_DEV_ADDONS_DIR (and keep its
thumbnails apart with MV_DEV_THUMBS_DIR). A release build refuses it.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("addon_pack", HERE / "addon-pack.py")
addon_pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(addon_pack)


def gather(piece: str, platform: str, build: Path, models: Path, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    built = build / "addons" / piece
    include = addon_pack.ADDONS[piece]["files"][platform]["include"]
    for name in include:
        if name == "models":
            continue
        src = built / name
        if not src.exists():
            raise FileNotFoundError(f"{piece}: {name} missing from {built}")
        (shutil.copytree if src.is_dir() else shutil.copy2)(src, out / name)
    if "models" in include:
        staged = models / piece
        shutil.copytree(staged / "models", out / "models")
        shutil.copy2(staged / "licences.json", out / "licences.json")
    notices = build / "_ort"
    for n in notices.glob("*/ThirdPartyNotices.txt"):
        shutil.copy2(n, out / "ThirdPartyNotices.txt")
        break


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", required=True)
    ap.add_argument("--models", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--addons", required=True)
    ap.add_argument("--platform", default="macos" if sys.platform == "darwin" else "win-x64")
    ap.add_argument("--pieces", default="ai,ai-audio,ai-faces")
    ap.add_argument("--version", default="1.0.0")
    a = ap.parse_args(argv)
    addons = Path(a.addons)
    addons.mkdir(parents=True, exist_ok=True)
    # Beside the target, not in %TEMP%: a Core pack is gigabytes, and the
    # system drive is where there is least room.
    work = Path(tempfile.mkdtemp(prefix=".mv-sideload-", dir=str(addons.parent)))
    try:
        for piece in [p for p in a.pieces.split(",") if p]:
            if a.platform not in addon_pack.ADDONS[piece]["files"]:
                print(f"skip {piece}: not built for {a.platform}")
                continue
            src = work / piece
            gather(piece, a.platform, Path(a.build), Path(a.models), src)
            addon_pack.main(["pack", "--addon", piece, "--platform", a.platform, "--src", str(src),
                             "--version", a.version, "--key", a.key, "--out", str(work / "dist")])
            base = work / "dist" / f"mediaviewer-addon-{piece}-{a.platform}"
            manifest = json.loads(Path(str(base) + ".json").read_bytes())
            folder = manifest["name"] if a.platform == "macos" else manifest["id"]
            target = addons / folder / a.version
            if target.exists():
                shutil.rmtree(target)
            target.mkdir(parents=True)
            with zipfile.ZipFile(str(base) + ".zip") as z:
                z.extractall(target)
            shutil.copy2(str(base) + ".json", target / "manifest.json")
            shutil.copy2(str(base) + ".json.sig", target / "manifest.json.sig")
            print(f"sideloaded {piece} {a.version} -> {target} ({manifest['installed_size'] / 1e6:.0f} MB)")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
