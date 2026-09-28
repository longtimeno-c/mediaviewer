#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference outputs for the PR 20 verify (plan/17): "a fixed set of test
images embeds to vectors within tolerance of the reference (PyTorch/ORT-Python)
outputs".

    python make_reference.py --pack D:/mv-pack/ai --out tests/data/ai/reference.json

Needs: onnxruntime, numpy, pillow, tokenizers (pip). The test images are
generated from integer formulas that tests/test_ai_infer.cpp repeats exactly,
so no image file is committed. Preprocessing is Hugging Face's
CLIPImageProcessor as the Xenova exports were made with it: PIL bicubic
resize of the short side to 224, centre crop, /255, mean/std. The tokenizer
golden comes from the checkpoint's own tokenizer.json.
"""
import argparse
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image
from tokenizers import Tokenizer

MEAN = np.array([0.48145466, 0.4578275, 0.40821073], np.float32)
STD = np.array([0.26862954, 0.26130258, 0.27577711], np.float32)


def card(kind: int) -> np.ndarray:
    if kind == 0:
        w, h = 320, 240
        y, x = np.mgrid[0:h, 0:w]
        return np.stack([(x * 7 + y * 3) & 255, (x * y) & 255, (x ^ y) & 255], -1).astype(np.uint8)
    if kind == 1:
        w, h = 200, 300
        y, x = np.mgrid[0:h, 0:w]
        inside = (x - 100) ** 2 + (y - 150) ** 2 < 60 ** 2
        return np.stack([(x * 255) // (w - 1), (y * 255) // (h - 1), np.where(inside, 255, 30)], -1).astype(np.uint8)
    w, h = 640, 480
    y, x = np.mgrid[0:h, 0:w]
    grey = (((x // 40) + (y // 40)) % 2) * 200 + 20
    img = np.stack([grey, grey, grey], -1)
    stripe = (y >= 200) & (y < 260)
    img[stripe] = [220, 30, 30]
    return img.astype(np.uint8)


def prep(rgb: np.ndarray, size: int = 224) -> np.ndarray:
    im = Image.fromarray(rgb, "RGB")
    w, h = im.size
    if w <= h:
        nw, nh = size, max(size, round(h * size / w))
    else:
        nw, nh = max(size, round(w * size / h)), size
    im = im.resize((nw, nh), Image.BICUBIC)
    left, top = (nw - size) // 2, (nh - size) // 2
    im = im.crop((left, top, left + size, top + size))
    a = (np.asarray(im, np.float32) / 255.0 - MEAN) / STD
    return a.transpose(2, 0, 1)


QUERIES = ["guy on a skateboard", "a birthday cake with candles", "a red stripe on a checkerboard"]
TOKEN_CASES = [
    "guy on a skateboard", "Guy On A Skateboard!!", "a dog's frisbee", "they're here, we'll go",
    "  lots   of   spaces  ", "numbers 12345 and 3.14", "café naïve résumé", "emoji 🙂 test",
    "hyphen-ated words", "under_score and CamelCase", "a", "", "what?! no... yes!!!",
    "birthday cake", "photos of Anna on the beach", "the quick brown fox jumps over the lazy dog",
    "ÀÉÎÕÜ upper accents", "tab\tand\nnewline", "x" * 300,
]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True, help="a staged Core pack (tools/package/ai-models.py)")
    ap.add_argument("--tokenizer-json", required=True, help="the checkpoint's tokenizer.json")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    pack = Path(args.pack)
    tok = Tokenizer.from_file(args.tokenizer_json)
    out = {"cards": [0, 1, 2], "queries": QUERIES, "models": {}, "tokens": {}}
    for s in TOKEN_CASES:
        out["tokens"][s] = tok.encode(s).ids[:76] + ([49407] if len(tok.encode(s).ids) > 76 else [])
    for folder in ("clip-b32", "clip-l14"):
        d = pack / "models" / folder
        spec = json.loads((d / "model.json").read_text())
        so = ort.SessionOptions()
        so.intra_op_num_threads = 1
        v = ort.InferenceSession(str(d / "image.onnx"), so, providers=["CPUExecutionProvider"])
        t = ort.InferenceSession(str(d / "text.onnx"), so, providers=["CPUExecutionProvider"])
        images = []
        for k in out["cards"]:
            x = prep(card(k))[None]
            e = v.run(None, {v.get_inputs()[0].name: x})[0][0].astype(np.float64)
            images.append([round(float(z), 7) for z in e / np.linalg.norm(e)])
        texts = []
        for q in QUERIES:
            ids = np.array([tok.encode(q).ids], np.int64)
            e = t.run(None, {"input_ids": ids})[0][0].astype(np.float64)
            texts.append([round(float(z), 7) for z in e / np.linalg.norm(e)])
        out["models"][spec["id"]] = {"dim": spec["dim"], "images": images, "texts": texts}
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(out, ensure_ascii=False) + "\n", encoding="utf-8")
    print("wrote", args.out)


if __name__ == "__main__":
    main()
