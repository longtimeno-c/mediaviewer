#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check and stage the Enhance add-on's model catalogue (docs/plans/enhance.md
"How a model is chosen").

    enhance-models.py check [--models PATH]
    enhance-models.py stage --out build/addons/enhance [--models PATH] [--upstream remote|bundle] [--cache DIR]

The catalogue (enhance-models.json beside this script; the fixture is
tests/data/enhance/fixture-models.json) lists, per kind (still, video,
audio), the models offered from the largest to the smallest, and per model
what it needs (run_bytes, cache_bytes, accelerators, cpu_unit_ms), the knobs
the sheet shows and the files it runs, pinned by Hugging Face revision and
SHA-256. A new model is an entry here, not a C++ change.

`check` is the catalogue's gate; it needs no network. Every file and model
must carry a licence from ai-models.py's ALLOWED_LICENCES (one allow-list for
both add-ons), every file must be a pinned, plain Hugging Face file, and none
may be over the 5 GB per-file cap (docs/design/12, 2026-10-13). A kind names
only models that serve it, and a model is named by every kind it serves.

`stage` runs `check`, then writes what src/addons/enhance reads:
models/enhance.json (the kinds), models/<id>/model.json (each model block as
written) and licences.json (relative path -> SPDX) for addon-pack.py. It owns
<out>/models: anything else under it is removed. With `--upstream remote`
(the default; release builds) no weights are staged: they go in remote.json,
{url, path, size, sha256, licence} each, which addon-pack.py signs into the
manifest's `remote`, and the add-on fetches the chosen model's files when
Download is clicked. `--upstream bundle` fetches them now into
models/<id>/ (an offline dev pack); a file already in --cache with the right
hash is not fetched again. The request is a plain GET of the pinned URL.
"""
import argparse
import importlib.util
import json
import math
from pathlib import Path
import re
import shutil
import sys

HERE = Path(__file__).resolve().parent
MODELS = HERE / "enhance-models.json"

_spec = importlib.util.spec_from_file_location("ai_models", HERE / "ai-models.py")
ai_models = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ai_models)
# The weights-licence gate and the pin's URL are Local search's (docs/design/17).
ALLOWED_LICENCES = ai_models.ALLOWED_LICENCES

KINDS = ("still", "video", "audio")
ACCELERATORS = {"cuda", "coreml"}
# Per weights file, not per add-on (docs/design/12, 2026-10-13). addon-pack.py
# and the add-on's fetcher refuse the same figure.
FILE_CAP = 5_000_000_000
GPL = "GPL-3.0-or-later"

MODEL_KEYS = {"id", "name", "model", "kinds", "run_bytes", "cache_bytes", "accelerators", "cpu_unit_ms",
              "licence", "knobs"}
FILE_KEYS = ("repo", "revision", "file", "as", "size", "sha256", "licence")
KNOB_KEYS = {"slider": {"min", "max", "step"}, "choice": {"options"}, "toggle": set()}
KNOB_COMMON = {"id", "label", "type", "kinds", "default"}


def load(path: Path):
    """The catalogue, and every key an object of it lists twice (json keeps the last)."""
    twice = []

    def pairs(items):
        out = {}
        for k, v in items:
            if k in out:
                twice.append(k)
            out[k] = v
        return out

    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=pairs), twice


def is_int(v) -> bool:
    """A JSON integer the add-on's reader holds exactly (a double's 53 bits)."""
    return isinstance(v, int) and not isinstance(v, bool) and -(1 << 53) < v < (1 << 53)


def is_number(v) -> bool:
    return is_int(v) or (isinstance(v, float) and math.isfinite(v))


def is_text(v, most: int = 200) -> bool:
    return isinstance(v, str) and 0 < len(v) <= most


def safe_relative(p) -> bool:
    """A file name or a relative path under the model's folder: no .., no root, no drive."""
    if not is_text(p, 255) or p.startswith(("/", "\\")) or "\\" in p or ":" in p:
        return False
    if any(ord(c) <= 0x20 or ord(c) >= 0x7F for c in p):
        return False
    return all(part not in ("", ".", "..") for part in p.split("/"))


def plain_hf_url(url: str) -> bool:
    """What the app fetches (src/addon/manifest.cpp remote_url_ok): Hugging Face over
    HTTPS, a fixed /resolve/ path, no query, fragment or credentials."""
    hf = "https://huggingface.co/"
    return (url.startswith(hf) and len(url) <= 1024 and "/resolve/" in url and "/../" not in url and
            not any(c in "?#@\\" or ord(c) <= 0x20 or ord(c) >= 0x7F for c in url))


