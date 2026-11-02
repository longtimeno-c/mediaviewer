# Investigation — Local search's native inference outside the viewer process

Issue #390, raised while fixing #388 (fix #389). Can part of the AI pack run outside
`MediaViewer.exe`? This is the investigation, written from the code on 2026-10-31. **Nothing here is
decided and nothing is built.** The owner runs the spike in §7 and makes the calls in §9.
Numbers marked *(est.)* are arithmetic on recorded rates, not measurements.

## Status (2026-10-31)

Investigated on paper from a Linux container, so nothing was run on Windows or the Mac. The issue's
sizes are stale: `src/addons/ai` is about 21.1k lines today (`engine.cpp` alone is 7,258), and
`src/infer` is 3.8k lines plus the 2k-line `wordpiece_strip.inc` table.

**Recommendation, in short:** do seam 3 now. It is small and it closes the hole #388 came through,
which seams 1 and 2 leave open (§6). Run the seam 1 spike in §7 as a measurement. Adopt seam 1 only
if it passes on both platforms and the owner wants crash and memory isolation for ORT. Do not start
seam 2.

## 1. Where the pack runs today

- `mv_ai` is a `MODULE` over `mv_ai_engine`, which links `mv_infer` (`cmake/ai.cmake:223-278`). It
  links nothing of the core. ONNX Runtime is loaded at run time from the pack, or from the
  `ai-cuda` piece (`src/addons/ai/pack.cpp:77-90`, `src/infer/ort_dylib_win.cpp:35`). The CUDA
  runtime and cuDNN that ORT's CUDA provider loads come from `PATH`
  ([17](../design/17-local-ai-search.md) "Runtime").
- The only isolation is background-priority workers, at most `min(2, cores / 4)`
  (`engine.cpp:2256`), and the yield policy ([17](../design/17-local-ai-search.md):692-700).
- On the Mac, Whisper aborted the process inside MPSGraph, so `pack.cpp` never gives audio models
  Core ML ([17](../design/17-local-ai-search.md):169-171). That is the one recorded in-process crash
  from inference.
- **The seam is already an interface.** The engine reaches models only through five abstract classes
  (`infer::embedder`, `src/infer/models.h:72-96`; `face_analyzer`, `sound_model`, `speech_model`,
  `passage_model`) and `engine_deps` (`src/addons/ai/engine.h:71-265`). `pack.cpp`'s `ort_*` adapters implement them
  over `src/infer` (`pack.cpp:515-695`). A proxy that implements the same five classes over a wire
  touches no engine code.
- **The seam must stay at the model level.** Whisper's decoder runs once per token, up to 224 tokens
  per 30 s window, and passes KV caches in and out each time (`src/infer/audio_models.cpp:289-323`).
  A proxy at `infer::session::run` (`ort.h:111-128`) would make hundreds of round trips per window.

## 2. Seam 1: the boundary between `src/addons/ai` and `src/infer`

### 2.1 Every call

