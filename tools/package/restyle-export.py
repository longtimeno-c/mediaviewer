#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export InstructPix2Pix to the ONNX files Enhance's Restyle runs.

    restyle-export.py --out DIR [--cache DIR]

docs/plans/edit-adjust.md slice 5b (owner 2026-11-03: CreativeML OpenRAIL-M
accepted). Downloads timbrooks/instruct-pix2pix at REVISION (its fp16
weights, ~2 GB) and writes what src/addons/enhance/models/restyle/model.json
names, with the names and shapes src/addons/enhance/diffusion.cpp feeds:

    text_encoder.onnx  input_ids int64 [b, 77]          -> hidden [b, 77, 768]      fp32
    unet.onnx (+.data) sample [b, 8, h, w], timestep f32 [b], hidden [b, 77, 768]
                                                        -> eps [b, 4, h, w]         fp16 weights
    vae_encoder.onnx   image [1, 3, H, W] in -1..1      -> latent [1, 4, H/8, W/8] (the mean) fp32
    vae_decoder.onnx   latent [1, 4, h, w]              -> image [1, 3, 8h, 8w]     fp32
    vocab.json, merges.txt                              CLIP's BPE tokenizer

Then prints each file's size and SHA-256: the `remote` pins (path
"restyle/<file>") addon-pack.py signs, once the files are hosted at a fixed
revision. torch.onnx's bytes vary with the torch version, so pin what you
upload, not what someone else exported. Needs requirements-export.txt (with
diffusers and accelerate); the app never does.
"""
import argparse
import hashlib
import os
import shutil
import sys

REPO = "timbrooks/instruct-pix2pix"
REVISION = "31519b5cb02a7fd89b906d88731cd4d6a7bbf88d"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cache", default=None)
    a = ap.parse_args()
    import onnx
    import torch
    from diffusers import StableDiffusionInstructPix2PixPipeline

    os.makedirs(a.out, exist_ok=True)
    torch.set_grad_enabled(False)
    pipe = StableDiffusionInstructPix2PixPipeline.from_pretrained(
        REPO, revision=REVISION, variant="fp16", torch_dtype=torch.float32, cache_dir=a.cache, safety_checker=None)

    class Text(torch.nn.Module):
        def __init__(s, te):
            super().__init__()
            s.te = te

        def forward(s, ids):
            return s.te(ids)[0]

    class Enc(torch.nn.Module):
        def __init__(s, v):
            super().__init__()
            s.v = v

        def forward(s, image):
            return s.v.encode(image).latent_dist.mode()

    class Dec(torch.nn.Module):
        def __init__(s, v):
            super().__init__()
            s.v = v

        def forward(s, latent):
            return s.v.decode(latent, return_dict=False)[0]

    class Unet(torch.nn.Module):
        def __init__(s, u):
            super().__init__()
            s.u = u

        def forward(s, sample, timestep, hidden):
            return s.u(sample, timestep.to(torch.float16), hidden, return_dict=False)[0]

    def export(m, args, name, inputs, outputs, axes):
        torch.onnx.export(m, args, os.path.join(a.out, name), input_names=inputs, output_names=outputs,
                          dynamic_axes=axes, opset_version=17, dynamo=False)

    export(Text(pipe.text_encoder), (torch.zeros(1, 77, dtype=torch.int64),), "text_encoder.onnx", ["input_ids"],
           ["hidden"], {"input_ids": {0: "b"}, "hidden": {0: "b"}})
    export(Enc(pipe.vae), (torch.zeros(1, 3, 256, 256),), "vae_encoder.onnx", ["image"], ["latent"],
           {"image": {2: "H", 3: "W"}, "latent": {2: "h", 3: "w"}})
    export(Dec(pipe.vae), (torch.zeros(1, 4, 32, 32),), "vae_decoder.onnx", ["latent"], ["image"],
           {"latent": {2: "h", 3: "w"}, "image": {2: "H", 3: "W"}})

    # The UNet in fp16, in its own folder first (the exporter spills its
    # weights beside the graph), then one external data file.
    tmp = os.path.join(a.out, "_unet")
    os.makedirs(tmp, exist_ok=True)
    unet = pipe.unet.to(torch.float16)
    export_path = os.path.join(tmp, "unet.onnx")
    torch.onnx.export(Unet(unet), (torch.zeros(3, 8, 32, 32, dtype=torch.float16),
                                   torch.tensor([999.0, 999.0, 999.0]), torch.zeros(3, 77, 768, dtype=torch.float16)),
                      export_path, input_names=["sample", "timestep", "hidden"], output_names=["eps"],
                      dynamic_axes={"sample": {0: "b", 2: "h", 3: "w"}, "timestep": {0: "b"}, "hidden": {0: "b"},
                                    "eps": {0: "b", 2: "h", 3: "w"}}, opset_version=17, dynamo=False)
    model = onnx.load(export_path)
    # onnx appends external data to a file already there: start clean.
    for f in ("unet.onnx", "unet.onnx.data"):
        if os.path.exists(os.path.join(a.out, f)):
            os.remove(os.path.join(a.out, f))
    onnx.save_model(model, os.path.join(a.out, "unet.onnx"), save_as_external_data=True,
                    all_tensors_to_one_file=True, location="unet.onnx.data", size_threshold=1024)
    shutil.rmtree(tmp)

    # The tokenizer's files as upstream has them at REVISION (transformers 5
    # saves only tokenizer.json).
    from huggingface_hub import hf_hub_download
    for f in ("vocab.json", "merges.txt"):
        shutil.copy(hf_hub_download(REPO, "tokenizer/" + f, revision=REVISION, cache_dir=a.cache), os.path.join(a.out, f))

    for f in ("text_encoder.onnx", "unet.onnx", "unet.onnx.data", "vae_encoder.onnx", "vae_decoder.onnx",
              "vocab.json", "merges.txt"):
        p = os.path.join(a.out, f)
        print(f"restyle/{f}\t{os.path.getsize(p)}\t{sha256(p)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
