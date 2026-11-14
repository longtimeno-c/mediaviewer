#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""The phone's CLAP text tower (docs/plans/mobile.md §9, "Sound search"): the Mac's sound text
model made small enough for a phone, and the folder the phone downloads.

    python3 tools/ios/clap_text_phone.py --out <folder> [--fp32 <text_model.onnx>]

From Xenova/larger_clap_general's fp32 text model, at the revision tools/package/ai-models.json
pins for the Mac's fp16 one:
- every MatMul with a constant weight becomes MatMulNBits: 8-bit symmetric, blocks of 16 along K,
  accuracy_level 1 (the weights widened block by block and multiplied in fp32, so a query's
  vector never depends on another in its batch);
- the word-embedding table becomes int8 per row: Gather the int8 rows and their scales, then
  Cast and Mul, so only the looked-up rows are ever float. (A DequantizeLinear on the table would
  be folded back to fp32 when ONNX Runtime loads it: 154 MB.)
Every other weight stays fp32. The output names the same vectors as the Mac's tower, so model.json
keeps the Mac's precision ("fp16"): the engine's sound key (clap-general/fp16/pre1) names the
audio side's vectors in the index, which this file answers. `text_precision` says what it is.

The folder holds model.json (the Mac's, from ai-models.json, with the text file renamed),
text.onnx, vocab.json and merges.txt. Prints each file's size and SHA-256 for
src/mobile/sound_tower.h. The same input gives the same bytes.

Needs onnxruntime 1.30 (its quantiser), onnx and numpy; see docs/DEVELOPMENT.md.
"""
import argparse
import hashlib
import json
import shutil
import sys
import urllib.request
from pathlib import Path

import numpy as np
import onnx
import onnxruntime
from onnx import TensorProto, helper, numpy_helper
from onnxruntime.quantization.matmul_nbits_quantizer import DefaultWeightOnlyQuantConfig, MatMulNBitsQuantizer

ROOT = Path(__file__).resolve().parents[2]
REPO = "Xenova/larger_clap_general"
FP32 = {"file": "onnx/text_model.onnx", "size": 501513769,
        "sha256": "386413c758300d8f2be107b9fefbc049945b1b1b302938b01039af0ad4984595"}
EMBEDDING = "text_model.embeddings.word_embeddings.weight"
BITS, BLOCK, ACCURACY = 8, 16, 1
# The bytes depend on the quantiser, the serialiser and the rounding: the pin was made with these.
TOOLS = {"onnxruntime": "1.30.0", "onnx": "1.23.2", "numpy": "2.5.4"}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def pinned_audio() -> tuple[dict, dict, dict]:
    """ai-models.json's CLAP: the model block, and the tokenizer files with their pins."""
    doc = json.loads((ROOT / "tools/package/ai-models.json").read_text(encoding="utf-8"))
    folders = doc["pieces"]["ai-audio"]["folders"]
    clap = folders["models/clap-general"]
    tokenizer = {f["as"]: f for f in folders["models/" + clap["model"]["tokenizer"]]["files"]}
    text = next(f for f in clap["files"] if f["as"] == clap["model"]["text"])
    return clap["model"], tokenizer, text


def fetch(repo: str, revision: str, file: str, size: int, digest: str, to: Path) -> None:
    if to.exists() and to.stat().st_size == size and sha256(to) == digest:
        return
    url = f"https://huggingface.co/{repo}/resolve/{revision}/{file}"
    print(f"downloading {url}", file=sys.stderr)
    part = to.with_suffix(to.suffix + ".part")
    with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "MediaViewer"})) as r, part.open("wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    if part.stat().st_size != size or sha256(part) != digest:
        part.unlink()
        sys.exit(f"{file}: not the pinned file")
    part.rename(to)


def quantise_matmuls(model: onnx.ModelProto) -> onnx.ModelProto:
    config = DefaultWeightOnlyQuantConfig(block_size=BLOCK, is_symmetric=True, bits=BITS, op_types_to_quantize=("MatMul",),
                                          quant_axes=(("MatMul", 0),))
    q = MatMulNBitsQuantizer(model, bits=BITS, block_size=BLOCK, is_symmetric=True, algo_config=config,
                             op_types_to_quantize=("MatMul",), quant_axes=(("MatMul", 0),), accuracy_level=ACCURACY)
    q.process()
    out = q.model.model
    for n in out.graph.node:
        if n.op_type == "MatMulNBits":
            for a in list(n.attribute):
                if a.name == "accuracy_level":
                    n.attribute.remove(a)
            n.attribute.append(helper.make_attribute("accuracy_level", ACCURACY))
    return out


def int8_embedding(model: onnx.ModelProto) -> None:
    g = model.graph
    init = {t.name: t for t in g.initializer}
    w = numpy_helper.to_array(init[EMBEDDING]).astype(np.float32)
    scale = np.abs(w).max(axis=1) / 127.0
    scale[scale == 0] = 1.0
    q = np.clip(np.round(w / scale[:, None]), -127, 127).astype(np.int8)
    g.initializer.remove(init[EMBEDDING])
    g.initializer.extend([numpy_helper.from_array(q, EMBEDDING + "_q8"),
                          numpy_helper.from_array(scale.astype(np.float32).reshape(-1, 1), EMBEDDING + "_scale")])
    users = [n for n in g.node if n.op_type == "Gather" and n.input[0] == EMBEDDING]
    if len(users) != 1:
        sys.exit(f"expected one Gather of the word embeddings, found {len(users)}")
    n = users[0]
    ids, out = n.input[1], n.output[0]
    at = list(g.node).index(n)
    g.node.remove(n)
    for k, made in enumerate([
        helper.make_node("Gather", [EMBEDDING + "_q8", ids], [out + "_q"], axis=0),
        helper.make_node("Gather", [EMBEDDING + "_scale", ids], [out + "_s"], axis=0),
        helper.make_node("Cast", [out + "_q"], [out + "_f"], to=TensorProto.FLOAT),
        helper.make_node("Mul", [out + "_f", out + "_s"], [out]),
    ]):
        g.node.insert(at + k, made)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--fp32", type=Path, help="the pinned fp32 text model, if already here")
    a = ap.parse_args()
    have = {"onnxruntime": onnxruntime.__version__, "onnx": onnx.__version__, "numpy": np.__version__}
    if have != TOOLS:
        sys.exit(f"made with {TOOLS}, this has {have}: the bytes would differ from the pin")
    if a.fp32 and (a.fp32.stat().st_size != FP32["size"] or sha256(a.fp32) != FP32["sha256"]):
        sys.exit(f"{a.fp32}: not the pinned fp32 export")
    spec, tokenizer, mac_text = pinned_audio()
    a.out.mkdir(parents=True, exist_ok=True)
    work = a.out / ".work"
    work.mkdir(exist_ok=True)
    fp32 = a.fp32 or work / "text_model.onnx"
    fetch(REPO, mac_text["revision"], FP32["file"], FP32["size"], FP32["sha256"], fp32)
    for name, f in tokenizer.items():
        fetch(f["repo"], f["revision"], f["file"], f["size"], f["sha256"], a.out / name)

    model = quantise_matmuls(onnx.load(str(fp32)))
    int8_embedding(model)
    onnx.checker.check_model(model)
    onnx.save(model, str(a.out / "text.onnx"))

    phone = dict(spec)
    phone["text"] = "text.onnx"
    phone["text_precision"] = f"int{BITS} (MatMulNBits block {BLOCK}, accuracy_level {ACCURACY}; int8 word embeddings)"
    phone["tokenizer"] = "clap-tokenizer"
    (a.out / "model.json").write_text(json.dumps(phone, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    shutil.rmtree(work)  # the fp32 download, 501 MB

    for name in ("model.json", "text.onnx", "vocab.json", "merges.txt"):
        p = a.out / name
        print(f'{{"{name}", "{sha256(p)}", {p.stat().st_size}}},')


if __name__ == "__main__":
    main()