| Call (engine side) | Interface | In | Out | When |
|---|---|---|---|---|
| `embed_images(views)` | `infer::embedder` (`models.h:79`) | N RGB8 frames, long edge 448 (1,024 with faces on a photo, 768 on a clip) | N × dim floats | Photos: batches of 4 (`kPhotoBatch`, `engine.cpp:104`, `:3653`, `:4102`). Clip moments: batches of 8 (`kFrameBatch`, `:107`, `:4406`, `:4475`). Live Photo movie: up to 32 in one call (`:4223`, `:4235`). iCloud poster (`:4291`), document page (`:6024`), find-similar on an unindexed frame (`:6941`): 1 |
| `embed_text` / `embed_text_mean` | `infer::embedder` (`models.h:81-86`) | query text; 1–2 number forms (`query.h number_forms`) | dim floats | Once per new query (`engine.cpp:6503`). The engine caches 64 queries (`:6507`). Generic prompts once per model open (`:1441`, `:2468`). 704 label words once per tower, then `labels.f32` (`:2064`, `vocabulary.cpp`) |
| `analyze(img)` | `face_analyzer` (`engine.h:81`) | the photo's 1,024 frame or the moment's 768 frame | ≤ 12 faces × (box, landmarks, dim floats, quality) (`pack.cpp:655`) | 1 per photo and per kept moment, with People on (`engine.cpp:4010-4024`) |
| `embed_audio(windows)` | `sound_model` (`engine.h:101`) | 4 windows × 480,000 float PCM (10 s at 48 kHz, 5 s hop) | 4 × 512 floats | Every 20 s of soundtrack (`engine.cpp:3768`, `:3787`, `:3818`) |
| `embed_text` | `sound_model` | query text | 512 floats | Once per query with Sounds (`engine.cpp:6322`) |
| `transcribe(pcm, start)` | `speech_model` (`engine.h:128`) | ≤ 480,000 float PCM (30 s at 16 kHz) | segments: times + text | Once per window. The next window starts at `consumed_ms` (`engine.cpp:3956`, `:3977`) |
| `embed_passages(texts)` | `passage_model` (`engine.h:143`) | 16 passages (`kMeaningBatch`, `engine.cpp:106`), ≤ 256 tokens each | 16 × 384 floats | Documents' meaning pass (`engine.cpp:6088-6093`) |
| `embed_query` | `passage_model` | query text | 384 floats | Once per query with the docs piece (`engine.cpp:6434`) |
| `open_clip`, `open_faces`, `open_sound`, `open_speech`, `open_docs`, `open_clip_text`, `open_sound_text`, `open_clip_spec`, `backend_available`, `first_compile`, `accelerated_failure`, `runtime_version`, `prepare` | `engine_deps` (`engine.h:163-265`, `pack.cpp:703-1110`) | quality, compute, spec key | a handle plus its metadata, fault and fault detail | At load, on a change of setting, on a tier switch. The self-test (`pack.cpp:482-502`) and the Core ML background swap (`upgrading_clip`, `pack.cpp:515`) move with them |

Two calls stay on the engine side because they are pure functions: `infer::dot`, and
`infer::speech_words` (`engine.cpp:78`, `:3996`, `:6354`).

These also stay in the viewer under seam 1, because they are not `src/infer`: OS OCR (`ocr_win.cpp`,
`ocr_mac.mm`), PDFium on untrusted PDFs (`pdf_pdfium.cpp`), the index DB, and the search matrix
(about 77 MB at 100k L/14 frames, [17](../design/17-local-ai-search.md):722).

### 2.2 Bytes per item *(est.)*

The model input is 224 for every tower in the pack (`tools/package/ai-models.json`, `image_size`).
The engine decodes at `max(448, 2 × 224)` (`engine.cpp:4035`). Each figure is the data as it is
handed to the interface today.

