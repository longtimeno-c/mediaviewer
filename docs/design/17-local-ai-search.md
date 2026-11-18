# 17 — Local AI search (video moments and photos)

How Local search works: the AI pack add-on, its models and runtime, the frame sampler, the
index, search and "nothing found", the query language, audio, documents, People, index sharing,
the phone's catalogue, the macOS Photos library source, and staying out of the viewer's way.

## What it is

An opt-in add-on (the AI pack), installed from Settings on Windows and macOS; without it the
feature is hidden. Type "man on a broom" and get **moments**: a video frame with its timestamp, or
a photo; Enter opens the clip at that moment; "Find similar" queries with the frame on screen. It
searches folders the user chose, entirely on this machine: low-priority workers, results through
the completion queue ([14](14-abi.md)), a separate SQLite index (nothing written to media), and no
embeddings in telemetry or crash reports.

## Model choice

A CLIP-family dual encoder; search is a dot product. Frames embed once at index time.

| Quality | Tower (Xenova ONNX of OpenAI CLIP, MIT, fp16) | Dim | Auto picks it when |
|---|---|---|---|
| Fast (1) | ViT-B/32 | 512 | CPU only |
| High (2) | ViT-L/14 | 768 | an accelerated provider (CUDA, Core ML) runs it and has not failed it here |

Vectors are stored int8 with a per-vector scale. Each tower's `model.json`
(`tools/package/ai-models.json`) holds thresholds, generic prompts and preprocessing: `dedupe`
0.97, `query_margin` 0.04, `query_z` 2.5, `result_z` 2.0, `result_margin` 0.015, `similar_min`
0.62; prompts "a photo.", "a picture of something.", "an image.", "a photo of a thing.", "a blurry
photo.". Changing quality migrates (the old index answers until the new completes); a query never
compares vectors from two towers.

### Model tiers (#482)

The model for each role comes from the pack's `models/tiers.json` (from `ai-models.json`'s `tiers`
block; [model-tiers plan](../plans/model-tiers.md)); code names no model folder. No `tiers.json`:
Default.

| | Maxxed | Default | Intel | Potato |
|---|---|---|---|---|
| Pictures (high / fast) | OpenCLIP ViT-H/14 / B/32 | L/14 / B/32 | B/32 | B/32 |
| Speech (high / fast) | Whisper large-v3-turbo / small | small / base | base | tiny |
| Sounds | CLAP | CLAP | CLAP, off by default | none |
| People | YuNet + AdaFace IR-50 | YuNet + AdaFace IR-50 | YuNet + AdaFace IR-50 | YuNet + SFace |
| Documents | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 |
| Clip gaps (min / max) | 250 / 1000 ms | 500 / 2000 ms | 500 / 2000 ms | 1000 / 6000 ms |

- ViT-H/14 (~2 GB, our fp16 export, `tools/package/clip-export.py`, golden-output verified) is the
  `ai-large` piece, Maxxed only.
- **Model contract** (`infer/models.h`, `audio_models.h`): a CLIP tower states `tokenizer_kind`
  (`clip-bpe`), `score` (`cosine`, or `sigmoid` ranked by cosine), named outputs, Core ML dim names;
  Whisper states `n_mels` (80, or 128 for large-v3 / turbo) and special tokens. Unknown kinds are
  unsupported. A model that fits is a manifest entry, no C++.
- **Choosing:** nothing downloads until a tier is chosen (`MV_AI_STATUS_TIER_UNCHOSEN`; picker on
  install and Settings → Local search → Models). `machine.h` recommends locally (never sent):
  Potato ≤ 2 cores or ≤ 8 GB; Intel on x86_64 Mac or a PC without accelerator; Maxxed with an
  accelerator and ≥ 32 GB; else Default. Pre-tier indexes keep Default. The control thread reads
  `tiers.json` and runs the check before models load, then posts `MV_ADDON_EVENT_AI_STATUS`.
- **Changing** tier fetches models and re-indexes while the old index answers (`open_clip_spec`
  opens the old tower by spec key); then old vectors and files go.
