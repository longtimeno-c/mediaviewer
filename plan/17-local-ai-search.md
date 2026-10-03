# 17 — Local AI search (video moments and photos)

**Status: proposed 2026-09-24, amended 2026-09-25, 2026-09-27 (audio) and 2026-09-28 (Photos library source, macOS), post-v1. Not in PR 8. Milestone H, PRs 20–24 (renumbered 2026-09-24 from 21–25; it follows the Import add-on, PRs 16–19). Windows and macOS together (owner, 2026-09-24).**
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
  The battery pause has a session override (owner, 2026-09-27): **Index anyway** wherever
  "Paused on battery" shows (search footer, Settings, the command-bar pill's menu on Mac) sets `battery_override`, never saved; going back to AC or a restart ends
  it. The viewer's own yields are not overridable.
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
never exported (amended 2026-09-28: only by the user's own index export with "Include
People" ticked, off by default; see "Sharing an index"), no names sent anywhere. Face data is never used for anything but local search.
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

**Amended again 2026-09-27 (Mac run): the z threshold scales with the index.** `top10_z`
is the mean of the ten best assets in SDs, and for a query that matches nothing it grows with
the number of assets like the top ten of N noise draws: 2.20 at 300, 2.64 at 1,000, 3.35 at
10,000. The fixed 2.5 was calibrated at 300, so on 1,000 COCO photos **every** nonsense query
passed (z 2.56-2.88; 23 / 25 and 25 / 25 of a held-out nonsense list, B/32 / L/14) and a real
library would return results for gibberish. Now (`vector_store::scan_stats`): a query stands
out at z >= max(query_z, 1.15 x the noise expectation for its asset count); the margin alone
passes it at `query_margin` only when its best assets score at least like noise, else at 1.5 x
(one nonsense query's best margin was 0.050 at 300 photos). Held out (captions of 150 photos
in the index, 25 nonsense strings): at 1,000 photos B/32 finds 149 / 150 captions and 2 / 25
nonsense (was 150 and 23), L/14 146 / 150 and 7 / 25 (was 150 and 25); at 300, 146 and 2 /
144 and 5. The labelled queries keep P@5 >= 0.8 at both sizes (`mv_ai_tests "[eval]"`,
`"[.calibration]"`). L/14's remaining nonsense passes are the next calibration item, on the
owner's real set.

**Precision scale (2026-09-27, owner: "the model seems to confuse helicopter with plane … a
scale in Settings, default in the middle").** Settings -> Local search -> **Precision**, five
steps from Broader to Stricter; the middle (2, the default) is the rule above, unchanged row for
row (`"[ai][infer][vectors]"` proves it against the old code). The engine setting `precision`
(0-4, saved with compute and quality) is read by each search as it starts: no reload, no
re-index, and the chrome re-runs an open search. One function, `find_text` (`vectors.h`), is
the engine's picture and sound search and what the calibration runs. Per level it scales the
stand-out factor (the calibrated query_z with it), the query margin, the row margin and the row
z, and at 3-4 keeps only rows within Δ SDs of the best row:

| Level | stand-out x noise | query margin | row margin | row z | Δ (SD) |
|---|---|---|---|---|---|
| 0 Broadest | 1.10 | x 0.8 | x 0.33 | x 0.8 | - |
| 1 | 1.12 | x 0.9 | x 0.67 | x 0.9 | - |
| **2 (default)** | **1.15** | **x 1** | **x 1** | **x 1** | - |
| 3 | 1.23 | x 1.125 | x 1.5 | x 1.2 | 3 |
| 4 Strictest | 1.35 | x 1.25 | x 2 | x 1.4 | 2 |

"Nothing found" always reads the calibrated rows, so each stricter level answers a subset of
the looser one (checked on every query below). Measured on the COCO Karpathy-test set with the
engine's int8 store (held-out captions of photos 150-299, 25 nonsense strings; "helicopter" /
"a helicopter" rows, COCO has planes and no helicopters; category P / R: rows a caption keyword
calls relevant, over the nine queries a plane, a bus, a truck, a cat, a dog, a horse, a cow, a
sandwich, a pizza):

| Tower, photos | Level | Captions found | Nonsense found | "helicopter" rows | Category P | Category R |
|---|---|---|---|---|---|---|
| B/32, 300 | 0 / 1 / **2** / 3 / 4 | 147 / 147 / **145** / 141 / 131 of 150 | 5 / 4 / **2** / 0 / 0 | 20 / 17 / **15** / 9 / 0 | .39 / .46 / **.59** / .85 / .90 | .81 / .80 / **.73** / .69 / .49 |
| B/32, 1,000 | 0 / 1 / **2** / 3 / 4 | 150 / 150 / **150** / 145 / 137 | 7 / 5 / **2** / 1 / 0 | 67 / 47 / **36** / 22 / 0 | .36 / .46 / **.58** / .84 / .93 | .83 / .81 / **.78** / .71 / .43 |
| L/14, 300 | 0 / 1 / **2** / 3 / 4 | 147 / 146 / **144** / 139 / 136 | 10 / 6 / **4** / 1 / 0 | 19 / 14 / **10** / 0 / 0 | .40 / .51 / **.59** / .85 / .92 | .88 / .88 / **.85** / .69 / .46 |
| L/14, 1,000 | 0 / 1 / **2** / 3 / 4 | 147 / 147 / **146** / 143 / 140 | 10 / 9 / **7** / 2 / 1 | 65 / 44 / **27** / 0 / 0 | .42 / .51 / **.61** / .76 / .88 | .87 / .84 / **.81** / .70 / .50 |

What the numbers say. L/14 (High, what Core ML and CUDA machines run) tells "helicopter" from a
real one-word subject: its planes stand out at 1.19-1.21 x noise against 1.25-1.37 for "a dog"
and "dog", so level 3 says nothing found for it on both sizes. B/32 (Fast, CPU-only machines)
cannot: its "a helicopter" stands out at 1.33 x noise, exactly as its "a dog" does, with a
better margin (0.039 against 0.022). Level 3 therefore still shows B/32's planes (fewer), and
level 4, which rejects them, also says nothing found for "a dog" and "a sandwich" on B/32 (and
for "a dog" on L/14 at 300 photos). The B/32 gap at level 4 is thin (1.327 against 1.35). Level
0 finds at least what 2 finds and more rows (recall +0.03-0.08), at the price of 5-10 of 25
nonsense strings answered. Knowing that a plane is not a helicopter needs the query compared
with other words, not with the index; that is a later item (a vocabulary of labels embedded
once per model), not a threshold. CLAP scales its two margins the same way (no z rule yet);
spoken words need all the query's words at 3-4 and half at 0-1 (one word alone still whole).

**Amended 2026-09-28: a subject that fills the library ("mountain").** The owner saw
"mountain" return nothing while "mountains" returned 274 photos, and scenes "not showing any
more". Replayed with the engine's own `find_text` over a copy of the owner's index (503 assets,
L/14, Precision 2) and the pack's text tower: "mountain" had its ten best assets at z **1.75**,
below the **2.40** noise alone scores at 503 assets, so the 2026-09-27 rule asked 1.5 x the
margin (0.060) of a query that does not stand out, and its best margin was **0.059**; "mountains"
reached 0.070 and passed. The rule assumed a query below noise is nonsense, but a subject in
half the library (274 of 503 assets) cannot stand out from it: it *is* the mean (the "scan
stats" test had said so since the z rule was written; the noise scaling made it bite). The two
words rank almost the same rows (Jaccard 0.93 of the rows over the result margin); the gate
alone split them. Now a query is believable at the calibrated margin when its rows clear that
margin on at least **max(5, 1 % of the assets)** assets (`scan_stats::over_margin`): on the
owner's index 12 nonsense strings had 0-3 such assets (0.6 %) and 11 subjects it seems to lack
("dog", "beach", "a cat", ...) 0, while "mountain" had 34, "mountains" 78, "lake" 16, "sky" 10,
"car" 6. Before / after on that index: "mountain" 0 -> 260 assets, "lakes" 0 -> 48, "sky" 0 ->
113, "car" 0 -> 28; the 25 others (with "cars" at 1 and "snowboarding" at 3) still find
nothing. The gap is thin (3 against 5) and measured on one 503-asset
library: the 1 % is a guess for large ones, and the COCO held-out `[.calibration]` has not been
re-run with it; that is the next calibration item. As well, a picture query's last noun is embedded
in **both numbers and averaged** (`query::number_forms`, one batched run of the text tower,
~5 % over one text), so "mountain" and "mountains" are the same query (277 assets either way).
CLAP and the speech words keep the words as typed.

**Amended 2026-09-28 (issue #85): "nothing found" at scale, and a label vocabulary.** The rule
above was set on 300-1,000 photos. On the owner's 23 k-asset Photos library (L/14) `xyzzy plugh
qwertyuiop` returned 676 results. Measured at library sizes with `mv_ai_tests "[.calibration]"`
(extended for it): COCO 2017 (val2017 then 20,000 of train2017 by id; 24,992 decode), both
towers on Core ML, 500 held-out captions, 25 tuning and 60 held-out nonsense strings, the
category and labelled queries, every Precision level at 1 k / 5 k / 10 k / 25 k. What it showed:
a real library's best scores have a heavier tail than noise, so at 25 k gibberish sits at
0.9-1.35 x noise's top-ten z and real subjects at 1.15-1.8. Neither z nor the margin separates
them alone, and both drift up with the library. Then every row above mean + 2 SD is a result, a
fixed slice of any big library, so a query that passes returns hundreds (median 735 at 25 k).
Many "nonsense" strings are not nonsense to CLIP: `qwerty uiop` and `xyzzy plugh qwertyuiop` rank
first of 705 labels on keyboards, and `314159` finds photos with numbers.

Now (`vectors.h`, `vocabulary.h`):

- **The gate needs z and margin together.** A query passes on its best margin alone at
  `query_margin` x (1.375 + 0.125 L), or when it stands out (z >= max(query_z, 1.18 x noise))
  with a best margin of `query_margin` x 0.375 (1 + L), where L = log10(assets / 1,000), from 100
  assets up. The broad-subject path ("mountain", above) is unchanged. Fitted on both towers at
  once (both ship `query_margin` 0.04), with every category and labelled query kept at every
  size.
- **A label vocabulary filters rows.** 705 everyday labels (people, animals, vehicles, places,
  scenes, objects, food, events, text and screens), deliberately not COCO's, are embedded once
  per tower and kept in `labels.f32` beside the index. Each row stores the score of its ninth-best
  label; a row is a result only when the query scores at least that. Gibberish loses to the
  labels on most of the photos it lands on; "a dog" beats every label but a few near-synonyms
  ("dog playing") on a dog. This is the vocabulary this plan named after the Precision scale.
- **Precision levels** keep their factors; their stand-out steps move up with the calibrated one
  (1.13 / 1.15 / **1.18** / 1.26 / 1.38).

Level 2, before -> after (nonsense: tuning + held-out passing, of 85; its rows: median when it
passes; category P / R over the nine category queries; "helicopter": rows for "helicopter" /
"a helicopter", COCO has none):

| Tower, photos | Captions found / own photo (of 500) | Nonsense passing | Nonsense rows | Category P / R | Labelled P@5 | "helicopter" |
|---|---|---|---|---|---|---|
| B/32, 1 k | 492 / 486 -> 475 / 460 | 15 -> 5 | 33 -> 10 | .51 / .77 -> .72 / .69 | .89 -> .89 | 33 / 32 -> 8 / 9 |
| B/32, 5 k | 493 / 485 -> 476 / 459 | 22 -> 8 | 144 -> 59 | .53 / .80 -> .74 / .73 | 1.0 -> 1.0 | 164 / 180 -> 35 / 42 |
| B/32, 10 k | 495 / 486 -> 482 / 466 | 24 -> 9 | 300 -> 91 | .54 / .79 -> .74 / .73 | .91 -> .91 | 334 / 357 -> 68 / 84 |
| B/32, 25 k | 497 / 487 -> 489 / 470 | 26 -> 9 | 736 -> 196 | .55 / .80 -> .74 / .72 | .94 -> .94 | 851 / 857 -> 181 / 219 |
| L/14, 1 k | 491 / 489 -> 478 / 474 | 25 -> 11 | 30 -> 57 | .55 / .77 -> .85 / .71 | .94 -> .97 | 0 / 0 -> 0 / 0 |
| L/14, 5 k | 496 / 490 -> 486 / 480 | 39 -> 15 | 161 -> 69 | .59 / .82 -> .87 / .75 | 1.0 -> 1.0 | 0 / 0 -> 0 / 0 |
| L/14, 10 k | 496 / 490 -> 486 / 479 | 52 -> 22 | 314 -> 122 | .59 / .81 -> .87 / .75 | 1.0 -> 1.0 | 282 / 270 -> 7 / 6 |
| L/14, 25 k | 498 / 490 -> 489 / 483 | 63 -> 23 | 735 -> 221 | .60 / .81 -> .87 / .75 | 1.0 -> 1.0 | 728 / 701 -> 38 / 47 |

The labelled queries keep P@5 >= 0.8 at every size on both towers. The held-out nonsense list
moves like the tuning one (L/14 at 25 k: 48 -> 17 of 60, 15 -> 6 of 25), so the gate is not
fitted to the strings it was tuned on. **Target recorded:** at 25 k assets and the default
Precision, at most 1 in 3 nonsense strings answered on L/14 and 1 in 8 on B/32; the fitted
rule gives 27 % and 11 %.

What it costs and what is still open:

- **Recall.** A caption finds its own photo 94-97 % of the time (97-98 % before). Category recall
  falls .05-.08 while precision rises .19-.28. B/32 at 1 k loses the most (486 -> 460 own
  photos).
- **Floods remain on L/14.** Placeholder and long-sentence gibberish (`lorem ipsum`, `colorless
  green ideas sleep furiously`) and `xyzzy plugh qwertyuiop` still answer at 25 k with 1,400-5,000
  rows. They pass on margin alone, and L/14 ranks them above almost every label on those photos.
  A stricter label bar (top 2 of 705) still leaves hundreds of rows at half the recall, and
  text-like labels ("gibberish", "random letters") change nothing. A result cap relative to the
  query's best rows is the next candidate. The owner's own string drops from 2,048 to 1,445
  rows on the COCO set; the Photos library was not re-run.
- **Cost.** The scan is unchanged (100 k x 768 rows: 1.66 ms before, 1.73 ms with the filter, one
  compare a row). Embedding the labels takes ~4 s (B/32) / ~6 s (L/14) once per tower, cached.
  The bars take 1.2 s per 100 k rows at load, filled in chunks behind the searches; until then a
  row passes. +4 bytes a row.
- The P@5 misses in `"[eval]"` on this set ("a train at the station" B/32, "a dog" L/14) are
  the raw ranking, the same before and after; that test's labels are the Karpathy set's.
- The subset check across levels fails only where a looser level hits `find_text`'s 5,000-row
  cap (9 checks, all L/14 nonsense; 14 before).

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
- **Results in the gallery**: base ABI 0.14 `mv_folder_open_list` - the same gallery, filmstrip,
  keys and thumbnails over a result list; a clip opens paused on its moment.
- **Commands** (plan/16): `Ctrl+F` search, `Ctrl+Shift+F` find similar, `N` / `Shift+N` next /
  previous matching moment; listed only while the pack is loaded.
- **Chrome**: WinUI `MediaViewer.Ai.Chrome` and SwiftUI `AI.bundle` - search panel, results,
  status pill, scrub-bar match dots, Settings -> Local search with per-piece install (clicks
  queue, Core first; "Install all"), compute / precision (the model stays on Auto), roots, People.

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

### Query syntax (2026-09-28)

Owner: "Trist" found nothing until the whole name was typed, and he wanted people, words said
and a description in one query (`Tristan:"hello"`). One parser in the pack
(`src/addons/ai/query.*`, `mv_ai_tests "[query]"`), so both chromes and the Voice add-on
([19](19-voice.md)) mean the same thing by the same words. Terms narrow each other (AND).

| Typed | Means |
|---|---|
| `Tristan beach` | photos / moments with Tristan, ranked by "beach" (no "nothing found" test inside a person's own) |
| `Tristan "hello"`, `Tristan:"hello"`, `said:hello` | Tristan, and a clip whose transcript has those words in that order; the line is the snippet |
| `Tristan Aaryan`, `Tristan and Aaryan` | both in the same photo or clip (before 2026-09-28 "and" meant either) |
| `Tristan or Aaryan` | either |
| `Anna Smith`, `anna` | a named person: the whole name or the first name, any case, accents folded |
| `@tri`, `person:"anna s"` | a person by the start of a name, or a near spelling; nobody by that name: nothing found |
| `Trist`, `tristna` (alone) | a lone word that starts or nearly spells a name: those people first, then what the word describes (so "car" still finds cars when there is a Carla) |
| `-Nico`, `-beach`, `-"goodbye"`, `-video` | leave out a person, what a picture search finds, a phrase said, a kind |
| `beach video`, `is:photo`, `videos of Anna` | a kind: the last word, `is:` / `type:`, or a leading "videos of"; "photos of …" asks for anything (a spoken request) |
| `in:2024`, `in:2024-06`, `before:2025`, `after:2023-05`, `since:2024-03-01`, `until:2024` | the **file's** date (modification time, UTC). Capture dates (EXIF) are not in the index; a camera dump copied without its times will not match |
| `file:IMG_12`, `file:"trip 2024"`, `-file:copy` | the file's **name** (not its folder) contains the text, case and accents folded (2026-09-28, owner: file search "as a keyword like file:[filename]" when Local search is installed). Indexed files only: a folder not yet indexed has nothing to match, and the base app's file search (plan/16) needs no index |
| `video in:2024`, `file:IMG_12` | filters alone: everything they allow, newest first |

Typing a name: the panel names people for the word being typed (`suggest_json`, appended to
`mv.ai.1`; prefix first, then a near spelling of one letter, two from eight letters); **Tab** or
a click completes the word. Not in the index, so not in the syntax: OCR (quoted words search
speech only until an OCR piece exists), places, EXIF capture dates, camera. A word that is not a
name, a kind or an operator stays in the description; an unfinished `in:20` is ignored, not
searched. Per keystroke the parse is linear in the query and one `SELECT` of the named people.

### Gallery search bar (2026-09-27)

Removed 2026-09-28 at the owner's request; replaced by a search icon in the path bar (it opens
the `Ctrl+F` / `⌘F` panel; plan/12 2026-09-28). Later the same day the icon became always
visible: without the pack it and `Ctrl+F` open the base app's file search, the old bar's Names
mode shown on request (plan/16 "File search").

### People refinement (2026-09-28, owner report)

Owner: "a few cases where people are clearly allocated under the wrong person"; asked for a final
check that re-looks at the face thumbnails, in a few passes. Shared core only (`src/addons/ai`,
`src/infer`); no ABI or chrome change, so no host half on either platform.

**Why faces landed under the wrong person.** Measured against the code, not guessed:

1. **Wrong units.** `same_person` (0.40, just above SFace's 0.363 verification threshold) is a
   *pairwise* cosine, but a face was scored against the *normalised* centroid of the cluster.
   Face · centroid/|centroid| equals the mean pairwise cosine divided by |mean|, which is about
   √ρ for a person whose faces agree at ρ; for ρ ≈ 0.5 that inflates a stranger ~1.4×. A
   lookalike at 0.30 pairwise (below verification) read 0.42 and joined.
2. **Online, first come.** Each face joined the best cluster at arrival, with no second look: no
   margin over the runner-up, the centroid drifting with every face it took in, the order of
   indexing deciding.
3. **The idle merge chained.** `consolidate(0.55)` compared normalised centroids (so ~0.28 mean
   pairwise across two clusters merged) and grew the survivor as it went, so A took B, then the
   A+B centroid took C. It could also glue an unnamed cluster onto a named person.
4. **Every face counted the same.** A blurred, tiny or profile crop (YuNet 0.8, 4 % of the short
   side) has a noisy vector near "a face in general"; it joined, and pulled the centroid.
5. **The user's own work was not an anchor.** Naming, merging and splitting changed rows but left
   nothing that later clustering had to respect, other than `rejected` and `no_merge`.

**What changed in the online path** (`faces.cpp`): scores are mean pairwise cosine
(sum · v / n) everywhere (assign, "this person", consolidate); a new face with a second person
within 0.03 of the best waits **unassigned** (`person_id` NULL) rather than guessing;
consolidate merges at mean pairwise **0.42**, an average that stays an average after a merge (no
walk). The user's faces are **pinned**: the faces they split out; the cover of a person when they
name it and of both sides when they merge (the faces they were looking at); on upgrade, the cover
of every named person. Covers now prefer pinned faces, so the face shown is the anchor.

**The refinement** (`face_refine.h/.cpp`, pure; `faces_db::refine_begin / refine_commit`;
`engine::person_refine`). Per person: *refs* = pinned + good-quality members (≤ 256);
*anchors* = the pinned faces plus the refs' medoid (for a person with pins, the medoid only if it
is within 0.40 of a pin, so an impostor majority cannot define a named person); *core* = refs
within 0.40 of an anchor (one hop, no chain); *exemplars* = pinned then core, ≤ 12. A face's
**support** by a person is the mean of its top 3 cosines to that person's exemplars (not itself),
in pairwise units. Candidates are shortlisted by the core mean (sum · v / n), top 3 re-scored by
exemplars. Then, for every face that is not pinned:

| Verdict | Rule |
|---|---|
| **move** to b | support(b) ≥ join (0.40) and beats both its own person and the runner-up by margin 0.08; never a weak face; never into a person it was rejected from |
| **evict** to unassigned | support(own) < keep 0.30 (0.34 for a weak face) |
| **admit** (unassigned) | the same test as move |
| **regroup** | unassigned good faces within 0.40 of a leader and at 0.40 mean to the group so far (average linkage), ≥ 2, become a new unnamed person |

Passes are Jacobi (a pass judges every face against the same prototypes, so order never
matters), up to 4, until nothing changes; a face changes at most once per call, and keep < join
is the hysteresis that stops a face flapping between calls. A pass after the first rebuilds and
re-judges only the persons the previous one changed. Weak = quality < 0.35, where quality =
min(size, sharpness, frontal) × (0.6 + 0.4 × detector score): size ramps 40 → 112 px (SFace's
input), sharpness is the Laplacian variance of the aligned crop's luma on a log ramp 20 → 300,
frontal is the nose's offset from the eye midline against half the eye distance. Rows from before
this change have no quality; they get a proxy from score and box size.

**Why these numbers.** join = core = the model's `same_person` (0.40): moving a face should need
the same evidence that let it join. keep 0.30 sits below SFace's 0.363, so a face is evicted only
when even its three nearest housemates do not vouch for it; 0.34 for a weak face because its
vector is noisier and a doubt should cost it more. The margin 0.08 is about the spread of one
SFace face's cosines to a person's faces; below that, "closer" is noise (the synthetic
ambiguous case sits at ~0.10 with one noisy face, and at ~0 when truly halfway). All of this is
defaults in `refine_params`; none has been fitted to a labelled face set yet (see Unverified).

**Re-looking at the thumbnails.** The existing SFace model at higher quality, not a new model:
**flip averaging** (the ArcFace template is mirror-symmetric, so the mirrored 112 × 112 crop is
aligned too; the embedding is the normalised mean of both; one more ~1 ms embedder run per
face). New analyses record it (`tta`); borderline faces of stills from before it (support below
join, or a rival within the margin) are re-analysed, 16 assets per call and once per asset per
session, and the new vectors replace the old box by box (IoU ≥ 0.5 keeps the row's id, person
and pin). The spec key is unchanged: a flip-averaged vector is the same model in the same space,
and bumping the key would re-detect the whole library. A second face model (e.g. an ArcFace
R100) would need a licence check first: most public ArcFace/InsightFace weights are
non-commercial and fail `ai-models.py check`, and it would add ~100-250 MB to the People piece.
Not proposed until the numbers below say SFace + refinement is not enough.

**When it runs** (owner, 2026-09-28: plan/12). **Only when the user asks:** "Refine faces" in
the person's sheet (Mac) or detail pane (Windows) under Settings → People calls `mv.ai.1`'s
`person_refine` on a worker. Nothing refines in the background; the idle consolidate merge is
unchanged. The call takes a full snapshot (every person is a candidate, rebuilt from its faces)
and judges **only that person's faces** (`refine_input::focus`): a face may move to someone
else, leave to unassigned, or, with others that left together, become a new unnamed person.
Other people's faces and unassigned faces are never moved, and nothing is admitted into the
person. The snapshot and the commit hold the People lock, the compute holds nothing and stops
on shutdown. The commit skips any face the user (or a scan) changed after the snapshot: the
user wins. The chrome reports how many faces left. The incremental path (cached prototypes,
`refine_begin(false)`) stays in faces.db and its tests but no caller uses it now.

**Cost** (Apple M5, one thread, `-O2`, synthetic 128-d SFace-like vectors, Zipf-sized people):

| Library | Full call | Incremental (one person + 20 new faces) |
|---|---|---|
| 5 k faces / 100 people | 34 ms | 1.1 ms |
| 20 k / 300 | 135 ms | 4.1 ms |
| 100 k / 1,500 | 2.7 s | 58 ms |

The full call is O(faces × people × 128) for the shortlist; memory is the snapshot's vectors
(512 B a face: ~50 MB at 100 k, freed after). On that synthetic set with 5 % of faces misfiled,
every misfiled face was either moved to the right person (~65 %) or unfiled (~35 %, mostly faces
whose true person had one face), none stayed wrong, and 6 of ~19 k correct faces were unfiled.

**Tests.** `tests/test_ai_face_refine.cpp` (`[refine]`, in `mv_ai_tests`, no pack needed):
chaining (a glued-on person comes back out as one new person), outlier and clear-mistake moves,
pinned anchors defining a named person against an impostor majority, rejection and weak faces,
an ambiguous face staying, a focused call moving only its person's faces, incremental calls
against cached prototypes, convergence (a second
call moves nothing), quality measures; and through faces.db: a library clustered with the old
looseness comes apart with the named person keeping its pinned cover, a user split made between
snapshot and commit wins, a re-analysis replaces a vector in place.

**Unverified.** Built with CMake on neither platform (the build trees were wiped): the pure
logic, faces.cpp and the tests were compiled standalone with Apple clang 21 (`-Wall -Wextra
-Wconversion -Wshadow`, ASan + UBSan) against the system SQLite and a minimal Catch2 stand-in,
passing on 100 random seeds; engine.cpp, pack.cpp and models.cpp were only checked with
`-fsyntax-only`. Not run: `mv_ai_tests` under MSVC `/W4 /WX`, the engine people case with the
refinement live, the real SFace pack (so flip averaging's gain and the thresholds are untested on
real faces), a labelled people set (none exists; it is what should tune join / keep / margin),
and PR 1's present-loop gate on either platform while a full call runs.

### Merge duplicates (2026-10-03, owner)

Owner: "a rescan all option that goes through and does A/B testing on images and tries to merge
them into the right person … I have a lot of duplicates." Shared core, `mv.ai.1` appended
(`people_dedupe`), a button in both chromes. Only ever on request (plan/12, 2026-09-28: nothing
refines People in the background; the idle consolidate merge is unchanged).

**What a click does** (`engine::people_dedupe`):

1. **Every face re-checked.** The refinement above, with no focus: a full snapshot, every
   person rebuilt, every unpinned face judged (move, evict, admit, regroup), Jacobi passes,
   then `refine_commit` (the user's later changes win, as before).
2. **The same person twice** (`find_duplicates` in `face_refine.h`, pure, no lock held). Over
   the rebuilt prototypes: two people link when each one's exemplars **vouch** for the other's
   at `join` (the model's `same_person`, 0.40), vouching being the refinement's own support
   (each exemplar's top-3 mean cosine to the other's exemplars), averaged, both ways, the
   smaller kept. Links are grouped strongest first by **complete linkage**: a group takes a
   person only when every member links to it, so a chain of lookalikes cannot walk (the lesson
   of the old consolidate). Exemplars are pinned and good-quality faces, so weak crops that
   drag a cluster's average down do not hide a duplicate: this is what the idle consolidate,
   which compares whole-cluster averages at 0.42, misses. Never linked: two people **named
   differently**, a pair a **split** kept apart (`no_merge`), a pair where a face of one was
   **rejected** from the other. Cores far apart (mean pairwise below 0.24) are not scored.
3. **Who survives.** In a group: a named person over an unnamed one, then the one with more
   faces, then the older; the others merge into it (`faces_db::merge_auto`, which checks the
   pair again). Nothing is pinned: the user did not say "same person".

The call returns people merged away and faces moved; the chrome says "3 people merged, 12 faces
moved." or "No duplicates found, and every face matches." Undo is Split (a split pair never
merges again on its own). Cost is the full refinement's (above) plus pairs × exemplars²:
negligible beside it at hundreds of people. `mv_ai_tests "[dedupe]"` covers it on synthetic
SFace-like vectors (a duplicate the cluster average misses, strangers, a lookalike chain, names,
apart pairs, survivors) and through faces.db and the engine (a split stays apart, a second call
changes nothing).

### People in the open folder (2026-09-28, owner)

Owner: "in people I can see everyone in any folder I open; can I just show people from the
folder / subfolders I'm on?" Both halves, one shared core change.

- **mv.ai.1 appends `people_in_json(scope_dir, scope, out, cap, needed)`** [worker-thread]:
  `people_json` narrowed to the people with a face in a photo or clip under `scope_dir` by
  `mv_ai_scope`, the same three scopes `search_text` takes. `faces` counts their faces there and
  `cover_*` is the clearest of those (pinned first, then score × area), so a card's count and
  face are the folder's. A person with nobody there is left out. *Who qualifies at all* (the
  minimum faces, or a name) is still judged over the whole index: a folder never shows a
  two-face cluster Everywhere would hide, and never hides a named person who is there once.
  NULL / `MV_AI_SCOPE_ALL` is `people_json`. `faces_db::people(min, assets)` does the work:
  one indexed pass per person over its faces against the scope's asset set, the set built as a
  search builds it (`scope_assets`).
- **Chrome:** a "People in · This folder | + Subfolders | Everywhere · <folder>" control at the
  top of the grid (Settings → People on the Mac, the People window on Windows), the search
  panel's words. Default **+ Subfolders**; it follows the folder the viewer opens (the host's
  `folderChanged` / `OnFolderChanged`); no folder open reads as Everywhere with the control
  disabled. "Show photos" on a card searches the same scope, so the list is the photos the card
  counted. Everywhere is exactly what the grid showed before.
- **Not persisted:** the choice is per window / per Settings session; reopening starts at
  + Subfolders. The status bar's people count (`mv_ai_status.people`) stays the whole index's.
- A pack from before the entry (its `struct_size` stops short) shows everyone, as before.

### Sharing an index (2026-09-28, owner)

Owner: indexing a library once and carrying it to another machine is "much easier than
re-encoding it every time, especially when it comes to a NAS", with toggles for face data and
thumbnails, export and import on every platform the pack runs on. Shared core
(`src/addons/ai/transfer.*`), `mv.ai.1` appended, a host-table entry pair, and both chromes.

**The file** (`.mvindex`, SQLite, `info.format = "mediaviewer.index"`, version 1) holds the chosen
roots with each asset's path **relative to its root** (`/`-separated), its `(mtime, size)`,
kind and duration, and the rows that describe it: `progress` (done and partial only),
`frames`, `speech`. Optional: **People** (`people`, `faces` without a path, `rejected`,
`no_merge`, `face_scanned`) and **thumbnails** (the JPEG-512 cache's bytes for a still and each
stored moment; only what the cache already holds; an export never decodes a library). The
roots keep their original absolute path and a display name so an import can offer the same
place (a NAS mounted at the same path answers itself). Written to `<dest>.part` and renamed.
The export reads index.db and faces.db on its own read connections: indexing is never paused
by an export.

**Import** maps each root in the file to a folder on this machine (the chrome shows every root
with its original path, pre-filled when that folder exists here). Per asset at
`folder + rel`:

| Here | Does |
|---|---|
| no row | the row and its vectors are added (`seen` 0) |
| same `(mtime, size)`, already done for a spec | this machine's rows for that spec stay |
| same `(mtime, size)`, not done | replaced by the file's |
| different `(mtime, size)` | this machine's row stays (it saw the file; the file did not) |

Then the imported roots are **rescanned by the ordinary delta**: a file whose `(mtime, size)`
differs from the file's row loses its imported vectors and is queued, exactly as an edited file
is (no content hash, plan/17 "Index store"). A root that is offline keeps its rows until it is
reachable, as today. Indexing pauses for the import (it runs on the control thread; workers
wait as for Clear) and the search matrix, sounds, transcripts and People reload after it.

**Models.** Vectors from one tower are never mixed with another's (PR 23). Picture rows are
imported for a spec this pack is using (the active or the building one). When this machine's
index is empty and the file's tower is one the pack carries, the import **adopts it**: Quality
is set to that tower and it becomes the active spec, so nothing is re-embedded (on a CPU-only
machine an imported High index keeps answering with the High text tower, ~10 ms more a query).
Otherwise the picture rows are skipped and the chrome says which Quality would use them before
the import runs (`inspect_export`'s `picture_usable`). Sound and speech rows are imported for
the spec of the piece loaded here; none loaded, they are skipped and counted.

**People** (biometric; plan/17 PR 24 "never exported" amended): off by default on both sides,
ticked by the user, with the warning that the file then identifies the people in it. An import
with People needs the ai-faces piece and turns the People opt-in on (the checkbox says so). A
named person joins the local person of the same name (case-insensitive), else becomes a new
one; unnamed clusters come in as new people and the idle merge treats them as any other.
Assets whose faces this machine has already scanned keep this machine's faces.

**Thumbnails** go through two host-table entries appended to v2, `thumbnail_jpeg` (look up the
cached JPEG-512 of a still or moment as bytes, never make one) and `thumbnail_store_jpeg` (store
bytes under this machine's file stamp). The host checks the bytes are a baseline JPEG no larger
than 512 on the long edge before it stores them (a shared file is untrusted input); the pack
stores a thumbnail only when the local file's `(mtime, size)` equals the file's row.

**What it is not.** Not a sync: an import is a one-off merge, and nothing watches the file. Not
a way onto an Intel Mac: the pack still has no x86_64 macOS build (ORT), and a query needs the
text tower on the machine that searches. Not for the Photos library root (below): its keys are
this Mac's PhotoKit identifiers, not paths under a folder another machine could map, so an
export leaves it out and the chrome's picker does not offer it.

**Rule 6.** The file carries folder and file names, and, if ticked, faces and names. It is the
user's own export to a place they chose; nothing about it is logged or sent.

## Photos library source (macOS, issue #72, 2026-09-28)

**Status: built on branch `mac-photos-library-source` (PR #86). The owner settled its calls on
2026-09-28** (plan/12): Mac-only is fine; the base-app key and entitlement are accepted; results
open in place and are never written; an iCloud original is downloaded only when viewed, and
cleared after. The system Photos library (where iCloud
Photos lives on a Mac) is one more remembered root beside folder roots, feeding the same
indexer, index and search. Mac-only under D9: the source is behind a portable interface
(`src/addons/ai/photos_source.h`; PhotoKit in `photos_mac.mm`, `photos_none.cpp` elsewhere). The
index and search are shared. Windows has no PhotoKit, and iCloud for Windows syncs to a folder,
which a folder root already covers.

**Source, not a folder.** A Photos asset has no path. It is keyed `photos:<localIdentifier>`
under one root whose path is `photos:`. The key scheme is the source, so there is no schema
change. `(mtime, size)` is `(modificationDate, pixel count)`: an edit in Photos re-queues the
asset, like an edited file. The scan is PhotoKit's enumeration of the user's library: iCloud
Photos included, shared albums and the Hidden album excluded, one row per burst and per Live
Photo (its still). A `PHPhotoLibraryChangeObserver` rescans, at most once per 10 s (an iCloud
sync arrives as a burst). The full re-enumeration is the delta. Persistent change tokens are
not used because the measured cost does not need them (below).

**Local only, read-only.** Every PhotoKit request has network access off. Nothing is written to
the library. A still is embedded from PhotoKit's local rendition at the indexer's size (the
edit as Photos shows it, colour-matched to sRGB by CoreGraphics, D6). A clip is sampled from
its local file, which `requestAVAsset` hands over. The host's sampler and audio reader open it
read-only. An asset only iCloud has is **unavailable** (progress state 4), and is neither done
nor failed:

- `mv_ai_status.assets_unavailable` counts these assets; Settings says "N only in iCloud".
- An iCloud-only clip is found by its local poster, stored as one row at 0 ms.
- Once per launch, and when access returns, each is asked whether it is on this Mac now (a
  PhotoKit lookup per asset; a still that is still in iCloud answers without a decode). Only
  those that are go back to pending; the rest keep their state and their poster row, so
  nothing is re-read or re-embedded for an asset that has not changed. They are not asked on
  every change notice, which would ask about every iCloud-only asset each time a photo is
  favourited. The People pass treats them the same way: marked scanned while unavailable, and
  scanned once the original is here.

**Permission.** The pack never raises the system prompt. Settings → Local search → *Photos
Library* → **Add Photos Library** asks, from that click, and only then calls
`index_photos_library`. Until then the source answers `permission_denied` without touching
PhotoKit's library, because any fetch would itself raise the prompt. When access is off
(denied, restricted, or revoked later), the rows stay searchable, like a drive that is not
plugged in, and the root is left out of the work. Settings then says so and offers *Open
Privacy Settings*.

- The base app gains `NSPhotoLibraryUsageDescription` in its Info.plist.
- It also gains the hardened-runtime entitlement
  `com.apple.security.personal-information.photos-library`
  (`packaging/macos/MediaViewer.entitlements`).
- Without these, macOS kills an app that asks, and denies one that runs hardened. They cannot
  live in the pack. This is a change to the base bundle with the pack absent: see plan/12.

**Search and results.** `scope_dir "photos:"` scopes a search to the library: the search
panel's *Look in* row gains a **Photos** segment once the library is indexed. *Everywhere*
includes the library.

- A Photos still's tile is the key itself. The chrome draws it from PhotoKit's own cached
  rendition, so there is no second thumbnail cache.
- A result shows a small Photos badge.

**Opening a result (owner, 2026-09-28: "without writing").** The viewer reads each result where
Photos keeps it: the current rendition's file (`requestContentEditingInput` / `requestAVAsset`).
There are no copies. The host refuses every write to it (`src/shell/write_guard.h`). Any path
inside a `*.photoslibrary` bundle is protected, and so is any file the list registered:

- **Refused:** Move to Trash, Move To, ratings and every metadata edit (the pane's edit
  controls show disabled), rotate / flip, Save copy / export, and trims / clip exports (they
  would write beside the source).
- **Allowed:** Copy To and drag-out, because both copy.

A refusal beeps and says "From your Photos library: read-only here."

**An original only iCloud has** (Optimize Mac Storage: most, on the owner's library) opens at
once as Photos' best local picture, `<name> (preview).jpg`, in
`~/Library/Caches/MediaViewer/Photos Library/`. If the user actually stays on it (400 ms), the
original comes from iCloud (owner: "when viewing but … cleared after"). It is the only network
request, it is made on the user's own viewing, and it replaces the preview in place. That is
the viewer's usual first-picture-then-full-resolution refinement.

- **Cleared:** downloads are deleted when the next list opens and at quit; the whole folder is
  emptied when the chrome attaches.
- **Mapping back:** scrub markers, N / Shift+N and Find Similar on an opened Photos result ask
  about the asset, not the file.
- **The index** still never downloads.

### Measured (2026-09-28, Apple M5, macOS 26.6, the owner's library: 23,089 assets, Optimize Mac Storage on)

`tools/ai/photos-spike.sh` (the source alone, no model) and `tools/ai-bench/photos-bench.sh`
(the whole pack):

| What | Result |
|---|---|
| Enumeration (the scan and the delta) | 0.92 s cold, 0.43 s warm (0.53 / 0.23 s on a second run) |
| Local still at the indexer's 448 px, 2 threads | 304–411 per s, p50 4.3–5.7 ms, p95 6.4–10.5 ms |
| Stills with a local rendition | 979 of 1,000 sampled (2.1 % iCloud-only) |
| Clips with a local file | 0 of 20 sampled: with Optimize Mac Storage, clips are posters only |
| Originals on this Mac (open in place at once) | 26 of 200 sampled (13 %); the rest open as previews, the original fetched on view |
| Opening: resolve the file, per asset (the spike also clones; the app no longer does) | p50 7.0 ms, p95 9.6 ms |
| Peak footprint of the source alone | 70 MB (stills), 212 MB (with opening) |

PhotoKit is not the bottleneck: the image tower is (ViT-L/14 31 img/s, B/32 499 img/s on
Core ML, above).

**The whole pack over the library** (`photos-bench.sh`, dev-signed sideload, Auto: ViT-L/14,
on CPU for the first ~30 min while Core ML compiled for a new pack folder, and stopped ~10 min
for the pacing runs below). At the 1-hour cap:

- **Pictures: every asset handled.** 19,007 were indexed, 4,082 were iCloud-only (their clips
  searchable by the poster), and 0 failed. There were 26,215 searchable rows, because the local
  clips were sampled; the 20-clip sample above had missed them.
- **Audio:** the track had started (347 of 3,826 clips).
- **Speed:** about 23 assets/s end to end on Core ML.
- **Queries:** 17–30 ms. The first took 1.7 s: the text tower warming up.
- **"Nothing found" fails at this size:** `xyzzy plugh qwertyuiop` returned 676 results, and the
  real queries returned 74–733. This is the L/14 calibration item above, now on a real
  23 k-asset library. It needs the owner's labelled set to retune, and is not a source
  problem: owed before the Photos source ships. Tracked as a follow-up that needs a labelled
  eval: [#85](https://github.com/longtimeno-c/mediaviewer/issues/85). Recalibrated on COCO at 1 k-25 k (above, "Amended 2026-09-28
  (issue #85)"); the Photos library itself has not been re-run.

**Present loop while the library indexes (worst case, 2026-09-28).** `frametime --seconds 60`
(Mac PR 1 gate) was run while `photos-bench.sh` indexed the library in **another process that
never yields**, with ViT-L/14 on CPU (Core ML was still compiling) on every core. It was
alternated with runs in which the bench process was stopped (SIGSTOP). Runs made while the
display was off (12:54–13:02, per `pmset -g log`) are void.

| Bench | Runs | Frames | Dropped | p99 | Idle, % of one core |
|---|---|---|---|---|---|
| Stopped | 2 | 3,600 / 3,600 | 0 / 0 | 16.85 / 16.85 ms | 1.66 / 0.97 |
| Indexing | 4 | 3,600 / 3,600 / 3,598 / 3,600 | 0 / 0 / **1** / 0 | 16.9 / 16.9 / 16.95 / 17.1 ms | 1.04 / 0.86 / 0.28 / 0.39 |

What the numbers say:

- **Dropped frames:** one missed refresh (a single 33.3 ms frame) in one of the four indexing
  runs. This is the same single-miss pattern Windows logged with and without the pack.
- **Idle CPU:** this used machine is noisy; a stopped-bench run failed the idle limit too. It is
  not evidence about indexing either way.
- **Still owed:**
  - the in-app soak, where the engine yields to the viewer. This bench deliberately does not
    yield, so it measures the worst case, not the product;
  - a quiet machine (the D6 item);
  - a library with Optimize Mac Storage off.
- **Windows:** the Windows half is nothing by design (D9 question below).

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
4. ~~**Photos library: download iCloud originals?**~~ **Settled 2026-09-28:** the index never does;
   the viewer fetches an original only when that item is viewed, and clears it after.
5. ~~**Photos library: how a result opens.**~~ **Settled 2026-09-28:** in place, read-only
   (`shell/write_guard.h`); no copies.
6. ~~**A Mac-only source in a dual-track add-on (D9).**~~ **Settled 2026-09-28:** yes, it is Mac
   only. The base-app `NSPhotoLibraryUsageDescription` and photos-library entitlement are
   accepted too.

## Explicitly not in this feature

AI culling/"best photo" scoring (owner did not ask for it 2026-09-24; stays out), auto-tagging
into keywords, generative anything, cloud inference or cloud model calls, telemetry about queries,
a catalog/albums layer, and any inference in the base installer. A microphone, a recognizer,
and a speaking voice are the Voice add-on ([19](19-voice.md)), not model files in this pack.