def one_of(v, allowed) -> bool:
    return isinstance(v, str) and v in allowed


def kind_list(v) -> bool:
    return isinstance(v, list) and bool(v) and all(one_of(k, KINDS) for k in v) and len(set(v)) == len(v)


def knob_problems(where: str, knob, kinds: list, seen: set) -> list:
    if not isinstance(knob, dict):
        return [f"{where}: a knob is not an object"]
    kid = knob.get("id")
    where = f"{where}.knobs.{kid}"
    bad = []
    if not isinstance(kid, str) or not re.fullmatch(r"[a-z0-9_]{1,32}", kid):
        bad.append(f"{where}: a knob id is [a-z0-9_]{{1,32}}")
    elif kid in seen:
        bad.append(f"{where}: listed twice")
    else:
        seen.add(kid)
    if not is_text(knob.get("label")):
        bad.append(f"{where}: needs a label of 1 to 200 characters")
    kt = knob.get("type")
    if not one_of(kt, KNOB_KEYS):
        return bad + [f"{where}: type {kt!r} is not slider, choice or toggle"]
    for k in sorted(set(knob) - KNOB_COMMON - KNOB_KEYS[kt]):
        bad.append(f"{where}: unknown field {k}")
    if not kind_list(knob.get("kinds")) or not set(knob["kinds"]) <= set(kinds):
        bad.append(f"{where}: kinds must be some of the model's own ({', '.join(kinds)})")
    default = knob.get("default")
    if kt == "slider":
        lo, hi, step = knob.get("min"), knob.get("max"), knob.get("step")
        if not (is_number(lo) and is_number(hi) and lo < hi):
            bad.append(f"{where}: needs a number min below a number max")
        elif not is_number(step) or step <= 0 or step > hi - lo:
            bad.append(f"{where}: needs a step above 0 and within the range")
        elif not is_number(default) or not lo <= default <= hi:
            bad.append(f"{where}: default {default!r} is outside {lo}..{hi}")
    elif kt == "choice":
        options = knob.get("options")
        if not isinstance(options, list) or not 1 <= len(options) <= 256:
            bad.append(f"{where}: needs 1 to 256 options")
            options = []
        ids = []
        for o in options:
            if (not isinstance(o, dict) or set(o) != {"id", "text"} or not is_text(o["id"], 64) or
                    not is_text(o["text"])):
                bad.append(f"{where}: an option is {{id, text}}, both text")
                continue
            if o["id"] in ids:
                bad.append(f"{where}: option {o['id']!r} listed twice")
            ids.append(o["id"])
        if ids and default not in ids:
            bad.append(f"{where}: default {default!r} is not one of its options")
    elif not isinstance(default, bool):
        bad.append(f"{where}: a toggle's default is true or false")
    return bad


def model_problems(folder: str, mid: str, model) -> list:
    if not isinstance(model, dict):
        return [f"{folder}: lacks its model block"]
    bad = [f"{folder}: unknown field {k}" for k in sorted(set(model) - MODEL_KEYS)]
    if model.get("id") != mid:
        bad.append(f"{folder}: id {model.get('id')!r} is not {mid!r}")
    for key in ("name", "model"):
        if not is_text(model.get(key)):
            bad.append(f"{folder}: needs a {key} of 1 to 200 characters")
    kinds = model.get("kinds")
    if not kind_list(kinds):
        bad.append(f"{folder}: kinds must be one or more of {', '.join(KINDS)}, each once")
        kinds = []
    if not is_int(model.get("run_bytes")) or model["run_bytes"] <= 0:
        bad.append(f"{folder}: run_bytes must be a whole number of bytes above 0")
    if not is_int(model.get("cache_bytes")) or model["cache_bytes"] < 0:
        bad.append(f"{folder}: cache_bytes must be a whole number of bytes, 0 or more")
    acc = model.get("accelerators")
    if not isinstance(acc, list) or not all(one_of(a, ACCELERATORS) for a in acc) or len(set(acc)) != len(acc):
        bad.append(f"{folder}: accelerators must be some of {', '.join(sorted(ACCELERATORS))}, each once")
        acc = None
    if "cpu_unit_ms" in model and (not is_int(model["cpu_unit_ms"]) or model["cpu_unit_ms"] <= 0):
        bad.append(f"{folder}: cpu_unit_ms must be a whole number of milliseconds above 0")
    if acc == [] and "cpu_unit_ms" not in model:
        bad.append(f"{folder}: no accelerator and no cpu_unit_ms: it runs nowhere")
    if not one_of(model.get("licence"), ALLOWED_LICENCES):
        bad.append(f"{folder} is {model.get('licence')}: not an allowed licence")
    knobs = model.get("knobs", [])
    if not isinstance(knobs, list):
        bad.append(f"{folder}: knobs must be a list")
        knobs = []
    seen = set()
    for knob in knobs:
        bad += knob_problems(folder, knob, kinds, seen)
    return bad