- **Thresholds:** `tools/ai-eval/calibrate.py` (bar COCO-1k R@10 ≥ 0.88, labelled P@5 ≥ 0.8;
  quantile match against B/32's hand-set values).
- **Licence gate:** `tools/package/ai-models.py check` refuses a licence outside the allow-list
  (CC-BY-NC fails). CLIP, ORT, YuNet, AdaFace weights: MIT; CLAP, Whisper: Apache-2.0.

### PR 20 spike results

Ryzen 16-thread / RTX 4070, ORT 1.30, COCO Karpathy-test 1 k (text → image).

| Tower | R@1 | R@10 | CPU img/s (4 thr) | CUDA img/s | size |
|---|---|---|---|---|---|
| ViT-B/32 fp16 | 0.493 | 0.888 | 23.3 | 150.8 | 303 MB |
| ViT-B/32 int8 (rejected) | 0.398 | 0.822 | 31.5 | - | 154 MB |
| ViT-L/14 fp16 | 0.555 | 0.905 | 1.9 | 98.0 | 856 MB |

L/14 on CPU is too slow for large libraries, hence High on accelerators only.

## Runtime

**ONNX Runtime**, loaded at run time (never linked), behind `src/infer` (`ort.h`, `models.h`):

```
src/infer   embedder { embed_image(s), embed_text }    clip_model, clap, whisper, face_models
            backends: cpu (always), cuda (Windows piece), openvino (code path), coreml (macOS)
```

- **ORT telemetry off:** outside Windows `dylib::open` sets `ORT_DISABLE_TELEMETRY=1` before
  `dlopen` (every load); on Windows `DisableTelemetryEvents` before and after `CreateEnv`.
- `infer/` includes no D3D, DirectML or Metal; ORT never shares the render device.
- **Compute** (`mv_ai_compute`): Auto · a provider · CPU only (Mac: Auto / Core ML / CPU). Changing
  it re-creates sessions, not the index.
- **Self-test:** a provider is kept only if it matches CPU at cosine ≥ 0.99 on a fixed input and is
  faster; else CPU with `provider_fault` (not in build, runtime missing, failed, mismatch, slower)
  and `provider_detail_utf8` (`session::last_error`, paths replaced by `<model>` / `<path>`).
- **Large-tower fallback:** verdicts in `data/provider.txt` (runtime | provider | spec → ok or
  fault, open seconds, message; `provider_verdict.h`). Where a provider failed ViT-L/14, Auto opens
  ViT-B/32 on it and sets `MV_AI_STATUS_SMALL_FALLBACK`; **Try the larger model again**
  (`"retry_large"`) forgets the verdict; a new runtime retries itself.
- **Windows:** `ai-cuda` piece (ORT CUDA 13; CUDA runtime and cuDNN 9 user-supplied on PATH).
  OpenVINO is a code path only (`MV_AI_BACKEND_OPENVINO`). No DirectML.
- **macOS:** Core ML provider over CPU, arm64 only (`cmake/darwin.cmake` has no AI target on Intel).
  Image-tower dims are pinned (`session_options::fixed_dims`, batch 4; `embed_images` splits/pads).
  Core ML opens slowly even from `data/cache/coreml`, so the pack answers on CPU and a background
  thread opens Core ML, self-tests and swaps it in (`upgrading_clip`, `pack.cpp`); **indexing waits
  for the swap** (`engine::upgrading`). A cache entry cut short by a quit is dropped next start.
  Text queries and audio models stay on CPU.
- An index records model id, spec and precision and is local to one machine (*Sharing an index*).

## The AI pack (delivery)

Installed through the add-on mechanism ([18](18-import.md#add-ons-how-import-is-installed)).

| Piece | Contents | Size |
|---|---|---|
| `ai` (Core) | `mv_ai`, ORT (CPU; Core ML on Mac), PDFium (`LICENSES/pdfium`), the chrome (`MediaViewer.Ai.Chrome` / `AI.bundle`); CLIP towers and tokenizer fetched | ~100 MB + 1.16 GB fetched |
| `ai-large` | OpenCLIP ViT-H/14 fp16 | ~1.97 GB |
| `ai-faces` | YuNet, AdaFace IR-50 fp16 | ~87 MB |
| `ai-audio` | CLAP `larger_clap_general`, Whisper base and small | ~1 GB |
| `ai-docs` | all-MiniLM-L6-v2 fp32, WordPiece vocab | ~91 MB |
| `ai-cuda` (Windows) | ORT's CUDA build | ~205 MB |

- Pieces declare `part_of: ai` and an `arch`; Install / Remove each, "Install all", "Update all";
  clicks queue Core first, and a piece's install first updates a stale Core. Remove offers to delete
  the index; `data/cache` is always deleted.
- **Models from Hugging Face:** each upstream file is in the piece's signed manifest (`remote`: URL,
  size, SHA-256, licence), fetched by plain GET of the pinned `resolve/<revision>/<file>` into
  `data/fetched/<piece>/` (`fetch.h`). Resumable; a mismatch is refetched, never loaded; a 404 is
  "gone". Picture tower first. Status `fetch_state`, `fetch_done_bytes` / `fetch_total_bytes`.
- The pack itself: plain GET of fixed release URLs. Mac: Developer ID, notarized, library
  validation.
- **Location:** `%LocalAppData%\MediaViewer\addons\ai\<version>`;
  `~/Library/Application Support/MediaViewer/Add-ons/AI/<version>`. Index and caches in `data/`.
- **Offline / sideload:** drop the pack in that folder (`tools/package/ai-sideload.py`); verified
  the same way.
- **Verification:** the first check in a process hashes every file, later ones stat and re-walk;
  only the add-on being loaded is hashed.

## Frame sampling

`src/edit/clip_sample.*`, its own software decoder, never the playback decoder.

1. **Keyframe pass:** `skip_frame = AVDISCARD_NONKEY`.
2. **Coverage fallback:** a **maximum gap** (2 s) decodes forward to fill long GOPs; a **minimum
   gap** (500 ms) stops scene-cut floods (per tier, above). The People pass alone decodes the
   moments the picture pass kept; a clip with no picture rows uses its own 2 s / 10 s grid.
3. **Dedupe:** drop a frame with cosine > `dedupe` to the last *kept* frame.
4. **Frame → tensor** (`infer/preprocess.h`): rotation and SAR, PIL-exact bicubic, model mean/std;
   HDR/PQ/HLG through the SDR tone map (`rgba_converter`) first.
5. **Record** `pts` in stream time base and ms plus a keyframe / grid-fill flag.
6. **Photos:** one row from a first-pixel-quality decode (RAW: embedded preview). Live Photo and
   RAW+JPEG pairs index once, on the still. A Live Photo's movie (`IMG_1234.HEIC` +
   `IMG_1234.MOV`; `assets.motion*`) is sampled seeded with the still's vector and stored on the
   still's id with `pts_ms`, ≤ 32 moments; a moment's result is the movie (kind clip). Still commits
   first (partial), movie after (done); a changed movie re-queues only the motion. Online-only
   movies only with *Index online-only files*. Photos library Live Photos stay a still.

## Index store

`index.db` (SQLite, WAL) in `data/`, apart from the thumbnail DB (`index_db.cpp`):

```
meta(key, value)
roots(id, path, recursive, enabled, last_scan_at, media, docs)
assets(id, path, root_id, mtime, size, kind, duration_ms, seen, cloud,
       tags, taken_at, lat, lon, camera, lens, made_with, ai, place, country, region,
       rating, label, iso, aperture_x100, focal_mm, shutter_den, shutter_us,
       motion, motion_mtime, motion_size, motion_cloud)
asset_text(rowid = asset id, text)                                 -- FTS5
progress(asset_id, spec, state, resume_ms, tries, indexed_at)      -- per asset and spec
frames(id, asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags, generic, scale, emb)
speech(id, asset_id, spec, start_ms, end_ms, text)                 -- Whisper lines
cues(id, asset_id, start_ms, end_ms, kind, text)                   -- chapters, subtitles, picture text
passages(id, asset_id, spec, page, ord, text)                      -- documents
passage_text(rowid, words)                                         -- FTS5
passage_vecs(passage_id, spec, generic, scale, emb)
tracks(asset_id PRIMARY KEY, points BLOB)                          -- a clip's GPS route
```

`emb` int8, `scale` its scale, `generic` its best cosine to the generic prompts. `kind` 3 is a
document, 4 audio. Each schema step is one transaction skipping existing columns; new indexes are
made at 3 and migrated forward.

| Step | Adds |
|---|---|
| →2, →3 | `roots.media`; `assets.cloud` |
| →4 | tag columns, `asset_text` |
| →5 | `rating` (1..5, −1 rejected, 0 unrated), `label`, `iso`, `aperture_x100` (f/2.8 → 280), `focal_mm`, `shutter_den` (N of 1/N s), `shutter_us`; NULL until read and for clips |
| →6 | `cues` |
| →7 | `assets.motion*`; next walk queues each motion alone |
| →8 / →9 | `kind` 4 audio / `kind` 3 document, `roots.docs`, `passages`, `passage_text` |
| →10 | `passage_vecs` |
| →11 | `tracks`; non-clip rows at tag level 2 move to 3 |

Also in `data/`: `faces.db` (People), `labels.f32`, `selftest.txt` (CPU self-test reference by
runtime, provider, model, piece).

### Schema 4 (#291, 2026-10-12)

- **Tags:** `tags` is a level (0 unread, 1 basic, 2 rating/exposure, 3 route; `kTagsLevel`; a
  changed file resets to 0). `taken_at` = camera wall clock read as UTC (Photos: `creationDate`);
  `lat` / `lon` WGS 84 (a route's first fix); `camera`, `lens`, `made_with`, `ai` (positive evidence
  only). `place` / `country` / `region` stay NULL (places are [27](27-places.md)'s).
- **`asset_text`:** FTS5 `unicode61 remove_diacritics 2`; the search fields (camera, lens,
  made-with, software, title, caption, keywords, creator; [06](06-metadata.md) "Search text") joined
  by U+001F. The engine matches them in memory with the hosts' fold; `index_db::match_text` quotes
  every word (`fts_phrases`). Without FTS5 a same-shaped plain table (`meta('asset_text')`). Numbers
  are never search text.
- **Backfill:** host table `tag_facts` (`MV_ADDON_TAG_FACTS_V2_SIZE` adds rating/exposure) from the
  host's `meta_cache.db`; the control thread fills rows below `kTagsLevel` after each walk, 128 per
  transaction, ≤ 2 s a turn, yielding to `should_yield`. A host lacking a level stops the backfill
  there for the run. Level 3 asks `route_of` (128 points) only for `MV_ADDON_TAG_HAS_ROUTE`.
  Level 4 (schema 12; `MV_ADDON_TAG_FACTS_V3_SIZE`) takes `duration_ms`, the clip's header length
  from the same bounded read, into `assets.duration_ms` (a 0 leaves it be; the content pass's
  sampler length still overwrites it). So a clip has its length — and the phone's catalogue
  shows it — right after the walk, before the content pass reaches it or if the sampler cannot
  open it. Schema 12's step moves level-3 rows with nothing to add (not a clip or audio, or a
  length already known) to 4, so an upgrade reads only clips without one. The publish stamp
  (`content_stamp`) includes `TOTAL(duration_ms)`.
  `wait_idle` waits for it. Exports carry no tags.
- **`tracks.points`:** ≤ 128 (`kTrackPoints`) evenly spaced, ends kept, int32 microdegree (lat, lon)
  LE. Served by `located_assets` / mv.ai.1 `each_location` as `MV_ADDON_LOCATION_ROUTE_POINT` rows
  after the asset's own row ([27](27-places.md) "Routes").

**Roots and walks:**

- **Folder roots persist.** A `roots` row per indexed folder; reopening runs a startup delta scan,
  then the watch queues only changes. Roots can be paused, rescanned, removed.
- **The host watches each folder root** that is enabled and not held (`watch_tree`; `io::tree_watcher`
  in `src/io/tree_watch.h`): Windows `ReadDirectoryChangesW` incl. attributes (`tree_watch_win.cpp`);
  Mac one FSEvents stream, 0.5 s latency (`tree_watch_mac.cpp`); Linux none (`tree_watch_none.cpp`).
  Changed folders are walked once quiet 2 s, or 10 s after the first (`engine_deps::tree_quiet_s` /
  `tree_max_wait_s`): own files (`scan_dirs`, `walk_files2` depth 0), or to root depth for a new,
  renamed or vanished folder. `index_db::end_scan_dirs` deletes only under walked folders and only
  when the walk vouches: a partial failure (`MV_ERR_IO`) adds, deletes nothing and marks the root
  "error"; an unlistable folder keeps its rows (`engine::keep_unlistable`). > 64 folders or
  `MV_ADDON_TREE_OVERFLOW`: whole root. Backstop walks: network shares and unwatched roots every
  15 min, other roots hourly. `roots_json` `"watching"`.
- **Staleness:** `(path, mtime, size)` or `spec` change re-queues; no content hashing. Resume is per
  asset and spec (`resume_ms`).
- Tiles are the **JPEG-512 cache**; moments are `path#t=ms` rows via the host table.
- **Unfinished roots wait for Resume** (`engine_options::hold_unfinished`, `addon_entry.cpp`):
  - A root with work left at engine start (pictures, or any pass in `resume.json`) is **held**: not
    walked, queued, fetched or counted until `set_setting("resume_root", <id>)` (0 = all), Rescan,
    re-enable or Index this folder. Search uses what is committed; `roots_json` `"held": true`.
  - `data/resume.json` (after each walk, ≤ every 5 s, on change, `.tmp` + rename):
    `{"version":1,"roots":[{"id","path","kind","done","total","held"}]}` (`engine::work_left_in_root`;
    `done`/`total` count pictures).
  - **Launch notice** without loading the pack (Mac `addons_mac.mm` via `addon/ai_resume.h`,
    `ResumeNoticeView.swift`; Windows `IslandHost.ResumeNotice.cs`): "Resume indexing Photos?" in the
    canvas corner, no focus. Resume loads the pack and sends `resume_root` 0; Dismiss loads nothing.
  - Settings shows a held root as "N of M · paused since last time" with **Resume**.
- A root whose walk fails keeps its rows (`"error":"unreadable"`).

## Cloud files in indexed folders

`walk_files2` lists online-only files flagged (`FILE_ATTRIBUTE_RECALL_ON_*` / `SF_DATALESS`), never
following links or junctions; table v1's `walk_files` omits them. `assets.cloud` marks them: no pass
reads one and they are not work; status says "N only in OneDrive / iCloud Drive"
(`cloud_files_left`, roots `cloud_only`).

**Index online-only files** (`cloud_files`, off by default): the fetch thread brings a file down in
place (`CfHydratePlaceholder` in 16 MB slices / `startDownloadingUbiquitousItemAtURL`), every track
takes it first, then it is evicted (`CfDehydratePlaceholder` / `evictUbiquitousItemAtURL`). ≤ 2
files on disk, shared with iCloud clips. Waits on battery, metered network, < 10 GB + file free, and
pause. Refused files skip until next start. Fetched ids in `<data>/cache/cloud-fetched` for crash
cleanup. Port `src/addons/ai/cloud_files.h` (`cloud_win.cpp`, `cloud_mac.mm`, `cloud_none.cpp`);
`cldapi.dll` delay-loaded, absent → `"cloud_supported":false`.

## Search

- **Query encode:** text tower, cached; a picture query's last noun is embedded singular and plural
  and averaged (`query::number_forms`).
- **Scan** (`vectors.h`): brute-force int8 dot product over an in-memory matrix (~77 MB at 100 k
  L/14 rows, ~31 ms one core); no ANN. Removals tombstone in one pass with ≤ 1 compaction
  (`remove_assets`); paths resolve via a `path_key → id` map.
- **Ranking:** top frames **grouped per clip**, best moment first, "N more in this clip"
  (`mv_ai_result`). `find_text` ≤ 5,000 rows; a result list ≤ 1,000.
- **Order.** Best match first when something answers the description; otherwise (gate holds every
  row, or filters only) everything the query allows, **newest by date created first**
  (`engine::newest_first`: `taken_at`, else Photos `created`, else mtime; ties by path). Hosts keep
  the order.
  **The phone** (owner, 2026-11-14) ranks such a description instead: every allowed picture by its
  margin over the generic prompts, best first, no cut-off (`engine_options::rank_when_unanswered`),
  so "dog" shows the dogs the gate held back. Filters alone still list newest first.
- **Scope:** `MV_AI_SCOPE_TREE` (open folder and subfolders, default) · `MV_AI_SCOPE_ALL`; kind and
  tower filters. No folder open: everywhere. Opening a folder re-asks the search.
- **Find similar:** the still or paused frame; stored vector if indexed, else embedded on demand.

### Nothing found

Rule (`vector_store::scan_stats`, `find_text`):

- **Margin:** a row's score minus its `generic`; a result row needs `result_margin`.
- **Stand-out z:** `top10_z` = SDs the ten best **assets** sit above the mean of every asset's best.
  Noise grows with N (~2.2 at 300, 2.6 at 1 k, 3.3 at 10 k), so stands out = z ≥ max(`query_z`,
  1.18 × noise expectation).
- **Gate:** L = log10(assets / 1,000) (≥ −1); pass on best margin ≥ `query_margin` × (1.375 +
  0.125 L), or stand out with best margin ≥ `query_margin` × 0.375 (1 + L). < 100 assets: margin
  alone.
- **Broad subjects** pass at the plain margin when ≥ max(5, 1 %) assets clear it
  (`scan_stats::over_margin`).
- **Result z:** once standing out, rows ≥ `result_z` SD above the mean are also results.

### Nothing found at scale

`vocabulary.h`: 705 everyday labels embedded once per tower (`labels.f32`). Each row stores its
ninth-best label score; a row is a result only when the query scores at least that. Bars fill in
chunks at load (a row passes until filled). Target at 25 k assets: ≤ 1/3 nonsense answered on L/14,
≤ 1/8 on B/32.

### Sounds (CLAP)

CLAP's margin barely separates, so from `kMinAssets` (30) clips a sound query uses the stand-out
rule alone (`text_thresholds::stand_out`): answered when its ten best stand out at `stand_out` (1.06)
× noise z; rows ≥ `result_z` (3.0) SD and over `result_margin`; rank among pictures = z ×
`rank_per_sd` (0.2); a clip found both ways keeps its better rank. Below 30 clips, the margin rule.

### Precision scale

Setting `precision` 0–4 (default 2), read per search (`precision_scale::at`):

| Level | stand-out × noise | query margin | row margin | row z | keep within Δ SD of best |
|---|---|---|---|---|---|
| 0 Broadest | 1.13 | × 0.8 | × 0.33 | × 0.8 | - |
| 1 | 1.15 | × 0.9 | × 0.67 | × 0.9 | - |
| **2** | **1.18** | **× 1** | **× 1** | **× 1** | - |
| 3 | 1.26 | × 1.125 | × 1.5 | × 1.2 | 3 |
| 4 Strictest | 1.38 | × 1.25 | × 2 | × 1.4 | 2 |

Each stricter level answers a subset of the looser. CLAP scales alike; speech needs all query words
at 3–4, half at 0–1.

### Query syntax

One parser (`src/addons/ai/query.*`, `mv_ai_tests "[query]"`) for both chromes, Final Cut Pro
([23](23-nle-search.md)) and voice ([19](19-voice.md)). Terms AND.

| Typed | Means |
|---|---|
| `Tristan beach` | Tristan's photos / moments ranked by "beach" (no "nothing found" inside a person) |
| `Tristan "hello"`, `Tristan:"hello"`, `said:hello` | words in order in a transcript, chapter, text subtitle or picture text; the line is the snippet. Quotes also match documents; `said:` never |
| `text:lease`, `says:"net 30 days"`, `-text:draft` | document text only, at its page |
| `is:document` / `is:doc`, `is:pdf`, `is:docx` (`is:word`), leading/trailing `documents` / `docs` / `pdf(s)` | documents; format by extension |
| `is:audio` / `is:music` / `is:song`, leading/trailing `audio` / `songs` / `tracks` | audio files (bare `music` is a description) |
| `Tristan Aaryan`, `Tristan and Aaryan` / `Tristan or Aaryan` | both in one photo or clip / either |
| `Anna Smith`, `anna` | a named person, full or first name, folded |
| `@tri`, `person:"anna s"` | name prefix or near spelling; no such person: nothing found |
| `Trist`, `tristna` (alone) | matching people first, then what the word describes |
| `-Nico`, `-beach`, `-"goodbye"`, `-video` | exclude a person, description, phrase, kind |
| `beach video`, `is:photo`, `type:video`, `videos of Anna` | a kind (last word, `is:` / `type:`, leading "videos of"); "photos of …" is anything |
| `in:2024`, `in:2024-06`, `before:`, `after:`, `since:`, `until:` | date taken (`taken_at`, no TZ conversion), else mtime (UTC) |
| `file:IMG_12`, `file:"trip 2024"`, `-file:copy` | file name contains, folded |
| filters alone (`video in:2024`) | everything allowed, newest first |
| `camera:R5`, `lens:"RF 50"` | tag contains |
| `ai:yes` / `ai:no` / `made:yes`; `made:chatgpt`, `ai:firefly` | AI provenance (C2PA / IPTC, known generator); the named generator |
| `rating:5`, `rating:3+`, `rating:0`, `rejected`, `rating:-1` | stars (0 unrated, −1 rejected; `+` at least) |
| `label:red` | colour label contains |
| `iso:400`, `iso:100-800`, `iso:1600+` | exact, inclusive range, at least |
| `f:2.8`, `f:1.4-2.8` / `mm:35`, `mm:24-70` | aperture ±0.05 stop / focal mm |
| `shutter:1/250`, `shutter:250`, `shutter:2s` | bare N = 1/N s; `s` or decimal = seconds; ±1/6 stop |
| `place:portugal`, `in:lisbon`, `-place:spain` | where taken; `in:` is a date if it parses, else a place; a digit-led word is never a place. **Needs Places**; without it matches nothing |
| (rating … shutter) | no value for a kept term: no match; an excluded term drops only values in range; filters run before towers |
| plain words (`R5`, `holiday`) | also files whose tags hold every word at a word start, first, tag as snippet (`MV_AI_MATCH_TAGS`) |

Lead-ins ("show me …", "find …") are trimmed. Name suggestions (`suggest_json`: prefix, then a
one-letter near spelling, two from eight letters); Tab completes. Unknown words stay in the
description; an unfinished `in:20` is ignored. Folding: `core/fold.h` (decompose, drop marks,
lowercase; ß, æ, ø, ł kept); the Mac chrome's name half uses Swift's full case folding.

**Places over the index.** For each `place:` / `in:<place>` term the engine calls host table
`place_matches` on its search worker; the host routes it through `slot_registry::provider_paths` to
the provider declaring `place:` for Local search (Places) as its own request (folder "" = whole
index), polling every 20 ms; the engine abandons it when a newer search queues or the pack stops.
Kept assets are in **every** kept place and **none** excluded, by path key, within the rest of the
query; a pure place query is newest first. No provider or host row: no match. Place-term queries
are not sent to other providers (`has_place_terms`, `src/addon/slots.cpp`); chromes check
`mv_slots_place_terms` (Mac `placeTerms:`).

Places reads coordinates via host table `locations_of_index`: (path, lat, lon) from index.db only,
served as mv.ai.1 `each_location`, 512 rows a `SELECT` in id order (`index_db::located_assets`),
lock held per `SELECT` only. Hosts publish the pack's row in `src/addon/index_locations.h` on attach
and retract it before unload (unload waits for walks). Stale or removed → `MV_ERR_CANCELLED`
(checked every 64 rows); no pack → `MV_ERR_NOT_FOUND`.

## UI and commands

- **Results are the gallery:** gallery, filmstrip, keys and thumbnails over a result list
  (`mv_folder_open_list`); tiles show `mm:ss`. **Enter** on a video tile opens the clip paused on the
  moment, the scrub bar marking its other matches.
- **Search panel** (`MediaViewer.Ai.Chrome` / `AI.bundle`) from the path bar's icon or `Ctrl+F` /
  `⌘F`; first use loads the pack (*Starting on demand*). Query field with name suggestions, *Look in*
  (folder and subfolders · everywhere · Photos on the Mac), kind and Pictures / Sounds / Speech
  filters. Without the pack the same key opens file search ([16](16-commands.md) "File search").
- **Commands** (`src/shell/command_table.cpp`, only while the pack is loaded, except `Ctrl+F`):

  | Command | Windows | Mac |
  |---|---|---|
  | `search.open` | `Ctrl+F` | `⌘F` |
  | `search.similar` (Find similar) | `Ctrl+Shift+F` | `⌘⇧F` |
  | `search.next_match` / `search.prev_match` | `N` / `Shift+N` | same |
  | Find this person | `Ctrl+Shift+P` | see [16](16-commands.md) |

- **Index this folder and subfolders:** offered for an unindexed folder with Core installed.
- **Status pill** (never modal; `mv_ai_status`): progress ring, active root, rate, ETA range from
  completed work, menu (Open search; Index anyway on battery); full form in the search footer and
  Settings with stage chips and a detail line. Windows: `IAddonHost3.SetIndexingPill` (older:
  `SetIndexingStatus`).
- **Settings → Local search:** pieces, Compute, Quality, Precision, "Index videos for", battery
  threshold, index cap, roots, online-only files, Text in pictures, Documents, People, sharing,
  Photos Library and Final Cut Pro (Mac). The pack posts `AI_ROOTS` when a root's count moves (≤ once
  a control pass), on rescan and at walk start/end.

