# Plan — Local search model tiers, the install-time picker, models from Hugging Face

Issue #482, following #480 (the AI pack on Intel Macs). Today the Core pack carries two CLIP
towers and Auto picks between them by provider; every model file is zipped into the add-on by
release CI. This plan makes **which models run** a choice of four tiers (Maxxed, Default, Intel,
Potato), makes a new model a manifest entry rather than a C++ change, adds a picker on install,
and moves upstream model files out of our releases: the add-on fetches them from Hugging Face at
a pinned revision and verifies them against its signed manifest. Written 2026-10-18.

What exists: `tools/package/ai-models.json` already carries each model's dim, input size,
mean/std, tokenizer, context and thresholds, and `ai-models.py stage` writes them as a
`model.json` per folder. The index keys every row by model `spec`. What does not:
`src/addons/ai/pack.cpp` names the folders (`clip-b32`, `clip-l14`, `whisper-small`,
`whisper-base`, `clap-general`, `faces`, `doc-text`), so a different tower is a code change.

## Status (2026-10-18)

T1 to T7 are built on one branch (#482); what they do is in
[17](../design/17-local-ai-search.md) "Model tiers" and "The AI pack". Departures from the
slices below:
- **Pieces:** ViT-H/14 rides in a new piece, `ai-large`, which only Maxxed installs; the hosts
  install the pieces the picker asks for through a new host hook.
- **Candidates swapped before landing:** Maxxed documents stay all-MiniLM-L6-v2
  (multilingual-e5-small already missed the R@10 target in the #354 D3 spike, and it needs a
  SentencePiece tokenizer); Maxxed faces stay AdaFace IR-50 (IR-101's LFW gain does not pay for
  twice the cost without a real-library gain); SigLIP 2 waits for a SentencePiece tokenizer.
- **Calibration** is a quantile match against B/32's hand-set thresholds (`tools/ai-eval`).
- **Not done here:** the per-tier measurement rows on Windows, and both present-loop gates
  while indexing on every tier (T8); the PR says what was measured.

## The tiers (from #482)

| | Maxxed | Default | Intel | Potato |
|---|---|---|---|---|
| Pictures | OpenCLIP ViT-H/14 *or* SigLIP 2 So400m | L/14 accelerated, B/32 on CPU | B/32 | TinyCLIP ViT-39M/16 or 8M/16 |
| Speech | Whisper large-v3-turbo | Whisper small / base | Whisper base | Whisper tiny |
| Sounds | CLAP general | CLAP general | CLAP, off by default | none |
| People | YuNet + AdaFace IR-101 | YuNet + AdaFace IR-50 | YuNet + AdaFace IR-50 | YuNet + SFace, fewer clip frames |
| Documents | multilingual-e5-small | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 |
| Clip sampling | denser | as today | as today | sparser, photos first |

The Maxxed and Potato models are **candidates**. Each one must pass the PR 20 eval (COCO-1k R@10;
labelled-query P@5 ≥ 0.8) and the weights-licence gate. A candidate that fails is replaced by
the next one; it is never waved through.

## Slices

Each slice is one PR. The shared core change and both host halves land together (D9). Each verify
line also needs both present-loop gates.

### T1 — Tier profiles as data (no behaviour change)

- `ai-models.json` gains a `tiers` block: a `default` tier id and `profiles`, each naming a model
  folder per role: `pictures` (`high`, `fast`), `speech` (`high`, `fast`), `sounds`, `faces`,
  `documents`. A role's folder lives in that role's piece: pictures in `ai`; speech and sounds in
  `ai-audio`; faces in `ai-faces`; documents in `ai-docs`. Only **Default** is defined.
  The other tiers come in with their models (T5–T7), because `check` refuses a profile that
  names a folder the manifest does not have.
- `ai-models.py stage --piece ai` writes `models/tiers.json` into the Core pack. `check`
  refuses a profile with a missing role, a folder outside its role's piece, or an unknown
  default.
- `src/addons/ai/tiers.{h,cpp}` reads a profile. A pack staged before this change has no
  `tiers.json`, so the reader falls back to today's folders, kept in that one place as
  `legacy_profile()`. `pack.cpp` asks the profile for every folder. A tower's `quality` slot
  comes from the profile (`high` = 2, `fast` = 1), not from `model.json`.
- The engine always uses the file's default tier. There is no setting yet.

**Verify:** `mv_ai_tests "[tiers]"` passes on Mac and Windows CI; `ai-models.py check` passes
and refuses a broken profile (`test_addon_pack.py`). With a pack staged by the new
`ai-models.py`, the index's spec keys are unchanged, so nothing re-indexes and every existing
AI test still passes. Both present-loop gates hold. (Nothing here runs per frame.)

### T2 — The model contract in `src/infer`

- `model.json` gains `tokenizer_kind` (`clip-bpe` today; `wordpiece` already exists for the
  documents model; `sentencepiece` is reserved for SigLIP 2) and `score` (`softmax` today, or
  `sigmoid`). Core ML needs fixed dims, so `fixed_batch` and `image_size` are stated rather than
  assumed.
- A spec whose kind the build does not implement is refused with `unsupported_format`, and the
  tier is offered without that role. It must never crash or mis-tokenise.
- **Verify:** a test pack with an unknown `tokenizer_kind` loads the other roles and reports the
  one it could not load. Spec keys of the existing towers are unchanged.

### T3 — The shared model downloader (Hugging Face, resumable, verified)

- One piece of code, shared with Enhance (#297, "weights downloaded on install"). Its input is a
  list of `{url, size, sha256, as}` from the add-on's **Ed25519-signed** manifest; its output is
  verified files in the add-on's folder. Each file is a plain GET of
  `https://huggingface.co/<repo>/resolve/<revision>/<file>`: no token, identifier, query or
  cookies.
- Resumes from the last verified byte (a `.part` file plus a range request) and after a restart.
  When Hugging Face cannot be reached, it backs off and retries by itself, and the status reads
  *Waiting for Hugging Face… downloads resume automatically*. A 404 on a pinned revision is
  reported as gone, not retried. A file whose hash does not match is deleted and fetched again,
  never loaded.
- Release CI stops zipping upstream files. `addon-pack.py` writes the file list into the
  manifest, and the add-on carries code, ORT, chrome, `ai-models.json` and **our own exports**
  only (AdaFace, and later ViT-H/14, SigLIP 2 or TinyCLIP exports, each under GitHub's 2 GiB
  asset limit, a large one as its own piece).
- Retires the 5 GB `ai` family ceiling (owner, 2026-10-18): drop `addon-pack.py ceiling` in
  `release.yml` and the size sum in `ai-models.py check`. The picker still shows each tier's
  download and disk size.
- Docs: decision-log rows (supersedes 2026-10-13's "nothing new is downloaded" and the ceiling);
  17 "The AI pack"; 18's download section; DEVELOPMENT.
- **Verify:** a fresh install fetches no upstream model from GitHub. Cutting the network mid-file
  and restoring it resumes without user action and ends with a verified file. A flipped byte is
  refetched. The `ai` release assets drop to about 100 MB a platform.

### T4 — The local check, the picker, and changing tier

- **Local check** (in the add-on, never sent anywhere): CPU arch and cores, RAM, free disk, the
  accelerator (Apple silicon class and Neural Engine; an NVIDIA adapter with CUDA), laptop or
  desktop. It recommends Maxxed only with an accelerator and ≥ 32 GB; Intel on x86_64 Macs and
  CPU-only PCs; Potato on ≤ 2 cores or ≤ 8 GB; Default otherwise. After install, the existing
  self-test can still demote a tower (L/14 → B/32).
- **Picker**, owned by the add-on (an `AI.bundle` sheet on the Mac, a `MediaViewer.Ai.Chrome`
  dialog on Windows); the host only opens it. The recommended tier comes first and is
  preselected, and every tier is listed with its model names, licences, download and disk size
  (including the Core ML cache) and estimated time per 10 k photos. People, Sounds and Documents
  are checkboxes. Each file shows its download progress, and Cancel works throughout. Search
  opens once the picture tower is verified. A footnote says what Hugging Face sees.
- **Settings → Local search → Models** reopens the picker. A tier change downloads the new
  models and re-indexes, and the old index answers until the new one covers the folder. This is
  today's quality migration, keyed by spec. Then the old models are deleted.
- New engine setting `tier`, a new ABI status field and chrome strings on both hosts.
- **Verify:** on each platform, the picker recommends per the rules above on at least two machine
  classes. A tier switch keeps search answering throughout and ends with the old models gone.

### T5 — Calibration tool and the Intel tier

- `tools/ai-eval calibrate <model folder>` writes `dedupe`, `query_margin`, `query_z`,
  `result_*`, `similar_min` and the label bars into the model's `ai-models.json` entry from the
  COCO-1k and labelled-query sets, so a new tower needs no hand tuning.
- The Intel profile (B/32 only, Whisper base, Sounds off by default) as data. It follows #480's
  x86_64 ORT build. Auto never offers L/14 on it.

### T6 — Potato

- TinyCLIP export (`tools/package/*-export.py` with a golden check, like `face-export.py`),
  calibrated by T5, plus Whisper tiny and SFace (upstream ONNX, `opencv/face_recognition_sface`;
  its thresholds are `faces.h`'s SFace fallbacks; switching embedder re-analyses via
  `faces.spec`). Sparser clip sampling as profile data, which needs the sampler's gaps to become
  inputs.
- **This slice proves the contract:** TinyCLIP goes in with no C++ change.
- B/32 int8 stays rejected (R@10 0.822 < 0.88) unless Potato gets its own recall target, as a
  dated decision.

### T7 — Maxxed

- ViT-H/14 (our export; CLIP BPE, so no new tokenizer) first, and SigLIP 2 only if T2's
  `sentencepiece` lands. Whisper large-v3-turbo; AdaFace IR-101 only if the eval shows a gain on
  real libraries (`face-export.py --arch ir101`). Settings says that the Core ML cache passes
  ~4 GB.

### T8 — Measurements

- A row per tier in 17 "Measurements": model names, img/s, Whisper RTF, COCO-1k R@10, size, time
  to first searchable result, on Mac and Windows. Both present-loop gates **while indexing** on
  every tier.

## Not in this plan

Third-party model packs through open add-ons (25-open-addons). They would be signed and data
only, and still go through the licence gate.
