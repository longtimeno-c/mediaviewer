# 17 — Local AI search (video moments and photos)

**Status: proposed 2026-09-24, amended 2026-09-25 and 2026-09-27 (audio), post-v1. Not in PR 8. Milestone H, PRs 20–24 (renumbered 2026-09-24 from 21–25; it follows the Import add-on, PRs 16–19). Windows and macOS together (owner, 2026-09-24).**
Nothing here changes the PR 1–8 viewer. It is an opt-in add-on that arrives as a separate
component, after the viewer ships.

## What it is

Type "man on a broom" or "birthday cake" and get a grid of **moments**: for a video, a keyframe
thumbnail with its timestamp; for a photo, the photo. Enter opens the clip seeked to that moment.
"Find similar" takes the frame on screen as the query. It works over the folder (or folders) you
point it at, entirely on this machine.

It is a **search index over your own files**, built in the background. It is not tagging, not face
recognition, not a catalog, not cloud anything.

## Why it is compatible with the rules

| Rule | How it holds |
|---|---|
| 1 — nothing blocking on UI/render | Sampling, inference and search run on their own low-priority workers. Results arrive through the completion queue (14-abi) |
| 4 — zero dropped frames | Indexing yields to the render loop (see *Not hurting the viewer*). PR 1's present-loop verify is re-run **while indexing** |
| 5 — never modify an original | The index is a separate SQLite file. No sidecar, no write to media |
| 6 — nothing leaves the machine | Inference is local. Embeddings are derived from user content, so they get the same treatment as pixels: never in telemetry or crash reports, excluded from minidumps |
| 7 — no required codec pack | The analogue: **the base viewer never requires the AI pack.** No pack → feature hidden, nothing else changes |

## Contradictions this plan has to resolve (read before building)

These are earlier decisions the feature would touch. They are recorded in
[12-decision-log.md](12-decision-log.md) (2026-09-24); none is silently reversed.