## Not hurting the viewer

- **Workers:** own thread role at OS background priority (CPU and I/O), ≤ `min(2, cores / 4)`,
  never the viewport decode pool; the generation counter cancels stale work.
- **Yield policy** (`engine::yield_reason`, `wait_turn`): wait while `should_yield` (playback,
  panning, springs, slideshow, transition, 2 s after a dropped frame), on battery below the
  threshold (30 %), and while paused. **Index anyway** sets `battery_override` for that battery
  spell, never saved; viewer yields cannot be overridden.
- **Starting on demand:** launch only verifies the pack (Windows `AiOnDemand`, Mac
  `mv_addon2_ai_on_demand`); the first search loads it and indexing runs for the session (held roots
  excepted). Settings offers **Start now**; installing Core loads at once. Readers in later windows
  and the Final Cut Pro agent are on demand too ([18](18-import.md) "One host process",
  [23](23-nle-search.md)).
- **Loading** models waits for a quiet viewer (LOADING, `yield_reason` VIEWER); pictures first, then
  audio. Settings never loads the runtime.
- **GPU:** vendor providers own their devices; CPU is the pressure valve. **Memory:** frame and
  tensor memory capped.
- **Disk:** `index_cap_bytes` (default 8 GB, choices 2–50 GB; over it `MV_AI_STATUS_INDEX_FULL`);
  per-folder delete.

