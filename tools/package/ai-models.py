#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the AI pack's model files (docs/design/17 "The AI pack").

    ai-models.py stage --piece ai --out build/addons/ai [--cache DIR] [--upstream remote]
    ai-models.py stage --piece ai-faces --out build/addons/ai-faces [--cache DIR]
    ai-models.py check

`--upstream remote` (release builds, #482 T3): an upstream file is not staged
at all. It goes in `remote.json` at the piece's root, {url, path, size, sha256,
licence} each, which addon-pack.py signs into the manifest's `remote` list; the
add-on fetches it from Hugging Face on install and checks it against that list
(docs/plans/model-tiers.md T3). Only files we build ourselves (`export`) and
the model.json / tiers.json we write ship in the add-on. The default,
`--upstream bundle`, stages everything, as sideloaded dev packs need.

An entry with `export` is a checkpoint the pack runs as ONNX it builds itself
(the People embedder, docs/design/17 "People model"): `stage` verifies the pinned
source, runs the named tool (tools/package/face-export.py) and keeps the result
only if it reproduces `export.golden`. That step needs requirements-export.txt;
`check` does not.

`stage` downloads every file ai-models.json lists for a piece from its pinned
revision, verifies size and SHA-256, and lays it out as the add-on reads it
(src/addons/ai/pack.h): models/<folder>/<file> plus a model.json per model
folder, and `licences.json` (relative path -> SPDX) for addon-pack.py. A file
already in --cache with the right hash is not fetched again. The request is a
plain GET of a fixed URL: no token, no identifier.

Staging the Core piece (`ai`) also writes `models/tiers.json`: the model tiers
(docs/plans/model-tiers.md), each naming a model folder per role, which
src/addons/ai/tiers.h reads so the add-on's code names no model.

`check` is the weights-licence gate docs/design/17 names: every file must carry a
licence from ALLOWED_LICENCES (non-commercial or research-only weights fail),
every tier must name a model folder of the right piece for every role, and
every upstream file must be a pinned Hugging Face file. CI runs it; it needs no
network. (The 5 GB ceiling it once checked is retired: the add-on carries no
upstream model, docs/design/12 2026-10-18.)
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
                    "GPL-3.0-or-later"}
# The piece each role's model folders live in (src/addons/ai/tiers.h). Pictures
# and speech have a high and a fast slot (mv_ai_quality 2 and 1).
ROLES = {"pictures": "ai", "speech": "ai-audio", "sounds": "ai-audio", "faces": "ai-faces",
         "documents": "ai-docs"}
SLOTTED = {"pictures", "speech"}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load() -> dict:
    return json.loads(MODELS.read_text(encoding="utf-8"))


def url_of(entry: dict) -> str:
    """The fixed URL of a pinned upstream file: a plain GET, no token, no query."""
    return f"https://huggingface.co/{entry['repo']}/resolve/{entry['revision']}/{entry['file']}"


def tier_folders(data: dict, profile: dict) -> dict:
    """Piece -> the model folders a tier needs, tokenizer folders included."""
    out = {}
    for role, piece in ROLES.items():
        value = profile.get(role)
        folders = [f for f in (value.values() if isinstance(value, dict) else [value]) if f]
        for folder in folders:
            want = out.setdefault(piece, [])
            if folder not in want:
                want.append(folder)
            tok = data["pieces"][piece]["folders"][folder]["model"].get("tokenizer")
            tok_folder = f"models/{tok}" if tok else None
            if tok_folder and tok_folder in data["pieces"][piece]["folders"] and tok_folder not in want:
                want.append(tok_folder)
    return out


def fetch(entry: dict, cache: Path) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / f"{entry['sha256']}-{Path(entry['file']).name}"
    if target.exists() and target.stat().st_size == entry["size"] and sha256_file(target) == entry["sha256"]:
        return target
    url = url_of(entry)
    tmp = target.with_suffix(target.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "MediaViewer-pack-builder"})
    with urllib.request.urlopen(req, timeout=120) as r, tmp.open("wb") as out:
        shutil.copyfileobj(r, out, 1 << 20)
    if tmp.stat().st_size != entry["size"] or sha256_file(tmp) != entry["sha256"]:
        tmp.unlink()
        raise ValueError(f"{entry['repo']}/{entry['file']}: size or SHA-256 differs from ai-models.json")
    tmp.replace(target)
    return target


def export(entry: dict, src: Path, target: Path) -> None:
    ex = entry["export"]
    if ex.get("tool") != "face-export.py":
        raise ValueError(f"{entry['as']}: unknown export tool {ex.get('tool')}")
    golden = " ".join(str(v) for v in ex["golden"])
    cmd = [sys.executable, str(Path(__file__).with_name("face-export.py")), "--arch", ex["arch"],
           "--weights", str(src), "--out", str(target), "--expect", golden]
    if ex.get("fp16"):
        cmd.append("--fp16")
    import subprocess
    subprocess.run(cmd, check=True)