| Item | In | Out | Round trips |
|---|---|---|---|
| Photo, 4:3, People off | 448 × 336 × 3 = **451,584 B** | 2,048 B (B/32) · 3,072 (L/14) · 4,096 (H/14) | 0.25 |
| Photo, the pack crops to 224² before sending (`clip_tensor` resizes to RGB8 before it normalises, `preprocess.cpp:117-140`, so the result is bit-identical) | 224 × 224 × 3 = **150,528 B** | same | 0.25 |
| Photo, People on (one 1,024 frame serves both calls if the wire keeps it) | 1,024 × 768 × 3 = **2,359,296 B** | + ≤ 12 × ~2.1 KB | 1.25 |
| Clip moment, 16:9 | 448 × 252 × 3 = 338,688 B (768 × 432 × 3 = 995,328 with People) | dim × 4 | 1/8 |
| An hour of clip, pictures | 1,800–7,200 sampled frames (`min_gap` 500 ms, `max_gap` 2 s, `engine.cpp:4326-4334`; the sampler fills to the max gap, `mediaviewer_addon.h:91-92`) → **0.61–2.44 GB** | | 225–900 |
| An hour of clip, sound | 720 windows × 1.92 MB = **1.38 GB** (each sample twice, because of the 50 % hop). Each hop sent once: 691 MB. CLAP mels computed pack-side (1001 × 64 floats, `audio_features.h:49-50`): 184 MB | 720 × 2 KB | 180 |
| An hour of clip, speech | ≥ 120 windows × 1.92 MB = **≥ 230 MB** (Whisper's mels, 80 × 3000 floats computed pack-side, are 960 KB: half) | segment text, ~KB | ≥ 120 |
| Document passages | ≤ 16 × ~1.5 KB | 24,576 B | 1/16 passage |
| **Query, uncached** | text ≤ ~100 B × 1–3 | L/14 3,072 + CLAP 2,048 + docs 1,536 = **≤ 6,656 B** | ≤ 3 (1 if batched) |
| Query, cached (64 entries) | 0 | 0 | 0 |

### 2.3 Rates and round trips *(est.)*

These use the recorded embed rates. The photo stream carries 451,584 B per image, in batches of 4.

| Where | Recorded rate | Round trips/s | MB/s at 448 | MB/s at the 224 crop | Pipe copy as share of batch time (1.81 MB a batch at 1–3 GB/s, est.) |
|---|---|---|---|---|---|
| Windows CPU, B/32 fp16 | 23.3 img/s ([17](../design/17-local-ai-search.md):110, 4 threads) | 5.8 | 10.5 | 3.5 | 0.4–1 % |
| Windows CUDA, B/32 fp16 | 150.8 img/s (:110) | 37.7 | 68.1 | 22.7 | 2–7 % |
| Windows CUDA, L/14 fp16 | 98.0 img/s (:113) | 24.5 | 44.3 | 14.8 | 1.5–4.4 % |
| Mac Core ML, B/32 | 499 img/s (:1525) | 124.8 | 225.3 | 75.1 | **7.5–22.5 %** |
| Mac Core ML, L/14 | 31 img/s (:1526) | 7.8 | 14.0 | 4.7 | 0.5–1.4 % |
| Mac, the whole pack on a 23k Photos library | ~23 assets/s (:1543) | 5.8 | 10.4 | 3.5 | <1 % |
| People on, at 23 / 150.8 photos/s | not recorded | +23 / +151 | 54 / 356 (1,024 frames) | — | not estimated |
| 1-hour 4K clip on CUDA: pictures 190 s, sound ~70 s, speech ~17 s (:1513-1516) | — | ≤ 4.7 · 2.6 · ≥ 7 | ≤ 12.8 · 19.7 · ≥ 13.6 | — | — |

What the table says *(est.)*:

- **Bandwidth is not the limit.** A pipe or a shared mapping carries 225 MB/s.
- **Copies matter only at accelerated B/32 rates**, Core ML most. There, the 224 crop or a shared
  slot is needed. With a slot, the host's `decode_still_rgb` writes straight into it, because it
  takes the output pointer (`mediaviewer_addon.h:579`; `host.cpp:188-193` allocates today).
  Two workers may also hide part of the copy behind the other worker's run.
- **Queries are small.** The FCP agent's XPC path costs +0.19 ms p50 and +0.39 ms p95 over
  in-process, for a whole search with its rows ([23](../design/23-nle-search.md):158). The text tower
  costs 3.3–6.4 ms on CPU (Mac, [17](../design/17-local-ai-search.md):162) and ~30 ms with CUDA
  (Windows, :1511-1512). The risk is head-of-line blocking: one pipe shared with a 7.7 MB CLAP batch
  in flight puts the query behind it. A query needs its own lane, at normal priority.
- **Memory that would leave the viewer** is the opened models: about 1.1 GB on disk for the Default
  fast set (B/32 303 MB, CLAP 394, Whisper base 146, AdaFace 175, MiniLM 90) and about 2.0 GB with
  L/14 and Whisper small (`ai-models.json` sizes), plus ORT's arenas. A helper that exits when idle
  gives that back. It then pays a cold start: the FCP agent's is 4.6 s ([23](../design/23-nle-search.md):159).

## 3. Seam 2: the whole engine as a helper

The size of the surface:

- **Host table:** the pack calls 25 of the 77 entries (`grep` over `src/addons/ai`).
  - I/O: `walk_files`, `walk_files2`, `stat_file`, `make_directories`, `remove_file`, `data_dir`,
    `piece_dir`.
  - Pixels: `decode_still_rgb`, `sampler_open` / `next` / `close`, `video_frame_rgb`.
  - Audio: `audio_open` / `read` / `close`.
  - Thumbnails: `thumbnail_path`, `moment_thumbnail`, `thumbnail_jpeg`, `thumbnail_store_jpeg`.
  - Facts: `tag_facts`, `read_cues`, `document_text`, `place_matches`.
  - Viewer state: `should_yield`, `post_event`.
- **Chrome table `mv.ai.1`:** 47 entries (`mediaviewer_ai.h:349` on). Status is polled up to 4 times a
  second (`AiChrome.cs:89`). Results are read row by row (`result_at`, `result_path`, `result_thumb`)
  up to 1,000 rows, so a wire must batch them, as `search_wire.h` does (2,000 rows, 16 MiB a reply,
  `src/nle/search_wire.h:29-32`).
- **What would cross:** if the helper is the app's own executable hosting the pack, as the FCP agent
  is, it decodes with the same core code and no pixels cross. What still crosses is the
  viewer-state calls: `should_yield` (every `wait_turn`, best as a shared flag), `post_event`,
  `place_matches` (the Places add-on lives in the viewer), the thumbnail cache's writes, and all of
  `mv.ai.1`.
- **Isolation:** seam 2 also takes PDFium, OS OCR and the clip sampler out of the viewer, which seam 1
  does not.
- **The C# mirror:** the WinUI chrome would still call `MvAiApi` in process, through a stub pack that
  forwards over the wire. The struct mirror stays unless the chrome speaks the wire itself.
- **Cost:** two writers to `thumbs.sqlite`, and the one-host-process rule
  ([18](../design/18-import.md):380-388) restated for a helper.

## 4. What exists to reuse

| Piece | Gives | Lacks for this |
|---|---|---|
| `io/child_process` (`child_process.h`, `_win.cpp`, `_posix.cpp`) | Spawn with piped stdin and stdout. Kill-on-close job object and `DIE_ON_UNHANDLED_EXCEPTION` (`_win.cpp:114-128`). `posix_spawn` on Mac and Linux. No pid in the interface (D9) | **Text lines only** (`write_line` / `read_line`): no binary frames. No shared memory. One reader. No priority control. On POSIX the child does not die with the parent, so the helper must exit on stdin EOF. Windows inherits every inheritable handle (`bInheritHandles` TRUE, no handle list). Not compiled into the pack today, though the module graph allows `addons → io` (`tools/check-module-graph.ps1:66`) |
| `edit/clip_wire.h` | Strict decode: an unknown op is refused | **No version field.** One JSON request per process. No binary payload |
| `MediaViewerClipJob` (`tools/clipjob`, `edit/clip_helper.cpp`) | The lifecycle: start, cancel, 5 s grace, kill, sweep. "A crash fails one job" ([12](../design/12-decision-log.md):641-647) | One short job per process, not a server. **No crash reporter** ([13](../design/13-updates-and-telemetry.md):261). Ships beside the exe, not in a pack |
| FCP agent + `mv_ai_reader_get` ([23](../design/23-nle-search.md)) | The pack hosted by another process. A versioned binary POD wire with every offset checked (`kWireVersion` 1). A peer signature check (`setCodeSigningRequirement`). Idle exit at 50 s. Measured overhead (§2.3) | Mac only (XPC, launchd). Read-only, text towers only, no pixels. It is the **app's** signed executable (`MediaViewer --search-agent`), not a binary in the pack |
| `ai-bench` (`tools/ai-bench/main.cpp`) | The installed pack, headless, loaded as the app loads it. Status JSON lines (assets/s), `indexed_s`, ms per query | No memory readout, no p50/p95, no helper switch, no People switch, no queries during indexing |
| Shared memory | — | **No port exists.** It would need `io/shared_buffer.h` with Windows (`CreateFileMapping`, an unnamed mapping duplicated into the child), Mac and Linux (`shm_open` / `memfd`) halves, to keep the core hostable on all three |

## 5. Trust model and rule 6

- **Today.** Every pack file is checked at install and at every load: the signed manifest (Ed25519),
  SHA-256 per file, and nothing extra ([18](../design/18-import.md):256-265). Fetched models are
  checked against the signed remote list (:266-272). Windows binaries are Authenticode-signed. The
  Mac bundle is Developer ID-signed, notarized, and loads under library validation, same Team ID.
- **A helper in the pack** (the 2026-10-11 rule: an add-on carries its own code, [25](../design/25-open-addons.md):30-35).
  It is one more file in the manifest, hashed at load like the DLLs. It is started by path after the
  check, the same check-then-use gap `LoadLibrary` has today in a user-writable folder. It must not
  hash the 1.2 GB again ([18](../design/18-import.md):396-398); the parent passes it the verified
  folder.
  - Mac: a Developer ID-signed, hardened-runtime executable under `~/Library/Application Support`.
    It needs library validation for `libonnxruntime.dylib`, and it must be checked whether
    Gatekeeper assesses a binary our own downloader wrote.
- **Or the app's own executable in a helper mode** (the FCP agent's model). It is already signed and
  notarized and can join the app's Crashpad handler. The app then carries a generic "host an add-on
  entry in a child" mode, which knows no add-on.