def file_problems(folder: str, files) -> list:
    if not isinstance(files, list) or not files:
        return [f"{folder}: lists no files"]
    bad = []
    names = set()
    for entry in files:
        if not isinstance(entry, dict):
            bad.append(f"{folder}: a file is not an object")
            continue
        where = f"{folder}/{entry.get('as', '?')}"
        missing = [k for k in FILE_KEYS if k not in entry]
        bad += [f"{where} lacks {k}" for k in missing]
        bad += [f"{where}: unknown field {k}" for k in sorted(set(entry) - set(FILE_KEYS))]
        if missing:
            continue
        if not one_of(entry["licence"], ALLOWED_LICENCES):
            bad.append(f"{where} is {entry['licence']}: not an allowed licence")
        if not safe_relative(entry["as"]) or entry["as"].lower() == "model.json":
            bad.append(f"{where}: 'as' is not a safe relative path beside model.json")
        elif entry["as"].lower() in names:
            bad.append(f"{where}: listed twice")
        names.add(str(entry["as"]).lower())
        size = entry["size"]
        if not is_int(size) or size <= 0:
            bad.append(f"{where}: size must be a whole number of bytes above 0")
        elif size > FILE_CAP:
            bad.append(f"{where}: {size} bytes is over the 5 GB per-file cap")
        if not (isinstance(entry["revision"], str) and re.fullmatch(r"[0-9a-f]{40}", entry["revision"]) and
                isinstance(entry["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", entry["sha256"])):
            bad.append(f"{where} is not pinned (a 40-hex revision and a 64-hex SHA-256)")
        elif not (isinstance(entry["repo"], str) and
                  re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*", entry["repo"]) and
                  safe_relative(entry["file"]) and plain_hf_url(ai_models.url_of(entry))):
            bad.append(f"{where} is not a plain Hugging Face URL")
    return bad


def problems(data, twice: list) -> list:
    """Why the catalogue is unusable, or [] (check prints these; stage refuses on them)."""
    bad = [f"{k}: duplicate model id" if k.startswith("models/") else f"{k!r} is listed twice" for k in twice]
    if not (isinstance(data, dict) and isinstance(data.get("kinds"), dict) and isinstance(data.get("models"), dict)):
        return bad + ["the catalogue needs a kinds and a models object"]
    bad += [f"unknown field {k}" for k in sorted(set(data) - {"comment", "kinds", "models"})]
    served = {}  # model id -> the kinds it says it serves
    for folder, entry in data["models"].items():
        m = re.fullmatch(r"models/([a-z0-9-]{1,32})", folder)
        if not m:
            bad.append(f"{folder}: a model folder is models/<id>, the id [a-z0-9-]{{1,32}}")
            continue
        if not isinstance(entry, dict):
            bad.append(f"{folder}: not an object")
            continue
        bad += [f"{folder}: unknown field {k}" for k in sorted(set(entry) - {"comment", "model", "files"})]
        bad += model_problems(folder, m[1], entry.get("model"))
        bad += file_problems(folder, entry.get("files"))
        kinds = entry["model"].get("kinds") if isinstance(entry.get("model"), dict) else None
        served[m[1]] = [k for k in kinds if k in KINDS] if isinstance(kinds, list) else []
    kinds = data["kinds"]
    bad += [f"kinds.{k}: unknown kind (still, video or audio)" for k in kinds if k not in KINDS]
    for kind in KINDS:
        spec = kinds.get(kind)
        if not isinstance(spec, dict):
            bad.append(f"kinds.{kind}: missing")
            continue
        bad += [f"kinds.{kind}: unknown field {k}" for k in sorted(set(spec) - {"comment", "name", "unit",
                                                                               "cpu_budget_ms", "models"})]
        for key in ("name", "unit"):
            if not is_text(spec.get(key)):
                bad.append(f"kinds.{kind}: needs a {key}")
        if not is_int(spec.get("cpu_budget_ms")) or spec["cpu_budget_ms"] <= 0:
            bad.append(f"kinds.{kind}: cpu_budget_ms must be a whole number of milliseconds above 0")
        listed = spec.get("models")
        if not isinstance(listed, list) or not all(isinstance(i, str) for i in listed):
            bad.append(f"kinds.{kind}: models must be a list of model ids")
            continue
        if len(set(listed)) != len(listed):
            bad.append(f"kinds.{kind}: names a model twice")
        for mid in listed:
            if mid not in served:
                bad.append(f"kinds.{kind}: {mid!r} is not a model")
            elif kind not in served[mid]:
                bad.append(f"kinds.{kind}: {mid} does not serve {kind}")
    for mid, kinds_of in served.items():
        for kind in kinds_of:
            spec = kinds.get(kind)
            if isinstance(spec, dict) and isinstance(spec.get("models"), list) and mid not in spec["models"]:
                bad.append(f"models/{mid}: serves {kind}, but kinds.{kind} does not list it")
    return bad


def read(args):
    """The catalogue --models names, and its problems."""
    path = Path(args.models) if args.models else MODELS
    try:
        data, twice = load(path)
    except (OSError, ValueError) as e:
        return None, [f"cannot read the catalogue: {e}"]
    return data, problems(data, twice)


def check(args) -> int:
    data, bad = read(args)
    for b in bad:
        print(b, file=sys.stderr)
    if not bad:
        print(f"ok: {len(data['models'])} models")
    return 1 if bad else 0


def prune(out: Path, keep: set) -> None:
    """Removes everything under <out>/models this stage did not write."""
    for p in sorted((out / "models").rglob("*"), reverse=True):
        rel = p.relative_to(out).as_posix()
        if p.is_symlink() or p.is_file():
            if rel not in keep:
                p.unlink()
        elif p.is_dir() and not any(p.iterdir()):
            p.rmdir()


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=1) + "\n", encoding="utf-8")


