# 17 — Local AI search (video moments and photos)

How Local search works: the AI pack add-on, its models and runtime, the frame sampler, the
index, search and its "nothing found" rule, the query language, audio, People, index sharing,
the macOS Photos library source, and how indexing stays out of the viewer's way.

Local search is an opt-in add-on (the AI pack) installed from Settings on Windows and macOS.
The base viewer never carries or requires it: with no pack, the feature is hidden and nothing
else changes. Roadmap labels: PRs 20–24 (inference host, sampler and index, search, find
similar and management, faces).

## What it is

Type "man on a broom" or "birthday cake" and get a grid of **moments**: for a video, a frame
thumbnail with its timestamp; for a photo, the photo. Enter opens the clip seeked to that
moment. "Find similar" takes the frame on screen as the query. It works over the folders the
user points it at, entirely on this machine.

It is a **search index over the user's own files**, built in the background. Indexing runs on
low-priority workers and results arrive through the completion queue ([14](14-abi.md)); the
index is a separate SQLite file (no sidecar, no write to media); inference is local, and
embeddings, like pixels, never go into telemetry or crash reports.

## Model choice

A **dual-encoder image/text embedding model** (CLIP family): one image tower, one text tower, a
shared vector space; search is a dot product. Frames are embedded once at index time; a query
embeds in milliseconds at search time.

The Core pack ships two OpenAI CLIP towers (Xenova ONNX exports, MIT), fp16, as the user's
**Search quality** setting:

| Quality | Tower | Dim | Used by Auto when |
|---|---|---|---|
| Fast (1) | CLIP ViT-B/32 | 512 | CPU only |
| High (2) | CLIP ViT-L/14 | 768 | an accelerated provider runs it (CUDA, Core ML), unless that provider failed it on this machine |

Stored vectors are int8 with a per-vector scale (≤ 0.5 points R@1 lost on both towers). Each
tower's thresholds, generic prompts and preprocessing live in its `model.json`
(`tools/package/ai-models.json`): `dedupe` 0.97, `query_margin` 0.04, `query_z` 2.5,
`result_z` 2.0, `result_margin` 0.015, `similar_min` 0.62, five generic prompts ("a photo.",
"a picture of something.", "an image.", "a photo of a thing.", "a blurry photo.").

Changing quality migrates: the old index answers until the new one completes, and a query
never compares vectors from two towers.

**Weights licence gate.** Every model file names its licence; `tools/package/ai-models.py check`
refuses one outside the allow-list (a CC-BY-NC file fails the pack, tested). Licences in the
pack: CLIP weights MIT, ONNX Runtime MIT, YuNet MIT, AdaFace MIT (weights; trained on WebFace4M,
whose terms are non-commercial research), CLAP Apache-2.0, Whisper Apache-2.0.

### PR 20 spike results

Measured on a Ryzen 16-thread / RTX 4070 box, ORT 1.30. Eval: 1,000 COCO Karpathy-test
images, text → image with each image's first caption, plus eight labelled natural-language
queries (P@10 by caption keywords, 1,225 images).

| Tower (ONNX, Xenova export of OpenAI CLIP, MIT) | R@1 | R@5 | R@10 | mean P@10 | "guy on a skateboard" P@10 | CPU img/s (4 thr) | CUDA img/s | size |
|---|---|---|---|---|---|---|---|---|
| ViT-B/32 fp32 | 0.494 | 0.777 | 0.888 | 0.86 | 1.0 | 20.1 | - | 606 MB |
| **ViT-B/32 fp16** | 0.493 | 0.777 | 0.888 | 0.86 | 1.0 | 23.3 | 150.8 | 303 MB |
| ViT-B/32 int8 | 0.398 | 0.695 | 0.822 | 0.88 | 1.0 | 31.5 | - | 154 MB |
| ViT-B/16 fp16 | 0.500 | 0.794 | 0.885 | 0.84 | - | 8.5 | 135.7 | 300 MB |
| **ViT-L/14 fp16** | 0.555 | 0.812 | 0.905 | 0.90 | 1.0 | 1.9 | 98.0 | 856 MB |

L/14 is +6 points R@1 over B/32 but 1.9 img/s on CPU, which would take days on a
300,000-asset library; hence High on accelerators and Fast on CPU. int8 weights lose 6.6 points
R@10; B/16 gains nothing at R@10 for 2.7× B/32's CPU cost.

**Recall target:** on the labelled set, the top five for each natural-language query hold at
least four relevant items (P@5 ≥ 0.8), "guy on a skateboard" included; the COCO-1k proxy
R@10 ≥ 0.88 (Fast) / ≥ 0.90 (High).

## Runtime

**ONNX Runtime**, loaded at run time (never linked; ORT's telemetry events off), behind our own
interface in `src/infer` (`ort.h`, `models.h`):

```
src/infer   embedder { embed_image(s), embed_text }    clip_model, clap, whisper, face_models
            backends: cpu (always), cuda (Windows piece), openvino (code path), coreml (macOS)
```

- `infer/` headers include no D3D, DirectML or Metal headers. ORT owns whatever device the
  provider creates; it never shares the render device and never touches the swapchain.
- **CPU is always underneath.** Settings → Local search → *Compute*: **Auto** (default) · a
  specific provider · **CPU only** (`mv_ai_compute`). Mac offers Auto / Core ML / CPU only.
- **The self-test:** a provider is kept only if it agrees with CPU within cosine 0.99 on a fixed
  input and is faster; otherwise Auto falls back to CPU and the status says why
  (`provider_fault`: not in this build, runtime missing, failed, mismatch, slower), with the
  provider's own message in `provider_detail_utf8` (`session::last_error`, paths replaced by
  `<model>` / `<path>`; a tooltip on the Mac, never telemetry). Changing compute re-creates the
  sessions; it does not re-index.