## ABI

`mv.ai.1` (`mediaviewer_ai.h`), beside `mv_addon_get`, wrapped by `Ai.cs` and the Swift chrome. Flat,
POD, opaque ids, `[no-block]` / worker annotations, append-only (`struct_size`).

| Group | Entries |
|---|---|
| Status and settings | `status` (`mv_ai_status`), `settings_json`, `set_setting`, `pause` |
| Roots | `roots_json`, `index_folder`, `root_set_enabled`, `root_rescan`, `root_remove`, `root_set_media`, `root_set_documents`, `folder_coverage`, `note_folder_opened`, `clear_index` |
| Search | `search_text`, `search_similar`, `result_count`, `result_at`, `result_path`, `result_thumb`, `result_snippet`, `result_duration`, `clip_matches`, `search_release`, `suggest_json` |
| People | `faces_enable`, `people_json`, `people_in_json`, `person_faces_json`, `person_rename`, `person_merge`, `face_reject`, `face_split`, `search_person`, `search_this_person`, `face_thumb`, `person_refine`, `people_dedupe`, `people_reanalyse`, review entries (*People*) |
| Sharing | `export_index`, `inspect_export`, `import_index`, `transfer_json`, `transfer_cancel` |
| Photos library (Mac) | `index_photos_library`, `photos_access` |

