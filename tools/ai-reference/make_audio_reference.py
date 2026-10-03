#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference outputs for the audio index (docs/design/17 "Audio"): log-mel features
as transformers' ClapFeatureExtractor / WhisperFeatureExtractor compute them
(their torch-free numpy path, transformers.audio_utils), CLAP embeddings from
ORT-Python on CPU, and the CLAP tokenizer's ids.

    python make_audio_reference.py --pack D:/mv-pack/ai-audio \
        --clap-tokenizer-json <larger_clap_general/tokenizer.json> --out tests/data/ai/audio_reference.json

The signals come from integer formulas tests/test_ai_audio.cpp repeats.
"""
import argparse
import json
from pathlib import Path

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer
from transformers.audio_utils import mel_filter_bank, spectrogram, window_function


def signal(kind: int, rate: int) -> np.ndarray:
    n = rate * (3 if kind == 0 else 12)
    t = np.arange(n, dtype=np.float64) / rate
    if kind == 0:
        return (0.5 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)
    # Two tones with a slow envelope plus deterministic noise (an LCG).
    x = 0.3 * np.sin(2 * np.pi * 220.0 * t) + 0.2 * np.sin(2 * np.pi * 1760.0 * t) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.5 * t))
    state = 12345
    noise = np.empty(n)
    for i in range(n):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        noise[i] = state / 0x7FFFFFFF - 0.5
    return (x + 0.05 * noise).astype(np.float32)


CLAP_MEL = mel_filter_bank(num_frequency_bins=513, num_mel_filters=64, min_frequency=50, max_frequency=14000,
                           sampling_rate=48000, norm="slaney", mel_scale="slaney")
WHISPER_MEL = mel_filter_bank(num_frequency_bins=201, num_mel_filters=80, min_frequency=0.0, max_frequency=8000.0,
                              sampling_rate=16000, norm="slaney", mel_scale="slaney")


def clap_features(x):
    x = x.astype(np.float64)
    N = 480000
    if len(x) > N:
        x = x[:N]
    elif len(x) < N:
        x = np.tile(x, int(N / len(x)))
        x = np.pad(x, (0, N - len(x)))
    m = spectrogram(x, window_function(1024, "hann"), frame_length=1024, hop_length=480, power=2.0,
                    mel_filters=CLAP_MEL, log_mel="dB")
    return m.T.astype(np.float32)  # [1001, 64]


def whisper_features(x):
    x = np.pad(x.astype(np.float64)[:480000], (0, max(0, 480000 - len(x))))
    m = spectrogram(x, window_function(400, "hann"), frame_length=400, hop_length=160, power=2.0,
                    mel_filters=WHISPER_MEL, log_mel="log10")[:, :-1]
    m = np.maximum(m, m.max() - 8.0)
    return ((m + 4.0) / 4.0).astype(np.float32)  # [80, 3000]


def sample(a: np.ndarray, rows):
    return {str(r): [round(float(v), 5) for v in a[r]] for r in rows}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pack", required=True)
    ap.add_argument("--clap-tokenizer-json", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    pack = Path(a.pack)
    out = {"clap": {}, "whisper": {}, "tokens": {}, "queries": ["a dog barking", "a pure tone", "people clapping"]}
    for k in (0, 1):
        f = clap_features(signal(k, 48000))
        out["clap"][str(k)] = {"rows": sample(f, [0, 1, 500, 1000]), "mean": float(f.mean())}
        w = whisper_features(signal(k, 16000))
        out["whisper"][str(k)] = {"cols": {str(c): [round(float(v), 5) for v in w[:, c]] for c in (0, 1, 150, 2999)},
                                  "mean": float(w.mean())}
    tok = Tokenizer.from_file(a.clap_tokenizer_json)
    for s in ["a dog barking", "People  clapping!", "rain on a tin roof, 3 minutes", "it's the 1990s", "café noise"]:
        out["tokens"][s] = tok.encode(s).ids
    d = pack / "models" / "clap-general"
    so = ort.SessionOptions()
    so.intra_op_num_threads = 1
    au = ort.InferenceSession(str(d / "audio.onnx"), so, providers=["CPUExecutionProvider"])
    tx = ort.InferenceSession(str(d / "text.onnx"), so, providers=["CPUExecutionProvider"])
    out["clap_audio"] = []
    for k in (0, 1):
        e = au.run(None, {"input_features": clap_features(signal(k, 48000))[None, None]})[0][0].astype(np.float64)
        out["clap_audio"].append([round(float(z), 7) for z in e / np.linalg.norm(e)])
    out["clap_text"] = []
    for q in out["queries"]:
        e = tx.run(None, {"input_ids": np.array([tok.encode(q).ids], np.int64)})[0][0].astype(np.float64)
        out["clap_text"].append([round(float(z), 7) for z in e / np.linalg.norm(e)])
    Path(a.out).write_text(json.dumps(out) + "\n", encoding="utf-8")
    print("wrote", a.out)


if __name__ == "__main__":
    main()