- **Rule 6.** Doc 25's line, "holds only with an OS sandbox per platform" (:308), is about strangers'
  native code. For our signed pack, rule 6 holds in a helper as it does in process: it is our code,
  and ORT's telemetry is off (`ort.h` `runtime::load`). What changes is that pixels, PCM, query text
  and embeddings cross a process boundary.
  - The channel must be private to the two processes: anonymous pipes, an unnamed mapping, no named
    object.
  - The header's promise, "stay in this process" (`mediaviewer_ai.h:21-23`), becomes "in the pack's
    processes".
- **A sandbox would be hardening, and seam 1 suits it.** The helper needs the pack folder, the
  fetched models, `data/` (the Core ML cache, `provider.txt`, `selftest.txt`) and the GPU. It needs no
  user file and no network. Seam 2's helper reads the user's files.
  - Windows: AppContainer is D8's subject, a separate call. cuDNN on `PATH` must stay loadable.
  - Mac: a sandbox profile for the helper.
- **Crash reporting.** A helper dump is mostly pixels, PCM and vectors. The scrub zeroes everything
  outside thread stacks ([13](../design/13-updates-and-telemetry.md):241-251). The helper needs
  Crashpad's client pointed at the app's handler (:220, :266). Without one, isolation turns a crash
  into silence.