`mv_ai_reader_get` (`MV_AI_READER_ENTRY_SYMBOL`) builds the engine read-only for the Final Cut Pro
agent. The host table (`mediaviewer_addon.h`, append-only, every layout served) gives pixels
(`video_frame`, `video_frame_rgb`), JPEG-512 moments, `audio_open` / `audio_read` / `audio_close`
(mono float PCM), `thumbnail_jpeg` / `thumbnail_store_jpeg` / `thumbnail_make`, `tag_facts`,
`route_of`, `read_cues`, `document_text`, `watch_tree`, `place_matches`, `locations_of_index`,
`place_labels`, and verified family pieces.

## Audio

- **Soundtracks:** "Index videos for" Pictures / Sound / Both, per-folder override
  (`root_set_media`, `MV_AI_MEDIA_*`); unset = Pictures, or Both with the audio piece.
- **Sounds:** CLAP `larger_clap_general` fp16, 48 kHz, 10 s windows, 5 s hop, stored like frames.
- **Speech:** Whisper (onnx-community) via the merged KV-cache decoder with timestamp rules,
  no-speech and log-prob guards, a re-listen when a window ends mid-sentence, energy-refined segment
  starts. Mel features match `transformers` `audio_utils` (periodic Hann, centred reflect, Slaney);
  GPT-2 byte-level BPE tokenizers (`infer/audio_features.*`, `audio_models.*`).
- **Search:** `MV_AI_FIND_PICTURES` / `SOUNDS` / `SPEECH` (none = all); each model ranks in its own
  units, merged per clip; speech results carry their sentence. `MV_AI_STATUS_AUDIO_READY`.
- **Audio files** (MP3, M4A, M4P; [audio-search](../plans/audio-search.md)): only while "Show audio
  files" is on (forwarded as `audio_files`); off drops them by name and deletes their rows
  (`index_db::remove_kind`). `asset_kind::audio` 4, `MV_AI_KIND_AUDIO` 8. Indexed: tags (title,
  artist, album, album artist, composer, genre) and cover art at `pts_ms` −1
  (`edit::clip::cover_art_rgb`). Never paired, no People or picture text, skipped by import.

## Cues (chapter titles and text subtitles, #406)

- **Read by the host** (`read_cues`, `meta/cues.h`): chapters and text subtitle packets (mov_text,
  SubRip, ASS / SSA, WebVTT) with markup removed. Bitmap subtitles and non-UTF-8 skipped. ≤ first
  two hours and 8,000 lines.
- **Stored** in `cues`, no spec; a changed `(mtime, size)` drops them; not exported.
- **Pass** after pictures, 64 cues per commit, spec `cues/1`; resume drops rows from its start.
- **Search:** quotes and `said:` match in order at `start_ms`; plain words by coverage.
  `MV_AI_MATCH_TEXT`; snippet "Chapter: …" / "Subtitle: …".

## Text in pictures (#403)

- **OS reader** (`text_reader.h`): Vision `VNRecognizeTextRequest` (accurate) on the Mac,
  `Windows.Media.Ocr` (profile languages, C++/WinRT) on Windows. No reader: the track is off.
- **Read with the picture** (2026-11-14): a photo still owed its text is decoded once at 2,048 px
  in the picture pass, read, then box-fitted (`fit_long_edge`) to that pass's edge for CLIP and
  People: one read per photo, not one per pass (a hard disk, not the GPU, set the pace).
- **Photos** (`track::text`), for what the picture pass did not read: 2,048 px decode, four a claim; one `cues` row, kind 3 (`picture`),
  `start_ms` −1, lines joined by spaces, ≤ 4,000 bytes; spec `ocr/vision-accurate/1` /
  `ocr/windows-media/1`.
- **Clip moments:** after the clip's picture pass, spec + `/moments`; each stored moment
  (`engine::stored_moments`) decoded again at 2,048 px via `video_frame`, read, dropped. A row per
  moment unless equal to the previous; commit every 8 (`resume_ms`). Picture roots only.
- **Search** as cues; snippet "In the picture: …". **Off switch:** `set_setting("read_text", 0/1)`;
  `text_available` false greys it.
- **Status:** `cues_total` / `cues_done`, `text_total` / `text_done` (failed and unavailable count as
  done), cached and recounted after changes or every 30 s.

## Documents (#354)

PDF and DOCX by words, page looks, meaning and OCR ([document-search](../plans/document-search.md)).

- **Gate:** "Show documents" (off by default; `document_files`). Off drops `.pdf` / `.docx` by name
  and deletes them (`index_db::remove_kind`). `asset_kind::document` 3, `MV_AI_KIND_DOCUMENTS` 4
  (`MV_AI_KIND_ALL` 15). Not exported unless ticked.
- **Per folder** `doc_index`: 0 Off, 1 Words (default), 2 Words and pages; override
  `root_set_documents` (`MV_AI_DOCS_*`, `roots.docs`).
- **Text:** PDF via **PDFium** in the pack (bblanchon/pdfium-binaries chromium/8066, SHA-256 in
  `cmake/ai.cmake`, `pdf_pdfium.cpp` / `infer::dylib`); index workers only, one call at a time, ≤ 500
  pages and 60 s a document. DOCX via host `document_text` (`codec::docx_text`), paragraph by
  paragraph with its page.
- **Passages** (`passages.h`): close at a sentence end past 40 words, or at 80; never across a page;
  ≤ 20,000 a document; hyphenation undone, typography made plain. `words` folded, led by the previous
  passage's last 8 words. A trigger deletes words with their passage.
- **Words pass** (`track::docs`, `doctext/1`): one document a claim; PDF commits every 8 pages,
  DOCX whole. Password-protected → `locked` (state 5, never retried); unopenable → failed (≤ 3);
  no PDFium / `document_text` → `unavailable`. Needs no model: while towers load, `claim` hands out
  documents alone (`wait_turn(false)`).
- **Scanned pages** (`track::scan`, at Words): a page with < 16 non-space characters is drawn
  (`video_frame_rgb`, `pts_ms` = page, 2,048 px), read by the Text-in-pictures reader, stored as
  passages under reader spec + `/pages`. Last of all passes, ≤ 4 pages a claim. Every DOCX, and any
  PDF with no textless page, is done with its words. `scans_total` / `scans_done`.
- **Page pictures** (`track::pages`, Words and pages, `docpage/<picture spec>`): first 3 pages plus
  ≤ 64 pages over half images (`pdf_reader::look`); drawn at 448 px, embedded by the picture tower in
  batches of 4, stored as `frames` with `pts_ms` = page and `kFramePage` (0x100). Dropping the level
  deletes them (`index_db::drop_pages`). `pages_total` / `pages_done`.
- **Meaning** (`ai-docs`, `track::meaning`, `all-minilm-l6-v2/fp32/pre1`, waits for the scan pass via
  `meaning_after`): all-MiniLM-L6-v2, 384-d, BERT uncased WordPiece (`infer/text_models.*`), mean
  pooling, 256 tokens, CPU 2 threads always (fp32: int8 makes vectors batch-dependent). 16 passages a
  run, committed per document to `passage_vecs`; a third in-memory matrix (`meanings_`). Status
  `meaning_total` / `meaning_done`, `MV_AI_STATUS_DOCS_READY`; `settings_json` `docs_ready`,
  `docs_model`.
