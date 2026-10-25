# Plan — Enhance: restored copies of photos, video and audio, models from Hugging Face chosen by spec

Issues #297 and #298 (one request filed twice, owner, 2026-10-10). **Enhance** is a first-party
add-on that writes a restored copy of the item on screen: a photo, a clip's picture or sound, or
an audio file. The original is never the output. It is not part of Local search: search reads
pixels and writes an index; Enhance writes a new file, never runs on the view and never shares
the render device. Written 2026-10-23.

What exists since the issue was filed: #482 ([model-tiers](model-tiers.md)) built the parts the
issue asked slice 1 to invent. A manifest may list upstream files the add-on fetches itself
(`remote`: URL, path, size, SHA-256, licence; `src/addon/manifest.h`), signed with the rest.
`src/addons/ai/fetch.h` fetches them from Hugging Face at a pinned revision, resumable and
verified, and says it was written so Enhance can use it as it is. Local search chooses *which*
models run from a local check of the machine (`machine.h`) and an add-on-owned picker. Screens
and slots ([25](../design/25-open-addons.md) §10) let a native add-on draw its own Settings page
and sheets, which both hosts render with their own controls. What does not exist: any Enhance
code, a per-file size cap, a free-disk gate, free RAM or VRAM, a model choice that is data rather
than four hard-coded tier ids, and any host-table row that writes pixels or sound to a new file.

## What changed since the issue