## 6. #388-class drift: what is mitigated, what seam 3 adds

#388 was a size hand-copied into C# (`MvAiStatus` pinned at 1336 bytes against a 1640-byte struct).
It refused every pack ([12](../design/12-decision-log.md):1103-1111). It was a **chrome ↔ pack**
mirror. Seams 1 and 2 leave `mv.ai.1` and `Ai.cs` as they are, so neither addresses #388.

| Mitigated by #389 and after | Where |
|---|---|
| `MvAiStatus` is no longer size-pinned. The pack fills what `struct_size` declares, and the chrome zeroes the rest | `Ai.cs:408-420`, `:478` |
| `MvAiResult` is pinned at 32, its array stride | `Ai.cs:414` |
| The table's required size is measured on a real struct, not through a null pointer (#509) | `Ai.cs:427-433` |
| `tools/abi-layout` prints MSVC's offset and size for each field of `mv_ai_status` and `mv_ai_result`. The ABI smoke test compares them with the C# mirror both ways, in CI | `tools/abi-layout/main.cpp:50`, `:113`; `AbiSmokeTest/NativeLayout.cs`; `.github/workflows/ci.yml:819-829` |
| The Mac chrome reads the C header, so it has no mirror to drift | `src.swift/AIChrome/Sources/CAiApi/include/cai.h` |