1. **`16-commands.md` lists "Face detect, AI cull, cloud albums" as out of scope** ("Rule 6; also
   not a viewer"). **Owner call 2026-09-24: face detection is wanted** (as in iOS Photos), so it
   is now planned as PR 24 below, local-only. Rule 6 holds because nothing leaves the machine;
   the row is narrowed to "cloud albums" and, until the owner says otherwise, **AI culling**.
   Cloud stays out.
2. **`13-updates-and-telemetry.md` and the 2026-09-20 log entry say do not ship ONNX/DirectML.**
   That was about the *base installer* (36 % of the payload for a viewer that did no inference).
   It stands. The AI pack is a **separate optional download**; the base installer stays < 250 MB
   and PR 8's packaging assert still fails if ORT/DML appear in the base tree. The optional AI
   installation has its own **3 GB installed-size ceiling**; that ceiling does not relax the base
   viewer's limit.
3. **CLAUDE.md: "Do not introduce … D3D12."** Avoided: the plan uses vendor providers, not
   DirectML (which runs on D3D12), so the rule is untouched. See *Runtime*.

## Model choice

A **dual-encoder image/text embedding model** (CLIP family). One image tower, one text tower, a
shared vector space; search is a dot product. This is the only class of model that makes
"search video by a sentence" cheap: frames are embedded **once at index time**, queries embed
~10 ms at search time.

- **Quality target:** ordinary descriptions such as "guy on a skateboard" must retrieve the
  relevant photo or video moment even when the wording is not a stored tag. This semantic
  retrieval is the reason to spend more than the smallest workable model.
- **Candidates:** SigLIP and OpenAI CLIP-family checkpoints exported to ONNX. ViT-B/32-class at
  224 px is the **quality floor, not the intended default**. PR 20 compares it with at least one
  larger, licence-clean SigLIP-so400m / ViT-L/14-class tower and selects the best recall that fits
  the 3 GB installation ceiling and has acceptable measured indexing throughput. A larger model
  is allowed to win; the eval, not download minimisation, decides.
- **Weights licence is a gate, not a footnote.** The app is GPL-3.0-or-later; weights are data,
  but redistribution terms still bind. CI check: the pack manifest names the licence for every
  model file, and non-commercial / research-only weights fail the build. Some LAION-trained and
  Meta checkpoints are not permissive — check each.
- **Precision:** FP16 or INT8 (quantized) ONNX. Pick by measured recall loss in PR 20, not by
  assumption.
- Multilingual text queries are a *model* property (e.g. multilingual SigLIP variants), decided
  in PR 20 on the eval set. English-only is an acceptable first pack.
- **Out of scope for this doc:** OCR, object boxes, captioning. Each is a later, separate pack
  with its own tower. (Speech transcript search and sound search over a video's own soundtrack
  were out of scope here until the owner added them on 2026-09-27: see *Audio* below.) Speaking a query
  ("pull up all the photos that include…") is not a tower in this pack: it is the Voice
  add-on ([19](19-voice.md)), which calls this search and ships as its own install.

## Runtime

**ONNX Runtime**, dynamic-linked, behind our own interface — the same D9 discipline as
`IVideoSource`:

```
src/infer   IEmbedder { load(pack), embed_image(span<u8 rgb>, w, h), embed_text(string_view) }
            Backends: ort_openvino, ort_cuda (optional sub-packs), ort_cpu (fallback, always present),
            coreml (macOS: ORT's Core ML provider, GPU / Neural Engine; CPU underneath)
```

- `infer/` headers include **no `d3d11.h`/`d3d12.h`/DirectML** — same rule as `gfx/` consumers.
  The backend TU owns those.
- **CPU fallback is mandatory**, not a nicety: machines without a usable GPU, and the "GPU is
  busy presenting" case, both need it. Slower indexing is acceptable; a broken feature is not.
- **Backends are chosen by the user, with CPU always underneath** (owner call 2026-09-24).
  Settings → Local search → *Compute*: **Auto** (default: best available vendor provider for the
  detected GPU/NPU, else CPU) · a specific provider · **CPU only**. If a vendor provider fails to
  load, errors, or is slower than CPU in a first-run self-test, Auto silently falls back to CPU
  and says so in the status line. The toggle is live; changing it re-loads the model, it does
  not re-index (embeddings are compared by `model_id`, and precision is part of that id).
- **Vendor providers, not DirectML.** This resolves the D3D12 question: no DirectML, so no D3D12
  is introduced. Candidates, each its **own optional sub-pack** so a user only downloads the one
  their hardware uses: **OpenVINO** (Intel iGPU/NPU, Apache-2.0), **CUDA/TensorRT** (NVIDIA;
  redistribution is under NVIDIA's EULA, not an OSI licence — legal check against the app's
  GPL-3.0-or-later status before shipping it, and if it fails the gate it is user-supplied
  instead). **AMD GPUs have no good ORT provider on Windows without DirectML**; they run CPU
  until a provider exists. That is a known gap, not a hidden one. Adding DirectML later as one
  more provider would need the CLAUDE.md D3D12 rule reworded and is not planned.
- ORT owns whatever device the provider creates. It never shares our render `ID3D11Device` (or `MTLDevice` on Mac),
  and never touches the swapchain. Not Windows App SDK AI / Windows ML — ORT directly, so the
  payload is ours and the base-installer exclusion (log 2026-09-20) stays intact.
- **macOS (dual-track, owner 2026-09-24):** the same ONNX model through **ONNX Runtime's
  Core ML provider** on Apple Silicon (GPU / Neural Engine), with ORT's CPU provider
  underneath. Not a hand port to Core ML, not `Vision`/`NaturalLanguage` embeddings: one
  model, one ORT, two providers per OS. Settings → Local search → *Compute* on Mac is
  **Auto / Core ML / CPU only**, with the same self-test fallback as Windows. Core ML ships
  inside the Mac Core pack (it is part of the OS, so there is no vendor sub-pack to download).
  Whether the official macOS ORT package's Core ML provider covers every operator in the chosen
  model is measured in the PR 20 spike. Unsupported operators fall back to CPU inside ORT,
  and the spike records the speed cost.
- Embeddings from different backends differ slightly, so **the index records `model_id` +
  `spec` + backend precision, and a query only ever compares vectors from the same model**:
  never mix. An index is local to one machine. It is never synced between a user's Windows PC
  and Mac, and each one indexes its own folders.

## The AI pack (delivery)

**Installs through the add-on mechanism that Import builds in PR 16**
([18-import.md](18-import.md#add-ons-how-import-is-installed)): the same Settings → Add-ons
page, signed manifest, verify-before-load, per-user versioned folder, silent updates and
sideloading. The AI pack adds its sub-packs and model files. It does not build a second
installer.

- **The whole feature is a downloadable extra, installed from Settings** (owner calls
  2026-09-24 and 2026-09-25) — it may use up to **3 GB installed** so model quality is not held
  to the base viewer's 250 MB budget. Settings → **Local search** shows what is
  installed, the size of each piece, and **Install / Remove** for each: *Core* (ORT + CPU +
  image/text model), *Faces* (PR 24), and one sub-pack per vendor provider. Remove deletes the
  files and offers to delete the index. Nothing about it appears elsewhere in the UI until the
  core pack is installed; the base app, installer and update size never include it.
- The **3 GB ceiling** covers the Core pack, the one selected vendor-provider sub-pack, and Faces
  when Faces is installed. `index.db` and the existing thumbnail cache are user data and do not
  count toward it. The signed manifest declares download and installed sizes per piece; CI rejects
  a supported combination over the ceiling, and the installer refuses a manifest that would
  exceed it. Do not silently install every hardware provider.
- Downloaded on **explicit opt-in** ("Install local search — downloads ~N GB, uses ~N GB") from the same
  signed release channel as updates ([13](13-updates-and-telemetry.md)). Contents:
  `onnxruntime.dll`, provider DLLs, the model files, `manifest.json` (versions, SHA-256,
  licence per file). Provider sub-packs are offered based on detected hardware, never
  auto-installed.
- Verify signature + hashes before load. The request carries **no identifier, no path, no
  telemetry** — a plain GET of a fixed URL. Ask before the first download, no pre-ticked box.
- Installed to `%LocalAppData%\MediaViewer\addons\ai\<version>` on Windows and
  `~/Library/Application Support/MediaViewer/Add-ons/AI/<version>` on Mac, per-user, removed
  by uninstall (with a separate "also delete search index" choice). The Mac pack
  (`libonnxruntime.dylib`, the add-on library, models) is Developer ID-signed and notarized,
  and loads only under library validation, like the Import add-on
  ([18](18-import.md#add-ons-how-import-is-installed)).
- Offline / sideload: the pack can be dropped into that folder by hand; the app verifies it the
  same way.

## Frame sampling (the "keyframes" part)

A separate demux/decode instance — the same pattern as PR 5c's scrub-preview decoder, **never
the playback decoder**.

1. **Keyframe pass.** Demux with `skip_frame = AVDISCARD_NONKEY`. I-frames are the cheap,
   already-independent frames and decode quickly. Phone/camera GOPs are typically ~1 s, so this
   is ~1 candidate per second for free.
2. **Coverage fallback.** Long-GOP or intra-poor sources (some MKV/TS) can go minutes between
   keyframes. Enforce a **maximum gap** (default 2 s): if none arrives, decode-forward to the
   next timestamp on the grid. Enforce a **minimum gap** too, so a burst of scene-cut keyframes
   does not flood the index.
3. **Dedupe by embedding.** After embedding, drop a frame whose cosine similarity to the last
   *kept* frame is above a threshold (start ~0.97, tune on the eval set). A static interview
   shot collapses to a few rows; an action scene keeps many.
4. **Frame → tensor.** Decode (D3D11VA on Windows / VideoToolbox on Mac where the device allows, else software), resize on the
   GPU or `swscale` to the model's input, normalise with the model's mean/std. Colour: HDR/PQ/HLG
   sources go through the **existing SDR tone-map** first — the model was trained on SDR sRGB
   ([05-video-pipeline.md](05-video-pipeline.md), D6).
5. **Record** `pts` in the stream time base **and** as milliseconds, plus a flag for
   keyframe / grid-fill. Jump-to-moment must land where the frame is, so PR 21 stores the exact
   PTS and lets the existing dual-mode seek (05) decode forward to it.
6. **Photos** are one row each (embedding of the first-pixel-quality decode, not the full RAW
   decode; RAW uses the embedded preview). Live Photo / RAW+JPEG pairs index once, on the pair's
   still.

## Index store

New SQLite file `%LocalAppData%\MediaViewer\ai\index.db` (Mac: `~/Library/Application Support/MediaViewer/ai/index.db`), separate from the thumbnail DB so
"clear the index" cannot corrupt thumbnails, and vice versa.

```
assets(id, path, mtime, size, kind, spec, state, indexed_at)        -- key: (path, mtime, size, spec)
frames(asset_id, pts_ms, pts_tb, flags, thumb_ref, emb BLOB)        -- emb = fp16 or int8 vector
roots(id, path, scope, enabled, last_scan_at)                        -- remembered index locations
meta(model_id, dim, spec, ...)
```

- **Folder roots persist.** Choosing "this folder" or "this folder and below" writes a `roots`
  row. Reopening the app or that folder does not rebuild it: a watcher plus a startup delta scan
  queues only new, changed or removed assets. The user can pause, rescan or remove each root.
- Keyed like the thumbnail cache: change `(path, mtime, size)` or the model `spec` and the row is
  stale and re-queued. No content hashing pass over a camera dump.
- `thumb_ref` points into the **existing JPEG-512 cache** (`jpg512.1`) — a result tile is a
  thumbnail the app already knows how to draw; do not build a second thumbnail path.
- Resumable: state is per asset (`pending / partial / done / failed`); a kill mid-clip resumes
  at the last committed frame.
- Path strings live here, locally. That is fine (rule 6 is about leaving the machine) — but the
  file is excluded from crash reporting and telemetry, like every other user-derived store.

## Search

- **Query encode** on the CPU backend, ~10 ms; cached for repeated queries.
- **Scan:** brute-force dot product over an in-memory (memory-mapped) matrix with AVX2. At 512
  dims, INT8, that is 0.5 KB/frame — 100 k frames ≈ 50 MB and single-digit milliseconds;
  1 M frames ≈ 500 MB, which is where scoping by folder matters. **No ANN index (HNSW) until a
  measured p95 says brute force is too slow** — it is a dependency and a rebuild story for a
  problem that may not exist at camera-dump scale.
- **Ranking:** top-K frames, then **group per clip** with the best moment first and "N more in
  this clip" — otherwise one 40-minute video with a dog in it fills the whole page.
- **Scope:** current folder (default) · this folder and below · all indexed folders. Kind filter
  (photos / video). Min-score cutoff so a nonsense query returns "nothing found", not the
  least-bad ten.
- **Find similar:** the current still or the paused frame's embedding as the query. For a paused
  frame, embed it on demand (it is not necessarily a sampled keyframe).

## UI and commands

Reuse, do not grow a router or a second present path ([16-commands.md](16-commands.md)):

- Results are the **gallery** (`G`) grid (the XAML island on Windows, the SwiftUI gallery on Mac) over a result set instead of a folder listing:
  same thumbnail cache, same selection, same keyboard model. A tile shows the frame thumb and
  `mm:ss`.
- **Enter** on a video tile opens the clip and seeks to that PTS, paused on the frame, with the
  scrub bar marking the other matches from the same clip (like the keyframe grid PR 13 will draw).
- New command ids, added as **rows in the 16 table** when PR 22 lands, not before:
  `search.open` (a query box, keyboard-focus), `search.similar`, `search.next_match` /
  `search.prev_match` (within a clip). Key assignment is done then, against the live table, so
  it cannot collide. Everything reachable without the mouse, per 16's verify.
- When an unindexed folder is open and the Core pack is installed, Local search offers
  **Index this folder** and **Index this folder and subfolders**. The recursive action is also
  available from the folder toolbar/menu. It adds a remembered root, starts background work,
  and makes that scope searchable as results commit; the user does not have to wait for the
  entire tree before trying a query.
- A visible **indexing status** (progress, pause/resume, "paused — playing video"). Never a modal.
  It shows assets/frames completed, the active root, measured rate and an ETA range. For a very
  large tree the ETA must be based on completed work, not a hard-coded claim.

## Not hurting the viewer

This is the risk that matters. It is a background load on the same machine and GPU as a viewer
whose entire selling point is smoothness.

- **New thread role: inference workers**, lowest priority (`THREAD_MODE_BACKGROUND_BEGIN` for I/O
  as well), at most `min(2, cores/4)`; never the pool that decodes for the viewport. Viewport
  decode jobs always preempt index jobs — the existing **generation counter** cancels stale
  work when the user navigates.
- **Yield policy:** indexing **pauses** during video playback, slideshow, and any window where
  the render loop is animating (springs, pan, fullscreen transitions), and on battery below a
  threshold, and when the F3 frame-time overlay's rolling p99 exceeds budget. Idle → resume.
- **GPU:** a vendor provider runs at low GPU priority where it exposes one; the CPU backend is the pressure valve. If a
  present is late while an inference dispatch is in flight, the yield policy backs off — this is
  measured, not assumed (see verify).
- **Memory:** cap decoded-frame and tensor memory; the index scan matrix is mapped, not copied.
  Counted against the budgets in [02-architecture.md](02-architecture.md).
- **Disk:** the index has a size cap and per-folder delete. Show its size in Settings. This cap is
  separate from the 3 GB installed add-on ceiling.
- **Loading (2026-09-27, measured).** Opening the models is the heaviest thing the pack does
  (a GPU context, a gigabyte of weights, the self-test). Done during the PR 1 soak it dropped two
  frames, so the load waits for the same quiet viewer indexing does, and reports LOADING with
  `yield_reason` VIEWER meanwhile. Pictures become searchable first; the audio models open after
  them. Settings never loads the runtime (it asked from the UI thread): until the control thread
  has, it shows no models. The provider self-test's CPU half is kept (`selftest.txt`: the CPU
  reference embedding and time, keyed by runtime, provider, model and piece), taking ~15 s off
  a 40 s CUDA ViT-L load; the provider still runs and is checked against it every start. The
  Windows status pill no longer appears, or spins, for a load that is waiting: a spinning element
  over a busy canvas cost the soak its frame-statistics continuity.
- **Verification (2026-09-27).** Listing, loading and each piece lookup hashed every file again
  (~6 GB per launch with the whole pack). The first check in a process hashes; later ones stat
  every file (size and modification time at the filesystem's precision) and re-walk for extras.

## ABI

Flat, POD, opaque handles, correlation ids, completion-queue delivery — no exceptions across the
line ([14-abi.md](14-abi.md)). Sketch only; the header is written in PR 20:

```
mv_ai_pack_status / mv_ai_pack_install(progress cb via completion queue)
mv_index_start(folder, scope) / mv_index_root_remove / mv_index_pause / mv_index_status
mv_search_query(text | frame) -> job id ; results arrive as completions
mv_search_result_get(job, i) -> { asset id, pts_ms, score }
```

C# wraps each in a `SafeHandle`/`IDisposable`; C++ never calls the dispatcher.

## Roadmap slices

Each is independent with a verify line, and each inherits PR 1's present-loop verify. Numbers
follow the Import add-on (PRs 16–19). **Every slice is dual-track** (owner, 2026-09-24):
shared `infer/`, sampler, index and search in the core, a WinUI half and a SwiftUI half,
and each verify line run on **both** platforms. Windows uses CPU + vendor providers; Mac uses
CPU + Core ML. The frame sampler uses the platform's own hardware decoder in a separate
instance (D3D11VA on Windows, VideoToolbox on Mac), never the playback decoder. Both
present-loop gates (Windows PR 1, Mac PR 1) hold **while indexing**.

### PR 20 — Inference host and the AI pack
`src/infer` (`IEmbedder`), ORT CPU + first vendor provider (OpenVINO or CUDA, by what the dev box has), pack manifest/verify/download/install as **per-piece Install/Remove in Settings**, the Auto/provider/CPU-only toggle, opt-in
flow, settings page. **No indexing, no UI beyond the opt-in.** A short spike first compares the
ViT-B/32 floor with at least one larger licence-clean tower: measure labelled-query recall,
image-tower throughput, model/download/installed size and Core ML operator coverage, then record
the selected checkpoint and precision here. Those numbers, not guesses in this doc, size
everything after. Prefer the better model up to the 3 GB ceiling when the recall gain is real.

**Verify (both platforms):** a fixed set of test images embeds to vectors within tolerance of the reference
(PyTorch/ORT-Python) outputs on CPU and on each platform's accelerated provider (the vendor
provider on Windows, Core ML on Mac); the Mac base app bundle is unchanged with the pack absent; a text query ranks a small labelled image set
correctly; **base installer and base install tree are byte-identical to PR 8's with the pack
absent** and the packaging assert still fails if ORT or provider DLLs land in the base tree; tampering with a
pack file or manifest is refused; CI and install both refuse any supported installed combination
over 3 GB; the download request contains no identifier; both present-loop gates hold with the
pack installed and idle; the selected model retrieves the PR 20 labelled examples, including
"guy on a skateboard", at the recorded target.

### PR 21 — Video sampler and index
Keyframe sampler with min/max gap, HDR tone-map, embedding dedupe, `index.db`, background queue,
remembered folder roots, incremental rescan, resume, stale detection, yield policy, indexing
status, and the **Index this folder and subfolders** action.

**Verify:** a 1-hour 4K HEVC clip indexes to completion in a recorded time; killing the process
mid-way and restarting resumes without re-doing committed frames; editing/replacing a file
re-queues it; reopening a remembered tree queues only its delta; a fresh and an incremental run
publish measured assets/frames per second and an ETA on the target CPU and accelerated hardware.
Record a measured completion range for a representative **300,000-asset** photo-heavy library and
a separately described mixed photo/video library; until that benchmark exists the plan makes no
fixed time promise. **Playing a different 4K clip and panning photos while indexing runs stays at 0
dropped frames** (`tools/frametime` soak with indexing active, not just idle); the pause-on-
playback policy visibly triggers; the index contains no data from an HDR clip that is clearly
wrong (tone-mapped, not washed out); minidump from a forced crash mid-index contains no path,
filename, embedding or pixel data.

### PR 22 — Search and results
Query box, brute-force scan, per-clip grouping, results in the gallery (both hosts), jump-to-moment,
match markers on the scrub bar, folder/kind scope, min-score cutoff. Photos indexed too. The
16-commands rows land here. Natural-language cases include "guy on a skateboard", not only
single-object nouns or exact metadata.

**Verify:** on a **labelled eval set** (a folder of real camera-dump clips and photos with known
moments, kept out of git like the RAW corpus) recall@10 meets a target set from PR 20's numbers
(record the target here when set — do not invent it now); a query returns in < 100 ms over
100 k indexed frames; Enter on a result lands on the right frame within one GOP-decode of the
stored PTS; "nothing found" appears for a nonsense query; the whole flow — open search, type,
navigate results, open a moment, next/prev match, close — works **without the mouse**.

### PR 23 — Find-similar, index management, hardening
Find-similar from a still and from a paused frame, pause/clear/size-cap/per-folder controls,
"delete index on uninstall" choice, model-upgrade migration (new `spec` re-queues, old index kept
until the new one completes), fuzz the pack manifest parser and the index reader.

**Verify:** find-similar returns visually related frames from the same and other clips; clearing
the index frees the disk and leaves thumbnails intact; a model upgrade never mixes vector spaces
(a query against a half-migrated index uses one model only); uninstall removes the pack, and the
index if chosen; manifest/index fuzz corpus runs clean.

### PR 24 — Faces (local people search)
Face detector + face-embedding model in the same AI pack (separate model files, separate licence
check — many face-recognition weights are non-commercial, so the licence gate is the first task),
sampled from photos and from the video keyframes PR 21 already extracts. Faces are clustered
on-device into unnamed **people**; the user names a cluster if they want; search gains "photos of
<name>" and "this person" from the current frame. Merge/split/"not this person" corrections are
the minimum UI — a face grouping that cannot be corrected is worse than none.

**Biometric handling** (stricter than the frame index, because a face embedding identifies a
person): stored in its own table/file, off by default until a separate opt-in ("Find people in
your photos"), one-click delete of all face data, never in telemetry, crash reports or minidumps,
never exported, no names sent anywhere. Face data is never used for anything but local search.
Some jurisdictions (e.g. Illinois BIPA, EU GDPR biometric rules) treat this as sensitive; the
purely local, opt-in, deletable design is what keeps it defensible, and licensing/legal review
is a gate before release.

**Verify:** on a labelled set, detection and clustering meet targets recorded from the PR 20
spike; correcting a cluster persists and re-applies; deleting face data leaves the frame index
intact and leaves no face vectors on disk (checked by scanning the index and pack folders);
a forced crash minidump contains no face vectors or crops; PR 1's present-loop holds while
faces are indexing.

## Implementation notes (2026-09-26, Milestone H branch `milestone-h-local-ai-search`)

All five slices (PRs 20-24) are written as one change, like Import's four, because they share
one engine. **Windows: built and tested. Mac: built and tested with the real pack (2026-09-27,
"Verified on macOS" below)**; the Mac checklist is `src.swift/AIChrome/MAC-VALIDATION.md`. Every present-loop gate and hardware timing on both
platforms is still owed unless listed as done below.

### PR 20 spike results (measured on this dev box: Ryzen 16 threads, RTX 4070, ORT 1.30)

Eval: 1,000 COCO Karpathy-test images, text -> image with each image's first caption, plus
eight labelled natural-language queries (P@10 by caption keywords, 1,225 images).

| Tower (ONNX, Xenova export of OpenAI CLIP, MIT) | R@1 | R@5 | R@10 | mean P@10 | "guy on a skateboard" P@10 | CPU img/s (4 thr) | CUDA img/s | size |
|---|---|---|---|---|---|---|---|---|
| ViT-B/32 fp32 | 0.494 | 0.777 | 0.888 | 0.86 | 1.0 | 20.1 | - | 606 MB |
| **ViT-B/32 fp16** | 0.493 | 0.777 | 0.888 | 0.86 | 1.0 | 23.3 | 150.8 | 303 MB |
| ViT-B/32 int8 | 0.398 | 0.695 | 0.822 | 0.88 | 1.0 | 31.5 | - | 154 MB |
| ViT-B/16 fp16 | 0.500 | 0.794 | 0.885 | 0.84 | - | 8.5 | 135.7 | 300 MB |
| **ViT-L/14 fp16** | 0.555 | 0.812 | 0.905 | 0.90 | 1.0 | 1.9 | 98.0 | 856 MB |

**Selected (owner rule: "prefer the better model up to 3 GB when the recall gain is real"):**
both **ViT-L/14 fp16** (quality "High", +6 points R@1 over B/32) and **ViT-B/32 fp16** ("Fast")
ship in the Core pack. Auto picks High where an accelerated provider runs it (CUDA, Core ML) and
Fast on CPU only, where L/14's 1.9 img/s would take days on a 300,000-asset library. int8
rejected (-6.6 points R@10); B/16 rejected (no R@10 gain at 2.7x B/32's CPU cost). Stored
vectors are int8 with a per-vector scale: measured loss <= 0.5 points R@1 on both towers.
Changing quality migrates (PR 23): the old index answers until the new one completes.

**"Nothing found" (the min-score cutoff).** An absolute cosine floor does not separate
nonsense from real queries on CLIP (their top scores overlap), and a collection z-score does
little better. What works: each stored vector also stores its best cosine against five generic
prompts ("a photo.", ...); a query is "nothing found" unless one of its top ten rows beats its
generic score by **0.04**, and a result row must beat it by 0.015. At 0.04: 96-99 % of real
queries kept, 69-100 % of nonsense rejected (B/32 and L/14, 300 and 1,000 images). Recorded in
each model.json.

**Amended 2026-09-27: short queries.** That calibration used caption-like queries. On one-word
subjects the margin threw away correct rankings: "dog" sits close to "a photo.", so on the
300-image set "dog" returned 1 of its 15 photos, and only 6 of 17 real queries (B/32) and 7 of
17 (L/14) passed at all, while their top five were right. A query now also passes when its ten
best **assets** stand out from the rest of the index: the mean of the ten best per-asset scores
at least **2.5** standard deviations above the mean of every asset's best score (`query_z`).
When it does, a row at least **2.0** SD above that mean is a result as well as the margin rows
(`result_z`). Per asset, so a long clip's hundreds of similar frames count once; below 30 assets
the margin alone decides. Same set: 15 / 17 (B/32) and 14 / 17 (L/14) real queries kept, 3 / 3
nonsense rejected on both; "dog" returns 15 rows, "a cat" 12. Pictures only: CLAP keeps the
margin rule until it has its own calibration.

**Recall target for PR 22 (recorded here as plan/17 asked):** on the labelled set, the top five
for each natural-language query hold at least four relevant items (P@5 >= 0.8), "guy on a
skateboard" included; the COCO-1k proxy R@10 >= 0.88 (Fast) / >= 0.90 (High). The owner's real
camera-dump eval set (kept out of git) is still to be labelled and run.

**Licence gate:** CLIP weights MIT (OpenAI's model card discourages "deployed use"; that is a
usage note, not a licence term; flagged to the owner), YuNet MIT, SFace Apache-2.0, ONNX
Runtime MIT. `tools/package/ai-models.py check` enforces the allow-list; a CC-BY-NC file fails
the pack (tested).

**Sizes:** Core ~1.18 GB installed (both towers, tokenizer, ORT CPU, mv_ai, chrome), People
piece 39 MB, NVIDIA piece ~205 MB (ORT's CUDA 13 build only: the CUDA runtime and cuDNN are
**user-supplied**, NVIDIA's EULA review against GPL-3.0-or-later not done). Worst supported
combination ~1.4-1.85 GB of 3 GB. OpenVINO has a code path but no piece yet (no Intel dev box).
**macOS: arm64 only** - Microsoft ships no x86_64 macOS build of ORT 1.30; Intel Macs are not
offered Local search.

### What was built

- **Host table v2** (`mediaviewer_addon.h`): stills and sampled video frames as pixels, moment
  thumbnails in the existing JPEG-512 cache (`path#t=ms` rows), and verified family pieces.
  Versions only append; the host serves every layout from 1 up and negotiates, so Import 1.0.0
  keeps loading (plan/18). Manifests gain `part_of` (pieces) and `arch`; the store enforces the
  3 GB family ceiling and hashes only the add-on being loaded.
- **Sampler** (`src/edit/clip_sample.*`): its own software decoder, keyframes via
  `skip_frame = NONKEY`, grid fill for long GOPs from kept packets, min/max gap, rotation, SAR,
  the SDR tone map for PQ/HLG (the clip core's `rgba_converter`, now shared). Hardware decode
  (D3D11VA / VideoToolbox) for the sampler is a later optimisation, not done.
- **`src/infer`**: ORT loaded at run time (never linked; telemetry events off), the CLIP BPE
  tokenizer (matches the checkpoint's tokenizer.json on 19 golden strings), PIL-exact bicubic
  preprocessing, CPU / CUDA / OpenVINO / Core ML sessions, the Auto self-test (agree with CPU
  within cosine 0.99, and be faster, else CPU with the reason), YuNet + SFace.
- **`src/addons/ai`** (`mv_ai`, `mv.ai.1` in `mediaviewer_ai.h`): index.db, remembered roots
  with delta scans and pairing (one row per Live Photo / RAW+JPEG pair), background workers at
  OS background priority yielding to the viewer (and for two seconds after any dropped frame),
  battery and user pause, dedupe, resume per asset, stale detection, the in-memory int8 matrix
  (deviation: loaded, not mapped; ~77 MB at 100 k L/14 frames), per-clip grouping, find-similar,
  people in a separate faces.db with rename / merge / not-this-person / split, one-click
  deletion.
- **Results in the gallery**: base ABI 0.13 `mv_folder_open_list` - the same gallery, filmstrip,
  keys and thumbnails over a result list; a clip opens paused on its moment.
- **Commands** (plan/16): `Ctrl+F` search, `Ctrl+Shift+F` find similar, `N` / `Shift+N` next /
  previous matching moment; listed only while the pack is loaded.
- **Chrome**: WinUI `MediaViewer.Ai.Chrome` and SwiftUI `AI.bundle` - search panel, results,
  status pill, scrub-bar match dots, Settings -> Local search with per-piece install, budget
  bar, compute / quality, roots, People.

### Audio (added 2026-09-27, owner)

The owner asked for audio as a separate index option: a video's **soundtrack**, indexed for what
it **sounds like** (CLAP) and for what is **said** (Whisper transcripts). Standalone audio files
are not a D5 format and stay out.

- **The option.** Settings "Index videos for" is Pictures / Sound / Both, and each remembered
  folder may override it (`root_set_media`, `MV_AI_MEDIA_*`). Unset means Pictures, or Both once
  the audio piece is installed: installing it is the choice. Adding Sound to a folder queues its
  clips; removing it drops their audio rows.
- **The piece.** `ai-audio` (~1 GB: CLAP ~377 MB, Whisper small ~463 MB and base ~140 MB, their
  tokenizers), `part_of` ai, inside the 3 GB family ceiling with every other piece
  (`ai-models.py` checks the largest combination). Nobody downloads it who does not want it.
- **Sounds: LAION CLAP `larger_clap_general`** (Apache-2.0; Xenova ONNX export, fp16), 48 kHz,
  10 s windows on a 5 s hop, stored like frames (int8 vectors, a generic-prompt score each).
  Picked over `htsat-unfused` on ESC-50 zero-shot: **87.2 %** against 84.4 %.
- **Speech: Whisper** (onnx-community exports, Apache-2.0) through the merged KV-cache decoder
  with timestamp rules, a no-speech and log-probability guard, a re-listen when a window ends
  mid-sentence or stops early, and an energy-based refinement of segment starts. **small**
  where the picture tower runs High (a GPU / the Neural Engine), **base** elsewhere. Real-time
  factor on this dev box:

  | Model | CPU (2 threads) | CUDA |
  |---|---|---|
  | base | 0.075 | 0.029 |
  | small | 0.23 | 0.036 |

  Mel features match `transformers`' `audio_utils` (periodic Hann, centred reflect padding,
  Slaney mels); the tokenizer is GPT-2 byte-level BPE (RoBERTa's for CLAP, Whisper's decode).
- **Host table.** v2 appends `audio_open` / `audio_read` / `audio_close`: mono float PCM at a
  requested rate from the viewer's own FFmpeg, from a start time (`src/edit/clip_sample.*`).
- **Search.** `MV_AI_FIND_PICTURES` / `SOUNDS` / `SPEECH` choose the towers (none set: all).
  Each model's rows rank in its own units (margin over its generic prompts for pictures and
  sounds, word coverage for speech) and merge per clip; a speech result carries its sentence
  (`result_snippet`). `MV_AI_STATUS_AUDIO_READY` and the sound / speech counts are in the status.
- **Chrome.** Both platforms: the option in Settings and per folder, "Sounds" and "Speech" in
  the search panel's filter, the snippet under a spoken result.

### Verified on Windows (2026-09-26)

- Embeddings within cosine 0.999 of the ORT-Python reference on CPU for both towers and
  three cards; CUDA 0.9991-0.99999 (`mv_ai_tests`, `tests/data/ai/reference.json`).
- Labelled ranking: >= 4 of top 5 relevant for every query on both towers, "guy on a
  skateboard" included; nonsense finds nothing.
- Engine suite (fake models over the real host table): indexing, resume without redoing
  committed frames, re-queue on edit, delta on reopen, removal, scope, grouping, dedupe, yield,
  pause, migration never mixing specs, clear, size cap, find-similar, people corrections that
  persist, and face deletion leaving no faces.db.
- Tamper / extra-file / non-pinned-key refusal and the ceiling (`test_addon_pack.py`), Import's
  41 cases, the full `mv_tests` suite, a full Release build with `/W4 /WX`.

### Verified on Windows (2026-09-27)

- Audio: FFT and mel features match the Python reference (`tests/data/ai/audio_reference.json`),
  CLAP embeddings at cosine ~1.0 to it, and Whisper base and small both find all three
  sentences of the speech clip at their times (`test_ai_audio.cpp`); the engine's audio tracks
  with fake models (`test_ai_engine.cpp`).
- The real pack end to end (`ai-bench`, dev-signed, sideloaded with `ai-sideload.py`): CUDA,
  ViT-L; 301 COCO photos indexed, 0 failed (three greyscale JPEGs with a grey ICC failed until the
  colour stage learned grey profiles, plan/12 PR 7 row 5); queries ~32 ms; "dog", "a cat", "car",
  "a train" find their photos, "xyzzy plugh qwertyuiop" and "asdf" find nothing.
- `< 100 ms over 100 k frames`: the int8 scan of 100,000 ViT-L rows (2,000 clips x 50, with
  the per-asset stats) takes **31 ms** on one core here (`mv_ai_tests "[perf]"`, which fails
  over 100 ms in optimised builds); with the ~30 ms CUDA text tower a query is ~60 ms.
- The PR 1 soak with the whole pack (all four pieces) sideloaded: 0 dropped frames, p99 16.9 ms,
  in 5 of the last 7 runs of 60 s. The two failures were each one missed vsync (33.4 ms, no
  dropped frame); the same single miss occurred with no add-on at all (1 of 4 30 s runs) on
  this shared dev box. Before this session's loading fixes the pack run dropped two frames and
  lost frame-statistics continuity. Worst case, ViT-L indexing a 4K HEVC clip on CUDA in
  **another process that does not yield**: 0 dropped, p99 16.8 ms, one missed vsync.
- **PR 21's 1-hour 4K HEVC clip** (a synthetic NVENC test pattern with a soundtrack, so its
  dedupe is not real footage's): pictures, sound and speech indexed in **302 s** on CUDA with
  ViT-L and Whisper small - model load ~25 s, 255 picture moments ~190 s, sound ~70 s, speech
  ~17 s. The picture phase is bounded by the sampler's software 4K HEVC keyframe decode, not the
  model (hardware decode for the sampler is the noted later optimisation).
- `mv_tests` (641 cases), `mv_ai_tests` (29), Import (41), PR 16, broken-file and clean-VM
  suites, `test_addon_pack.py` (16), a full Release build with `/W4 /WX`.

### Verified on macOS (2026-09-27, Apple M5, 24 GB, macOS 26.6, ORT 1.30)

First build of the Mac half. It compiled after two fixes (an `id` parameter shadowing the ObjC
type; an unused pinned key under `-Werror` in a dev-key build). `cmake/darwin.cmake` now builds
`mv_ai_tests` and `ai-bench` like Windows, and defines no AI target at all on Intel (ORT ships no
x86_64 macOS build; the release's Intel leg would otherwise have tried to link an arm64 pack
against x64 dependencies).

**Core ML (PR 20 spike on the Mac).** The Xenova exports declare every input dimension dynamic.
Core ML compiles only static shapes, so with the shapes left free ORT placed **130 of 830**
(B/32) and **250 of 1,622** (L/14) nodes on it, in 13 and 25 partitions: no faster than CPU
(B/32 76 against 68 img/s, L/14 4.8 against 5.0) and B/32 drifted to cosine 0.990 from the
reference, at the self-test's edge. Pinning the image tower's free dimensions at open
(`session_options::fixed_dims`, batch 4 = the engine's photo batch; `embed_images` splits and
pads other batch sizes) puts **every node** on Core ML:

| Tower | CPU (2 threads) | Core ML, pinned | cosine to reference (Core ML) | first compile | cached open |
|---|---|---|---|---|---|
| ViT-B/32 fp16 | 73 img/s | **499 img/s** | 0.9992 | 82 s | 17 s |
| ViT-L/14 fp16 | 4.9 img/s | **31 img/s** | 0.9983 | 5.3 min | 64 s |

Text queries stay on CPU (3.3 ms B/32, 6.4 ms L/14). `MLComputeUnits=CPUAndGPU` compiled B/32
faster (67 s, 386 img/s) but L/14 had not finished after 25 minutes; `ALL` stays. Even from
its cache (`data/cache/coreml` in the pack's folder) an open takes 17 s / 64 s: ORT's converter
inlines the weights, so Core ML re-parses a 1 GB / 3.5 GB `model.mil` every time (it is not the
graph optimizer: the basic level inlines them too, and no optimisation loses the coverage). So
on the Mac the pack **answers on CPU at once** and a background thread opens Core ML, runs the
same self-test, and swaps it in (`upgrading_clip` in `pack.cpp`); the status shows CPU until
then. The cache is ~1.2 GB (B/32) / ~4.1 GB (L/14) of derived data outside the 3 GB installed-size
ceiling: Remove deletes `data/cache` even when it keeps the index (`store::remove`), and each
start prunes entries whose model is gone (ORT keys them by the model's path, so an older pack's).

**Audio stays on CPU on the Mac.** CLAP's audio tower does not compile on Core ML (unbounded
dimensions) and Whisper's **aborts the process** inside MPSGraph ("original module failed
verification"), which no fallback can catch; the decoder's growing KV cache cannot be pinned.
`pack.cpp` never gives the audio models Core ML.

**Fixed from the Mac runs (shared code):** Whisper base could stop after the first sentence of
a full 30 s window and the full-window seek then skipped everything said up to its last 5 s
(the §12b clip lost "landing in Lisbon"); the re-listen now applies to full windows too.

**Review fixes (Mac host and chrome, compile-checked, then run):** the add-on store was created
from two threads at launch; Remove and reload joined the pack's workers and deleted up to 3 GB
on the main thread; a slow result list could leave the folder model stuck in list mode;
re-selecting the current result reopened the clip; a failed or superseded search's results
could be shown for the next query; Return before results landed opened the previous query's
list; Sounds / Speech bits were sent with the piece absent; tiles re-animated and the grid
scrolled on every indexing re-run; blank tiles after a re-run; blocking pack calls in the
management view; pack calls after unload.

Tests on the Mac: `mv_tests` 513 (5 RAW-corpus skips), Import 41, `mv_ai_tests` with the real
pack: the CPU reference (both towers, CLAP), Whisper base and small on the speech clip, the
grey-ICC JPEG, the Core ML agreement cases (new), `test_addon_pack.py`, `test_macpack.py` (33).

Owed (both platforms unless stated): a quiet machine for the soak's single-miss question (the D6
quiet-machine item in CLAUDE.md), the 1-hour timing on real 4K footage, the 300,000-asset range,
the query time on CPU-only and Mac target hardware (the text tower dominates there),
Enter-lands-on-frame, the keyboard-only flow, HDR clip check, the minidump check, the index /
manifest fuzzers, a real camera-dump eval set (the z rule was calibrated on 300 COCO photos),
CLAP's own "nothing found" calibration, Core ML coverage and throughput, and every Mac build.

## Open decisions (owner)

1. ~~**D3D12 / DirectML.**~~ **Settled 2026-09-24:** vendor providers (OpenVINO, CUDA/TensorRT)
   as optional sub-packs, CPU always the fallback, a Settings toggle for Auto / provider /
   CPU-only. No DirectML, so no CLAUDE.md change. Remaining risks: NVIDIA redistribution
   licence vs the GPL, and AMD GPUs running CPU (see *Runtime*).
2. **Exact model** (SigLIP vs CLIP and multilingual support) — decided by the PR 20 spike + eval
   set, with the weights-licence gate above. **Settled 2026-09-25:** ViT-B/32 is only the floor;
   quality may use the full 3 GB optional-install budget.
3. ~~**Is this a D10?**~~ **Settled 2026-09-24: no.** It stays a plan/17 proposal plus the
   decision-log entry; it is not a numbered D-decision.

## Explicitly not in this feature

AI culling/"best photo" scoring (owner did not ask for it 2026-09-24; stays out), auto-tagging
into keywords, generative anything, cloud inference or cloud model calls, telemetry about queries,
a catalog/albums layer, and any inference in the base installer. A microphone, a recognizer,
and a speaking voice are the Voice add-on ([19](19-voice.md)), not model files in this pack.