- **Ranking by meaning** (CLAP's rule): generic prompts "a document.", "some text.", "a page of a
  document.", "this is a paragraph of text.", "information."; row `result_margin` 0.25, query
  `query_margin` 0.415 (top ten), Precision-scaled; ordered by rank = 2 × (cosine − `rank_offset`
  0.096) / `query_margin`.
- **Search:** FTS5 narrows, the engine decides. Quotes: words in order; `text:` / `says:` documents
  only. Plain words by the speech rule (≥ 4-letter words match longer forms) under
  `MV_AI_FIND_TEXT`. Row: `pts_ms` = 0-based page, `MV_AI_MATCH_TEXT`; page pictures are
  `MV_AI_MATCH_PICTURE`; one row per document at its better page. `clip_matches` lists pages.
- **Results:** tile = matched page (page 0 the viewer thumbnail, others `path#t=<page>`); "p. 3";
  Enter opens at the page (`present_lab_mac::open_item(..., page)`; Windows
  `mv_folder_select_page`).
- **Status:** `docs_total`, `docs_done`, `docs_locked`, `docs_failed`; `roots_json` `documents`,
  `documents_done`, `docs`; `settings_json` `document_files`, `doc_index`, `pdf_available`.
- **Privacy:** text stays in `index.db`, never logged or reported; exported only when ticked.

## People

Faces in photos and frames are clustered on-device into unnamed **people**, optionally named;
search gains names and "this person".

**Biometric handling.** Only in `faces.db` (`PRAGMA secure_delete=ON`); off until "Find people in
your photos" (`faces_enable`); one-click delete destroys the file; never in telemetry, crash reports
or minidumps; exported only with "Include People". One edit also uses it, on request: **Open closed
eyes** ([edit-adjust plan](../plans/edit-adjust.md) slice 6), `eyes_json` matching the face to the
same person in frames within 10 s; writes nothing; refused while People is off.

Schema: `people(id, name, created_at)`, `faces(id, asset_id, path, pts_ms, x, y, w, h, score,
person_id, emb, pinned, quality, tta, spec)`, `faces_cover(person_id, pinned, quality, score·w·h,
asset_id)`, `rejected(face_id, person_id)`, `rejected_at(asset_id, pts_ms, x, y, w, h, person_id)`,
`no_merge(a, b)`, `merge_log(token, …)`, `scanned(asset_id, spec)`, `meta` (`rules`).

Corrections: rename, merge, "not this person" (`face_reject`, `faces_reject_json`), split
(`face_split`), "Move to…" (`faces_assign`), "Yes" / "Unpin" (`faces_pin`).

### People model

- **Detector:** YuNet 2023mar, 640 px, confidence ≥ 0.8, NMS IoU 0.3, faces ≥ 4 % of the short side,
  CPU.
- **Embedder:** AdaFace IR-50 (WebFace4M) fp16, 512-d, ArcFace 112 × 112, flip averaging (`tta`).
  Uses the towers' provider if a fixed crop matches CPU at ≥ 0.99. Spec
  `yunet-2023mar+adaface-ir50-webface4m/1`. Exported by `tools/package/face-export.py` (fp16 weights,
  fp32 I/O, checked against PyTorch; `golden` values in `ai-models.json`).
- **Thresholds** (`model.json` → `face_tuning`; `faces.h` SFace fallbacks): **same_person 0.30, keep
  0.20, keep_weak 0.24, margin 0.10, ambiguous 0.04, merge_at 0.32**; `shortlist` 3, `max_refs` 256,
  `max_negatives` 12, `track_ms` 15 s.
- **Quality** = min(size, sharpness, frontal) × (0.6 + 0.4 × score): size 40 → 112 px; sharpness =
  Laplacian variance of the aligned luma, log ramp 20 → 300; frontal = nose offset from the eye
  midline over half the eye distance. **Weak** < 0.35 (`weak_quality`), **unusable** < 0.10
  (`refine_params::unusable_quality`); missing quality is proxied from score and box.
- **Re-run** (`people_reanalyse`): re-analyses every asset; only the current `faces.spec` is
  compared. A face whose box matches (IoU ≥ 0.5) keeps id, person, name, pin and rejections. When
  done and idle, the control thread **settles**: full refinement (≤ 3 calls) then the merge. Settle
  also runs when `meta` `rules` is older (`faces_db::settle_due`; rules 3). A new face model starts a
  re-run. Status `MV_AI_STATUS_PEOPLE_RERUN` / `_SETTLING`, `people_scan_total` / `_done`,
  `people_model_utf8`.

### Online clustering

`faces.cpp`; scores are pairwise cosine everywhere, never cosine to a centroid.

1. Score every person by mean pairwise cosine; shortlist the best 3 ≥ `same_person − 2 × margin`.
2. Rescore **confirmed** people (with a pinned face) by **exemplars**: mean of the top 3 cosines to
   pinned faces and good core (≤ 12, `build_person_proto`). Unconfirmed keep the mean.
3. Join the best at `same_person`, unless the runner-up is within `ambiguous`: then unassigned.

- Unusable faces join nobody; weak ones may join (by mean) but never start a person.
- **One face per frame**, filed clearest first (except "one face twice").
- A person is passed over when the face is as like its rejected faces as like it (by `margin`).
- **Pins:** faces split out, moved, the cover shown when naming (`person_rename_cover`), both covers
  of a merge, faces said "Yes" to.
- Exemplar cache per person, rebuilt lazily under the lock from the first `max_refs` refs; dropped on
  any correction, re-analyse or import; rebuilt on ±25 % growth.
- **Idle consolidate** at mean pairwise `merge_at`, only pairs with a changed person.
- People reads hold the engine's model lock, so none grows with the library: `faces_cover` serves
  covers and counts; ≤ one `AI_PEOPLE` a second; `note_folder_opened` never waits on the queue lock.

### People refinement

`face_refine.h/.cpp` (pure), `faces_db::refine_begin / refine_commit`, `engine::person_refine`.
*refs* = pinned + good members (≤ 256); *anchors* = pins plus the refs' medoid (with pins, only if
within `same_person` of one); *core* = refs within `same_person` of an anchor; *exemplars* = pinned
then core, ≤ 12; **support** = mean top-3 cosine to exemplars.

| Verdict | Rule |
|---|---|
| **move** to b | support(b) ≥ join (`same_person`) and beats own, runner-up and neg(b) by `margin`; never weak; never into a person it was rejected from or that holds another face of its frame |
| **evict** | support(own) < `keep` (`keep_weak` if weak); or unusable; or person is `weak_only`; or rejected from it; or neg(own) ≥ join and > support(own); or a better face of its frame is there |
| **admit** (unassigned) | as move |
| **regroup** | unassigned good faces within join of a leader and at join mean (average linkage), ≥ 2, one per frame → new person |

- **A clip's frames are one sighting:** one exemplar per photo or clip (pins excepted); the medoid
  counts each once; support skips the face's own photo or clip while others exist.
- **Lone pins:** agreeing (≥ `core`) with no other pin nor the medoid; in pass 0 one another person
  supports at ≥ join by `margin` stops anchoring (`refine_output::distrusted`) when another pin
  remains. Unusable pins anchor only if nothing else does.
- **Negatives:** the clearest 12 rejected faces; neg = their top-3 mean. A rejected face supported at
  join + 2 margins is taken for a slip.
- **One face twice:** two faces in one frame are one person only at ≥ join + 2 margins.
- Jacobi passes, ≤ 4; a face changes ≤ once per call; keep < join is hysteresis.
- Pre-flip-averaging borderline faces are re-analysed 16 assets per call.
- **Refinement runs only when the user asks** ("Refine faces"): `person_refine` on a worker judges
  only that person's faces (`refine_input::focus`), admits nothing; the commit skips faces changed
  since the snapshot. Tests `[refine]`.

### Corrections that hold

- **The idle merge never joins two confirmed people**, even via an unconfirmed cluster; an
  unconfirmed cluster joins a confirmed person only when its exemplars do not call it a stranger
  (≥ `same_person − 2 × margin`). Splits, rejections and shared frames keep pairs apart.
- **A merge remaps corrections** (`remap_corrections_locked`); deleting a person deletes every row
  naming its id; open drops rejections of gone faces or people.
- **A changed file keeps its rejections** via `rejected_at` by box (IoU ≥ 0.5); `mark_scanned` drops
  what was not re-found.
- **Not this person** (`faces_reject_json`): also same-clip frames of that face (`spread`); each is
  filed under another person it clearly matches, else nobody; the answer lists where.
- **Move to** (`faces_assign`): `MV_AI_ASSIGN_PIN` pins; rejected from the old person; a rejection
  from the target withdrawn. `MV_AI_ASSIGN_UNDO` records nothing; person 0 = nobody.
- **Undo a merge** (`person_merge_undo`, token from `person_merge_covers`; `merge_log` cleared on
  open) restores id, name, pins and corrections.

### A person's own fragment is no rival

- A face its person supports at ≥ join + 2 margins is not moved.
- A smaller person whose core is no stranger (cross-core mean ≥ join − margin / 2) is no destination;
  a larger or confirmed one is.
- A lone pin is distrusted only when the rest of its person calls it a stranger (< join − margin).

### Tracks

A weak clip frame nobody has follows the clear frames of its clip within `track_ms` it resembles at
`same_person`, when all are in one person and no correction objects (`faces_db::track_locked`, and
in the settle).

### Measured

