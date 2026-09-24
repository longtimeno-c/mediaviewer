# 17 — Local AI search (video moments and photos)

**Status: proposed 2026-09-24, post-v1. Not in PR 8. Milestone H, PRs 20–24 (renumbered 2026-09-24 from 21–25; it follows the Import add-on, PRs 16–19).**
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
   and PR 8's packaging assert still fails if ORT/DML appear in the base tree.
3. **CLAUDE.md: "Do not introduce … D3D12."** Avoided: the plan uses vendor providers, not
   DirectML (which runs on D3D12), so the rule is untouched. See *Runtime*.

## Model choice

A **dual-encoder image/text embedding model** (CLIP family). One image tower, one text tower, a
shared vector space; search is a dot product. This is the only class of model that makes
"search video by a sentence" cheap: frames are embedded **once at index time**, queries embed
~10 ms at search time.

- **Candidates:** SigLIP (Apache-2.0) and OpenAI CLIP ViT-B/32 (MIT), exported to ONNX.
  ViT-B/32-class at 224 px is the size/quality floor; go larger only if the eval set (PR 22)
  shows the recall gain is worth the indexing time.
- **Weights licence is a gate, not a footnote.** The app is GPL-2.0-or-later; weights are data,
  but redistribution terms still bind. CI check: the pack manifest names the licence for every
  model file, and non-commercial / research-only weights fail the build. Some LAION-trained and
  Meta checkpoints are not permissive — check each.
- **Precision:** FP16 or INT8 (quantized) ONNX. Pick by measured recall loss in PR 20, not by
  assumption.
- Multilingual text queries are a *model* property (e.g. multilingual SigLIP variants), decided
  in PR 20 on the eval set. English-only is an acceptable first pack.
- **Out of scope for this doc:** speech transcript search (Whisper), OCR, object boxes,
  captioning. Each is a later, separate pack with its own tower.

## Runtime

**ONNX Runtime**, dynamic-linked, behind our own interface — the same D9 discipline as
`IVideoSource`:

```
src/infer   IEmbedder { load(pack), embed_image(span<u8 rgb>, w, h), embed_text(string_view) }
            Backends: ort_openvino, ort_cuda (optional sub-packs), ort_cpu (fallback, always present),
            later coreml (Mac half, if H goes dual-track)
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
  GPL-2.0-or-later status before shipping it, and if it fails the gate it is user-supplied
  instead). **AMD GPUs have no good ORT provider on Windows without DirectML**; they run CPU
  until a provider exists. That is a known gap, not a hidden one. Adding DirectML later as one
  more provider would need the CLAUDE.md D3D12 rule reworded and is not planned.
- ORT owns whatever device the provider creates. It never shares our render `ID3D11Device`,
  and never touches the swapchain. Not Windows App SDK AI / Windows ML — ORT directly, so the
  payload is ours and the base-installer exclusion (log 2026-09-20) stays intact.
- The macOS path is the same model through the CoreML EP or a direct Core ML port. Embeddings
  from different backends differ slightly, so **the index records `model_id` + `spec` and a
  query only ever compares vectors from the same model** — never mix.

## The AI pack (delivery)

**Installs through the add-on mechanism that Import builds in PR 16**
([18-import.md](18-import.md#add-ons-how-import-is-installed)): the same Settings → Add-ons
page, signed manifest, verify-before-load, per-user versioned folder, silent updates and
sideloading. The AI pack adds its sub-packs and model files. It does not build a second
installer.

- **The whole feature is a downloadable extra, installed from Settings** (owner call
  2026-09-24) — because it adds hundreds of MB. Settings → **Local search** shows what is
  installed, the size of each piece, and **Install / Remove** for each: *Core* (ORT + CPU +
  image/text model), *Faces* (PR 24), and one sub-pack per vendor provider. Remove deletes the
  files and offers to delete the index. Nothing about it appears elsewhere in the UI until the
  core pack is installed; the base app, installer and update size never include it.
- Downloaded on **explicit opt-in** ("Install local search — downloads ~N MB") from the same
  signed release channel as updates ([13](13-updates-and-telemetry.md)). Contents:
  `onnxruntime.dll`, provider DLLs, the model files, `manifest.json` (versions, SHA-256,
  licence per file). Provider sub-packs are offered based on detected hardware, never
  auto-installed.
- Verify signature + hashes before load. The request carries **no identifier, no path, no
  telemetry** — a plain GET of a fixed URL. Ask before the first download, no pre-ticked box.
- Installed to `%LocalAppData%\MediaViewer\ai-pack\<version>`, per-user, removed by uninstall
  (with a separate "also delete search index" choice).
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
4. **Frame → tensor.** Decode (D3D11VA where our device allows, else software), resize on the
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

New SQLite file `%LocalAppData%\MediaViewer\ai\index.db`, separate from the thumbnail DB so
"clear the index" cannot corrupt thumbnails, and vice versa.

```
assets(id, path, mtime, size, kind, spec, state, indexed_at)        -- key: (path, mtime, size, spec)
frames(asset_id, pts_ms, pts_tb, flags, thumb_ref, emb BLOB)        -- emb = fp16 or int8 vector
meta(model_id, dim, spec, ...)
```

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

- Results are the **gallery** (`G`) island's grid over a result set instead of a folder listing:
  same thumbnail cache, same selection, same keyboard model. A tile shows the frame thumb and
  `mm:ss`.
- **Enter** on a video tile opens the clip and seeks to that PTS, paused on the frame, with the
  scrub bar marking the other matches from the same clip (like the keyframe grid PR 13 will draw).
- New command ids, added as **rows in the 16 table** when PR 22 lands, not before:
  `search.open` (a query box, keyboard-focus), `search.similar`, `search.next_match` /
  `search.prev_match` (within a clip). Key assignment is done then, against the live table, so
  it cannot collide. Everything reachable without the mouse, per 16's verify.
- A visible **indexing status** (progress, pause/resume, "paused — playing video"). Never a modal.

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
- **Disk:** the index has a size cap and per-folder delete. Show its size in Settings.

## ABI

Flat, POD, opaque handles, correlation ids, completion-queue delivery — no exceptions across the
line ([14-abi.md](14-abi.md)). Sketch only; the header is written in PR 20:

```
mv_ai_pack_status / mv_ai_pack_install(progress cb via completion queue)
mv_index_start(folder, scope) / mv_index_pause / mv_index_status
mv_search_query(text | frame) -> job id ; results arrive as completions
mv_search_result_get(job, i) -> { asset id, pts_ms, score }
```

C# wraps each in a `SafeHandle`/`IDisposable`; C++ never calls the dispatcher.

## Roadmap slices

Each is independent with a verify line, and each inherits PR 1's present-loop verify. Numbers
follow the Import add-on (PRs 16–19). **Windows first, as proposed.** Whether H follows the
dual-track rule that PRs 9–19 follow is an open owner decision
([10](10-roadmap.md), 2026-09-24). A Mac half would need a Core ML provider.

### PR 20 — Inference host and the AI pack
`src/infer` (`IEmbedder`), ORT CPU + first vendor provider (OpenVINO or CUDA, by what the dev box has), pack manifest/verify/download/install as **per-piece Install/Remove in Settings**, the Auto/provider/CPU-only toggle, opt-in
flow, settings page. **No indexing, no UI beyond the opt-in.** A short spike first: measure
image-tower throughput on CPU and the available vendor provider on the dev box and record it here; those numbers, not the
guesses in this doc, size everything after.

**Verify:** a fixed set of test images embeds to vectors within tolerance of the reference
(PyTorch/ORT-Python) outputs on CPU and the vendor provider; a text query ranks a small labelled image set
correctly; **base installer and base install tree are byte-identical to PR 8's with the pack
absent** and the packaging assert still fails if ORT or provider DLLs land in the base tree; tampering with a
pack file or manifest is refused; the download request contains no identifier; PR 1's
present-loop holds with the pack installed and idle.

### PR 21 — Video sampler and index
Keyframe sampler with min/max gap, HDR tone-map, embedding dedupe, `index.db`, background queue,
resume, stale detection, yield policy, indexing status.

**Verify:** a 1-hour 4K HEVC clip indexes to completion in a recorded time; killing the process
mid-way and restarting resumes without re-doing committed frames; editing/replacing a file
re-queues it; **playing a different 4K clip and panning photos while indexing runs stays at 0
dropped frames** (`tools/frametime` soak with indexing active, not just idle); the pause-on-
playback policy visibly triggers; the index contains no data from an HDR clip that is clearly
wrong (tone-mapped, not washed out); minidump from a forced crash mid-index contains no path,
filename, embedding or pixel data.

### PR 22 — Search and results
Query box, brute-force scan, per-clip grouping, results in the gallery island, jump-to-moment,
match markers on the scrub bar, folder/kind scope, min-score cutoff. Photos indexed too. The
16-commands rows land here.

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

## Open decisions (owner)

1. ~~**D3D12 / DirectML.**~~ **Settled 2026-09-24:** vendor providers (OpenVINO, CUDA/TensorRT)
   as optional sub-packs, CPU always the fallback, a Settings toggle for Auto / provider /
   CPU-only. No DirectML, so no CLAUDE.md change. Remaining risks: NVIDIA redistribution
   licence vs the GPL, and AMD GPUs running CPU (see *Runtime*).
2. **Model** (SigLIP vs CLIP, size, multilingual) — decided by the PR 20 spike + eval set, with
   the weights-licence gate above.
3. ~~**Is this a D10?**~~ **Settled 2026-09-24: no.** It stays a plan/17 proposal plus the
   decision-log entry; it is not a numbered D-decision.

## Explicitly not in this feature

AI culling/"best photo" scoring (owner did not ask for it 2026-09-24; stays out), auto-tagging
into keywords, generative anything, cloud inference or cloud model calls, telemetry about queries,
a catalog/albums layer, and any inference in the base installer.