What **seam 3** adds (small, Windows chrome and CI):

1. **`mv_ai_api` in `tools/abi-layout`.** Today the 47-entry function table is not compared. A C#
   field inserted or reordered calls the wrong function, and nothing catches it. List each entry's
   offset, as is done for the two structs.
2. **A structured attach error.** `AiApi`'s constructor throws `InvalidOperationException`, and the
   user sees "installed but could not start" (`InstallWords.cs:83`). Name the check that failed
   (table older than the chrome, result stride, missing entry) in Settings' detail line and in the
   managed report: the check's name only, never a path ([13](../design/13-updates-and-telemetry.md):226-229).
3. Optionally, `struct_size` on `mv_ai_result` in a future `mv.ai.2`, so no size is pinned anywhere.
   This is an ABI change, so it is listed as an owner call, not part of seam 3.

## 7. The spike (owner runs it)

Seam 1, Windows first, then the Mac twin. It is a measurement, not a PR slice: off by default, and
never shipped as it is.

### 7.1 What is built (spike branch)

- CMake `-DMV_AI_INFER_HELPER=ON` builds `mv_ai_infer[.exe]` into the pack's folder, beside
  `mv_ai` and ORT. It holds `mv_infer` plus `pack.cpp`'s model half: `ensure`, `open_*`, the self-test,
  `upgrading_clip`, `provider.txt` and `selftest.txt`. Tiers, fetch and machine stay in the pack.
- `addons/ai/infer_wire.h`: binary frames. The header is magic, `kInferWireVersion`, op, flags,
  correlation id and payload length. A `hello` swaps versions and refuses a mismatch. Two lanes: an
  **index** lane served at background priority and a **query** lane at normal priority.
- `remote_embedder`, `remote_faces`, `remote_sound`, `remote_speech` and `remote_docs` implement the
  five `engine.h` classes. A helper death is `status::io`. The engine re-spawns on it and does not
  count it against `kMaxTries` (`engine.cpp:108`). A poison input still fails after 3 tries.
- `io/child_process` gains `write_bytes` and `read_exact`. `io/shared_buffer.h` (Windows and POSIX)
  holds a ring of frame slots, and the host decodes into a slot.
- A pack setting, `infer_helper`, exists only in this build: 0 in process, 1 helper over the pipe,
  2 helper with shared slots.

### 7.2 Harness extensions (precise)

