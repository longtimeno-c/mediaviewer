#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measures Restore faces' People guard (docs/plans/edit-refresh.md S10, docs/design/29).

The guard keeps a face whose restored, blended self no longer embeds as the same person.
This replays the add-on's steps on LFW with the People model (AdaFace IR-50 WebFace4M,
the pack's embedder) and prints the numbers the threshold (face_restore.h kFaceGuard)
is chosen from:

  - impostor pairs (different people, LFW's clustering set, from the People eval's
    saved vectors): the cosine at a false-accept rate of 1e-4, 1e-5 and 1e-6;
  - genuine restorations: each face aligned to the FFHQ 5-point template at 512,
    restored by RestoreFormer++ and GFPGAN v1.4, blended under the feathered face mask
    at Strength 0.7 and 1.0, then the original crop against the blended one (the
    pack's route: YuNet on each 512 crop, the ArcFace 112 alignment, flip averaging);
  - drift: the original crop against a DIFFERENT person's restoration blended in at the
    same Strength (what a restorer that invents a new face would give).

Inputs are local only (nothing is downloaded): the People eval folder
(~/Dev/.mv-toolchain/faceeval: lfw/, dets.json, yunet.onnx, ir50_webface4m.onnx,
emb_ir50_webface4m_*.npz, and its venv with onnxruntime and opencv) and the two models.

  <faceeval>/venv/bin/python -I tools/enhance/face_guard.py --faceeval <dir> \
      --restoreformer <restoreformer_plus_plus.onnx> --gfpgan <GFPGANv1.4.onnx> [--faces 160]
"""
import argparse, json, os, time
import numpy as np, cv2, onnxruntime as ort

FFHQ512 = np.array([[192.98138, 239.94708], [318.90277, 240.1936], [256.63416, 314.01935],
                    [201.26117, 371.41043], [313.08905, 371.15118]], np.float64)
ARC112 = np.array([[38.2946, 51.6963], [73.5318, 51.5014], [56.0252, 71.7366],
                   [41.5493, 92.3655], [70.7299, 92.2041]], np.float64)


def fit_similarity(src, dst):
    """face_restore.cpp fit_similarity: least squares, never a reflection. 2x3."""
    ms, md = src.mean(0), dst.mean(0)
    p, q = src - ms, dst - md
    den = (p ** 2).sum()
    a = (p[:, 0] * q[:, 0] + p[:, 1] * q[:, 1]).sum() / den
    b = (p[:, 0] * q[:, 1] - p[:, 1] * q[:, 0]).sum() / den
    tx = md[0] - (a * ms[0] - b * ms[1])
    ty = md[1] - (b * ms[0] + a * ms[1])
    return np.array([[a, -b, tx], [b, a, ty]], np.float64)


def face_mask(side):
    """face_restore.cpp face_mask."""
    c = (np.arange(side) + 0.5) / side
    u = (c[None, :] - 0.5) / 0.34
    v = (c[:, None] - 0.56) / 0.42
    t = np.clip((1.0 - np.sqrt(u * u + v * v)) / 0.25, 0, 1)
    m = t * t * (3 - 2 * t)
    m[0, :] = m[-1, :] = m[:, 0] = m[:, -1] = 0
    return m.astype(np.float32)


def degrade(rgb, rng):
    """The plan's test condition: 2x up, blurred, noisy, JPEG 35."""
    img = cv2.resize(rgb, None, fx=2, fy=2, interpolation=cv2.INTER_CUBIC)
    img = cv2.GaussianBlur(img, (0, 0), 2.0)
    img = np.clip(img.astype(np.float32) + rng.normal(0, 6, img.shape), 0, 255).astype(np.uint8)
    _, enc = cv2.imencode(".jpg", cv2.cvtColor(img, cv2.COLOR_RGB2BGR), [cv2.IMWRITE_JPEG_QUALITY, 35])
    return cv2.cvtColor(cv2.imdecode(enc, 1), cv2.COLOR_BGR2RGB)


class Restorer:
    def __init__(self, path, output, threads):
        so = ort.SessionOptions()
        so.intra_op_num_threads = threads
        so.log_severity_level = 3
        self.s = ort.InferenceSession(path, so, providers=["CPUExecutionProvider"])
        self.out = output or self.s.get_outputs()[0].name

    def __call__(self, crop):
        x = (crop.astype(np.float32) / 127.5 - 1.0).transpose(2, 0, 1)[None]
        y = self.s.run([self.out], {"input": x})[0][0]
        return np.clip((np.clip(y, -1, 1) + 1) * 127.5 + 0.5, 0, 255).astype(np.uint8).transpose(1, 2, 0)


class People:
    """The pack's route on a picture: YuNet, the face nearest the crop's face centre,
    ArcFace 112 alignment, IR-50 with flip averaging, L2."""

    def __init__(self, root, threads):
        self.det = cv2.FaceDetectorYN.create(os.path.join(root, "yunet.onnx"), "", (512, 512), 0.6, 0.3, 5000)
        so = ort.SessionOptions()
        so.intra_op_num_threads = threads
        so.log_severity_level = 3
        self.s = ort.InferenceSession(os.path.join(root, "ir50_webface4m.onnx"), so,
                                      providers=["CPUExecutionProvider"])
        self.inp = self.s.get_inputs()[0].name

    def embed(self, rgb):
        h, w = rgb.shape[:2]
        self.det.setInputSize((w, h))
        _, faces = self.det.detect(cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
        if faces is None:
            return None
        c = np.array([w * 0.5, h * 0.56])
        best = min(faces, key=lambda r: np.linalg.norm(r[0:2] + r[2:4] / 2 - c))
        if np.linalg.norm(best[0:2] + best[2:4] / 2 - c) > 0.18 * w:
            return None
        m = fit_similarity(best[4:14].reshape(5, 2).astype(np.float64), ARC112)
        crop = cv2.warpAffine(rgb, m, (112, 112), flags=cv2.INTER_LINEAR, borderValue=0)
        x = np.stack([crop, crop[:, ::-1]]).astype(np.float32)
        x = ((x - 127.5) / 127.5).transpose(0, 3, 1, 2)
        o = self.s.run(None, {self.inp: np.ascontiguousarray(x)})[0].reshape(2, -1)
        v = o[0] + o[1]
        return v / np.linalg.norm(v)


def impostor_thresholds(root):
    out = {}
    for tag in ("full", "small24"):
        p = os.path.join(root, f"emb_ir50_webface4m_{tag}.npz")
        if not os.path.exists(p):
            continue
        d = np.load(p)
        v, lab = d["v"], d["labels"]
        s = v @ v.T
        iu = np.triu_indices(len(v), 1)
        imp = s[iu][(lab[:, None] != lab[None, :])[iu]]
        out[tag] = {"pairs": int(imp.size), "max": float(imp.max())}
        for far in (1e-4, 1e-5, 1e-6):
            out[tag][f"th@{far:g}"] = float(np.quantile(imp, 1 - far))
    return out


def q(a):
    a = np.asarray(a)
    return {"n": int(a.size), "min": float(a.min()), "p0.5": float(np.quantile(a, 0.005)),
            "p1": float(np.quantile(a, 0.01)), "p5": float(np.quantile(a, 0.05)),
            "median": float(np.median(a)), "max": float(a.max())}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--faceeval", required=True)
    ap.add_argument("--restoreformer", required=True)
    ap.add_argument("--gfpgan", required=True)
    ap.add_argument("--faces", type=int, default=160)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    root = a.faceeval
    res = {"impostor": impostor_thresholds(root)}
    dets = json.load(open(os.path.join(root, "dets.json")))
    # One picture per person, the first people in name order whose face is
    # at least 60 px between box edges (LFW's subject), deterministic.
    keys, seen = [], set()
    for k in sorted(dets):
        person = k.split("/")[0]
        if person in seen or min(dets[k][2], dets[k][3]) < 60:
            continue
        seen.add(person)
        keys.append(k)
        if len(keys) == a.faces:
            break
    models = {"restoreformer": Restorer(a.restoreformer, "0", a.threads),
              "gfpgan": Restorer(a.gfpgan, "1288", a.threads)}
    people = People(root, a.threads)
    mask = face_mask(512)[..., None]
    rng = np.random.default_rng(1)
    sims = {f"{m}@{s}": [] for m in models for s in (0.7, 1.0)}
    drift = {f"{m}@0.7": [] for m in models}
    lost = 0
    secs = {m: [] for m in models}
    prev = {}
    for i, k in enumerate(keys):
        rgb = cv2.cvtColor(cv2.imread(os.path.join(root, "lfw", k)), cv2.COLOR_BGR2RGB)
        lm = np.array(dets[k][4:14], np.float64).reshape(5, 2)
        # Odd faces degraded (the restorer's job), even ones as LFW has them.
        if i % 2 == 1:
            rgb = degrade(rgb, rng)
            lm = lm * 2
        m = fit_similarity(lm, FFHQ512)
        aligned = cv2.warpAffine(rgb, m, (512, 512), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
        e0 = people.embed(aligned)
        if e0 is None:
            lost += 1
            continue
        for name, model in models.items():
            t0 = time.time()
            restored = model(aligned)
            secs[name].append(time.time() - t0)
            for strength in (0.7, 1.0):
                blended = aligned + strength * mask * (restored.astype(np.float32) - aligned)
                e1 = people.embed(np.clip(blended + 0.5, 0, 255).astype(np.uint8))
                sims[f"{name}@{strength}"].append(-1.0 if e1 is None else float(e0 @ e1))
            if name in prev:
                other = prev[name]
                blended = aligned + 0.7 * mask * (other.astype(np.float32) - aligned)
                e2 = people.embed(np.clip(blended + 0.5, 0, 255).astype(np.uint8))
                drift[f"{name}@0.7"].append(-1.0 if e2 is None else float(e0 @ e2))
            prev[name] = restored
        if (i + 1) % 20 == 0:
            print(f"{i + 1}/{len(keys)} faces", flush=True)
    res["faces"] = len(keys)
    res["lost_original"] = lost
    res["restored"] = {k: q(v) for k, v in sims.items()}
    res["drift_other_person"] = {k: q(v) for k, v in drift.items()}
    res["seconds_per_face"] = {k: float(np.median(v)) for k, v in secs.items()}
    print(json.dumps(res, indent=1))
    if a.out:
        json.dump(res, open(a.out, "w"), indent=1)


if __name__ == "__main__":
    main()