- **Auto never runs the large tower on CPU because a provider failed it (2026-10-05).** How the
  provider did with each tower is kept in `data/provider.txt` (runtime | provider | spec → ok or
  the fault, open seconds, message). Where it failed ViT-L/14, Auto opens ViT-B/32 on that provider
  instead, at once (the control thread reloads when a background upgrade fails) and on every later
  start, and sets `MV_AI_STATUS_SMALL_FALLBACK`. Settings → *Compute* then offers **Try the larger
  model again** (`"retry_large"`), which forgets the verdict. A new runtime tries again by itself.
  Each switch re-indexes (the towers' vectors differ), so a passing or untried tower is left alone.
- **Windows providers:** CUDA as the `ai-cuda` piece (ORT's CUDA 13 build; the CUDA runtime and
  cuDNN 9 are user-supplied, on PATH; without them Local search runs on the CPU). OpenVINO has
  a code path (`MV_AI_BACKEND_OPENVINO`) but no published piece. AMD GPUs run on CPU. There is
  no DirectML provider, so no D3D12.
- **macOS:** the same ONNX models through ORT's **Core ML** provider (GPU / Neural Engine), with
  ORT's CPU provider underneath. arm64 only: there is no x86_64 macOS ORT build, so Intel Macs
  are not offered Local search and `cmake/darwin.cmake` defines no AI target there.
- **Core ML needs static shapes.** The image tower's free dimensions are pinned at open
  (`session_options::fixed_dims`, batch 4 = the engine's photo batch; `embed_images` splits and
  pads other batch sizes), which puts every node on Core ML. Opening is slow even from its cache
  (`data/cache/coreml`, ~1.2 GB B/32 / ~4.1 GB L/14, outside the 3 GB ceiling), so the pack
  **answers on CPU at once** and a background thread opens Core ML, runs the self-test and swaps
  it in (`upgrading_clip` in `pack.cpp`). Text queries stay on CPU (3.3 ms B/32, 6.4 ms L/14).
- **Audio stays on CPU on the Mac:** CLAP's audio tower does not compile on Core ML (unbounded
  dimensions) and Whisper's aborts the process inside MPSGraph; `pack.cpp` never gives audio
  models Core ML.
- An index records the model id, spec and precision; a query only compares vectors from the same
  model. An index is local to one machine (see *Sharing an index* for carrying one).

## The AI pack (delivery)

