#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export a pinned face-embedder checkpoint to the ONNX the People piece runs.

    face-export.py --arch ir50 --weights model.safetensors --out embedder.onnx [--golden]
                   [--expect "v0 v1 ... v15 norm"]

The AdaFace checkpoints (CVLface, MIT; docs/design/17 "People model, 2026-10-03") ship as
safetensors with the authors' PyTorch code, not as ONNX. Loading them through that
code means running the repository's Python (`trust_remote_code`); this file instead
re-declares the IResNet ("IR") backbone the checkpoint was trained with and loads
the tensors as data. Every tensor must be consumed and every parameter filled, or
the export fails.

The graph: input "input" 1 x 3 x 112 x 112 RGB, (x / 255 - 0.5) / 0.5, an ArcFace
5-point aligned crop; output "embedding" 1 x 512, NOT normalised (its L2 norm is
AdaFace's quality signal; the add-on normalises). Batch is dynamic.

`--golden` prints the first 16 values of the normalised embedding of a fixed
synthetic input and its raw norm: the check ai-models.json records (`export.golden`).
`--expect` (what ai-models.py stage passes) fails the export unless the ONNX
graph reproduces them, so a re-export on another machine is verified by output,
not by bytes (torch.onnx's bytes vary with the torch version). Needs torch, safetensors, onnx
(requirements-export.txt); the app never does.
"""
import argparse
import sys

import numpy as np
import torch
from torch import nn

UNITS = {"ir18": (2, 2, 2, 2), "ir50": (3, 4, 14, 3), "ir101": (3, 13, 30, 3)}


class BasicBlockIR(nn.Module):
    def __init__(self, cin, depth, stride):
        super().__init__()
        if cin == depth:
            self.shortcut_layer = nn.MaxPool2d(1, stride)
        else:
            self.shortcut_layer = nn.Sequential(nn.Conv2d(cin, depth, 1, stride, bias=False),
                                                nn.BatchNorm2d(depth))
        self.res_layer = nn.Sequential(
            nn.BatchNorm2d(cin), nn.Conv2d(cin, depth, 3, 1, 1, bias=False), nn.BatchNorm2d(depth),
            nn.PReLU(depth), nn.Conv2d(depth, depth, 3, stride, 1, bias=False), nn.BatchNorm2d(depth))

    def forward(self, x):
        return self.res_layer(x) + self.shortcut_layer(x)


class Backbone(nn.Module):
    def __init__(self, arch):
        super().__init__()
        self.input_layer = nn.Sequential(nn.Conv2d(3, 64, 3, 1, 1, bias=False), nn.BatchNorm2d(64),
                                         nn.PReLU(64))
        blocks = []
        cin = 64
        for depth, n in zip((64, 128, 256, 512), UNITS[arch]):
            blocks.append(BasicBlockIR(cin, depth, 2))
            blocks += [BasicBlockIR(depth, depth, 1) for _ in range(n - 1)]
            cin = depth
        self.body = nn.Sequential(*blocks)
        self.output_layer = nn.Sequential(nn.BatchNorm2d(512), nn.Dropout(0.4), nn.Flatten(),
                                          nn.Linear(512 * 7 * 7, 512), nn.BatchNorm1d(512, affine=False))

    def forward(self, x):
        return self.output_layer(self.body(self.input_layer(x)))


class Half(nn.Module):
    """fp16 weights and compute, fp32 in and out (the pack's other models are fp16 too)."""

    def __init__(self, net):
        super().__init__()
        self.net = net.half()

    def forward(self, x):
        return self.net(x.half()).float()


def load(arch, weights):
    from safetensors.numpy import load_file
    tensors = load_file(weights)
    prefix = "model.net."
    state = {k[len(prefix):]: torch.from_numpy(v.copy()) for k, v in tensors.items() if k.startswith(prefix)}
    if len(state) != len(tensors):
        raise SystemExit(f"{weights}: {len(tensors) - len(state)} tensors outside {prefix}")
    net = Backbone(arch)
    missing, unexpected = net.load_state_dict(state, strict=False)
    if missing or unexpected:
        raise SystemExit(f"{weights} is not an AdaFace {arch}: missing {missing[:3]}, unexpected {unexpected[:3]}")
    return net.eval()


def golden_input():
    # A smooth, fixed 112 x 112 RGB pattern, already normalised to [-1, 1].
    y, x = np.mgrid[0:112, 0:112].astype(np.float32) / 111.0
    img = np.stack([np.sin(6 * x + 1), np.cos(5 * y), np.sin(4 * (x + y))])
    return torch.from_numpy(img[None].astype(np.float32))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", choices=sorted(UNITS), required=True)
    ap.add_argument("--weights", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--golden", action="store_true")
    ap.add_argument("--fp16", action="store_true", help="fp16 weights (half the size)")
    ap.add_argument("--expect", help="16 normalised values and the norm, space-separated")
    a = ap.parse_args(argv)
    torch.manual_seed(0)
    net = load(a.arch, a.weights)
    x = golden_input()
    with torch.no_grad():
        ref = net(x)[0].numpy()
    graph = Half(load(a.arch, a.weights)).eval() if a.fp16 else net
    torch.onnx.export(graph, (x,), a.out, input_names=["input"], output_names=["embedding"],
                      dynamic_axes={"input": {0: "n"}, "embedding": {0: "n"}}, opset_version=17,
                      do_constant_folding=True, dynamo=False)
    import onnxruntime as ort
    s = ort.InferenceSession(a.out, providers=["CPUExecutionProvider"])
    got = s.run(None, {"input": x.numpy()})[0][0]
    cos = float(got @ ref / (np.linalg.norm(got) * np.linalg.norm(ref)))
    if cos < (0.999 if a.fp16 else 0.9999):
        raise SystemExit(f"ONNX disagrees with PyTorch (cosine {cos:.6f})")
    v = got / np.linalg.norm(got)
    if a.golden:
        print("golden", " ".join(f"{t:.5f}" for t in v[:16]), f"{np.linalg.norm(got):.4f}")
    if a.expect:
        want = [float(t) for t in a.expect.split()]
        if len(want) != 17:
            raise SystemExit("--expect takes 16 values and the norm")
        if np.max(np.abs(v[:16] - np.array(want[:16]))) > 2e-3 or abs(np.linalg.norm(got) / want[16] - 1) > 1e-3:
            import os
            os.remove(a.out)
            raise SystemExit("the export does not reproduce the golden embedding: wrong weights or a broken exporter")
    print(f"exported {a.arch} -> {a.out} (ONNX vs PyTorch cosine {cos:.6f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