def tier_problems(data: dict) -> list:
    """Why the tiers block is unusable, or [] (check runs this; stage refuses on it)."""
    tiers = data.get("tiers")
    if not isinstance(tiers, dict) or not isinstance(tiers.get("profiles"), dict) or not tiers["profiles"]:
        return ["tiers: no profiles"]
    bad = []
    profiles = tiers["profiles"]
    if tiers.get("default") not in profiles:
        bad.append(f"tiers: default {tiers.get('default')!r} is not a profile")
    for tier, prof in profiles.items():
        if not isinstance(prof, dict) or not isinstance(prof.get("name"), str) or not prof["name"]:
            bad.append(f"tiers.{tier}: lacks a name")
            continue
        for role, piece in ROLES.items():
            want = data["pieces"].get(piece, {}).get("folders", {})
            value = prof.get(role)
            slots = value if role in SLOTTED else {"": value}
            if not isinstance(slots, dict) or not any(slots.values()):
                bad.append(f"tiers.{tier}: no {role} model")
                continue
            for slot, folder in slots.items():
                where = f"tiers.{tier}.{role}{'.' + slot if slot else ''}"
                if role in SLOTTED and slot not in ("high", "fast"):
                    bad.append(f"{where}: unknown slot")
                elif folder is None:
                    continue
                elif not isinstance(folder, str) or folder not in want or "model" not in want[folder]:
                    bad.append(f"{where}: {folder!r} is not a model folder of the {piece} piece")
    return bad


def stage(args) -> int:
    spec = load()["pieces"].get(args.piece)
    if spec is None:
        print(f"unknown piece {args.piece}", file=sys.stderr)
        return 2
    out = Path(args.out)
    cache = Path(args.cache) if args.cache else out.parent / ".model-cache"
    licences = {}
    remote = []
    for folder, f in spec["folders"].items():
        dest = out / folder
        dest.mkdir(parents=True, exist_ok=True)
        for entry in f["files"]:
            if args.upstream == "remote" and "export" not in entry:
                remote.append({"url": url_of(entry), "path": f"{folder}/{entry['as']}", "size": entry["size"],
                               "sha256": entry["sha256"], "licence": entry["licence"]})
                stale = dest / entry["as"]
                if stale.exists():
                    stale.unlink()
                continue
            src = fetch(entry, cache)
            target = dest / entry["as"]
            if "export" in entry:
                export(entry, src, target)
            elif not target.exists() or sha256_file(target) != entry["sha256"]:
                shutil.copyfile(src, target)
            licences[f"{folder}/{entry['as']}"] = entry["licence"]
        if "model" in f:
            text = json.dumps(f["model"], indent=1) + "\n"
            (dest / "model.json").write_text(text, encoding="utf-8")
            licences[f"{folder}/model.json"] = "GPL-3.0-or-later"
    if args.piece == "ai":
        data = load()
        problems = tier_problems(data)
        if problems:
            for p in problems:
                print(p, file=sys.stderr)
            return 1
        tiers = {k: v for k, v in data["tiers"].items() if k != "comment"}
        for tier, prof in tiers["profiles"].items():
            prof["folders"] = tier_folders(data, prof)
        (out / "models").mkdir(parents=True, exist_ok=True)
        (out / "models" / "tiers.json").write_text(json.dumps(tiers, indent=1) + "\n", encoding="utf-8")
        licences["models/tiers.json"] = "GPL-3.0-or-later"
    remote_path = out / "remote.json"
    if remote:
        remote_path.write_text(json.dumps(remote, indent=1) + "\n", encoding="utf-8")
    elif remote_path.exists():
        remote_path.unlink()
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
    bad = tier_problems(data)
    sizes = {}
    hf = "https://huggingface.co/"
    for piece, spec in data["pieces"].items():
        total = 0
        for folder, f in spec["folders"].items():
            for entry in f["files"]:
                if entry["licence"] not in ALLOWED_LICENCES:
                    bad.append(f"{piece}:{folder}/{entry['as']} is {entry['licence']}")
                for key in ("repo", "revision", "file", "as", "size", "sha256"):
                    if key not in entry:
                        bad.append(f"{piece}:{folder}/{entry.get('as', '?')} lacks {key}")
                ex = entry.get("export")
                if ex is not None and (ex.get("tool") != "face-export.py" or len(ex.get("golden", [])) != 17):
                    bad.append(f"{piece}:{folder}/{entry.get('as', '?')} has an export without its golden check")
                if len(entry.get("revision", "")) != 40 or len(entry.get("sha256", "")) != 64:
                    bad.append(f"{piece}:{folder}/{entry.get('as', '?')} is not pinned")
                elif not url_of(entry).startswith(hf) or any(c in url_of(entry) for c in "?#"):
                    bad.append(f"{piece}:{folder}/{entry.get('as', '?')} is not a plain Hugging Face URL")
                total += (ex or {}).get("installed_size", entry.get("size", 0))
        sizes[piece] = total
    for b in bad:
        print(b, file=sys.stderr)
    if not bad:
        print(f"ok: models {sizes}")
    return 1 if bad else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("stage")
    s.add_argument("--piece", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--cache")
    s.add_argument("--upstream", choices=["bundle", "remote"], default="bundle",
                   help="remote: list upstream files for the add-on to fetch instead of staging them")
    sub.add_parser("check")
    args = ap.parse_args(argv)
    return stage(args) if args.cmd == "stage" else check(args)


if __name__ == "__main__":
    sys.exit(main())