def stage(args) -> int:
    data, bad = read(args)
    if bad:
        for b in bad:
            print(b, file=sys.stderr)
        return 1
    out = Path(args.out)
    cache = Path(args.cache) if args.cache else out.parent / ".model-cache"
    index = {"schema": 1, "kinds": {k: {"name": data["kinds"][k]["name"], "unit": data["kinds"][k]["unit"],
                                        "cpu_budget_ms": data["kinds"][k]["cpu_budget_ms"],
                                        "models": list(data["kinds"][k]["models"])} for k in KINDS}}
    write_json(out / "models" / "enhance.json", index)
    licences = {"models/enhance.json": GPL}
    remote = []
    for folder, entry in data["models"].items():
        for f in entry["files"]:
            rel = f"{folder}/{f['as']}"
            if args.upstream == "remote":
                remote.append({"url": ai_models.url_of(f), "path": rel, "size": f["size"], "sha256": f["sha256"],
                               "licence": f["licence"]})
                continue  # a staged copy from an earlier bundle is pruned below
            try:
                src = ai_models.fetch(f, cache)
            except ValueError:
                print(f"{rel}: size or SHA-256 differs from the catalogue", file=sys.stderr)
                return 1
            target = out / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            if (not target.is_file() or target.stat().st_size != f["size"] or
                    ai_models.sha256_file(target) != f["sha256"]):
                shutil.copyfile(src, target)
            licences[rel] = f["licence"]
        write_json(out / folder / "model.json", entry["model"])
        licences[f"{folder}/model.json"] = GPL
    prune(out, set(licences))
    remote_path = out / "remote.json"
    if remote:
        write_json(remote_path, remote)
    elif remote_path.exists():
        remote_path.unlink()
    (out / "licences.json").write_text(json.dumps(licences, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    fetched = f", {len(remote)} files fetched on install" if remote else ""
    print(f"staged {len(data['models'])} models into {out}{fetched}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("--models", help="the catalogue (default: enhance-models.json beside this script)")
    s = sub.add_parser("stage")
    s.add_argument("--out", required=True)
    s.add_argument("--models", help="the catalogue (default: enhance-models.json beside this script)")
    s.add_argument("--cache", help="--upstream bundle's download cache (default: <out>/../.model-cache)")
    s.add_argument("--upstream", choices=["bundle", "remote"], default="remote",
                   help="remote: list the weights for the add-on to fetch; bundle: fetch them into the pack now")
    args = ap.parse_args(argv)
    return stage(args) if args.cmd == "stage" else check(args)


if __name__ == "__main__":
    sys.exit(main())
