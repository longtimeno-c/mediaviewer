#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export one tower of a pinned CLIP checkpoint to the ONNX the AI pack runs.

    clip-export.py --weights model.safetensors --config config.json --tower image|text
                   --out image.onnx [--fp16] [--golden] [--expect "v0 ... v15 norm"]

For towers with no upstream ONNX we trust (#482, docs/plans/model-tiers.md):
TinyCLIP (Microsoft, MIT; the Potato tier) is published only as one combined
graph, and OpenCLIP ViT-H/14 (LAION, MIT; Maxxed) only as PyTorch weights. The
checkpoint is read as data through transformers' CLIP classes (no remote code),
and each tower is exported on its own, as src/infer/models.h's contract wants:

  image  input "pixel_values" batch_size x num_channels x height x width (the
         model's mean/std applied by the pack), output "image_embeds" n x dim
  text   input "input_ids" batch x sequence (CLIP BPE ids, sot ... eot; the
         pooled token is the first eot), output "text_embeds" n x dim

Outputs are the projected embeddings, not normalised (the pack normalises).
`--fp16` stores fp16 weights with fp32 in and out, as the pack's other towers.
The image tower is traced in half precision; the text tower is exported in
fp32 and converted (onnxconverter-common), since tracing it in half leaves
LayerNorm with mixed types (and converting the image tower breaks its patch
Conv). Either way --expect checks the result.

`--golden` prints the first 16 values of the normalised embedding of a fixed
input (a synthetic image; the ids of "a photo of a dog") and its raw norm: the
check ai-models.json records (`export.golden`). `--expect` fails the export
unless the ONNX graph reproduces them within 2e-3, so a re-export elsewhere is
verified by output, not bytes. Needs torch, transformers, safetensors, onnx,
onnxruntime (requirements-export.txt); the app never does.
"""
import argparse
import json
import sys

import numpy as np
import torch

DOG_IDS = [49406, 320, 1125, 539, 320, 1929, 49407]  # "a photo of a dog", CLIP BPE


def fixed_image(size: int) -> np.ndarray:
    y, x = np.mgrid[0:size, 0:size].astype(np.float32) / size
    img = np.stack([np.sin(6 * x + 1), np.cos(5 * y), np.sin(4 * (x + y))]) * 1.5
    return img[None].astype(np.float32)


class Tower(torch.nn.Module):
    def __init__(self, model, tower: str, half: bool):
        super().__init__()
        self.model = model.half() if half else model
        self.tower = tower
        self.half_ = half

    def forward(self, x):
        if self.tower == "image":
            return self.model(pixel_values=x.half() if self.half_ else x).image_embeds.float()
        return self.model(input_ids=x).text_embeds


def load(weights: str, config_path: str, tower: str):
    from safetensors.torch import load_file
    from transformers import CLIPConfig, CLIPTextModelWithProjection, CLIPVisionModelWithProjection
    with open(config_path, encoding="utf-8") as f:
        cfg = CLIPConfig.from_dict(json.load(f))
    if tower == "image":
        model = CLIPVisionModelWithProjection(cfg.vision_config)
        prefixes = ("vision_model.", "visual_projection.")
    else:
        model = CLIPTextModelWithProjection(cfg.text_config)
        prefixes = ("text_model.", "text_projection.")
    # position_ids is a buffer some checkpoints carry and some do not.
    state = {k: v.float() for k, v in load_file(weights).items()
             if k.startswith(prefixes) and not k.endswith("position_ids")}
    missing, unexpected = model.load_state_dict(state, strict=False)
    missing = [k for k in missing if not k.endswith("position_ids")]
    if missing or unexpected:
        raise SystemExit(f"checkpoint does not fit the {tower} tower: missing {missing[:4]}, unexpected {unexpected[:4]}")
    model.eval()
    return model, cfg


def golden_of(vec: np.ndarray) -> list:
    v = vec.reshape(-1).astype(np.float64)
    n = float(np.linalg.norm(v))
    return [round(float(x), 5) for x in (v / n)[:16]] + [round(n, 4)]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", required=True)
    ap.add_argument("--config", required=True, help="the checkpoint's config.json")
    ap.add_argument("--tower", choices=["image", "text"], required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fp16", action="store_true")
    ap.add_argument("--golden", action="store_true")
    ap.add_argument("--expect")
    args = ap.parse_args(argv)

    model, cfg = load(args.weights, args.config, args.tower)
    net = Tower(model, args.tower, args.fp16 and args.tower == "image")
    size = cfg.vision_config.image_size
    if args.tower == "image":
        sample = torch.from_numpy(fixed_image(size))
        names, axes = ["pixel_values"], {"pixel_values": {0: "batch_size"}, "image_embeds": {0: "batch_size"}}
        out_name = "image_embeds"
    else:
        sample = torch.tensor([DOG_IDS], dtype=torch.int64)
        names, axes = ["input_ids"], {"input_ids": {0: "batch_size", 1: "sequence"}, "text_embeds": {0: "batch_size"}}
        out_name = "text_embeds"
    with torch.no_grad():
        torch.onnx.export(net, (sample,), args.out, input_names=names, output_names=[out_name],
                          dynamic_axes=axes, opset_version=17, dynamo=False)
    if args.fp16 and args.tower == "text":
        import onnx
        from onnxconverter_common import float16
        graph = onnx.load(args.out, load_external_data=True)
        graph = float16.convert_float_to_float16(graph, keep_io_types=True)
        onnx.save(graph, args.out, save_as_external_data=graph.ByteSize() > 2_000_000_000)

    import onnxruntime as ort
    sess = ort.InferenceSession(args.out, providers=["CPUExecutionProvider"])
    got = sess.run(None, {names[0]: sample.numpy()})[0]
    golden = golden_of(got[0])
    if args.golden:
        print(" ".join(str(x) for x in golden))
    if args.expect:
        want = [float(x) for x in args.expect.split()]
        if len(want) != 17 or any(abs(a - b) > 2e-3 * max(1.0, abs(b)) for a, b in zip(golden, want)):
            print(f"{args.out}: output {golden} differs from the recorded golden {want}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
