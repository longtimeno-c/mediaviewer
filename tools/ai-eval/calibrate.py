#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Evaluate a picture tower and calibrate its thresholds (#482 T5).

    calibrate.py --eval DIR --model FOLDER --tokenizer tokenizer.json
                 [--reference FOLDER] [--write ai-models.json --piece ai --folder models/x]
                 [--report report.json] [--limit 1000]

DIR is a labelled folder: labels.json [{"file", "sentences": [...]}] and img/
(COCO-1k: the first 1,000 val2017 images with their five captions, as the PR 20
spike used; kept out of git like the RAW corpus). FOLDER is a staged model
folder (model.json, image.onnx, text.onnx), as ai-models.py stage leaves it.

It runs the tower the way the pack does (tools/ai-reference/make_reference.py's
preprocessing, which the C++ matches) and reports the PR 20 bar:

  COCO-1k R@10  a caption finds its own photo in the top ten (bar: 0.88)
  P@5           the labelled queries' top five are relevant (bar: 0.8)

A tower under the bar fails: it is swapped for the next candidate, never waved
through (docs/plans/model-tiers.md).

Calibration matches quantiles against the reference tower (default: B/32,
whose thresholds were set by hand in PR 20 and checked on real libraries):
a threshold that a share u of the reference's scores fall under becomes the
score a share u of the new tower's fall under. So "nothing found" and the
result margin stay as strict as they are on B/32, in the new tower's own
cosine units:

  query_margin   the best photo's lead over the generic prompts, per query
  result_margin  a relevant photo's lead over the generic prompts
  similar_min    photo-to-photo cosine (find similar)
  query_z, result_z, dedupe   scale-free or frame-to-frame: kept

`--write` stores the thresholds, "calibrated": true and the eval numbers in the
model's ai-models.json entry. Needs numpy, onnxruntime, pillow, tokenizers.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image
from tokenizers import Tokenizer

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "ai-reference"))
from make_reference import prep  # noqa: E402  (the pack's preprocessing)

R10_BAR = 0.88
P5_BAR = 0.8
# test_ai_infer.cpp's labelled queries: a caption with one of `any` (and, when
# given, one of `also`) makes a photo relevant.
QUERIES = [
    ("guy on a skateboard", ["skateboard"], ["man", "guy", "boy", "person", "skateboarder", "male"]),
    ("a plate of pizza", ["pizza"], []),
    ("giraffes", ["giraffe"], []),
    ("a train at the station", ["train"], []),
    ("a dog", ["dog"], []),
    ("a cat", ["cat"], []),
    ("dog", ["dog"], []),
]
NONSENSE = ["qwzx plorn", "the seventh of grommet", "asdkjh", "zzzz yyyy", "flibbertigibbet wombat engine",
            "blue thursday arithmetic", "xylophone quantum sandwich", "lorem ipsum dolor"]


class Tower:
    def __init__(self, folder: Path, tok: Tokenizer):
        self.spec = json.loads((folder / "model.json").read_text(encoding="utf-8"))
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = 0
        self.image = ort.InferenceSession(str(folder / self.spec["image"]), opts, providers=["CPUExecutionProvider"])
        self.text = ort.InferenceSession(str(folder / self.spec["text"]), opts, providers=["CPUExecutionProvider"])
        self.tok = tok
        self.size = int(self.spec["image_size"])

    @staticmethod
    def pick(outs, sess, name, n, dim):
        names = [o.name for o in sess.get_outputs()]
        for i, t in enumerate(outs):
            if name and names[i] != name:
                continue
            if t.ndim == 2 and t.shape == (n, dim):
                return t
        raise ValueError("no [n, dim] output")

    def embed_images(self, paths, batch=8):
        dim = int(self.spec["dim"])
        out = []
        for i in range(0, len(paths), batch):
            xs = []
            for p in paths[i:i + batch]:
                rgb = np.asarray(Image.open(p).convert("RGB"))
                # [3, s, s], CLIP's mean / std applied (every tower here uses them)
                xs.append(prep(rgb, self.size).astype(np.float32))
            x = np.stack(xs)
            outs = self.image.run(None, {self.image.get_inputs()[0].name: x})
            out.append(self.pick(outs, self.image, self.spec.get("image_output", ""), len(xs), dim))
        e = np.concatenate(out)
        return e / np.linalg.norm(e, axis=1, keepdims=True)

    def embed_texts(self, texts):
        dim = int(self.spec["dim"])
        ctx = int(self.spec.get("context", 77))
        inp = self.text.get_inputs()
        out = []
        for t in texts:
            ids = self.tok.encode(t.lower()).ids[:ctx]
            feed = {inp[0].name: np.array([ids], dtype=np.int64 if "int64" in inp[0].type else np.int32)}
            if len(inp) == 2:
                feed[inp[1].name] = np.ones_like(feed[inp[0].name])
            outs = self.text.run(None, feed)
            out.append(self.pick(outs, self.text, self.spec.get("text_output", ""), 1, dim)[0])
        e = np.stack(out)
        return e / np.linalg.norm(e, axis=1, keepdims=True)