Benches: `mv_ai_tests` `"[.people-bench]"` (LFW, 5,985 faces of 423 people, BCubed),
`"[.people-settle]"`, `"[.people-api]"`, `"[.people-heldout]"`, `"[.people-unknown-order]"`,
`"[.people-why]"` (on a private faces.db copy; aggregate counts only).

| LFW | Precision | Recall | F | People (423) |
|---|---|---|---|---|
| SFace, full size | 0.9938 | 0.9938 | 0.9938 | 433 |
| IR-50, full size | 0.9994 | 0.9982 | 0.9988 | 426 |
| IR-50, ~24 px | 0.9994 | 0.9966 | 0.9980 | 429 |

Tried and rejected: admitting into unconfirmed people by average in the settle; farthest-point
exemplars.

### Reviewing a person

- `faces_db::review_begin` (locked) / `review_finish` (unlocked): each face's **support**, **rival**
  (best other person seen more than once, never sharing a frame) and **check** (unpinned with support
  < `same_person`, rival within `margin`, unusable, or a lone pin). `person_review_json` lists checks
  first, least supported first.
- **Unknown faces** (`unknown_faces_json`): unassigned faces ≥ `weak_quality` with their nearest
  person (≥ `same_person − 2 × margin`); named suggestions first (surest first), then anyone, then
  clearest. Scored outside the lock (`loose_begin` / `loose_finish`), cached between pages.
- **Might also be** (`person_suggest_json`): unknown faces and unnamed unpinned people reaching
  `same_person − margin`.
- Answers too large for the buffer are kept for the retry (`write_out_once`).

Chromes: "Check these" first; Yes / Not / Move to… / Unpin (keys Y, N, M, Space; ⌘Z / Ctrl+Z);
"Might also be" strip; "Unknown faces (N)"; "Merged · Undo". Undos use `MV_AI_ASSIGN_UNDO`; undoing
a "Yes" on an unknown face assigns person 0.

### Refine and Merge duplicates in detail

`person_refine_json` / `people_dedupe_json`: `MV_AI_DRY_RUN` looks without changing (faces leaving and
where, people merging into whom); a real run returns destinations and an undo token per automatic
merge (`merge_auto`, `person_merge_undo`), kept for the buffer retry. Chromes: look, Apply / Cancel,
Undo.

**Find this person** (`Ctrl+Shift+P`, [16](16-commands.md)): the largest face on screen
(`search_this_person`).

### Merge duplicates

`people_dedupe`, on request:

1. Full refinement, then `refine_commit` (later user changes win).
2. `find_duplicates` (`face_refine.h`, no lock): two people link when each's exemplars support the
   other at `join` (smaller kept); groups by **complete linkage**, strongest first. Never: different
   names, `no_merge`, a cross rejection. Cores < 0.24 mean pairwise are skipped.
3. Survivor: named, then more faces, then older (`faces_db::merge_auto`); nothing pinned.

Undo is Split. Tests `"[dedupe]"`.

### People in the open folder

`people_in_json(scope_dir, scope, …)`: people with a face under `scope_dir`; `faces` counts photos
and clips there; the cover is the clearest there. Qualifying (judged on the whole index): two photos
or clips, or **one when fewer than 100 are in view** (`people_min_faces`).
`faces_db::people(min, assets)` is one indexed pass per person. Chrome: "People in · This folder |
+ Subfolders" (default + Subfolders, follows the viewer's folder, absent with no folder, not
persisted). Settings shows the first 24 people and "Show all N" (Mac `AllPeopleSheet`, lazy).

## Sharing an index

`src/addons/ai/transfer.*`, `export_index` / `inspect_export` / `import_index`. A one-off merge.

**`.mvindex`** (SQLite, `info.format = "mediaviewer.index"`, v2; v1 imports): chosen roots (absolute
path, display name), assets by path **relative to the root** (`/`), `(mtime, size)`, kind,
duration; `progress` (done and partial), `frames`, `speech`. Optional (`MV_AI_TRANSFER_*`, all off
by default): **People** (`people`, pathless `faces`, `rejected`, `no_merge`, `face_scanned`; current
model only), **thumbnails** (`thumbs`, JPEG-512 bytes, made if missing), **Documents** (documents,
`passages`, `passage_vecs` with `info.docs_spec`, page-picture frames). Never: the Photos root, tags,
cues, picture text. Written to `<dest>.part` then renamed, from read connections (indexing goes on).

**Import** maps each root to a folder here. Per asset at `folder + rel`:

| Here | Does |
|---|---|
| no row | row and vectors added (`seen` 0) |
| same `(mtime, size)`, done for a spec | local rows for that spec stay |
| same `(mtime, size)`, not done | replaced |
| different `(mtime, size)` | local row stays |

Then the ordinary delta rescans. Indexing pauses during import (control thread) and models' matrices
reload after.

- **Models:** picture rows import for a spec in use; an empty local index **adopts** the file's
  tower if carried (sets Quality); else skipped (`picture_usable`). Sound / speech for the loaded
  audio spec only.
- **People** needs `ai-faces` and enables the opt-in; named people join a same-named local person
  (case-insensitive); assets already scanned keep local faces.
- **Thumbnails:** host `thumbnail_store_jpeg` checks a baseline JPEG ≤ 512 px; stored only when the
  local `(mtime, size)` matches.
- **Documents** only when ticked here and documents are shown; passages get new ids.
  `inspect_export` `documents` (`count`, `shown`, `meaning_match`).

## Setting a phone up