| Tool | Addition |
|---|---|
| `ai-bench` | `--infer-helper 0\|1\|2`: `set_setting("infer_helper")` before indexing |
| `ai-bench` | `--faces`: `faces_enable(1)` before indexing |
| `ai-bench` | `--query-stats`: after the query step, one line `{"queries":N,"p50_ms":…,"p95_ms":…,"max_ms":…}` |
| `ai-bench` | `--queries-during-index <qps>`: a thread issues `--query-file` queries at that rate while indexing, and prints its own `--query-stats` line |
| `ai-bench` | `--mem`: `{"phase":"loaded\|indexed\|queried","private_mb":…,"peak_ws_mb":…,"helper_private_mb":…}`. Windows `PROCESS_MEMORY_COUNTERS_EX`; Mac `phys_footprint`. The helper reports its own figure in a `stats` reply, with bytes and requests per op |
| `ai-bench` | `--kill-helper-after <s>`: a spike-only setting that kills the helper. The run prints `assets_failed` before and after |
| `mediaviewer_lab` | `--ai-start`: Settings' **Start now** after boot, so a soak runs while the pack loads and indexes (only if no switch does this today) |
| `mv_ai_tests "[.perf-bench][infer-wire]"` | 10,000 round trips of a 451,584 B frame and of a 6,656 B query reply, over the pipe and over the slots. Prints p50 and p95 µs and MB/s. Fails over 200 µs p95 for the query in optimised builds. Runs on Linux too |

### 7.3 Runs

- **Builds.** Base is `origin/main` in its own worktree and build folder. New is the spike branch,
  built with and without `MV_AI_INFER_HELPER`. Same machine, Release.
- **Packs.** `ai-sideload.py --pieces ai,ai-faces[,ai-cuda]` into a scratch `--addons` folder per
  build, with `MV_DEV_ADDONS_DIR` and `MV_DEV_THUMBS_DIR` set.
- **Corpora.** COCO-1k is val2017's first 1,000 images, as `tools/ai-eval/calibrate.py` uses them:
  decode is cheap there, so it is the worst case for the wire. The second set is 1,000 camera JPEGs
  from one card. Run one untimed pass first for a warm file cache. Every timed run starts from a
  fresh data folder.
- **Order.** Alternate base, new, base, new, three of each per cell, for mode 0 against 1, then
  0 against 2, then flag-off against `origin/main`.
- **Cells.** Windows: `--compute 1 --quality 1` (CPU, B/32), and `--compute 2` with `--quality 1` and
  with `--quality 2` (CUDA). One People pair on CUDA B/32. Mac: `--compute 1 --quality 1`, and
  `--compute 4` with `--quality 1` and `--quality 2` (Core ML, cache warm first).

```
ai-bench --addons <A> --index <corpus> --compute <c> --quality <q> --infer-helper <m> \
         --mem --query-file q200.txt --query-stats --queries-during-index 2 --timeout 1800
mediaviewer_lab --ai-start --soak 60 --gate --json soak-<build>-<m>.json <corpus>
mediaviewer_lab --soak 60 --pan-soak --open <42 MP ARW> --gate --json stress-<m>.json   # with ai-bench indexing beside it (does not yield)
mv_ai_tests "[.perf-bench][infer-wire]"
```

`q200.txt` holds 200 distinct queries, so none hits the 64-entry text cache. Keep the JSON under
`docs/perf/investigation/ai-out-of-process/`.

### 7.4 Pass / fail (proposed; the owner sets them)

| Measure | Pass |
|---|---|
| Indexing rate (`assets_per_second`, `indexed_s`) | Helper ≥ 95 % of in-process in every cell, repeated runs overlapping. Flag-off build against `origin/main`: runs overlap |
| Query latency, idle | p95 helper − in-process ≤ 1.0 ms. p50 ≤ 0.5 ms |
| Query latency, while indexing | p95 helper − in-process ≤ 2.0 ms (the query lane is not behind a batch) |
| Viewer memory after indexing | In-process `private_mb` − helper-mode `private_mb` ≥ 80 % of the opened models' on-disk size. Viewer + helper ≤ in-process + 200 MB |
| PR 1 present loop | Windows PR 1 `--gate` holds in every mode, in-app and in the stress run. 0 dropped frames. p99 within the gate's 10 % of base |
| Helper killed mid-index | 0 assets failed by the kill. Indexing resumes once the helper is back. The lab soak running meanwhile drops 0 frames |
| Bytes on the wire | The helper's `stats` within ±20 % of §2.2, or §2.2 is corrected |

