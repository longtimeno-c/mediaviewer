#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference outputs for the Documents piece's text model (#354 D3, docs/design/17
"Documents"): the Hugging Face tokenizer's ids and ORT-Python's vectors on CPU,
pooled and normalised as the model card says, for the passages and queries
tests/test_ai_docs.cpp embeds with the pack's own tokenizer and runtime. The
verify line wants cosine >= 0.999 on both platforms.

    python -I make_docs_reference.py --model <staged ai-docs>/models/doc-text \
        --tokenizer-json <the model repo's tokenizer.json> --out tests/data/ai/docs_reference.json
"""
import argparse
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer

PASSAGES = [
    "A receipt for two coffees and a croissant, paid by card at 08:14.",
    "The tenant shall pay the rent on the first day of each month, in advance.",
    "Graph neural networks pass messages along the edges of a graph to learn node features.",
    "If you work from home, you may be able to deduct part of your rent as a business expense.",
    "Le Vésuve est un volcan situé au bord de la baie de Naples, en Italie.",
    "Der Ätna ist der höchste aktive Vulkan Europas.",
    "Ünïcödé — \"quotes\", dashes - and Ångström units; naïve café résumé.",
    "Form 1040-X (Rev. February 2024): Amended U.S. Individual Income Tax Return. OMB No. 1545-0074.",
    "東京は日本の首都です。",
    "e-mail me@example.com, or call +44 (0)20 7946 0018 before 5pm!!",
    "x" * 300,
    " ".join(["word"] * 400),
]
QUERIES = [
    "tax return from my accountant",
    "wedding seating plan",
    "how do graph networks learn",
    "volcanoes in Italy",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="the staged models/doc-text folder")
    ap.add_argument("--tokenizer-json", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    spec = json.loads((Path(a.model) / "model.json").read_text())
    tok = Tokenizer.from_file(a.tokenizer_json)
    tok.no_padding()  # some tokenizer.json files pad to a fixed length
    tok.enable_truncation(spec.get("max_tokens", 256))
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    sess = ort.InferenceSession(str(Path(a.model) / spec["model"]), so, providers=["CPUExecutionProvider"])
    inputs = {i.name for i in sess.get_inputs()}

    def embed(text):
        ids = np.array([tok.encode(text).ids], np.int64)
        feed = {"input_ids": ids, "attention_mask": np.ones_like(ids)}
        if "token_type_ids" in inputs:
            feed["token_type_ids"] = np.zeros_like(ids)
        h = sess.run(None, feed)[0][0].astype(np.float64)
        v = h.mean(0) if spec.get("pooling", "mean") == "mean" else h[0]
        return (v / np.linalg.norm(v)).round(6).tolist(), ids[0].tolist()

    out = {"model": spec["id"], "precision": spec["precision"], "passages": [], "queries": []}
    for p in PASSAGES:
        v, ids = embed(spec.get("passage_prefix", "") + p)
        out["passages"].append({"text": p, "ids": ids, "vec": v})
    for q in QUERIES:
        v, ids = embed(spec.get("query_prefix", "") + q)
        out["queries"].append({"text": q, "ids": ids, "vec": v})
    Path(a.out).write_text(json.dumps(out, ensure_ascii=False))


if __name__ == "__main__":
    main()