Owned by the Phone access add-on ([plan](../plans/phone-access-addon.md)): Settings → Add-ons → Phone access
is its card (`src/addons/phone/screens.*`, drawn by both hosts), which drives `src/addons/phone/link.cpp`,
which calls the desk (`src/addons/phone/desk.*`, [28](28-driftwire.md#claiming-and-pairing-phone-access))
and reaches Local search through the host table's `catalogue_*` rows. Local search is the
catalogue source: `phone_publish` builds generations signed with the app's publisher key
(`catalogue/publisher.h`) under the library id and epoch in the share's `library.json`. `phone_json`
gives id, epoch and newest generation folder. The pack's `phone_link` entries are null.

- **Pairing:** QR = `catalog::pair_url` (five minutes). Check code `driftwire::pairing_check`
  (`pair_check.h`): 80 bits of a domain-separated BLAKE2b over library id, publisher key, service
  identity and access key, compared by the person. Phones polled every 2 s while shown.
- **Moving the signing key** (`catalog_chain.*`): `library.json` holds the key, epoch and a chain of
  records each signed by the key it leaves. A new desktop writes `takeover.request`; the old one
  signs the handover and raises the epoch. Phones `follow_chain` from their pinned key. Start over
  writes a new key with no record (phones rescan). Another library's or a lower epoch's
  library.json is never adopted.
- **Nearby copy** (`phone/nearby/`, `prepare_nearby`): the newest generation hard-linked with its own
  signed `current.json`, the previous kept, and the text tower; only with a phone paired.

## The phone's catalogue

A phone answers queries from a copy of the index ([plan](../plans/mobile.md) §5, §6).
`src/addons/ai/catalog.*` builds a **generation** from a read snapshot into
`<destination>/.mediaviewer/`; `engine::run_phone_publish`. Off until the first publish;
`phone_auto` stops the automatic half.

**A generation** (`gen/000123/`; `manifest.json` hashes every file; `current.json` signs the
manifest hash):

| File | What |
|---|---|
| `catalog.db` | `index.db`'s schema, only the answering tower's vectors and this share's folders; `assets.path` = `/<root id>/<rel>` |
| `settings.json` | what a reader needs, `faces` included |
| `labels.f32` | when present |
| `faces.db` | named people and their assets / moments; `emb` empty, box zero (`faces_db::open_read_only` accepts it) |
| `tiny.pack` | 64 px JPEG per asset (`kTinyEdge`, `kTinyQuality`) |
| `thumbs.idx` | asset → pack, offset, length |

Shared, share only, named by hash: `thumbs/t-<hash>.pack`, `previews/p-*.pack` (empty for now),
`models/<spec>/<hash>-<name>`; listed in the manifest (`thumb_packs`, `preview_packs`,
`model_files`), so everything parsed is signed.

**Formats** (LE, binary-searchable: `find_tiny`, `find_thumb`): `tiny.pack` = `MVTINY01`, u32 count,
u32 0, rows `(i64 asset, u64 offset, u32 length, u32 0)` by asset, then JPEGs. `thumbs.idx` =
`MVTIDX01`, u32 packs, u32 rows, 40-byte NUL-padded pack names, rows `(i64 asset, u32 pack, u32
length, u64 offset)` by asset.

**Names (N7).** `assets.path` / `motion` are NFC (`nfc.cpp`, `tools/fold/gen_nfc_table.py`, Unicode
16); on-disk bytes in `disk_path` / `disk_motion` (NULL when equal; catalogue only); open by
`COALESCE(disk_path, path)`.

**Places.** `place` = each geotagged asset's label from host `place_labels` (`mv.slots.1
place_label`, batches of 256); a read-only engine answers `place:` from it (`stored_place_matches`:
a whole component, or a prefix from four letters).

**Thumbnails.** `thumb_store`, ledger `phone/thumbs/ledger.db`, 16 tiles per commit, background
thread yielding to the viewer, ≤ 30 min a run (`thumbs_left`). Uncached JPEG-512s via
`thumbnail_make` (stored nowhere). `corrupt` / `unsupported_format` recorded until the file changes;
other failures retried, three strikes at one size and date = no picture. Packs named by hash; under
16 MiB folded into the next.

**When it runs.** Background priority, never on battery, in the control loop's tick. Compares
`index_db::content_stamp` (rows, never `seen`) and named people for the generation's folders; if
changed, quiet 120 s and ≤ hourly, publishes. Share away: the newest waits in `phone/gen/`
(`waiting`), checked every 30 s. Failed delivery backs off 30 s → 1 h. No room: `no_space`,
`needs_bytes`.

**Retention (§5 rule 4).** Newest two generations, plus older ones until 2 h after the next became
current by this machine's clock (`phone/arrivals.json`). Unnamed shared files go after 30 days,
small thumbnail packs after 2 h (`phone/unused.json`); an unreadable manifest deletes nothing.

**Ranking.** Ties break by asset id, then time (`vector_store::ranked_before`, `finish`), so phone
and desktop agree.

**The sample library** ([plan](../plans/mobile.md) §11.1): `tools/ios/sample-library.sh` indexes 47
CC0 / public-domain files (B/32, CPU) and publishes under a fixed id and the sample's key
(`mv_mobile_parity_desktop sample-publish`) to the GitHub release `sample-library`, flattened as
`<root id>__<path with '/' as "__">`.

- `files.json` = `{"signature", "list"}` (Ed25519 over `kFilesContext` + list) gives the size and
  SHA-256 of every file sync does not check (all but `current.json`, `gen/`, `models/`).
- Phone: `sample::static_remote` (`src/mobile/sample.*`), HTTPS by the host (`mv_fetch_fn`,
  `SampleFetch.swift`). Catalogue files pass through by range; others only from a whole hash-checked
  copy (`<thumb cache>/verified/<name>.<16 hex>`, ≤ 256 MB LRU); unlisted files never fetched. A
  cancelled read leaves a hash-named `.part` to resume by range. Unsigned list →
  `MV_LINK_REJECTED`.
- `mv_pair_parse` reads `SAMPLE` / `mediaviewer://sample` as `sample = 1`; `mv_link_open` with
  `static_base`.
- **Forget:** `mv_link_shut_down` stops reads and returns once core writers are done and refuse
  more; app writers run under `LinkConnection.whileLive`.

## Photos library source (macOS)

One more remembered root (`src/addons/ai/photos_source.h`; `photos_mac.mm`, `photos_none.cpp`). A
result's path is its key, listed as a virtual item ([26](26-photos-library.md)).

- **Source:** assets `photos:<localIdentifier>` under root `photos:`; `(mtime, size)` =
  `(modificationDate, pixel count)`. PhotoKit enumeration (iCloud included; shared albums and Hidden
  excluded; one row per burst and Live Photo). `PHPhotoLibraryChangeObserver` rescans ≤ every 10 s.
- **Local only, read-only:** network off except the opt-in fetch; stills from the local rendition
  (sRGB), clips from `requestAVAsset`. iCloud-only assets are **unavailable** (state 4,
  `assets_unavailable`); a clip is found by its poster at 0 ms. Re-checked once per launch, when
  access returns, and (assets first seen this session) on change notices.
- **Downloading iCloud-only clips** (`"icloud_videos"`, off): `engine::fetch_loop`, newest first,
  `photos_source::fetch_video` (`PHAssetResourceManager`) into `<data>/cache/icloud/<row id>.<ext>`;
  ≤ 2 (`kFetchAhead`); `requeue_unavailable`, then `claim` takes them first
  (`index_db::pending_among`); `file_of` maps the key; `fetch_reap` deletes. Waits (`icloud_fetch`)
  on battery, costly network (`network_unmetered`), < 10 GB free, pause. Status
  `icloud_videos_left` / `_fetched`. Photos are never fetched.
- **Permission:** only from Settings → **Add Photos Library**, then `index_photos_library`; before,
  `permission_denied`. Access off: rows searchable, root idle, *Open Privacy Settings*. Base app:
  `NSPhotoLibraryUsageDescription`, `com.apple.security.personal-information.photos-library`
  (`packaging/macos/MediaViewer.entitlements`).
- **Search:** `scope_dir "photos:"`; tiles from PhotoKit's cached rendition with a badge.
- **Opening:** read in place; `src/shell/write_guard.h` refuses writes inside `*.photoslibrary` and to
  registered result files (trash, move, ratings, metadata, rotate, export, trims: "From your Photos
  library: read-only here."); Copy To and drag-out allowed; `⌘E` shows it in Photos. An iCloud-only
  original opens as `<name> (preview).jpg` in `~/Library/Caches/MediaViewer/Photos Library/`, fetched
  after 400 ms on it; downloads deleted on the next list and at quit.

Harnesses: `tools/ai/photos-spike.sh`, `tools/ai-bench/photos-bench.sh`.

## Measurements

COCO-1k, `tools/ai-eval/calibrate.py`, ORT 1.30 CPU, Apple M-series (bar R@10 ≥ 0.88, P@5 ≥ 0.8):

| Tower | Tier | R@10 | P@5 |
|---|---|---|---|
| CLIP ViT-B/32 | Default fast, Intel, Potato | 0.881 | 0.943 |
| CLIP ViT-L/14 | Default high | 0.904 | 0.914 |
| OpenCLIP ViT-H/14 | Maxxed high | 0.946 | 0.971 |
| TinyCLIP ViT-8M/16 / 39M/16 | rejected (misses bar / 4× slower than B/32) | 0.838 / 0.922 | 0.886 / 1.000 |

ViT-H/14 thresholds: query_margin 0.023, result_margin −0.015, similar_min 0.449.

### Verified on Windows

Ryzen 16-thread, RTX 4070, ORT 1.30: embeddings within cosine 0.999 of the ORT-Python reference
(`tests/data/ai/reference.json`; audio `audio_reference.json`); engine suite `test_ai_engine.cpp`;
100 k ViT-L rows scan in 31 ms (`"[.perf-bench]"` fails over 100 ms). Whisper RTF: base 0.075 CPU /
0.029 CUDA; small 0.23 / 0.036.

### Verified on macOS

Apple M5, ORT 1.30, dims pinned:

| Tower | CPU (2 thr) | Core ML | cosine | first compile | cached open |
|---|---|---|---|---|---|
| ViT-B/32 | 73 img/s | 499 img/s | 0.9992 | 82 s | 17 s |
| ViT-L/14 | 4.9 img/s | 31 img/s | 0.9983 | 5.3 min | 64 s |

Core ML recompiles each load (hence CPU first, then swap). `MLComputeUnits` `ALL`. Checklist:
`src.swift/AIChrome/MAC-VALIDATION.md`.

## Not built

- Hardware decode for the frame sampler; an OpenVINO piece; a memory-mapped matrix.
- Local search on Intel Macs; file watching on Linux.
- Place names in the index; a pre-compiled large tower for Core ML.
- Clip text read from the picture pass's own decode; sound and speech for audio files.
- Background People refinement (`refine_begin(false)` has no caller).
- Inference outside the viewer's process ([ai-out-of-process](../plans/ai-out-of-process.md)).