**Mac twin.** The same commands and thresholds, with the Mac PR 1 gate.
- `posix_spawn`, and a helper dev-signed in the sideloaded pack.
- Core ML compiles in the helper, into the same `data/cache/coreml`.
- Core ML B/32 at 499 img/s is the hardest cell for the wire (§2.3).
- A spike-only switch that gives Whisper Core ML reproduces the MPSGraph abort
  ([17](../design/17-local-ai-search.md):170). The pass is: the helper dies, the viewer keeps
  presenting, and the clip's speech fails cleanly.

Linux: `[infer-wire]` runs there, and `child_process_posix` already builds. No Linux host run is asked
for (`docs/plans/linux.md` §10).

### 7.5 After the spike

The owner decides. A decision-log row in [12](../design/12-decision-log.md) records it with the JSON.
If seam 1 is adopted, it lands as a dual-track PR (D9): both platforms, both PR 1 gates, and the
verify line from §7.4. Then [17](../design/17-local-ai-search.md) "Runtime" and "Not hurting the
viewer", [13](../design/13-updates-and-telemetry.md) and `docs/DEVELOPMENT.md` are updated.

## 8. Weighing the three

| | Seam 1: inference only | Seam 2: whole engine | Seam 3: checks only |
|---|---|---|---|
| Fixes #388's class (chrome ↔ pack) | No | No (the stub still mirrors) | **Yes**, for the table order and the attach error |
| ORT, CUDA, Core ML crash out of the viewer | Yes | Yes | No |
| PDFium, OS OCR and the sampler out of the viewer | No | Yes | No |
| ~1.1–2.0 GB of weights out of the viewer | Yes | Yes, plus the search matrix | No |
| What crosses | §2: 150 KB–2.4 MB per photo, ≤ 7 KB per query | 25 host calls (pixels stay put if it is the app's exe) + 47 chrome calls + events | Nothing |
| Can be sandboxed without user files | Yes | No | — |
| New pieces | Wire, two lanes, slots port, helper binary, signing, Crashpad client | Those, plus a forwarding stub for `mv.ai.1` and a second thumbnail writer | Two CI checks and one error type |
| Risk to the viewer's numbers | Copy cost at Core ML B/32 rates; query cold start after an idle exit | Most moving parts | None |

## 9. Owner calls

1. **Whether to run the §7 spike at all**, or take seam 3 alone. The recommendation is seam 3 now
   and the spike as a measurement.
2. **Where the helper binary lives.** In the pack, per the 2026-10-11 rule: its own Authenticode
   signature, and Developer ID with notarization. Or the app's own executable in a generic
   add-on-helper mode, the FCP agent's precedent: signed with the app, and Crashpad for free.
3. **Pipe or shared slots**, by the spike's numbers. If slots, `io` gains a shared-memory port with
   Windows, Mac and Linux halves.
4. **Text towers.** In the helper, so ORT is out of the viewer entirely, with a round trip and a cold
   start after an idle exit. Or kept in process, so queries are unchanged and ORT stays in the viewer.
5. **Idle exit.** After how many seconds, if at all (cf. #292; the FCP agent uses 50 s), and whether
   the helper stays while the search panel is open.
6. **Crash reporting** for the helper (Crashpad client on the app's handler), and whether
   `MediaViewerClipJob` gets the same.
7. **Sandbox.** None, as in process today. Or an OS sandbox for the seam 1 helper: AppContainer on
   Windows, which is D8's subject; a sandbox profile on the Mac.
8. **The thresholds in §7.4**: 95 % indexing rate, +1 / +2 ms query p95, the memory delta, 0 drops.
9. **`mv.ai.2`** with `struct_size` on `mv_ai_result`, or keep the stride pinned and checked.
10. **Linux.** Whether a helper, if adopted, waits for the Linux halves' order (`docs/plans/linux.md`
    §10, call 8) or lands dual-track with the Linux code kept hostable.
