#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage the AI pack's model files (plan/17 "The AI pack").

    ai-models.py stage --piece ai --out build/addons/ai [--cache DIR]
    ai-models.py stage --piece ai-faces --out build/addons/ai-faces [--cache DIR]
    ai-models.py check

`stage` downloads every file ai-models.json lists for a piece from its pinned
revision, verifies size and SHA-256, and lays it out as the add-on reads it
(src/addons/ai/pack.h): models/<folder>/<file> plus a model.json per model
folder, and `licences.json` (relative path -> SPDX) for addon-pack.py. A file
already in --cache with the right hash is not fetched again. The request is a
plain GET of a fixed URL: no token, no identifier.

`check` is the weights-licence gate plan/17 names: every file must carry a
licence from ALLOWED_LICENCES (non-commercial or research-only weights fail),
and the Core pack's supported combinations must fit the 3 GB ceiling given
the sizes listed. CI runs it; it needs no network.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
MODELS = Path(__file__).with_name("ai-models.json")

ALLOWED_LICENCES = {"MIT", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "Zlib", "ISC", "CC0-1.0",
                    "GPL-2.0-or-later"}
CEILING = 3_000_000_000  # plan/17: Core + one vendor piece + Faces, installed


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load() -> dict:
    return json.loads(MODELS.read_text(encoding="utf-8"))


def fetch(entry: dict, cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / f"{entry['sha256']}-{Path(entry['file']).name}"
    if target.exists() and target.stat().st_size == entry["size"] and sha256_file(target) == entry["sha256"]:
        return target
    url = f"https://huggingface.co/{entry['repo']}/resolve/{entry['revision']}/{entry['file']}"
    tmp = target.with_suffix(target.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "MediaViewer-pack-builder"})
    with urllib.request.urlopen(req, timeout=120) as r, tmp.open("wb") as out:
        shutil.copyfileobj(r, out, 1 << 20)
    if tmp.stat().st_size != entry["size"] or sha256_file(tmp) != entry["sha256"]:
        tmp.unlink()
        raise ValueError(f"{entry['repo']}/{entry['file']}: size or SHA-256 differs from ai-models.json")
    tmp.replace(target)
    return target


def stage(args) -> int:
    spec = load()["pieces"].get(args.piece)
    if spec is None:
        print(f"unknown piece {args.piece}", file=sys.stderr)
        return 2
    out = Path(args.out)
    cache = Path(args.cache) if args.cache else out.parent / ".model-cache"
    licences = {}
    for folder, f in spec["folders"].items():
        dest = out / folder
        dest.mkdir(parents=True, exist_ok=True)
        for entry in f["files"]:
            src = fetch(entry, cache)
            target = dest / entry["as"]
            if not target.exists() or sha256_file(target) != entry["sha256"]:
                shutil.copyfile(src, target)
            licences[f"{folder}/{entry['as']}"] = entry["licence"]
        if "model" in f:
            text = json.dumps(f["model"], indent=1) + "\n"
            (dest / "model.json").write_text(text, encoding="utf-8")
            licences[f"{folder}/model.json"] = "GPL-2.0-or-later"
    # Merge with licences from an earlier stage into the same folder.
    lic_path = out / "licences.json"
    if lic_path.exists():
        prev = json.loads(lic_path.read_text(encoding="utf-8"))
        prev.update(licences)
        licences = prev
    lic_path.write_text(json.dumps(licences, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(f"staged {args.piece} into {out}")
    return 0


def check(_args) -> int:
    data = load()
    bad = []
    sizes = {}
    for piece, spec in data["pieces"].items():
        total = 0
        for folder, f in spec["folders"].items():
            for entry in f["files"]:
                if entry["licence"] not in ALLOWED_LICENCES:
                    bad.append(f"{piece}:{folder}/{entry['as']} is {entry['licence']}")
                for key in ("repo", "revision", "file", "as", "size", "sha256"):
                    if key not in entry:
                        bad.append(f"{piece}:{folder}/{entry.get('as', '?')} lacks {key}")
                if len(entry.get("revision", "")) != 40 or len(entry.get("sha256", "")) != 64:
                    bad.append(f"{piece}:{folder}/{entry.get('as', '?')} is not pinned")
                total += entry.get("size", 0)
        sizes[piece] = total
    # The largest supported combination: Core models + Faces + the runtime and
    # a vendor piece (ORT CUDA: ~205 MB; allow 400 MB for native code and chrome).
    worst = sizes.get("ai", 0) + sizes.get("ai-faces", 0) + 400_000_000 + 250_000_000
    if worst > CEILING:
        bad.append(f"Core + Faces + a vendor piece is ~{worst / 1e9:.2f} GB, over the 3 GB ceiling")
    for b in bad:
        print(b, file=sys.stderr)
    if not bad:
        print(f"ok: models {sizes}, worst supported combination ~{worst / 1e9:.2f} GB of 3 GB")
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("--piece", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--cache")
    sub.add_parser("check")
    args = ap.parse_args(argv)
    return stage(args) if args.cmd == "stage" else check(args)


if __name__ == "__main__":
    sys.exit(main())