| The issue says | Now |
|---|---|
| The manifest gains a `weights` list | It is `remote` (#482), with the same fields. Enhance uses it. |
| `installed_size` includes the weights | `installed_size` counts shipped files; the signed manifest also carries `download_size`, the sum of `remote`. |
| Bytes land in `<addons>/.staging/`, then move into the version folder | Fetched files never enter the version folder (it verifies as signed, "no extras"). They land as `<path>.part`, are hashed, renamed, and get a `<path>.ok` record. Enhance keeps them under `data/cache/models/`, which every Remove deletes (`store::remove`), so "Remove deletes the weights" holds by construction. |
| Free disk counts the file plus the staging copy | There is no copy: the `.part` is renamed in place. The gate is the bytes still to fetch plus the model's `cache_bytes`. |
| Family `ai` still refuses a combination over 5 GB | Retired 2026-10-18 (#482). Not a verify any more. Enhance's own 5 GB **per weights file** cap (2026-10-13 row) stands and is now enforced. |
| Photo, video and audio are separate pieces | One add-on whose Settings page offers a model per kind, each of a stated spec; only the chosen model of the chosen kind is fetched, so a photo model still never pulls a video model. **Owner call 1.** |
| The signed piece carries the native add-on and *the chrome* | No chrome: the Models page and, from E2, the sheet are view trees both hosts already draw. |
| Sideload: drop the file at the pinned path | Works: a file of the right size at its pinned path under `data/cache/models/` is hashed once and adopted (a `.ok` written) instead of fetched again. The fetcher is shared, so this also holds for Local search. |
| Until the hash matches, the command says the model did not finish, with Resume | As written. A download resumes by itself through an outage *within* a session, but nothing is requested after a restart until the person clicks Resume. |

## How a model is chosen (the spec contract)

Local search picks one of four named tiers by hard-coded thresholds. Enhance's choice is **data**:
each kind lists its models from the largest to the smallest, each model states what it needs,
and a generic rule compares that with what this machine has. A new model is a manifest entry, not
a C++ change.

**The catalogue** (`tools/package/enhance-models.json`, staged as `models/enhance.json` plus a
`models/<id>/model.json` per model; the pins go into the signed manifest's `remote`). The
figures below are illustrative:

```json
{
  "kinds": {
    "still": {"name": "Photos", "unit": "a 12 MP photo", "cpu_budget_ms": 60000,
              "models": ["photo-large", "photo-standard", "photo-small"]},
    "video": {"name": "Video",  "unit": "a second of 1080p", "cpu_budget_ms": 30000, "models": []},
    "audio": {"name": "Audio",  "unit": "a minute of sound", "cpu_budget_ms": 20000, "models": []}
  },
  "models": {
    "models/photo-standard": {
      "model": {
        "id": "photo-standard", "name": "Standard", "model": "Real-ESRGAN x4plus",
        "kinds": ["still"], "run_bytes": 2254857830, "cache_bytes": 0,
        "accelerators": ["cuda", "coreml"], "cpu_unit_ms": 240000, "licence": "BSD-3-Clause",
        "knobs": [
          {"id": "scale", "label": "Scale", "type": "choice", "default": "4",
           "options": [{"id": "2", "text": "2×"}, {"id": "4", "text": "4×"}], "kinds": ["still"]},
          {"id": "denoise", "label": "Denoise", "type": "slider", "min": 0, "max": 1, "step": 0.05,
           "default": 0.5, "kinds": ["still"]}
        ]
      },
      "files": [{"repo": "…", "revision": "<40 hex>", "file": "…onnx", "as": "model.onnx",
                 "size": 67040989, "sha256": "<64 hex>", "licence": "BSD-3-Clause"}]
    }
  }
}
```

- `run_bytes`: peak memory one unit of work needs at the model's tile size (weights plus
  activations), measured. `cache_bytes`: what the provider compiles beside it (Core ML).
- `accelerators`: the providers the model has passed its self-test on (`cuda`, `coreml`).
- `cpu_unit_ms`: measured time for one `unit` on the reference CPU. CPU is offered only when it
  is within the kind's `cpu_budget_ms`; with no `cpu_unit_ms` the model needs a GPU.
- `knobs`: what the sheet shows (E2): `slider`, `choice` or `toggle`, with range, default and
  the kinds each applies to (`still`, `video`, `audio`), in the screen vocabulary's own shapes.
- A model may serve two kinds (one per-frame upscaler for stills and clips): both lists name it.

**The local check** (`src/addons/enhance/machine*.cpp`; computed here, shown on the page, never
sent anywhere): OS and CPU architecture, processor threads, total and free RAM, the accelerator
(Apple silicon; an NVIDIA adapter with its driver's CUDA library) and its memory (dedicated VRAM;
on Apple silicon the GPU shares RAM), and free space on the add-on's data volume.

**The rule** (`src/addons/enhance/choose.*`), per model, in order:

1. Any pinned file over **5 GB** (5,000,000,000 bytes): *refused*, never fetched.
2. An x86_64 Mac: no ONNX Runtime build, nothing offered (the add-on is `arch: arm64` there).
3. Provider: the machine's accelerator when the model lists it; else the CPU when
   `cpu_unit_ms ≤ cpu_budget_ms`; else *needs a GPU*.
4. Memory, to **offer**: `run_bytes` within the accelerator's memory (CUDA: VRAM; Core ML and
   CPU: total RAM). A graphics card too small for a model that rule 3 would also allow on the CPU
   hands it to the CPU when RAM holds it. To **run** (E3, before the first tile): within free
   VRAM / free RAM, or the command states the shortfall and does not start.
5. Disk: the bytes still to fetch plus `cache_bytes` within free space.

The **recommended** model of a kind is the first, largest to smallest, that passes all five. The
page lists every model with its download size, memory, provider, licence and, when it does not
fit, why ("Needs an NVIDIA graphics card", "Needs 24 GB of memory; this Mac has 16 GB", "Needs 4.2 GB
free on this disk; 1.3 GB is free"). A model that does not fit cannot be downloaded.

## Candidates (not pinned here)

The first weights for each kind are chosen when that kind's slice is built, by the licence gate
and a measurement (time per unit, `run_bytes`, output against the input), as the issue says.
Hugging Face is not reachable from the session that wrote this plan, so no revision or SHA-256
below is verified; the licences were read from the upstream LICENSE files.

| Kind | Small (CPU) | Standard (GPU) | Large (GPU) |
|---|---|---|---|
| Photos | realesr-general-x4v3, BSD-3-Clause, ~4.6 MB ONNX, ~1.2 M params | Real-ESRGAN x4plus, BSD-3-Clause, ~64 MB | Swin2SR real-world ×4 (Apache-2.0, ONNX on HF) or HAT / SwinIR-L (Apache-2.0, our export, ~140–170 MB) |
| Video (per frame) | realesr-general-x4v3 on a ≤ 540p downscale | Real-ESRGAN x2plus (BSD-3-Clause, our export) | RealBasicVSR ×4 (Apache-2.0), if its export holds |
| Audio | GTCRN (MIT) or DPDFNet-2 16 kHz (Apache-2.0) | DeepFilterNet3 (MIT OR Apache-2.0) or DPDFNet-8 48 kHz | MossFormer2 SE 48 kHz (Apache-2.0, ~229 MB) |

Refused by the gate as they stand: CodeFormer (S-Lab, non-commercial), GPEN (no licence), the
Qualcomm Real-ESRGAN builds (asset licence), GFPGAN (NVIDIA StyleGAN2 terms; FFHQ). Face
restoration and Demucs are owner call 5. A 42 MP still at ×4 is ~675 MP: the photo models offer
×2 and the default scale follows the input size (E3).

## Slices

Each slice is one PR. The shared core change and both host halves land together (D9); every
verify line also needs both present-loop gates, and launch → first pixel and a cached pan within
noise of an install without Enhance.

### E1 — The add-on, its Models page, weights from Hugging Face

- `src/addons/enhance`: a native slot add-on (`id` `enhance`, no chrome, `host_api` 3,
  contribution API 8) whose one contribution is a `settings` screen, **Models**. Nothing loads
  at launch beyond the add-on itself; nothing is read or hashed until the page opens.
- The catalogue, the local check and the rule above, as data and pure functions.
- Downloads through the shared fetcher (`mv_model_fetch`: `fetch.*`, `sha256.*` and the two
  transports, now a library both add-ons link), into `data/cache/models/`, one process at a time
  (a lock file; every window loads slot add-ons), on a thread of its own at background priority.
  The choices are one file every window shares: a click changes only its own kind's key, under a
  lock, and a prune keeps what any window chose.
  No request is made before Download is clicked in this process; the URL is the pinned one,
  with no query, and the transports send no cookie, token or identifier.
- Gates before any GET: the 5 GB cap (also in the packer and the catalogue check), the disk
  gate, and the model fitting this machine. A wrong hash is deleted and fetched again, three
  times, then reported; the model stays unavailable.
- The fetcher gains `max_file_bytes` (a pin over it is refused with no request) and adopts a
  file already at its pinned path when its hash matches.
- Changing a kind's model fetches the new one and then deletes the old one's files; Delete
  removes a kind's model. Remove deletes every weight (`data/cache`).
- Packaging: `enhance-models.json` and `enhance-models.py` (`check`, `stage`), sharing
  `ai-models.py`'s licence allow-list and pin rules; `addon-pack.py` packs `enhance` and refuses a
  pin over 5 GB. The real catalogue lists no model yet; a fixture catalogue of real, already
  verified Hugging Face pins (YuNet, SFace, Whisper turbo's encoder: 0.2 MB, 39 MB and 1.27 GB)
  exercises download and the spec rule end to end. Not released (no release job, no host offer)
  until E3 ships a command.
- **Docs:** decision-log rows (Enhance is its own add-on and one add-on; the network grant; the
  5 GB per-file cap; weights in `data/cache`; nothing before the click; adoption by hash); 18's
  fetched-files paragraph; DEVELOPMENT (build, sideload with the fixture catalogue, tests,
  "Where this actually is"); the roadmap entry.
- **Verify:** `mv_import_tests "[enhance]"` passes on Linux (portable core), macOS and Windows
  CI, and `"[ai][fetch]"` with it (in `mv_import_tests` on Linux, `mv_ai_tests` on macOS and
  Windows): a signed fixture add-on loads with no model present and makes no
  request; Download fetches, verifies and adopts; a cut body resumes with Range; a wrong hash is
  deleted and the model stays unavailable; a pin over 5 GB and a download larger than the free
  disk are refused before any GET; a dropped file is adopted by hash; Remove deletes the
  weights; the recommendation follows the rule on at least four machine classes.
  `test_addon_pack.py` (`EnhancePackTest`) and `enhance-models.py check` pass with no network.
  On a Mac and a PC, the sideloaded fixture build shows Settings → Add-ons → Enhance → Models
  with this machine's facts and recommendation, downloads a model from Hugging Face, survives a
  network cut, and Remove leaves no weights. Both present-loop gates hold (nothing here runs per
  frame).

### E2 — The command and the sheet

- An **Enhance…** command and context-menu entry, and a `sheet` screen built from the chosen
  models' `model.json` knobs: a still, a clip and an audio file each show only their own; a clip
  first offers picture, sound or both, and either target stays available with the other named as
  not installed. Values persist per kind (the add-on's own file in `data/`). A kind with no model
  keeps the command greyed with the model named; a document stays greyed; an animation stays
  greyed until a model declares it. Changing a knob never starts a job.
- Host side (both hosts): the `screen` payload reaches a slot add-on as an object (today it is
  replaced by `[]`) and carries the item's **kind** (`still`, `clip`, `audio`, `document`,
  `animation`), computed by one shared classifier so both hosts agree (today `video` differs);
  a per-kind `kinds` field on commands and items greys them; an item-changed event so navigation
  cancels.
- **Verify:** on both hosts a still, a clip and an MP3 each show only their knobs, the clip offers
  picture / sound / both, values come back per kind after a restart, and the command is greyed
  with the reason when that kind has no model.

### E3 — A still

- ONNX Runtime in the add-on (owner call 2), a session in `src/infer` with a generic provider
  self-test (an image metric, not CLIP's cosine), cancellable between tiles.
- New host-table rows: a full-resolution, tiled read of the still with its crop and colour stack
  baked (the stack lives in the shell today), and a still export through the export path
  (`io::unique_name`, carried metadata, `-enhance` suffix, owner call 4). Tiles stream to the
  encoder, so a 42 MP image is never one allocation.
- The run gate (free RAM / VRAM against `run_bytes`) before the first tile. The first photo
  models pinned by measurement.
- Release: `release.yml` packs and signs `enhance`, `github-release.py` publishes it; both hosts
  offer Enhance in Settings → Add-ons with the two sizes ("Install Enhance — 30 MB; the photo
  model you choose, 64 MB") and the Hugging Face line. **Docs:** a new `docs/design/29-enhance.md`,
  07 (export suffix), README (the feature), 13 (privacy paragraph).
- **Verify:** the original is byte-for-byte unchanged; a 42 MP still is tiled; Cancel leaves no
  file; a machine under `run_bytes` does not start; output against input on the eval set.

### E4 — Audio

- A host-table reader of every channel at the native rate (today `audio_*` is mono) and a writer
  of a new audio file (FLAC or WAV: no software AAC or MP3 encoder, owner call 3), from an MP3 or
  M4A and from a clip when the target is the sound. Playback of the original is unchanged.

### E5 — A clip's picture

- An every-frame decoder off the playback path and a frames-in hardware encode (labelled
  re-encode, never x264/x265), picture only or with E4's sound when both were chosen. HDR
  sources: an SDR map in, SDR tags out, said in the label.

## Owner calls

1. **One add-on, not three pieces.** The issue's pieces kept a photo model from pulling a video
   model. With weights as pins in one signed manifest, only the model chosen for one kind is
   fetched, and pieces would add three GitHub assets of two small files each, a host install
   hook for a chrome-less add-on and a parent-and-piece removal cascade that does not exist for
   slot add-ons. E1 is built this way; going back to pieces changes packaging only (the
   catalogue, the rule, the fetcher and the gates stay).
2. **ONNX Runtime and CUDA for Enhance.** A host's `piece_dir` serves only its own family, so
   Enhance cannot reach the `ai` pack's ORT or `ai-cuda`. Either Enhance carries its own ORT
   (~30 MB) and an `enhance-cuda` piece (~205 MB, CUDA and cuDNN still user-supplied), or a
   shared runtime family becomes a host rule. Two ORT builds in one process need a test first.
3. **Audio output format:** FLAC (lossless, no new encoder) or an OS AAC encoder (`aac_at`;
   Windows unmeasured).
4. **Output names:** `IMG_0001-enhance.jpg`, and for clips and audio the same suffix, not the
   trim family's `_suffix`.
5. **Face restoration and source separation:** no candidate clears the licence gate today.
6. **Where exports live:** most candidates are `.pth` on Hugging Face; ONNX copies are mostly
   personal repos, and a 404 is final. A project-owned Hugging Face repo of our exports would
   keep pins stable.

## Not in this plan

- Loading Enhance at launch, running a model on the view, as a refinement of the open photo, or
  as a rewrite of the original. A live preview on the playback clock. Smart cut.
- Bundling weights in git or in a GitHub release asset.
- Third-party model packs (open add-ons; signed, data only, and still behind the licence gate).