def evaluate(tower: Tower, eval_dir: Path, limit: int) -> dict:
    labels = json.loads((eval_dir / "labels.json").read_text(encoding="utf-8"))[:limit]
    paths = [eval_dir / "img" / item["file"] for item in labels]
    imgs = tower.embed_images(paths)
    captions, owner = [], []
    for i, item in enumerate(labels):
        for s in item["sentences"][:5]:
            captions.append(s)
            owner.append(i)
    caps = tower.embed_texts(captions)
    generic = tower.embed_texts(tower.spec.get("generic_prompts", ["a photo."]))
    gen_best = (imgs @ generic.T).max(axis=1)  # each photo's generic bar
    sims = caps @ imgs.T
    ranks = (-sims).argsort(axis=1)
    owner = np.array(owner)
    hit = lambda k: float(np.mean([owner[q] in ranks[q, :k] for q in range(len(captions))]))
    # Margins: a caption's own photo over that photo's generic bar (result),
    # and each query's best photo over its bar (query).
    pos_margin = sims[np.arange(len(captions)), owner] - gen_best[owner]
    top = ranks[:, 0]
    query_margin = sims[np.arange(len(captions)), top] - gen_best[top]
    nons = tower.embed_texts(NONSENSE) @ imgs.T
    nons_top = nons.argmax(axis=1)
    nonsense_margin = nons[np.arange(len(NONSENSE)), nons_top] - gen_best[nons_top]
    pair = imgs @ imgs.T
    off = pair[~np.eye(len(imgs), dtype=bool)]
    # P@5 on the labelled queries.
    joined = [" ".join(item["sentences"]).lower() for item in labels]
    p5 = []
    for text, any_, also in QUERIES:
        q = tower.embed_texts([text])[0]
        best = (-(imgs @ q)).argsort()[:5]
        rel = [any(a in joined[i] for a in any_) and (not also or any(b in joined[i] for b in also)) for i in best]
        p5.append(float(np.mean(rel)))
    return {
        "photos": len(labels), "r1": hit(1), "r5": hit(5), "r10": hit(10), "p5": float(np.mean(p5)),
        "pos_margin": pos_margin, "query_margin": query_margin, "nonsense_margin": nonsense_margin, "pairs": off,
    }


def quantile_map(value: float, ref: np.ndarray, new: np.ndarray) -> float:
    u = float(np.mean(ref <= value))
    return float(np.quantile(new, min(max(u, 0.0), 1.0)))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--eval", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--tokenizer", required=True, help="CLIP's tokenizer.json (BPE, lower-cased)")
    ap.add_argument("--reference", help="the reference tower's staged folder (default: none, eval only)")
    ap.add_argument("--write", help="ai-models.json to update")
    ap.add_argument("--piece", default="ai")
    ap.add_argument("--folder")
    ap.add_argument("--report")
    ap.add_argument("--limit", type=int, default=1000)
    args = ap.parse_args(argv)
    tok = Tokenizer.from_file(args.tokenizer)
    new = Tower(Path(args.model), tok)
    got = evaluate(new, Path(args.eval), args.limit)
    report = {"model": new.spec.get("id"), "photos": got["photos"], "r1": round(got["r1"], 4),
              "r5": round(got["r5"], 4), "r10": round(got["r10"], 4), "p5": round(got["p5"], 4),
              "passes": got["r10"] >= R10_BAR and got["p5"] >= P5_BAR}
    if args.reference:
        ref_tower = Tower(Path(args.reference), tok)
        ref = evaluate(ref_tower, Path(args.eval), args.limit)
        rs = ref_tower.spec
        cal = {
            "query_margin": quantile_map(rs["query_margin"], ref["query_margin"], got["query_margin"]),
            "result_margin": quantile_map(rs["result_margin"], ref["pos_margin"], got["pos_margin"]),
            "similar_min": quantile_map(rs["similar_min"], ref["pairs"], got["pairs"]),
            "query_z": rs.get("query_z", 2.5), "result_z": rs.get("result_z", 2.0), "dedupe": rs.get("dedupe", 0.97),
        }
        report["reference"] = {"model": rs.get("id"), "r10": round(ref["r10"], 4), "p5": round(ref["p5"], 4)}
        report["calibrated"] = {k: round(v, 4) for k, v in cal.items()}
        # Nonsense should mostly fall under "nothing found".
        report["nonsense_found"] = int(np.sum(got["nonsense_margin"] >= cal["query_margin"]))
    print(json.dumps(report, indent=1))
    if args.report:
        Path(args.report).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    if args.write:
        if not (args.folder and "calibrated" in report):
            print("--write needs --folder and --reference", file=sys.stderr)
            return 2
        path = Path(args.write)
        data = json.loads(path.read_text(encoding="utf-8"))
        model = data["pieces"][args.piece]["folders"][args.folder]["model"]
        model.update(report["calibrated"])
        model["calibrated"] = True
        model["eval"] = {"set": f"coco-{report['photos']}", "r10": report["r10"], "p5": report["p5"]}
        path.write_text(json.dumps(data, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    return 0 if report["passes"] else 1


if __name__ == "__main__":
    sys.exit(main())