The pack installs through the add-on mechanism ([18](18-import.md#add-ons-how-import-is-installed)):
signed manifest, verify-before-load, per-user versioned folder, explicit updates, sideloading.

- **Pieces**, each with its own Install / Remove under Settings → **Local search**:

  | Piece | Contents | Size |
  |---|---|---|
  | `ai` (Core) | `mv_ai`, ORT (CPU; Core ML on Mac), both CLIP towers, tokenizer, the chrome (`MediaViewer.Ai.Chrome` / `AI.bundle`) | ~1.18 GB installed |
  | `ai-faces` (People) | YuNet detector, AdaFace IR-50 embedder (fp16) | ~87 MB model |
  | `ai-audio` (Sound) | CLAP `larger_clap_general`, Whisper base and small, tokenizers | ~1 GB |
  | `ai-cuda` (Windows) | ORT's CUDA build | ~205 MB |

  Pieces declare `part_of: ai` and an `arch`. Clicks queue, Core first; "Install all"; "Update
  all" when more than one installed piece has a newer version, and a piece's Install or Update
  queues a Core that is itself behind first, so no piece runs ahead of the engine it was built
  with. Remove deletes the files and offers to delete the index; `data/cache` (Core ML) is
  deleted even when the index is kept.
- **3 GB installed ceiling** for the family (`family_ceiling`, `addon-pack.py ceiling`): every
  supported combination must fit, and the store refuses an install that would exceed it. The
  index and the thumbnail cache are user data and do not count. Worst supported combination
  ~2.93 GB.
- Downloaded on explicit opt-in, a plain GET of fixed release URLs with no identifier. The Mac
  pack is Developer ID-signed and notarized and loads under library validation.
- **Location:** `%LocalAppData%\MediaViewer\addons\ai\<version>` on Windows and
  `~/Library/Application Support/MediaViewer/Add-ons/AI/<version>` on Mac; the index and caches
  are in that add-on's `data` folder.
- **Offline / sideload:** the pack can be dropped into that folder by hand
  (`tools/package/ai-sideload.py` for dev builds); the app verifies it the same way.
- **Verification cost:** the first check in a process hashes every file; later ones stat every
  file (size and modification time) and re-walk for extras. Listing peeks at manifests without
  hashing; only the add-on being loaded is hashed.

## Frame sampling

`src/edit/clip_sample.*`: its own software decoder instance, never the playback decoder.

1. **Keyframe pass.** Demux with `skip_frame = AVDISCARD_NONKEY`. Phone and camera GOPs are
   ~1 s, so this yields ~1 candidate per second cheaply.
2. **Coverage fallback.** Long-GOP sources can go minutes between keyframes. A **maximum gap**
   (2 s) fills the grid by decoding forward from kept packets; a **minimum gap** (500 ms) stops a
   burst of scene-cut keyframes flooding the index. People-only sampling uses 2 s / 10 s.
3. **Dedupe by embedding.** A frame whose cosine to the last *kept* frame is above `dedupe`
   (0.97) is dropped. A static interview shot collapses to a few rows; an action scene keeps
   many.
4. **Frame → tensor** (`infer/preprocess.h`). Rotation and SAR applied, resized with PIL-exact
   bicubic to the model's input, normalised with the model's mean/std. HDR/PQ/HLG sources go
   through the clip core's SDR tone map (`rgba_converter`) first: the model was trained on SDR
   sRGB.
5. **Record** `pts` in the stream time base and in milliseconds, plus a keyframe / grid-fill
   flag, so jump-to-moment lands on the exact frame through the player's seek.
6. **Photos** are one row each, embedded from a first-pixel-quality decode (RAW uses the embedded
   preview). Live Photo and RAW+JPEG pairs index once, on the pair's still.

## Index store

`index.db` (SQLite, WAL) in the pack's data folder, separate from the thumbnail DB so clearing
the index cannot touch thumbnails. Schema (`index_db.cpp`):

```
meta(key, value)
roots(id, path, recursive, enabled, last_scan_at, media)          -- remembered index locations
assets(id, path, root_id, mtime, size, kind, duration_ms, seen)
progress(asset_id, spec, state, resume_ms, tries, indexed_at)     -- per asset and model spec
frames(id, asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags, generic, scale, emb)
speech(id, asset_id, spec, start_ms, end_ms, text)                 -- Whisper transcript lines
```

`emb` is the int8 vector, `scale` its per-vector scale, `generic` its best cosine against the
tower's generic prompts. People live in a separate `faces.db`; the label bars in `labels.f32`;
the CPU self-test reference in `selftest.txt` (keyed by runtime, provider, model and piece).

- **Folder roots persist.** Indexing a folder (with its subfolders) writes a `roots` row.
  Reopening the app or the folder does not rebuild it: a watcher plus a startup delta scan
  queues only new, changed or removed assets. Each root can be paused, rescanned or removed.
- Keyed like the thumbnail cache: change `(path, mtime, size)` or the model `spec` and the row
  is stale and re-queued. No content hashing.
- Result tiles are the existing **JPEG-512 cache**; moment thumbnails are stored there as
  `path#t=ms` rows through the host table. There is no second thumbnail path.
- Resumable: state is per asset and spec; a kill mid-clip resumes at the last committed frame
  (`resume_ms`).
- The index holds paths locally and is excluded from crash reporting and telemetry.

## Search

- **Query encode:** the text tower, cached for repeated queries. A picture query's last noun is
  embedded in **both numbers and averaged** (`query::number_forms`, one batched run), so
  "mountain" and "mountains" are the same query.
- **Scan** (`vectors.h`): brute-force int8 dot product over an in-memory matrix of every stored
  frame of the active model (loaded, not memory-mapped; ~77 MB at 100 k L/14 frames). 100,000
  ViT-L rows scan in ~31 ms on one core. There is no ANN index.
- **Ranking:** top frames, then **grouped per clip** with the best moment first and "N more in
  this clip", so one long video cannot fill the page (`mv_ai_result`). `find_text` returns at
  most 5,000 rows.
- **Scope:** the open folder and its subfolders (default) · all indexed folders
  (`MV_AI_SCOPE_TREE` / `MV_AI_SCOPE_ALL`; the panel offers no folder-only scope). Kind filter
  (photos / videos) and tower filter (pictures / sounds / speech). A nonsense query returns
  "nothing found", not the least-bad ten (below).
- **Find similar:** the current still or the paused frame as the query. An indexed still or
  moment uses its stored vector; another paused frame is embedded on demand.

### Nothing found

An absolute cosine floor does not separate nonsense from real queries on CLIP (their top scores
overlap). The rule (`vector_store::scan_stats`, `find_text`) combines four signals:

- **Margin over generic prompts.** Each stored row keeps its best cosine against the generic
  prompts; a row's margin is its score minus that. A result row must beat it by
  `result_margin` (0.015).
- **Stand-out z.** `top10_z` is how many SDs the ten best **assets** (per asset, so a long clip's
  similar frames count once) sit above the mean of every asset's best score. For a query that
  matches nothing it grows with the library like the top ten of N noise draws (~2.2 at 300
  assets, 2.6 at 1,000, 3.3 at 10,000), so "stands out" means z ≥ max(`query_z`,
  1.18 × the noise expectation for this many assets).
- **The gate needs z and margin together.** With L = log10(assets / 1,000) (≥ −1), a query
  passes on its best margin alone at `query_margin` × (1.375 + 0.125 L), or when it stands out
  with a best margin of `query_margin` × 0.375 (1 + L). Below 100 assets the margin alone
  decides.
- **Broad subjects.** A subject that fills much of the library ("mountain" in a mountain
  library) cannot stand out from it: it *is* the mean. A query is believable at the plain
  calibrated margin when its rows clear that margin on at least max(5, 1 % of the assets)
  assets (`scan_stats::over_margin`).
- **Result z.** Once a query stands out, a row at least `result_z` (2.0) SD above the mean is a
  result as well as the margin rows.

### Nothing found at scale

**A label vocabulary filters rows** (`vocabulary.h`). 705 everyday labels (people, animals,
vehicles, places, scenes, objects, food, events, text and screens; deliberately not COCO's) are
embedded once per tower and kept in `labels.f32`. Each row stores the score of its ninth-best
label; a row is a result only when the query scores at least that. Gibberish loses to the
labels on most of the photos it lands on; "a dog" beats every label but near-synonyms on a dog.
Embedding the labels takes ~4 s (B/32) / ~6 s (L/14) once per tower, cached; the per-row bars
take 1.2 s per 100 k rows at load, filled in chunks behind searches (until then a row passes).
+4 bytes a row; the scan cost is unchanged (100 k × 768: 1.66 → 1.73 ms).

Calibrated on COCO 2017 at 1 k / 5 k / 10 k / 25 k photos, both towers on Core ML (500
held-out captions, 85 nonsense strings), default Precision:

| Tower, photos | Captions found / own photo (of 500) | Nonsense passing (of 85) | Nonsense rows (median) | Category P / R | Labelled P@5 |
|---|---|---|---|---|---|
| B/32, 1 k | 475 / 460 | 5 | 10 | .72 / .69 | .89 |
| B/32, 25 k | 489 / 470 | 9 | 196 | .74 / .72 | .94 |
| L/14, 1 k | 478 / 474 | 11 | 57 | .85 / .71 | .97 |
| L/14, 25 k | 489 / 483 | 23 | 221 | .87 / .75 | 1.0 |

Target: at 25 k assets and default Precision, at most 1 in 3 nonsense strings answered on L/14
and 1 in 8 on B/32 (the fitted rule gives 27 % and 11 %). Placeholder and long-sentence
gibberish (`lorem ipsum`) can still pass on L/14 at large sizes, with hundreds of rows. CLAP
keeps the margin rule alone (no z calibration of its own).

### Precision scale

Settings → Local search → **Precision**, five steps from Broader to Stricter; the middle (2) is
the default and is the rule above, unchanged. The engine setting `precision` (0–4, saved) is
read by each search as it starts: no reload, no re-index, and the chrome re-runs an open search.
Per level (`precision_scale::at`) it scales the stand-out factor, the query margin, the row
margin and the row z, and at 3–4 keeps only rows within Δ SDs of the best row:

| Level | stand-out × noise | query margin | row margin | row z | Δ (SD) |
|---|---|---|---|---|---|
| 0 Broadest | 1.13 | × 0.8 | × 0.33 | × 0.8 | - |
| 1 | 1.15 | × 0.9 | × 0.67 | × 0.9 | - |
| **2 (default)** | **1.18** | **× 1** | **× 1** | **× 1** | - |
| 3 | 1.26 | × 1.125 | × 1.5 | × 1.2 | 3 |
| 4 Strictest | 1.38 | × 1.25 | × 2 | × 1.4 | 2 |

"Nothing found" always reads the calibrated rows, so each stricter level answers a subset of the
looser one. L/14 tells a near-miss category ("helicopter" against COCO's planes) from a real
subject at level 3; B/32 cannot (its near misses stand out as strongly as real subjects). CLAP
scales its two margins the same way; spoken-word search needs all the query's words at 3–4 and
half at 0–1.

### Query syntax

One parser in the pack (`src/addons/ai/query.*`, `mv_ai_tests "[query]"`), so both chromes, the
Final Cut Pro panel ([23](23-nle-search.md)) and spoken requests ([19](19-voice.md)) mean the
same thing by the same words. Terms narrow each other (AND).

| Typed | Means |
|---|---|
| `Tristan beach` | photos / moments with Tristan, ranked by "beach" (no "nothing found" test inside a person's own) |
| `Tristan "hello"`, `Tristan:"hello"`, `said:hello` | Tristan, and a clip whose transcript has those words in that order; the line is the snippet |
| `Tristan Aaryan`, `Tristan and Aaryan` | both in the same photo or clip |
| `Tristan or Aaryan` | either |
| `Anna Smith`, `anna` | a named person: the whole name or the first name, any case, accents folded |
| `@tri`, `person:"anna s"` | a person by the start of a name, or a near spelling; nobody by that name: nothing found |
| `Trist`, `tristna` (alone) | a lone word that starts or nearly spells a name: those people first, then what the word describes (so "car" still finds cars when there is a Carla) |
| `-Nico`, `-beach`, `-"goodbye"`, `-video` | leave out a person, what a picture search finds, a phrase said, a kind |
| `beach video`, `is:photo`, `videos of Anna` | a kind: the last word, `is:` / `type:`, or a leading "videos of"; "photos of …" asks for anything |
| `in:2024`, `in:2024-06`, `before:2025`, `after:2023-05`, `since:2024-03-01`, `until:2024` | the **file's** date (modification time, UTC). Capture dates (EXIF) are not in the index |
| `file:IMG_12`, `file:"trip 2024"`, `-file:copy` | the file's **name** (not its folder) contains the text, case and accents folded. Indexed files only |
| `video in:2024`, `file:IMG_12` | filters alone: everything they allow, newest first |

Lead-ins ("pull up all the …", "show me …", "find …") are trimmed. Typing a name: the panel
names people for the word being typed (`suggest_json`; prefix first, then a near spelling of one
letter, two from eight letters); **Tab** or a click completes the word. A word that is not a
name, a kind or an operator stays in the description; an unfinished `in:20` is ignored. Not in
the index, so not in the syntax: OCR (quoted words search speech only), places, EXIF capture
dates, camera. Per keystroke the parse is linear in the query plus one `SELECT` of the named
people.

## UI and commands

- **Results are the gallery:** the same gallery, filmstrip, keys and thumbnails over a result
  list instead of a folder listing (base ABI `mv_folder_open_list`). A tile shows the frame
  thumbnail and `mm:ss`. **Enter** on a video tile opens the clip paused on that moment, with
  the scrub bar marking the clip's other matches.
- **Search panel** (`MediaViewer.Ai.Chrome` / `AI.bundle`): opened from the search icon in the
  path bar or `Ctrl+F` / `⌘F`. Query field with name suggestions, *Look in* (this folder and
  subfolders · everywhere · Photos on the Mac when indexed), kind and Pictures / Sounds / Speech
  filters, the snippet under a spoken result. Without the pack the same icon and key open the
  base app's file search ([16](16-commands.md) "File search").
- **Commands** (rows in `src/shell/command_table.cpp`, listed only while the pack is loaded,
  except `Ctrl+F`):

  | Command | Windows | Mac |
  |---|---|---|
  | `search.open` | `Ctrl+F` | `⌘F` |
  | `search.similar` (Find similar) | `Ctrl+Shift+F` | `⌘⇧F` |
  | `search.next_match` / `search.prev_match` (within a clip) | `N` / `Shift+N` | same |

- **Index this folder and subfolders:** offered when an unindexed folder is open and the Core
  pack is installed (also in the folder toolbar/menu and Settings → Add a folder). It adds a
  remembered root and starts background work; the scope becomes searchable as results commit.
- **Indexing status:** a status pill (progress, pause/resume, "paused — playing video"), never a
  modal. It shows assets/frames completed, the active root, measured rate and an ETA range from
  completed work (`mv_ai_status`). The Windows pill does not appear for a load that is waiting
  on the viewer.
- **Settings → Local search:** per-piece install, Compute, Quality, Precision, "Index videos
  for", battery threshold, index size and cap, remembered roots, People, sharing, the Photos
  Library (Mac) and Final Cut Pro search (Mac, [23](23-nle-search.md)).

## Not hurting the viewer

Indexing is a background load on the machine and GPU of a viewer whose point is smoothness.

- **Inference workers:** a thread role of their own at OS background priority (I/O too), at
  most `min(2, cores / 4)`, never the pool that decodes for the viewport. The generation
  counter cancels stale work on navigation.
- **Yield policy** (`engine::yield_reason`, `wait_turn`): indexing waits while the host's
  present loop is busy (`should_yield`: playback, panning, springs, a slideshow, a transition,
  and for 2 s after any dropped frame), while on battery below the threshold (default 30 %), and
  while the user has paused it. **Index anyway** wherever "Paused on battery" shows (search
  footer, Settings, the Mac command-bar pill's menu) sets `battery_override` for that spell on
  battery; it is never saved, and going back to AC or restarting ends it. The viewer's own
  yields are not overridable.
- **Loading:** opening the models (a GPU context, a gigabyte of weights, the self-test) waits for
  the same quiet viewer indexing does, and reports LOADING with `yield_reason` VIEWER meanwhile.
  Pictures become searchable first; the audio models open after them. Settings never loads the
  runtime. The self-test's CPU half is cached in `selftest.txt` (~15 s off a 40 s CUDA ViT-L
  load); the provider still runs and is checked against it every start.
- **GPU:** a vendor provider owns its own device; the CPU backend is the pressure valve.
- **Memory:** decoded-frame and tensor memory are capped; the search matrix is ~77 MB at 100 k
  L/14 frames.
- **Disk:** the index has a size cap (`index_cap_bytes`, default 8 GB; over it, indexing stops
  with `MV_AI_STATUS_INDEX_FULL`) and per-folder delete; its size shows in Settings.

## ABI

`mv.ai.1` (`mediaviewer_ai.h`), the pack's own table beside `mv_addon_get`, wrapped by
`Ai.cs` and the Swift chrome. Flat, POD, opaque ids, `[no-block]` / worker annotations, append-only
(`struct_size` tells the host which entries a pack has). Groups:

| Group | Entries |
|---|---|
| Status and settings | `status` (`mv_ai_status`), `settings_json`, `set_setting` (compute, quality, precision, battery, cap, video_index, reload), `pause` |
| Roots | `roots_json`, `index_folder`, `root_set_enabled`, `root_rescan`, `root_remove`, `root_set_media`, `folder_coverage`, `note_folder_opened`, `clear_index` |
| Search | `search_text`, `search_similar`, `result_count`, `result_at`, `result_path`, `result_thumb`, `result_snippet`, `result_duration`, `clip_matches`, `search_release`, `suggest_json` |
| People | `faces_enable`, `people_json`, `people_in_json`, `person_faces_json`, `person_rename`, `person_merge`, `face_reject`, `face_split`, `search_person`, `search_this_person`, `face_thumb`, `person_refine`, `people_dedupe`, `people_reanalyse` |
| Sharing | `export_index`, `inspect_export`, `import_index`, `transfer_json`, `transfer_cancel` |
| Photos library (Mac) | `index_photos_library`, `photos_access` |

A second export, `mv_ai_reader_get` (`MV_AI_READER_ENTRY_SYMBOL`), builds the same engine
read-only for the Final Cut Pro search agent ([23](23-nle-search.md)).

The host table (`mediaviewer_addon.h`) v2 gives the pack stills and sampled video frames as
pixels, moment thumbnails in the JPEG-512 cache, `audio_open` / `audio_read` / `audio_close`
(mono float PCM at a requested rate from the viewer's FFmpeg), `thumbnail_jpeg` /
`thumbnail_store_jpeg` (sharing), and verified family pieces. Table versions only append; the
host serves every layout from 1 up, so Import keeps loading.

## Audio

A video's **soundtrack** is indexed for what it **sounds like** (CLAP) and what is **said**
(Whisper transcripts). Standalone audio files are not indexed.

- **The option.** Settings "Index videos for" is Pictures / Sound / Both, and each remembered
  folder may override it (`root_set_media`, `MV_AI_MEDIA_*`). Unset means Pictures, or Both once
  the audio piece is installed. Adding Sound to a folder queues its clips; removing it drops
  their audio rows.
- **Sounds: LAION CLAP `larger_clap_general`** (Apache-2.0; fp16), 48 kHz, 10 s windows on a
  5 s hop, stored like frames (int8 vectors, a generic-prompt score each). It scored 87.2 % on
  ESC-50 zero-shot against 84.4 % for `htsat-unfused`.
- **Speech: Whisper** (onnx-community exports, Apache-2.0) through the merged KV-cache decoder
  with timestamp rules, a no-speech and log-probability guard, a re-listen when a window
  (including a full 30 s window) ends mid-sentence or stops early, and an energy-based refinement
  of segment starts. **small** where the picture tower runs High, **base** elsewhere. Real-time
  factor on the Windows dev box:

  | Model | CPU (2 threads) | CUDA |
  |---|---|---|
  | base | 0.075 | 0.029 |
  | small | 0.23 | 0.036 |

  Mel features match `transformers`' `audio_utils` (periodic Hann, centred reflect padding,
  Slaney mels); the tokenizers are GPT-2 byte-level BPE (RoBERTa's for CLAP, Whisper's decode)
  (`infer/audio_features.*`, `audio_models.*`).
- **Search.** `MV_AI_FIND_PICTURES` / `SOUNDS` / `SPEECH` choose the towers (none set: all).
  Each model's rows rank in their own units (margin over generic prompts for pictures and
  sounds, word coverage for speech) and merge per clip; a speech result carries its sentence
  (`result_snippet`). `MV_AI_STATUS_AUDIO_READY` and the sound / speech counts are in the status.

## People

Local face search: faces are detected in photos and video frames, clustered on-device into
unnamed **people**, and named if the user wants. Search gains people by name and "this person"
from the current frame.

**Biometric handling.** Face data lives in its own `faces.db` (`PRAGMA secure_delete=ON`), off
until a separate opt-in ("Find people in your photos", `faces_enable`), deleted in one click
(the file is destroyed), never in telemetry, crash reports or minidumps, and exported only by the
user's own index export with "Include People" ticked (off by default). Face data is used for
nothing but local search.

`faces.db` schema: `people(id, name, created_at)`, `faces(id, asset_id, path, pts_ms, x, y, w, h,
score, person_id, emb, pinned, quality, tta, spec)`, `rejected(face_id, person_id)`,
`no_merge(a, b)`, `scanned(asset_id, spec)`, `meta`.

Corrections: rename, merge, "not this person" (`face_reject`), split (`face_split`); they
persist and re-apply.

### People model

**Detector:** YuNet 2023mar (MIT), 640 px input, confidence ≥ 0.8, NMS IoU 0.3, faces at least
4 % of the short side; on CPU. **Embedder: AdaFace IR-50 (WebFace4M), fp16**, 512-d, on the
ArcFace 112 × 112 template with flip averaging (below). It runs on the towers' compute choice
(Core ML ~6 ms a face; CUDA with the `ai-cuda` piece) and keeps the provider only if a fixed
crop agrees with CPU at cosine ≥ 0.99. Windows without the CUDA piece runs it on CPU, on the
background workers. Spec key `yunet-2023mar+adaface-ir50-webface4m/1`.

Chosen on LFW through our own pipeline (13,233 photos; the 6,000 standard pairs; 5,985 photos
of 423 people for clustering, at full size and with faces shrunk to ~40 px and ~24 px):

| Embedder (licence) | Size | LFW | TAR@1e-4 full | @1e-4 40 px | @1e-4 24 px | F 24 px | CPU, 1 thread | Core ML |
|---|---|---|---|---|---|---|---|---|
| SFace 2021dec (Apache-2.0) | 39 MB | 99.38 % | 99.00 % | 98.52 % | 94.33 % | 0.958 | 17 ms | — |
| AdaFace IR-18 WebFace4M (MIT) | 96 MB | 99.43 % | 99.62 % | 99.40 % | 97.26 % | — | — | 17 ms |
| **AdaFace IR-50 WebFace4M (MIT)** | 175 MB / **87 MB fp16** | **99.80 %** | **99.93 %** | **99.92 %** | **99.81 %** | **0.998** | 97 ms | **6 ms (fp16)** |
| AdaFace IR-101 WebFace12M (MIT) | 261 MB | 99.80 % | 99.94 % | 99.94 % | 99.90 % | 0.998 | 2× IR-50 | — |

fp16 IR-50 agrees with fp32 at cosine ≥ 0.99999 and settles to the same people. There is no
upstream ONNX: `tools/package/face-export.py` re-declares the IR backbone, loads the pinned
safetensors as data, exports fp16 weights with fp32 input/output, and checks ONNX against
PyTorch; `ai-models.json` pins the source with an `export` block whose `golden` values verify a
re-export by output, not bytes. Pack builders need `requirements-export.txt`; the app does not.

**Thresholds** (`model.json` → `face_tuning`; `faces.h` holds SFace-era fallbacks), at IR-50's
FAR ~1e-4 point: **same_person 0.30, keep 0.20, keep_weak 0.24, margin 0.10, ambiguous 0.04,
merge_at 0.32.** Through the real code (`mv_ai_tests "[.people-bench]"`), BCubed on the 5,985
faces:

| | Precision | Recall | F | People (423 true) |
|---|---|---|---|---|
| SFace, full size | 0.9938 | 0.9938 | 0.9938 | 433 |
| SFace, ~24 px faces | 0.9972 | 0.9760 | 0.9865 | 473 |
| **IR-50 (0.30), full size** | **0.9994** | **0.9982** | **0.9988** | 426 |
| **IR-50 (0.30), ~24 px faces** | **0.9994** | **0.9966** | **0.9980** | 429 |

**Re-run** (`people_reanalyse`; "Re-analyse faces" in Settings → People on both platforms). It forgets what the People pass scanned and the workers analyse every photo
and clip again. faces.db records each vector's embedder (`faces.spec`) and compares only vectors
of the current one. A re-analysed face whose box matches an existing one (IoU ≥ 0.5) **keeps its
row: id, person, name, pin, "not this person"**; a new face waits unassigned; an old face the
new pass does not find goes when its asset is done. Until then old faces still show their person
but are compared with nothing. When every asset is done and indexing is idle, the control thread
**settles** once: a full refinement with no focus (up to three calls until nothing moves), then
the merge. Pinned faces never move. A new face model in a pack update starts the same re-run by
itself (faces.db notes the spec it last opened with). Progress: `MV_AI_STATUS_PEOPLE_RERUN` /
`_SETTLING`, `people_scan_total` / `_done`, `people_model_utf8`. Settle cost: ~0.3 s for 6 k
faces on the control thread.

### Online clustering

`faces.cpp`. Scores are mean pairwise cosine (sum · v / n) everywhere — assign, "this person",
consolidate — not cosine to a normalised centroid (which inflates a stranger by ~√ρ). A new face
joins the best person at `same_person`; one with a second person within `ambiguous` of the best
waits **unassigned** (`person_id` NULL). The idle consolidate merges at mean pairwise `merge_at`,
an average that stays an average after a merge (no chaining). The user's faces are **pinned**:
faces they split out; the cover of a person when they name it and of both sides when they merge;
covers prefer pinned faces.

**Quality** = min(size, sharpness, frontal) × (0.6 + 0.4 × detector score): size ramps 40 → 112
px, sharpness is the Laplacian variance of the aligned crop's luma on a log ramp 20 → 300,
frontal is the nose's offset from the eye midline against half the eye distance. Weak = quality
< 0.35. Rows without a recorded quality get a proxy from score and box size.

### People refinement

`face_refine.h/.cpp` (pure), `faces_db::refine_begin / refine_commit`, `engine::person_refine`.
Per person: *refs* = pinned + good-quality members (≤ 256); *anchors* = the pinned faces plus the
refs' medoid (for a person with pins, the medoid only if it is within `same_person` of a pin, so
an impostor majority cannot define a named person); *core* = refs within `same_person` of an
anchor (one hop); *exemplars* = pinned then core, ≤ 12. A face's **support** by a person is the
mean of its top 3 cosines to that person's exemplars. Candidates are shortlisted by the core
mean, the top 3 re-scored by exemplars. For every face that is not pinned:

| Verdict | Rule |
|---|---|
| **move** to b | support(b) ≥ join (`same_person`) and beats both its own person and the runner-up by `margin`; never a weak face; never into a person it was rejected from |
| **evict** to unassigned | support(own) < `keep` (`keep_weak` for a weak face) |
| **admit** (unassigned) | the same test as move |
| **regroup** | unassigned good faces within join of a leader and at join mean to the group so far (average linkage), ≥ 2, become a new unnamed person |

Passes are Jacobi (every face judged against the same prototypes, so order never matters), up to
4, until nothing changes; a face changes at most once per call, and keep < join is the
hysteresis that stops flapping. Later passes rebuild only the persons the previous one changed.

**Re-looking at the thumbnails:** flip averaging — the mirrored aligned crop is embedded too and
the vector is the normalised mean (one more embedder run per face; `tta`). Borderline faces from
before it are re-analysed, 16 assets per call and once per asset per session, replacing vectors
box by box (IoU ≥ 0.5 keeps the row's id, person and pin).

**Refinement runs only when the user asks:** "Refine faces" in the person's sheet (Mac) or detail
pane (Windows) calls `person_refine` on a worker. It takes a full snapshot and judges **only that
person's faces** (`refine_input::focus`): a face may move to someone else, leave to unassigned,
or with others that left together become a new unnamed person; nothing is admitted into the
person. Snapshot and commit hold the People lock, the compute holds nothing. The commit skips any
face the user (or a scan) changed after the snapshot. The chrome reports how many faces left.

Cost (Apple M5, one thread, synthetic 128-d vectors): 34 ms at 5 k faces / 100 people, 135 ms at
20 k / 300, 2.7 s at 100 k / 1,500 for a full call. Tests: `tests/test_ai_face_refine.cpp`
(`[refine]`).

### Merge duplicates

`people_dedupe`, a button in both chromes, only on request:

1. **Every face re-checked:** the refinement with no focus, then `refine_commit` (the user's
   later changes win).
2. **The same person twice** (`find_duplicates` in `face_refine.h`, no lock held). Two people
   link when each one's exemplars **vouch** for the other's at `join` (the refinement's support,
   averaged, both ways, the smaller kept). Links group strongest first by **complete linkage**
   (a group takes a person only when every member links to it, so a chain of lookalikes cannot
   walk). Exemplars are pinned and good-quality faces, so weak crops cannot hide a duplicate the
   idle whole-cluster merge misses. Never linked: two people named differently, a pair a split
   kept apart (`no_merge`), a pair where a face of one was rejected from the other. Cores below
   0.24 mean pairwise are not scored.
3. **Who survives:** a named person over an unnamed one, then more faces, then older; the
   others merge into it (`faces_db::merge_auto`). Nothing is pinned.

The chrome says "3 people merged, 12 faces moved." or "No duplicates found, and every face
matches." Undo is Split (a split pair never merges again on its own). Tests: `"[dedupe]"`.

### People in the open folder

`people_in_json(scope_dir, scope, …)`: `people_json` narrowed to the people with a face in a
photo or clip under `scope_dir` by the same scopes `search_text` takes. `faces` counts their faces
there and the cover is the clearest of those, so a card's count and face are the folder's. *Who
qualifies at all* (minimum faces, or a name) is still judged over the whole index.
`faces_db::people(min, assets)` does one indexed pass per person against the scope's asset set.

Chrome: a "People in · This folder | + Subfolders · <folder>" control at the top of the People
grid (Settings → People on both platforms). Default **+ Subfolders**; it
follows the folder the viewer opens. Everyone shows only when no folder is open (the viewer's
home): the control is then absent. A scope
of Everywhere left from before reads as + Subfolders when a folder opens. "Show photos" on a card
and the empty-grid wording search the same scope. The choice is not persisted. The status bar's
people count stays the whole index's. A pack without the entry shows everyone. The search panel
keeps all three scopes.

## Sharing an index

Index a library once and carry it to another machine (`src/addons/ai/transfer.*`,
`export_index` / `inspect_export` / `import_index`, both chromes).

**The file** (`.mvindex`, SQLite, `info.format = "mediaviewer.index"`, version 1) holds the chosen
roots with each asset's path **relative to its root** (`/`-separated), its `(mtime, size)`, kind
and duration, and its rows: `progress` (done and partial only), `frames`, `speech`. Optional
(`MV_AI_TRANSFER_*`): **People** (`people`, `faces` without a path, `rejected`, `no_merge`,
`face_scanned`) and **thumbnails** (`thumbs`: the JPEG-512 cache's bytes for a still and each
stored moment; one the cache lacks is made then, between the viewer's busy spells). Roots keep
their original absolute path and a display name so an import can offer the same place. Written
to `<dest>.part` and renamed. The export reads index.db and faces.db on its own read connections,
so indexing is not paused by it.

**Import** maps each root in the file to a folder here (the chrome pre-fills folders that exist).
Per asset at `folder + rel`:

| Here | Does |
|---|---|
| no row | the row and its vectors are added (`seen` 0) |
| same `(mtime, size)`, already done for a spec | this machine's rows for that spec stay |
| same `(mtime, size)`, not done | replaced by the file's |
| different `(mtime, size)` | this machine's row stays |

The imported roots are then rescanned by the ordinary delta: a file whose `(mtime, size)` differs
from the file's row loses its imported vectors and is queued. Indexing pauses for the import (it
runs on the control thread) and the search matrix, sounds, transcripts and People reload after.

**Models.** Picture rows are imported for a spec this pack is using. When this machine's index is
empty and the file's tower is one the pack carries, the import **adopts it** (Quality is set to
that tower), so nothing is re-embedded. Otherwise picture rows are skipped and the chrome says
which Quality would use them (`inspect_export`'s `picture_usable`). Sound and speech rows are
imported for the loaded audio piece's spec, else skipped and counted.

**People** (biometric): off by default on both sides, with a warning that the file then
identifies the people in it. An import with People needs the ai-faces piece and turns the People
opt-in on. A named person joins the local person of the same name (case-insensitive), else
becomes a new one; unnamed clusters come in as new people. Assets whose faces this machine has
already scanned keep this machine's faces. Only the current face model's faces are exported.

**Thumbnails** go through the host's `thumbnail_jpeg` (look up, never make) and
`thumbnail_store_jpeg`; the host checks the bytes are a baseline JPEG no larger than 512 on the
long edge before storing (a shared file is untrusted input), and the pack stores one only when
the local file's `(mtime, size)` equals the file's row.

It is a one-off merge, not a sync. The Photos library root is never exported (its keys are this
Mac's PhotoKit identifiers). The file carries folder and file names and, if ticked, faces and
names; nothing about it is logged or sent.

## Photos library source (macOS)

A Photos result's path stays its key: the host lists it as a virtual item and resolves it to the
file as it is shown, the same mechanism that opens the whole library as a folder once it was added
in Settings ([26](26-photos-library.md)). The chrome prepares no files itself.

The system Photos library (where iCloud Photos lives on a Mac) is one more remembered root beside
folder roots, feeding the same indexer, index and search. Mac only: the source sits behind a
portable interface (`src/addons/ai/photos_source.h`; PhotoKit in `photos_mac.mm`,
`photos_none.cpp` elsewhere). On Windows, iCloud for Windows syncs to a folder, which a folder
root covers.

**Source, not a folder.** An asset is keyed `photos:<localIdentifier>` under one root whose path
is `photos:`. `(mtime, size)` is `(modificationDate, pixel count)`, so an edit in Photos
re-queues the asset. The scan is PhotoKit's enumeration of the library: iCloud Photos included,
shared albums and the Hidden album excluded, one row per burst and per Live Photo. A
`PHPhotoLibraryChangeObserver` rescans at most once per 10 s; the full re-enumeration is the
delta (0.9 s cold, 0.4 s warm for 23 k assets).

**Local only, read-only.** Every PhotoKit read has network access off (except the opt-in fetch
below); nothing is written to
the library. A still is embedded from PhotoKit's local rendition at the indexer's size (the edit
as Photos shows it, colour-matched to sRGB). A clip is sampled from its local file
(`requestAVAsset`), opened read-only. An asset only iCloud has is **unavailable** (progress state
4), neither done nor failed:

- `mv_ai_status.assets_unavailable` counts them; Settings says "N only in iCloud".
- An iCloud-only clip is found by its local poster, stored as one row at 0 ms.
- Once per launch, and when access returns, each is asked whether it is on this Mac now; only
  those that are go back to pending. The People pass treats them the same way.

**Downloading iCloud-only clips (opt-in, 2026-10-05).** Settings → Photos Library → *Download
iCloud videos to index them* (`"icloud_videos"`, off by default; [12](12-decision-log.md)). The
engine's fetch thread (`engine::fetch_loop`) takes unavailable clips newest first and downloads each
with `photos_source::fetch_video` (`PHAssetResourceManager`, network on; the current edit, else the
original) into `<data>/cache/icloud/<row id>.<ext>`:

- At most two on disk (`kFetchAhead`). Each is put back to pending (`requeue_unavailable`, its
  poster row dropped) and `claim` takes the fetched clips' pictures, sound, speech and People work
  before anything else (`index_db::pending_among`), so a download is indexed at once.
- `file_of` answers the downloaded file for that `photos:` key; `fetch_reap` deletes it once no track
  wants the clip. A file left by a crash is deleted at the next start, and its clip, still pending,
  goes back to unavailable and is fetched again.
- It waits (`mv_ai_status.icloud_fetch`) on battery, on an expensive or constrained network
  (`network_unmetered`, `NWPathMonitor`), under 10 GB free, and while indexing is paused. A clip iCloud
  does not send is skipped until the next start. `icloud_videos_left` / `_fetched` and the download's
  progress are in the status; the status line says "Downloading from iCloud (42%) · 1,847 clips left".
- Photos are never fetched: PhotoKit keeps a local preview of nearly every photo, which is what the
  picture pass reads.

**Permission.** The pack never raises the system prompt itself. Settings → Local search →
*Photos Library* → **Add Photos Library** asks from that click, then calls
`index_photos_library`. Until then the source answers `permission_denied` without touching the
library. When access is off (denied, restricted, revoked), the rows stay searchable and the root
is left out of the work; Settings offers *Open Privacy Settings*. The base app carries
`NSPhotoLibraryUsageDescription` and the hardened-runtime entitlement
`com.apple.security.personal-information.photos-library`
(`packaging/macos/MediaViewer.entitlements`), which cannot live in the pack.

**Search and results.** `scope_dir "photos:"` scopes a search to the library: the panel's *Look
in* gains a **Photos** segment once it is indexed; *Everywhere* includes it. A Photos tile is
drawn from PhotoKit's cached rendition (no second cache) with a small Photos badge.

**Opening a result.** The viewer reads each result where Photos keeps it (the current
rendition's file, `requestContentEditingInput` / `requestAVAsset`); there are no copies. The
host refuses every write to it (`src/shell/write_guard.h`): any path inside a
`*.photoslibrary` bundle, and any file the result list registered.

- **Refused:** Move to Trash, Move To, ratings and every metadata edit (the pane's edit controls
  show disabled), rotate / flip, Save copy / export, trims and clip exports. A refusal beeps and
  says "From your Photos library: read-only here."
- **Allowed:** Copy To and drag-out, because both copy.
- **`⌘E`** shows the result in the Photos app, selected, not its file in Finder
  ([26](26-photos-library.md) "Show in Photos").

**An original only iCloud has** opens at once as Photos' best local picture,
`<name> (preview).jpg`, in `~/Library/Caches/MediaViewer/Photos Library/`. If the user stays on
it for 400 ms, the original is fetched from iCloud (the only network request, made on the user's
own viewing) and replaces the preview in place. Downloads are deleted when the next list opens
and at quit; the folder is emptied when the chrome attaches. Scrub markers, N / Shift+N and Find
similar on an opened Photos result ask about the asset, not the file. The index never downloads.

Harnesses: `tools/ai/photos-spike.sh` (the source alone) and `tools/ai-bench/photos-bench.sh`
(the whole pack).

## Measurements

### Verified on Windows

Ryzen 16-thread, RTX 4070, ORT 1.30.

- Embeddings within cosine 0.999 of the ORT-Python reference on CPU for both towers; CUDA
  0.9991–0.99999 (`mv_ai_tests`, `tests/data/ai/reference.json`). Audio FFT / mel features match
  the Python reference (`tests/data/ai/audio_reference.json`); CLAP at cosine ~1.0; Whisper base
  and small find every sentence of the speech clip at its time (`test_ai_audio.cpp`).
- Engine suite over the real host table with fake models (`test_ai_engine.cpp`): indexing,
  resume without redoing committed frames, re-queue on edit, delta on reopen, removal, scope,
  grouping, dedupe, yield, pause, migration never mixing specs, clear, size cap, find-similar,
  people corrections, face deletion leaving no faces.db.
- Query scan: 100,000 ViT-L int8 rows (2,000 clips × 50, with per-asset stats) in **31 ms** on
  one core (`mv_ai_tests "[perf]"`, which fails over 100 ms in optimised builds); with the ~30 ms
  CUDA text tower a query is ~60 ms.
- A 1-hour 4K HEVC clip (synthetic, with a soundtrack): pictures, sound and speech in **302 s**
  on CUDA with ViT-L and Whisper small — model load ~25 s, 255 picture moments ~190 s, sound
  ~70 s, speech ~17 s. The picture phase is bounded by the sampler's software 4K HEVC keyframe
  decode, not the model.
- PR 1 soak with every piece sideloaded: 0 dropped frames, p99 16.9 ms.

### Verified on macOS

Apple M5, 24 GB, macOS 26.6, ORT 1.30. Core ML with the image tower's dimensions pinned:

| Tower | CPU (2 threads) | Core ML, pinned | cosine to reference (Core ML) | first compile | cached open |
|---|---|---|---|---|---|
| ViT-B/32 fp16 | 73 img/s | **499 img/s** | 0.9992 | 82 s | 17 s |
| ViT-L/14 fp16 | 4.9 img/s | **31 img/s** | 0.9983 | 5.3 min | 64 s |

Re-measured 2026-10-05 on the same Mac (macOS 26.6), a standalone probe with the pack's settings:
B/32 opens in 99 s cold / 20 s cached (peak 6.8 / 5.0 GB); **L/14 takes 493 s from its cache, peak
12.7 GB**, and a cold compile was still running after 28 min. Core ML re-compiles on every load and
writes the program as text (ORT inlines the transposed `linear` weights: `model.mil` 1.05 GB for
B/32, 3.5 GB for L/14), single-threaded. So on this Mac an L/14 start runs on CPU for minutes, and
a failed upgrade left it on CPU for good (the 2026-10-05 fallback). Not yet fixed: shipping the
large tower so Core ML need not rewrite it each load.

With shapes left dynamic ORT placed only 130 of 830 (B/32) and 250 of 1,622 (L/14) nodes on
Core ML, no faster than CPU. `MLComputeUnits` stays `ALL`. The Mac checklist is
`src.swift/AIChrome/MAC-VALIDATION.md`.

Photos library (a real 23,089-asset library, Optimize Mac Storage on): local stills at 448 px
304–411 per s (p50 4.3–5.7 ms) on 2 threads; resolving a file to open p50 7.0 ms; the source's
peak footprint 70 MB (212 MB with opening). PhotoKit is not the bottleneck; the image tower is.
The whole pack handled every asset (19,007 indexed, 4,082 iCloud-only, 0 failed) at ~23 assets/s
on Core ML; queries 17–30 ms after a 1.7 s first query. The Mac PR 1 gate run beside a
non-yielding bench process indexing on every core: 0 dropped frames in 3 of 4 runs, one missed
refresh in the fourth, p99 ≤ 17.1 ms.

## Not built

- Hardware decode (D3D11VA / VideoToolbox) for the frame sampler; it decodes in software.
- An OpenVINO piece (the code path exists, nothing is published).
- A memory-mapped search matrix (it is loaded into memory).
- Local search on Intel Macs (no x86_64 ORT).
- OCR, places, EXIF capture dates or camera in the index and query language.
- A "nothing found" z calibration for CLAP.
- Background (unrequested) People refinement; the incremental refinement path
  (`refine_begin(false)`) exists in faces.db and its tests but has no caller.
