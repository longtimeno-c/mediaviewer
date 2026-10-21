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

### Model tiers (#482)

**Which model fills each role** (the picture tower for each quality slot, the Whisper models,
CLAP, the face models and the documents model) comes from a **tier profile**: the pack's
`models/tiers.json`, written from the `tiers` block of `ai-models.json`
([model-tiers plan](../plans/model-tiers.md)). The add-on's code names no model folder; a tower
answers for the quality slot the profile gives it. A pack staged without `tiers.json` opens
today's Default folders.

| | Maxxed | Default | Intel | Potato |
|---|---|---|---|---|
| Pictures (high / fast) | OpenCLIP ViT-H/14 / B/32 | L/14 / B/32 | B/32 | B/32 |
| Speech (high / fast) | Whisper large-v3-turbo / small | small / base | base | tiny |
| Sounds | CLAP | CLAP | CLAP, off by default | none |
| People | YuNet + AdaFace IR-50 | YuNet + AdaFace IR-50 | YuNet + AdaFace IR-50 | YuNet + SFace |
| Documents | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 | all-MiniLM-L6-v2 |
| Clip gaps (min / max) | 250 / 1000 ms | 500 / 2000 ms | 500 / 2000 ms | 1000 / 6000 ms |

ViT-H/14 (~2 GB, our fp16 export by `tools/package/clip-export.py` from the pinned checkpoint,
verified by a golden output) rides in its own piece, `ai-large`, which only Maxxed installs.

**Potato's picture tower is B/32 (2026-10-18).** Both TinyCLIP candidates were exported and
measured and neither is a Potato: ViT-8M/16 misses the bar (COCO-1k R@10 0.838 < 0.88), and
ViT-39M/16 passes (0.922) but runs about 4× slower than B/32 on two CPU threads (6.2 vs
25.6 img/s, fp16, ONNX Runtime 1.30, M-series). B/32 is the cheapest tower that passes; Potato
saves elsewhere (Whisper tiny, SFace, no sound search, sparser clips). B/32 int8 weights stay
rejected (R@10 0.822). MobileCLIP waits for its weights licence to clear `ai-models.py check`.

- **The model contract** (`infer/models.h`, `audio_models.h`): a CLIP tower states
  `tokenizer_kind` (`clip-bpe`), `score` (`cosine`, or `sigmoid`, still ranked by cosine),
  named outputs and the Core ML dim names; Whisper states `n_mels` (80, or 128 for large-v3 /
  turbo) and its special tokens. A kind this build lacks is unsupported, never guessed. A model
  that fits is a manifest entry: with the contract in place, ViT-H/14, Whisper tiny and turbo
  and SFace went in as data, with no C++ change (TinyCLIP too, before it was measured out).
- **Choosing.** Nothing downloads until a tier is chosen (`MV_AI_STATUS_TIER_UNCHOSEN`; the
  add-on's picker opens on install and from Settings → Local search → Models). The local check
  (`machine.h`) recommends one: Potato on ≤ 2 cores or ≤ 8 GB, Intel on an x86_64 Mac or a PC
  with no accelerator, Maxxed with an accelerator and ≥ 32 GB, else Default. It is shown in the
  picker and never sent anywhere. An index made before tiers keeps Default without asking.
  The control thread reads `tiers.json` and runs the check first, before the models wait for
  a quiet viewer, and posts `MV_ADDON_EVENT_AI_STATUS`: the first status read has the tiers,
  and each chrome counts the offer as made only once the picker opened (#509).
- **Changing** tier fetches the new models, then re-indexes with the new towers while the old
  index answers (the old tier's tower still opens by its spec key, `open_clip_spec`); once the
  new one covers the folders the old vectors and the old tier's fetched files go.
- **Thresholds** come from `tools/ai-eval/calibrate.py`: the PR 20 eval (COCO-1k R@10 ≥ 0.88,
  labelled-query P@5 ≥ 0.8) and a quantile match against B/32's hand-set thresholds. Results are
  in "Measurements".

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
  (`data/cache/coreml`, ~1.2 GB B/32 / ~4.1 GB L/14, outside the 5 GB install ceiling), so the pack
  **answers on CPU at once** and a background thread opens Core ML, runs the self-test and swaps
  it in (`upgrading_clip` in `pack.cpp`). Text queries stay on CPU (3.3 ms B/32, 6.4 ms L/14).
  **Indexing waits for the swap** (`engine::upgrading`, 2026-10-05): the status stays *Loading* /
  *Preparing … Search works meanwhile*, and no picture is embedded on the stand-in. L/14 on CPU
  is a twelfth of B/32's rate there and took the cores the Core ML load needed (a MacBook Air
  loaded L/14 from its cache in 694 s while it ran). A cache entry a quit cut short (ORT writes
  `model.txt` first and takes any `model` folder as converted) is dropped at the next start, so
  it is converted again rather than loaded broken and recorded as Core ML failing the tower.
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
  | `ai` (Core) | `mv_ai`, ORT (CPU; Core ML on Mac), PDFium (a PDF's text, #354; ~6 MB, its parts' notices in `LICENSES/pdfium`), the chrome (`MediaViewer.Ai.Chrome` / `AI.bundle`); the CLIP towers and tokenizer are fetched | ~100 MB, plus 1.16 GB fetched (Default) |
  | `ai-large` (Maxxed) | OpenCLIP ViT-H/14, our fp16 export | ~1.97 GB |
  | `ai-faces` (People) | YuNet detector, AdaFace IR-50 embedder (fp16) | ~87 MB model |
  | `ai-audio` (Sound) | CLAP `larger_clap_general`, Whisper base and small, tokenizers | ~1 GB |
  | `ai-docs` (Documents) | all-MiniLM-L6-v2 (fp32), its WordPiece vocab (#354 D3) | ~91 MB |
  | `ai-cuda` (Windows) | ORT's CUDA build | ~205 MB |

  Pieces declare `part_of: ai` and an `arch`. Clicks queue, Core first; "Install all"; "Update
  all" when more than one installed piece has a newer version, and a piece's Install or Update
  queues a Core that is itself behind first, so no piece runs ahead of the engine it was built
  with. Remove deletes the files and offers to delete the index; `data/cache` (Core ML) is
  deleted even when the index is kept.
- **Models from Hugging Face (#482).** The pieces on GitHub carry code, the signed model list
  and our own exports only; each upstream model file is in the piece's signed manifest
  (`remote`: URL, size, SHA-256, licence) and the add-on fetches the ones the chosen tier
  needs into `data/fetched/<piece>/`, a plain GET of the pinned `resolve/<revision>/<file>`
  URL (`fetch.h`). Resumable, verified (a mismatch is fetched again, never loaded), patient
  (an outage waits and resumes by itself; a 404 is reported as gone). The picture tower comes
  first, so search opens before speech, sounds, faces and documents arrive. Status:
  `fetch_state`, `fetch_done_bytes` / `fetch_total_bytes`. No family ceiling since 2026-10-18
  ([12](12-decision-log.md)); the picker shows each tier's download and disk size.
- The add-on is downloaded on explicit opt-in, a plain GET of fixed release URLs with no
  identifier. The Mac pack is Developer ID-signed and notarized and loads under library
  validation.
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
   burst of scene-cut keyframes flooding the index. The People pass on its own (re-analyse, or
   People turned on after the pictures) decodes the moments the picture pass kept, through the
   host's frame fetch, so a face sharp on one indexed frame is seen (#407); only a clip with no
   picture rows falls back to its own 2 s / 10 s grid.
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
   preview). Live Photo and RAW+JPEG pairs index once, on the pair's still. A Live Photo's movie
   (the folder pair `IMG_1234.HEIC` + `IMG_1234.MOV`; schema 7's `motion` columns) is sampled
   with the same sampler and the same 0.97 dedupe, seeded with the still's vector, and its frames
   are stored on the still's id with their `pts_ms`, at most 32 (#407). A moment opens the movie
   at that time (the result's path is the movie and its kind a clip; `N` walks the still's
   moments); a match at no timestamp is the still. The still commits first (progress partial),
   the movie after (done), so a kill resumes the motion alone. A movie edited, added or brought
   down from the cloud under the same still re-queues only the motion. An online-only movie is
   read only with *Index online-only files* on and the fetch's gate open (brought down in place
   and given back), otherwise the still is indexed alone. Photos library Live Photos are one
   PhotoKit asset and stay a still.

## Index store

`index.db` (SQLite, WAL) in the pack's data folder, separate from the thumbnail DB so clearing
the index cannot touch thumbnails. Schema (`index_db.cpp`):

```
meta(key, value)
roots(id, path, recursive, enabled, last_scan_at, media)          -- remembered index locations
assets(id, path, root_id, mtime, size, kind, duration_ms, seen, cloud,  -- cloud: schema 3
       tags, taken_at, lat, lon, camera, lens, made_with, ai,         -- schema 4 (#291)
       place, country, region,                                        -- schema 4, always NULL
       rating, label, iso, aperture_x100, focal_mm, shutter_den, shutter_us)  -- schema 5 (#405)
asset_text(rowid = asset id, text)                                  -- FTS5, schema 4
progress(asset_id, spec, state, resume_ms, tries, indexed_at)     -- per asset and model spec
frames(id, asset_id, spec, pts_ms, pts_tb, tb_num, tb_den, flags, generic, scale, emb)
speech(id, asset_id, spec, start_ms, end_ms, text)                 -- Whisper transcript lines
cues(id, asset_id, start_ms, end_ms, kind, text)                   -- schema 6: chapters, text subtitles (#406)
```

`emb` is the int8 vector, `scale` its per-vector scale, `generic` its best cosine against the
tower's generic prompts. Each schema step (1 -> 2 `media`, 2 -> 3 `cloud`, 3 -> 4 tags, 4 -> 5
rating and exposure, 5 -> 6 cues) is one
transaction and skips a column already there, so a crash mid-migration never leaves an index that
will not open. A new index is made as schema 3 and takes the same 3 -> 4 step, so a fresh index and
a migrated one cannot differ.

### Schema 4 (#291, 2026-10-12)

What each asset's tags say, so Local search can filter on them:

- **Columns:** `tags` (0 until read; a changed file goes back to 0), `taken_at` (date taken, the
  camera's wall clock read as UTC; for a Photos item its `creationDate`), `lat` / `lon` (WGS 84,
  from EXIF / XMP GPS, a clip's ISO 6709 or `PHAsset.location`), `camera`, `lens`, `made_with`
  and `ai` (summary::ai_generated: positive evidence only).
- **`asset_text`:** an FTS5 table (`unicode61 remove_diacritics 2`), rowid = asset id. `text` is
  the search fields as shown (camera, lens, made-with, software, title, caption, keywords,
  creator; [06](06-metadata.md) "Search text"), U+001F between fields. The engine matches the
  same fields in memory, folded with the hosts' rule, so a result can say which field matched;
  `index_db::match_text` is the SQL path for a reader; it quotes every typed word (`fts_phrases`), so typed text is never FTS5 syntax. A trigger drops a row with its asset.
  SQLite without FTS5 gets a plain table of the same shape (`meta('asset_text')` says which). The
  vcpkg `sqlite3[fts5]` feature is on for both platforms.
- **`place` / `country` / `region` stay NULL.** Place names are the Places add-on's
  ([27](27-places.md)); the app links no geocoder (owner, 2026-10-11). The columns are there so a
  later step can fill them from the provider without another migration. Today `place:` is
  answered at query time (below).
- **Filling it:** the host table's `tag_facts` (appended after `walk_files2`, read only when
  `struct_size` covers it) serves each file from the host's `meta_cache.db`, a bounded head read
  on a miss ([06](06-metadata.md) "Search text"). The control thread backfills assets with
  `tags = 0` after each walk, 128 per transaction, up to two seconds a turn, waiting while
  `should_yield` says the viewer is busy. The `tags` column is the bookmark, so a kill, a
  restart or an index migrated from schema 3 resumes where it stopped and reads nothing twice.
  Photos library items take `PHAsset.location` and `creationDate` from the last enumeration. A host without `tag_facts` (the pack updated before the app) backfills nothing and leaves `tags = 0`, so the first host that has it fills them; a failed commit waits the loop's usual second before trying again.
  An exported index (Sharing an index) carries no tags; the receiver reads them itself.
- `wait_idle` waits for the backfill too.

### Schema 5 (#405, 2026-10-15)

Rating, colour label and exposure, for the `rating:` / `label:` / `iso:` / `f:` / `mm:` /
`shutter:` terms. Columns, NULL until read: `rating` (1..5, −1 rejected, 0 unrated), `label` (as
written), `iso`, `aperture_x100` (f/2.8 → 280), `focal_mm`, `shutter_den` (N when the exposure
time is 1/N s) and `shutter_us` (the exposure time in microseconds, always set when there is
one, so 2 s and 1/250 both match). A clip has no exposure block: those stay NULL.

- **Where they come from:** the host's `tag_facts`, whose struct gained the fields at its end
  (`MV_ADDON_TAG_FACTS_V2_SIZE`); the host's meta_cache.db reads them in the same bounded head
  read as the search fields (`kSearchFactsVersion` 2, so each file's cache row is read once more).
- **`tags` is a level now:** 1 = schema 4's facts read, 2 = these too (`kTagsLevel`). The step
  leaves every schema-4 row at 1, so the backfill reads it once more for the new facts alone;
  nothing is re-embedded. A host from before #405 answers the shorter struct: its rows stop at 1,
  the engine asks no more of it that run, and the first host that serves the fields reads them.
- **Never search text:** the numbers are not in `asset_text`, so a plain "400" is not an ISO
  match.
- A rating written to a RAW's XMP sidecar leaves the RAW's own stamp alone, so the index keeps
  the old rating until the file's stamp changes or the index is rebuilt (the base app's
  meta_cache drops its row; the AI index has no such signal yet).
### Schema 6 to 10 (#406, #407, #355, #354)

Schema 6 adds `cues` ("Cues" below). Schema 8 lets `assets.kind` be 4, an audio file (no table
change). Schema 9 lets it be 3, a document, and adds `roots.docs`, `passages` and `passage_text`
("Documents" below). Schema 10 (D3) adds `passage_vecs(passage_id, spec, generic, scale, emb)`, a
passage's vector under the Documents piece's text model, int8 like frames; a passage that goes
takes its vectors with it. Schema 7 adds `motion`, `motion_mtime`, `motion_size` and
`motion_cloud` to `assets`: a Live Photo still's movie and its stamp (Frame sampling, step 6). The
first walk after the step fills them and queues each Live Photo's motion alone; the still is not
embedded again.

People live in a separate `faces.db`; the label bars in `labels.f32`;
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
- **Unfinished roots wait for Resume (issue #299, 2026-10-11).** A quit or a kill while a root
  is unfinished does not resume on the next open, and unfinished work is not a reason to load the
  pack. The shipped pack runs with `engine_options::hold_unfinished` (`addon_entry.cpp`):
  - **Held.** A root with work left when the engine started is *held*: its pictures not all
    indexed, or listed in the last session's `resume.json` (any pass left: documents' words or
    pages, a clip's text, the text in photos, sounds, speech, People; until 2026-10-21 only the
    pictures counted, so those passes carried on unasked at the next load). Held means not
    walked, not queued on any track, not fetched from iCloud or a cloud drive, and left out of
    the status counts, until `set_setting("resume_root", <id>)` (0: every held root), a Rescan,
    re-enabling it or Index this folder on it. Search answers from what it committed meanwhile;
    `roots_json` marks it `"held": true`. Opening search loads the pack (#292, above) but does
    not resume a held root. Same rule for the Photos library and an ordinary folder.
  - **`data/resume.json`.** The control thread keeps it current (after each walk, and at most
    every 5 s otherwise, written only when it changed, through a `.tmp` and a rename):
    `{"version":1,"roots":[{"id","path","kind","done","total","held"}]}`, the enabled roots
    with work left on any pass that is on (`engine::work_left_in_root`, claim's passes and
    filters), and every held root; `done`/`total` are still the pictures'. It holds local paths beside `index.db`, which holds them
    already; nothing is sent anywhere (rule 6).
  - **The launch notice, both hosts.** The worker that verifies the installed pack at launch
    also reads `resume.json` (Mac: `addons_mac.mm` through `addon/ai_resume.h`; Windows:
    `IslandHost.ResumeNotice.cs`). Nothing of the pack loads for it. With roots listed, a small
    notice in the bottom-right corner, not a modal, asks "Resume indexing Photos?" (two roots:
    "Photos and Holiday"; more: "Photos, Holiday and 2 more"). **Resume** loads the pack as the
    first search does (no panel opens; the pill says "Starting Local search…") and, once it
    attaches, sends `resume_root` 0: each root is walked (its startup delta was skipped), then
    indexed from `resume_ms`, redoing nothing committed. **Dismiss** leaves them paused and loads
    nothing; the notice does not come back that session, and the next launch asks again. The Mac
    notice is a self-sizing SwiftUI card pinned over the canvas corner (`ResumeNoticeView.swift`,
    above the filmstrip when it is up); on Windows it is a small island of its own over that
    corner, as the AI badge is (`IslandHost.ResumeNotice.cs`), placed from the window's client
    rectangle on every resize and above the other islands. It was a windowed `Popup` of the
    command-bar island until #443, and never showed on Windows. A launch read that lands before
    the command bar is up waits for it, as the Mac's store waits for its host. Neither takes
    focus. A later window (the reader) indexes nothing and shows no notice.
  - **Settings → Local search**, in the pack's folder list (`ManagementView.swift`,
    `ManagePanel.cs`): a held root's row reads "N of M · paused since last time" and offers
    **Resume** in place of Pause, which sends `resume_root` for that root alone.
  - **What it costs** is in the decision log: files added to a held root wait too. A pack that
    has never run with the option has no `resume.json`, so the first launch after the update asks
    nothing; the roots it holds then show Resume in Settings.
- A folder whose walk fails (offline, moved, no access) keeps its rows; `roots_json` says
  `"error":"unreadable"` and Settings says "Couldn't read this folder" rather than "Up to date".

## Cloud files in indexed folders

OneDrive (Windows) and iCloud Drive (Mac) keep some files online-only. They appear in the folder,
but reading one downloads it. The walk lists them, flagged: host table `walk_files2`, from
`FILE_ATTRIBUTE_RECALL_ON_*` on Windows and `SF_DATALESS` on the Mac. It still never follows a
symlink or junction. Table v1's `walk_files` leaves them out, so Import never starts a download.
The index marks them (`assets.cloud`): no picture, sound, speech or People pass reads one, they
are not counted as work in hand, and the status and each folder's row say "N only in OneDrive" /
"only in iCloud Drive" (`cloud_files_left`, roots `cloud_only`).

**Index online-only files** (`cloud_files`, off by default; owner, 2026-10-05) mirrors the Photos
library's iCloud videos. The same fetch thread brings a file down in place: on Windows through the
Cloud Files API (`CfHydratePlaceholder` in 16 MB slices, so it reports progress and stops between
them), and on the Mac through `startDownloadingUbiquitousItemAtURL`. It clears the mark, so every
track takes the file first, and makes it online-only again once no track wants it
(`CfDehydratePlaceholder`, `evictUbiquitousItemAtURL`). The two-files-on-disk limit is shared
with iCloud clips. It waits on battery, on a metered network (Network List Manager cost /
`NWPathMonitor`), under 10 GB plus the file free on the file's drive, and while paused. A file the
provider will not send is skipped until the next start. Row ids of files brought down are kept in
`<data>/cache/cloud-fetched`, so a crash's leftovers are made online-only again at the next start.
The port is `src/addons/ai/cloud_files.h` (`cloud_win.cpp`, `cloud_mac.mm`, `cloud_none.cpp`), in
the pack, not the base app. On Windows `cldapi.dll` is delay-loaded and probed: without it
(before 10 1709) the pack still loads and searches, `settings_json` says `"cloud_supported":false`,
and the option is shown greyed with the reason. The option is always listed on Windows. Another sync client's files on the Mac (not ubiquitous items) stay
unindexed.
- The index holds paths locally and is excluded from crash reporting and telemetry.

## Search

- **Query encode:** the text tower, cached for repeated queries. A picture query's last noun is
  embedded in **both numbers and averaged** (`query::number_forms`, one batched run), so
  "mountain" and "mountains" are the same query.
- **Scan** (`vectors.h`): brute-force int8 dot product over an in-memory matrix of every stored
  frame of the active model (loaded, not memory-mapped; ~77 MB at 100 k L/14 frames). 100,000
  ViT-L rows scan in ~31 ms on one core. There is no ANN index. Dropping assets (a root
  removed, a scan's gone or changed list, a reader's refresh) tombstones their rows in one
  pass with at most one compaction (`remove_assets`), and a path finds its asset through a
  `path_key → id` map kept beside the asset table, not a walk of it (`clip_matches`,
  `search_similar`; issue #174).
- **Ranking:** top frames, then **grouped per clip** with the best moment first and "N more in
  this clip", so one long video cannot fill the page (`mv_ai_result`). `find_text` returns at
  most 5,000 rows; a result list holds at most 1,000.
- **Order (#412, 2026-10-15).** Best match first when the search knows what it is answering.
  When no pass answers a description (the "nothing found" gate holds every row back), the list
  is not empty and not a score ranking of the least-bad rows: it is what the rest of the query
  allows, **newest by date created first** (`engine::newest_first`) — a Photos item's
  `creationDate` (`taken_at`, else the enumeration's `created`, never its edit time), a file's
  `taken_at`, else its modification time; the same second by path. A filter-only query (`in:2024`, `file:IMG_12`) is listed the same way.
  The hosts keep the order they are handed: the panel's grid, and the gallery and filmstrip
  opened from it, are one sequence.
- **Scope:** the open folder and its subfolders (default) · all indexed folders
  (`MV_AI_SCOPE_TREE` / `MV_AI_SCOPE_ALL`; the panel offers no folder-only scope). Kind filter
  (photos / videos) and tower filter (pictures / sounds / speech). A nonsense query is not
  answered with the least-bad ten (below, "Nothing found"). With no folder open the folder scope searches
  everywhere, and the Mac panel shows *Everywhere* chosen; opening another folder asks a
  folder-scoped search again (on the panel's next showing if it is closed), never leaving the
  last folder's results under the new one (issue #191).
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
| `Tristan "hello"`, `Tristan:"hello"`, `said:hello` | Tristan, and a clip whose transcript, a chapter title or a text subtitle line (#406, "Cues"), or a photo or clip moment whose text (#403, "Text in pictures"), has those words in that order; the line is the snippet. Quoted words also match a document's text (#354); `said:` never does |
| `text:lease`, `says:"net 30 days"`, `-text:draft` | a document's text only (#354, "Documents"): the words in that order, at the page that holds them |
| `lease documents`, `is:document`, `is:pdf`, `is:docx` | documents (#354); `is:pdf` / `is:docx` (`is:word`) the format |
| `Tristan Aaryan`, `Tristan and Aaryan` | both in the same photo or clip |
| `Tristan or Aaryan` | either |
| `Anna Smith`, `anna` | a named person: the whole name or the first name, any case, accents folded |
| `@tri`, `person:"anna s"` | a person by the start of a name, or a near spelling; nobody by that name: nothing found |
| `Trist`, `tristna` (alone) | a lone word that starts or nearly spells a name: those people first, then what the word describes (so "car" still finds cars when there is a Carla) |
| `-Nico`, `-beach`, `-"goodbye"`, `-video` | leave out a person, what a picture search finds, a phrase said, a kind |
| `beach video`, `is:photo`, `videos of Anna` | a kind: the last word, `is:` / `type:`, or a leading "videos of"; "photos of …" asks for anything |
| `in:2024`, `in:2024-06`, `before:2025`, `after:2023-05`, `since:2024-03-01`, `until:2024` | the **date taken** (`taken_at`: the camera's wall clock read as UTC, no timezone conversion; a Photos item's `creationDate`), and the file's modification time (UTC) for a file with none or whose tags are not read yet (#404) |
| `file:IMG_12`, `file:"trip 2024"`, `-file:copy` | the file's **name** (not its folder) contains the text, case and accents folded. Indexed files only |
| `video in:2024`, `file:IMG_12` | filters alone: everything they allow, newest first (by the same date) |
| `camera:R5`, `camera:canon`, `-camera:iphone` | the file's camera tag (make and model) contains the text, folded (#291) |
| `lens:24-70`, `lens:"RF 50"` | the lens tag contains the text |
| `ai:yes`, `ai:no`, `made:yes` | AI provenance (positive evidence: C2PA / IPTC trained-algorithmic source, or a known generator); `ai:no` is every file without that evidence |
| `made:chatgpt`, `ai:firefly`, `-made:midjourney` | the generator the file names ("made with") contains the text |
| `rating:5`, `rating:3+`, `rating:0`, `rejected`, `rating:-1`, `-rating:1` | the stars the file states (0 unrated, −1 rejected), or at least that many (`+`); `-` leaves that rating out (#405) |
| `label:red`, `-label:red` | the colour label contains the word, folded |
| `iso:400`, `iso:100-800`, `iso:1600+` | ISO: exact, an inclusive range, or at least |
| `f:2.8`, `f:1.4-2.8` | aperture, within 0.05 stops of the stored value (f/2.8 and APEX's f/2.83 are one) |
| `mm:35`, `mm:24-70` | focal length in whole millimetres |
| `shutter:1/250`, `shutter:250`, `shutter:2s` | the exposure time (a bare number N is 1/N s, as a camera shows it; `s` or a decimal point is seconds), within a sixth of a stop |
| `place:portugal`, `in:lisbon`, `-place:spain` | where it was taken. `in:` is a date when it reads as one (`in:2024`) and a place otherwise; a word that starts with a digit is never a place; `in:` never names a folder (the scope does). **Needs the Places add-on** (#441): which assets are in the place is its search provider's answer, which the engine asks for through the host and intersects with the rest of the query ("Places over the index" below). Without Places a place term matches nothing: the panel says "Place search needs the Places add-on" and its help line leaves `place:` out |
| (any of the six above) | a file with no value for a kept term does not match it (an `iso:` term skips a clip); a left-out term drops only a value in its range. The terms filter before the towers run, so `iso:400 beach` ranks the ISO 400 frames by "beach" |
| `R5`, `chatgpt`, `holiday` (plain words) | besides the picture / sound / speech search, files whose tags hold every word at the start of a word ("r5" in "Canon EOS R5", "70mm" in "RF24-70mm"; not "cat" inside "location") come first, with the tag as the snippet (`MV_AI_MATCH_TAGS`, shown unquoted under the tile) |

Lead-ins ("pull up all the …", "show me …", "find …") are trimmed. Typing a name: the panel
names people for the word being typed (`suggest_json`; prefix first, then a near spelling of one
letter, two from eight letters); **Tab** or a click completes the word. A word that is not a
name, a kind or an operator stays in the description; an unfinished `in:20` is ignored. Not in
the index, so not in the syntax: nothing of a file's text is left out now; quoted words search
speech, chapter titles, subtitle lines and the text in photos ("Text in pictures"). Date terms use the date
taken where the tag backfill has read one (#404, [12](12-decision-log.md)). Per keystroke the parse is linear in the query plus one `SELECT` of the named people.

Words fold with the shared rule (`core/fold.h`, the hosts' name fold): decompose, drop marks,
lowercase; ß, æ, ø and ł stay themselves. Before #291 the pack's own Latin-1 fold turned ß into
"ss" and æ into "ae"; "strasse" no longer finds "Straße", as in file search on Windows and its tag half on the Mac (the Mac chrome's name half uses Swift's full case folding, which does turn ß into "ss"; see `core/fold.h`).

**Places over the index (#398, #441).** A place term in Local search is answered by the add-on
whose search provider declares `place:` for Local search (Places), and intersected in the engine.
For each `place:` / `in:<place>` term, kept or left out, the engine calls the host table's
`place_matches` (appended after `locations_of_index`) on its search worker. The host routes it
through the slot registry (`slot_registry::provider_paths`) to that provider as a request of its
own, with its own generation and folder "" (the whole index), so a chrome's search never replaces
it and it never replaces one. The registry collects the provider's rows and returns their paths
when it posts done. The engine then keeps the assets the scope, kinds, dates and other terms
allow that are in **every** kept place and in **none** left out (`-place:spain`), by path key.
A pure `place:portugal` is the provider's rows within the scope and kinds, newest first. A
description (`beach place:portugal`) ranks only those assets. Two kept places are their overlap.
While it waits, the engine stops (no rows) once a newer search is queued or the pack is stopping.
The registry asks it every 20 ms, and the provider sees the request go stale and stops its walk.
With no such provider (Places not installed, or removed meanwhile), or a host table without the
row, a place term matches nothing. Before #441 the engine kept every geotagged asset and listed
them newest first, so `place:Portugal` without Places showed a photo taken in England as its
first result; with Places the provider's rows were only offered beside the list ("+ N in
Portugal"). Local search no longer sends a query with place terms to other providers
(`has_place_terms`, the pack's rule for the operators, in `src/addon/slots.cpp`): the pack's list
is the whole answer. A plain word (`portugal`) still goes to them. The chromes ask
`mv_slots_place_terms` (Mac: the host's `placeTerms:`) whether the words have a place term and
whether Places is loaded. With a term and no Places they search nothing and show the empty state;
the help line names `place:` only while Places is loaded.

Over a folder Places walks the host's `locations_in`; for Local search (folder "") it walks the
host table's `locations_of_index`, appended after `read_cues`. That call streams (path, lat, lon) for every
indexed asset with coordinates, from what index.db already holds (schema 4's `lat` / `lon`,
filled by the tag backfill). No file and no file's metadata is read. The pack serves it as
mv.ai.1 `each_location`, 512 rows a `SELECT` in id order (`index_db::located_assets`), with
index.db's lock held for the `SELECT` only and never while a row is visited, so indexing goes on
between batches. Because the pack is an add-on, the core cannot link it. Each host publishes the
pack's row in a process-wide source (`src/addon/index_locations.h`) when the pack attaches (the
writer or a later window's reader). It retracts the row before the pack goes: a walk in flight
stops at its next row, and the library is unloaded only once no walk is inside it. The Mac waits
on its add-on queue, never the main thread. The host's thunk is generation-tagged. A request that
goes stale, or an add-on being removed (detach joins its worker), stops the walk with
`MV_ERR_CANCELLED`; staleness is checked every 64 rows. With no pack attached the call returns
`MV_ERR_NOT_FOUND`, and Places posts done with no rows.

## UI and commands

- **Results are the gallery:** the same gallery, filmstrip, keys and thumbnails over a result
  list instead of a folder listing (base ABI `mv_folder_open_list`). A tile shows the frame
  thumbnail and `mm:ss`. **Enter** on a video tile opens the clip paused on that moment, with
  the scrub bar marking the clip's other matches.
- **Search panel** (`MediaViewer.Ai.Chrome` / `AI.bundle`): opened from the search icon in the
  path bar or `Ctrl+F` / `⌘F`. The first time in a session it starts the pack first (*Starting on
  demand*, below): the command-bar pill reads "Starting Local search…" and the panel opens once
  the pack attaches. Query field with name suggestions, *Look in* (this folder and
  subfolders · everywhere · Photos on the Mac when indexed), kind and Pictures / Sounds / Speech
  filters, the snippet under a spoken result. Without the pack the same icon and key open the
  base app's file search ([16](16-commands.md) "File search").
  Both hosts draw it alike (#202): a spinner in the query row while the words on screen are
  answered; a click selects a tile and a double-click opens (Ctrl / ⌘ for the gallery grid); a
  caption only under a tile whose words matched (speech, a tag, a document's text); capsule
  badges (a clip's moment with a play glyph, a page, "+N in this clip") and a match badge only
  when a sound or speech matched; a placeholder icon until the thumbnail lands; the selection
  ring hinted, not hidden, while the keyboard is in the query; keycap hints for Enter / Return
  and Ctrl+Enter / ⌘↩; and an icon over each empty state.
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
  on the viewer. On both hosts the command-bar pill has a ring (the pass's progress, with an arc
  that turns while work runs), fades in and out, and has a menu: Open search, and Index anyway
  while on battery. The search footer and Settings → Local search show the same pill in full:
  the ring (a check when idle), the line, the other passes as stage chips ("Pictures ✓",
  "Sound 14%") while sound or speech work is in hand, and a detail line for a provider fallback,
  an upgrade, a full index and (in the footer) a OneDrive wait. On Windows the pack reports these through `IAddonHost3.SetIndexingPill`;
  an older pack's `SetIndexingStatus` still drives the line.
- **Settings → Local search:** per-piece install, Compute, Quality, Precision, "Index videos
  for", battery threshold, index size and cap, remembered roots (with *Index Pictures* / *Videos*
  (*Movies*) offered while no root covers them, and the reason when an add fails), *Index
  online-only files* (shown once there are any), People, sharing, the Photos Library (Mac) and
  Final Cut Pro search (Mac, [23](23-nle-search.md)).
  Each remembered root shows "N of M" (or "Up to date"), its share of the index on disk, and
  "Checking for changes…" while it is being walked. The rows follow the work: the pack posts
  `AI_ROOTS` when a root's count moves (at most once a control pass, ~1 s), when a rescan is
  asked for, and when a walk starts and ends; a row button greys out until the row is read
  again, and Windows re-reads the panel each time Settings opens (2026-10-05).

## Not hurting the viewer

Indexing is a background load on the machine and GPU of a viewer whose point is smoothness.

- **Inference workers:** a thread role of their own at OS background priority (I/O too), at
  most `min(2, cores / 4)`, never the pool that decodes for the viewport. The generation
  counter cancels stale work on navigation.
- **Yield policy** (`engine::yield_reason`, `wait_turn`): indexing waits while the host's
  present loop is busy (`should_yield`: playback, panning, springs, a slideshow, a transition,
  and for 2 s after any dropped frame), while on battery below the threshold (default 30 %), and
  while the user has paused it. **Index anyway** wherever "Paused on battery" shows (search
  footer, Settings, the command-bar pill's menu) sets `battery_override` for that spell on
  battery; it is never saved, and going back to AC or restarting ends it. The viewer's own
  yields are not overridable.
- **Starting on demand (issue #292, 2026-10-11):** at launch nothing of the pack loads: no ONNX
  Runtime, no model sessions, no indexing threads. The host only verifies the installed pack, so
  the search icon and `Ctrl+F` / `⌘F` offer Local search (Windows `AiOnDemand`, Mac
  `mv_addon2_ai_on_demand`). The first search of the session loads the pack (the query-side
  models with the rest), and the pack's startup delta and indexing then run for the rest of the
  session (a root left unfinished last session excepted: it waits for Resume, below); the command-bar pill reads "Starting Local search…" meanwhile, since the panel is the
  pack's and cannot show before it attaches. Opening Settings does not start it: Settings →
  Local search says it starts at the first search and offers **Start now**, which loads it so the
  pack's own panel (compute, folders, People) shows. Installing Core (or a piece with Core
  already running) in Settings still loads it at once. A later window's reader was already on
  demand ([18](18-import.md) "One host process"), and so is the Final Cut Pro agent: launchd
  starts it only when the extension connects, and it exits when idle ([23](23-nle-search.md)).
  The cost is in the decision log: files added while search was never opened are indexed only
  once search is first opened that session.
- **Loading:** opening the models (a GPU context, a gigabyte of weights, the self-test) waits for
  the same quiet viewer indexing does, and reports LOADING with `yield_reason` VIEWER meanwhile.
  Pictures become searchable first; the audio models open after them. Settings' own reads never
  load the runtime. The self-test's CPU half is cached in `selftest.txt` (~15 s off a 40 s CUDA
  ViT-L load); the provider still runs and is checked against it every start.
- **GPU:** a vendor provider owns its own device; the CPU backend is the pressure valve.
- **Memory:** decoded-frame and tensor memory are capped; the search matrix is ~77 MB at 100 k
  L/14 frames.
- **Disk:** the index has a size cap (`index_cap_bytes`, default 8 GB; over it, indexing stops
  with `MV_AI_STATUS_INDEX_FULL`) and per-folder delete; its size shows in Settings, where both
  hosts pick the cap (2, 5, 10, 20 or 50 GB) and removing a folder asks first.

## ABI

`mv.ai.1` (`mediaviewer_ai.h`), the pack's own table beside `mv_addon_get`, wrapped by
`Ai.cs` and the Swift chrome. Flat, POD, opaque ids, `[no-block]` / worker annotations, append-only
(`struct_size` tells the host which entries a pack has). Groups:

| Group | Entries |
|---|---|
| Status and settings | `status` (`mv_ai_status`), `settings_json`, `set_setting` (compute, quality, precision, battery, cap, video_index, reload), `pause` |
| Roots | `roots_json`, `index_folder`, `root_set_enabled`, `root_rescan`, `root_remove`, `root_set_media`, `root_set_documents` (#354, appended), `folder_coverage`, `note_folder_opened`, `clear_index` |
| Search | `search_text`, `search_similar`, `result_count`, `result_at`, `result_path`, `result_thumb`, `result_snippet`, `result_duration`, `clip_matches`, `search_release`, `suggest_json` |
| People | `faces_enable`, `people_json`, `people_in_json`, `person_faces_json`, `person_rename`, `person_merge`, `face_reject`, `face_split`, `search_person`, `search_this_person`, `face_thumb`, `person_refine`, `people_dedupe`, `people_reanalyse` |
| Sharing | `export_index`, `inspect_export`, `import_index`, `transfer_json`, `transfer_cancel` |
| Photos library (Mac) | `index_photos_library`, `photos_access` |

A second export, `mv_ai_reader_get` (`MV_AI_READER_ENTRY_SYMBOL`), builds the same engine
read-only for the Final Cut Pro search agent ([23](23-nle-search.md)).

The host table (`mediaviewer_addon.h`) v2 gives the pack stills and sampled video frames as
pixels, moment thumbnails in the JPEG-512 cache, `audio_open` / `audio_read` / `audio_close`
(mono float PCM at a requested rate from the viewer's FFmpeg), `thumbnail_jpeg` /
`thumbnail_store_jpeg` (sharing), `read_cues` (#406), `document_text` (#354: a DOCX's text; a
document's page is `video_frame_rgb` with `pts_ms` the page), and verified family pieces. Table
versions only append; the host serves every layout from 1 up, so Import keeps loading.

## Audio

A video's **soundtrack** is indexed for what it **sounds like** (CLAP) and what is **said**
(Whisper transcripts).

**Standalone audio files** (MP3, M4A, M4P; #355, [audio-search](../plans/audio-search.md)) are
indexed only while File types "Show audio files" is on. Each chrome forwards that setting to the
pack as `audio_files` when it attaches and on every change; off is the default. Off, the walk
drops them by name, so they cost nothing. Turned on, every enabled root is walked again. Turned
off, the search leaves them out at once and the control thread deletes their rows
(`index_db::remove_kind`). An audio file is `asset_kind::audio` (4; schema 8, no table change)
and `MV_AI_KIND_AUDIO` (8; `MV_AI_KIND_ALL` is 11, so an older chrome's 3 still means photos and
videos). Slice A1 indexes its tags (title, artist, album, album artist, composer, genre) and its
cover art: one picture at `pts_ms` -1, read from the header (`edit::clip::cover_art_rgb`), on the
picture track while the folder's media takes pictures. No cover art is done, not failed. It never
pairs, never goes to People or to the text-in-photos pass, and an `.mvindex` import skips it. Its
sound and speech are slice A2. Query words: `is:audio` / `is:music` / `is:song`, and a trailing
or leading `audio` / `songs` / `tracks`; bare `music` stays a description.

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

## Cues (chapter titles and text subtitles, #406)

Exact text a clip carries, so a chapter titled "birthday cake" or a caption "suited and booted"
is found without the audio piece and without Whisper hearing it.

- **Read by the host** (`read_cues` on the host table, appended after `tag_facts`;
  `meta/cues.h`): chapters from the header, and the packets of text subtitle streams (mov_text,
  SubRip, ASS / SSA, WebVTT) read as text with their markup removed. No subtitle decoder and no
  new library: a cue is its packet. Bitmap subtitles (DVD, PGS) are pictures and are skipped, as
  is a cue whose bytes are not UTF-8. At most the first two hours and 8,000 lines a file. A
  file with neither chapters nor a text subtitle stream is done after its header.
- **Stored** in `cues(id, asset_id, start_ms, end_ms, kind, text)` (schema 6), not as speech rows
  under the Whisper spec: a shared index must not disagree about what was said, and removing the
  audio piece keeps them. No spec: the text is the file's; a changed `(mtime, size)` drops the
  rows. An export carries none (its cue progress is left out), so the receiver reads them.
- **Its own pass:** a work track after the pictures (they never wait on it), on the index workers,
  `wait_turn` between cues. It commits 64 cues at a time under progress spec `cues/1`; a kill
  resumes at the last committed cue's start, and that commit first drops the rows from there, so
  a resumed clip never holds a line twice. A host without `read_cues` leaves the track off.
- **Search:** quoted phrases and `said:` match cue text as they match transcript lines, at the
  cue's `start_ms`; plain words also match it by the speech rule (word coverage), beside the
  picture search, not instead of it. Such a row has `MV_AI_MATCH_TEXT`, and its snippet names the
  source: "Chapter: Birthday cake" or "Subtitle: suited and booted", shown unquoted (both chromes).
  Burned-in text is read by the OS instead ("Text in pictures"). The cue pass and the text pass
  share the table but not their rows: a resumed cue pass drops only chapters and subtitles.

## Text in pictures (#403)

Words in a photo (a sign, a poster, a shirt, a screenshot) are searchable: "suited and booted"
finds the photo with those words on it, as Apple Photos' Live Text does. CLIP does not read; this
does.

- **Read by the operating system** (`text_reader.h`, a port in the pack like `cloud_files.h`):
  Apple Vision's `VNRecognizeTextRequest` (accurate level, language correction, languages
  detected) on the Mac, `Windows.Media.Ocr` for the user's profile languages on Windows (C++/WinRT
  from the Windows SDK; `windowsapp.lib`, no package). No model ships and nothing leaves the
  machine. A Windows with no OCR language installed, or another platform, has no reader: the
  track stays off and nothing else changes.
- **Its own pass** (`track::text`), after the pictures and the clips' cues: a 2,048 px decode
  (small text needs pixels; the picture pass reads 448) and the OS reader, four photos a claim,
  `wait_turn` before each. One `cues` row per photo, kind 3 (`picture`), `start_ms` −1, the lines
  joined by spaces so a phrase split over two lines of a sign is one phrase, at most 4,000 bytes.
  Progress under the reader's spec (`ocr/vision-accurate/1`, `ocr/windows-media/1`): read once,
  ever, until the file changes; a photo read again (a new reader, a Live Photo's movie changed)
  replaces its row. An export carries none.
- **Clip moments** (2026-10-15): a clip is taken only once its picture pass is done
  (`index_db::pending`'s `after`), under a spec of its own, the reader's plus `/moments`
  (`ocr/vision-accurate/1/moments`), so photos already read stay read. The moments are the ones
  the picture pass stored (`engine::stored_moments`, as the People pass reads them), each seeked
  and decoded again at 2,048 px (the clip's own size when smaller) by the host's `video_frame`,
  read, and dropped: nothing is embedded or kept but the text. One `picture` row per moment at its
  `pts_ms`; a moment whose text is the one before's is not stored again, so a sign held in shot is
  one row at the moment it appears. Committed every 8 moments (partial, `resume_ms` the last
  moment read); a stop commits what was read and a restart goes on after it; a fresh read first
  drops the clip's earlier text rows. A clip where no moment decoded or read is failed (tried
  again like any failure). Only clips whose root indexes pictures; the iCloud fetch keeps a
  downloaded clip until its moments are read. A quoted search returns the clip at that moment with
  "In the picture: …" as the snippet.
- **Search** as for cues: quoted words and `said:` match the photo's text, plain words by word
  coverage beside the picture search; words are folded (`core/fold.h`) on both sides, so
  "koln" finds "KÖLN". The row has `MV_AI_MATCH_TEXT` and the snippet "In the picture: …".
- **Off switch:** Settings → Local search → Text in pictures → **Read text in photos** on both
  hosts (`set_setting("read_text", 0/1)`, saved, read back from `settings_json`) leaves the
  photo and clip passes out from the next claim; on brings them back for what was not read; text
  already read stays searchable. `settings_json`'s `text_available` is false when the OS has no
  reader (a Windows without an OCR language): the switch is shown off and greyed, and says why.
- **Status:** `mv_ai_status` counts both text passes (appended `cues_total` / `cues_done`, a clip's
  own text; `text_total` / `text_done`, photos plus clips whose pictures are done; failed and
  unavailable count as done). While either is short the state is not idle, so no chrome says "Up
  to date" while they run; the status lines name them ("Reading chapters and subtitles 3 of 40
  clips", "Reading text in pictures 120 of 400"), after the pictures, as the pack runs them.
  The three counts cost 14 ms together at 25,000 assets (`mv_ai_tests "[.perf-bench]"`, beside the
  picture count's 8 ms), and the status refreshes once a second even idle, so they are kept
  between refreshes and counted again only after a cue or text item, a `read_text` change, a
  change in the picture counts, spec or held roots, while a text pass has work, or every 30 s.
- **Measured** (Apple M-series, `mv_ai_tests "[.ocr-fixture]"` over `tools/ai/ocr-fixture.py`'s
  40 labelled camera-sized photos: signs, posters, shirts, screenshots): Vision found 38 of 40
  phrases (0.95), "suited and booted" among them; it read "trom" for "from" on one sign and lost
  the last word of a tilted shirt. Per photo: the text pass's 2,048 px decode 10.1 ms median
  against the picture pass's 448 px decode 8.6 ms, and the read 44 ms median, 84 ms mean. Two runs,
  the same numbers. Windows is not measured.
- **Measured, clip moments** (Apple M-series, `MV_OCR_CLIP_DIR=<dir> mv_ai_tests
  "[.ocr-moments]"`: six of the owner's clips (3840×2160 and 2704×1520 GoPro, two 1080×1920
  iPhone, a 2688×1520 CCTV clip with a burned-in clock, a 720×1280 phone clip), 12 moments each; 72 moments, 19 with text). Per moment, 180, 210 and 218 ms
  median over three runs (264–269 ms mean), on a machine other builds kept at load 8–12 (two more
  runs at load 33: 303 and 400 ms; not a quiet-machine number). Of that the OS read is 20–22 ms;
  the rest is the host's seek and software decode from the keyframe before the moment, 165–170 ms
  at 2,048 px and the same (166–169 ms) at 448 px: the decode, not the edge or the reader, is the
  cost. So a clip costs its moment count × ~0.2 s once, on one background worker, after its
  pictures; the People pass already pays one such decode a moment. Windows is not measured.

## Documents (#354)

PDFs and Word documents are searchable by the **words in them**: a quoted phrase from any page
finds the document and opens it on that page, plain words rank documents by how many of them a
passage holds. At **Words and pages** they are also found by **what a page looks like** ("a
receipt", "a floor plan"), and with the **Documents** piece by **what a passage is about** ("tax
return from my accountant" finds the page about one without those words). A **scanned** page,
one with no text layer, is read by the operating system, so its words are found the same way.
Slices D1 to D4 of [document-search](../plans/document-search.md).

- **Behind File types "Show documents"** (off by default), as audio files are behind "Show audio
  files": each chrome forwards it to the pack as `document_files` when it attaches and on every
  change. Off, the walk drops `.pdf` and `.docx` by name, so they cost nothing; on, every root is
  walked again; off again, documents leave results at once and the control thread deletes them
  and their text (`index_db::remove_kind`). A document is `asset_kind::document` (3; schema 9)
  and `MV_AI_KIND_DOCUMENTS` (4; `MV_AI_KIND_ALL` is 15, so an older chrome's 3 or 11 never sees
  one). It never pairs, never goes to the picture, People or text-in-pictures passes in D1, and
  an `.mvindex` export leaves it out, name and all (page pictures too), unless Documents is
  ticked for that export (v2, D3; "Sharing an index").
- **What a folder indexes them for.** Settings "Documents" (`doc_index`): 0 Off, 1 **Words**
  (the default: cheap, no model), 2 **Words and pages** (page pictures, below). Each folder
  may override it (`root_set_documents`, `MV_AI_DOCS_*`, `roots.docs`): the case is a Downloads
  folder that should search photos and not bank statements. A folder's Off leaves its documents
  out of the search at once and drops them with the walk that follows; a change of the setting
  walks every root.
- **Text.** A PDF's is read by **PDFium in the pack** (owner call 2): bblanchon/pdfium-binaries
  chromium/8066, pinned by SHA-256 in `cmake/ai.cmake`, staged beside `mv_ai` and loaded at run
  time (`pdf_pdfium.cpp`, `infer::dylib`), so the base installer does not grow and both OSes
  read the same text into the same index. The viewer still draws pages with the OS. PDFium is
  hostile-input code in this process: it runs on the index workers only, one call at a time
  (it is not thread-safe), at most 500 pages and 60 s a document, through a file reader that
  reads the blocks it asks for. A DOCX's is the **host's** (`document_text` on the host table,
  appended after `keep_alive`): `codec::docx_text`, the viewer's own DOCX reader and
  layout with no picture decoded, a paragraph at a time with the page that shows it.
- **Passages** (`passages.h`): a few sentences each (a passage closes at a sentence end past 40
  words, and at 80 wherever it is), never across a page, at most 20,000 a document. A PDF page's
  lines are joined (a word hyphenated over a line break is whole again); typographic quotes,
  dashes, no-break spaces and ligatures (ﬁ) are made plain. Each passage stores its text (the
  snippet) and its **words**: folded (`core/fold.h`) and split by the speech rule, led by the last
  8 words of the passage before, so a quoted phrase over a passage break is still one phrase.
  Schema 9: `passages(id, asset_id, spec, page, ord, text)` and `passage_text(rowid, words)`, FTS5
  when SQLite has it (else a plain table the engine scans); a deleted passage takes its words
  with it (a trigger, which the cascade from `assets` fires too). A changed file drops them.
- **Its own pass** (`track::docs`, progress spec `doctext/1`), after the pictures and before a
  clip's own text, one document a claim, `wait_turn` before each page (a DOCX: every 64
  paragraphs). A PDF commits every 8 pages, so a stop resumes at the next page; a DOCX is read
  whole (its layout needs every page before the one asked for) and a resumed one starts again. A
  password-protected file (PDFium's password error; a DOCX that is an OLE container, not a zip)
  is **`locked`** (progress state 5): never retried, counted apart. One that does not open fails
  (tried again up to three times). A PDF with no text layer (a scan) is done with no passages; D4
  reads those. With no PDFium (an older pack) or no `document_text` (an older host) a document
  is set aside (`unavailable`) and read again at the next start that can.
- **Scanned pages (D4)**, at Words (no setting of their own): a PDF page with **no text layer**
  (fewer than 16 characters other than spaces, so a stamped page or Bates number alone is still
  none) is drawn by the host (`video_frame_rgb`, `pts_ms` the page, at 2,048 px, the host's
  ceiling: 1,583 × 2,048 for a Letter page, about 186 dpi) and read by the **OS text reader of
  "Text in pictures"** (`text_reader.h`: Vision on the Mac, `Windows.Media.Ocr` on Windows; no
  model). Its lines are joined as a PDF page's are (`pdf_paragraphs`: a word hyphenated over a
  line is whole again) and stored as **passages of that page** under the reader's spec plus
  `/pages` (`ocr/vision-accurate/1/pages`), beside the text layer's `doctext/1` rows, which it
  never touches; search reads both alike, so a quoted phrase finds the scanned page and Enter opens
  it there. Its own pass (`track::scan`), **last of all**: after every other pass, the People pass
  included, and one document at a time (`pending`'s `after` is `doctext/1`). The pass asks PDFium
  again which pages from its resume point have no text layer (cheap beside a draw and a read),
  then reads **at most 4 pages a claim**, committing each (state partial, resume the next page),
  and returns: the next claim looks at every other pass first, so a 100-page scan never stands in
  front of a new photo for more than one chunk, and a stop resumes at the next page. A fresh read
  first drops what an earlier one stored. Like the words pass it **needs no model**, so it also runs
  while the picture towers load. A document read whole from its first page with no page lacking a
  text layer, and every DOCX, is done for this pass with its words, so a text library is never
  opened twice. Not behind **Read text in photos**: that switch is the photos' and clips'; a
  machine with no reader (a Windows without an OCR language) has no pass and no count. A page
  that does not draw or read is skipped; a document none of whose pages draw or read is done with
  nothing read, not failed. With the Documents piece the meaning pass (D3) waits for this one
  (`meaning_after`), so a scan's text is embedded too.
  `mv_ai_status` appends `scans_total` (documents whose words are read) and `scans_done`; while
  they differ the state is not idle, and both chromes read "Reading scanned pages 3 of 5
  documents" after the other passes' lines.
  **Measured** (M5, 2026-10-17; DEVELOPMENT, "slice D4"): over 50 synthetic scanned pages
  (`tools/ai-bench/doc-scans`), a quoted search finds a mean 99.4 % of a page's words, 49 of 50
  pages at 90 % or more. On the worst, 83.5 %, Vision skipped four lines at that scale. A dense
  page costs about 60 ms to draw and 640 ms to read, so a 100-page scan is about 70 s of one
  worker. While a Core ML compile holds the Neural Engine, Vision waits behind it. The real-machine
  timing of a long scan beside new photos, Windows and the Mac PR 1 gate with scans read are owed.
- **Page pictures (D2)**, at Words and pages: a document's pages through the **picture tower
  already loaded** (no new model), so a visual query finds the page. Its own pass (`track::pages`,
  progress spec `docpage/<picture spec>`, so a model change pictures them again) takes a document
  once its words are read (`pending`'s `after` is `doctext/1`; a locked or broken one never comes),
  after the words pass and before a clip's own text. Which pages: the **first 3**, plus any page
  **mostly covered by images** (PDFium's image objects, each clipped to the page, over half its
  area: a scan, a photo, a full-page figure; `pdf_reader::look`), at most **64** of those a
  document, within the 500-page and 60 s caps. A DOCX (and a PDF with no PDFium): the first 3,
  the host saying where the document ends. Each page is drawn by the host, `video_frame_rgb` with
  `pts_ms` the page (the viewer's own renderer, as the tiles are), at the photo pass's edge (448 px
  for ViT-B/32 and ViT-L/14), embedded in batches of 4, and stored in one transaction as `frames`
  rows under the tower's spec with `pts_ms` the page and `kFramePage` (0x100) in `flags` (no
  schema change), replacing the pages it had. A stop leaves the document pending: it is drawn
  again whole. A page that does not draw ends the document there; none drawn fails it (its words
  still find it). A folder or the setting going below Words and pages deletes those documents'
  page pictures and their progress on the control thread (`index_db::drop_pages`) and takes them
  out of the matrix. `mv_ai_status` appends `pages_total` and `pages_done`; while they differ the
  state is not idle, and both chromes read "Looking at document pages 12 of 200".
  **Measured** (M5, ViT-L/14, 2026-10-16; DEVELOPMENT, "slice D2"): 20 visual queries over 120
  labelled visual PDFs among 200 text documents reach mean P@5 0.80 as typed and 0.83 with Match by
  Picture only. Every hit is on the right page, and 316 documents (851 pages) are pictured in 49 s.
  Weak spots: a word match outranks a picture ("a receipt" against documents that say "receipt"),
  plain text pages pass for "a spreadsheet", and person queries meet the "nothing found" gate. The
  Mac PR 1 gate held while pages were pictured: 0 dropped, p99 17.05 ms against 16.80 alone.
- **Meaning (D3), with the Documents piece** (`ai-docs`, optional, as Sound is): a document's
  passages through a small sentence-embedding model, **all-MiniLM-L6-v2** (Apache-2.0, the
  Xenova ONNX export, fp32, 384 dimensions, ~91 MB), chosen by the spike below. Its tokenizer is
  BERT's uncased WordPiece in `infer/text_models.*` (BertNormalizer's cleaning, CJK split and
  accent strip: `core/fold.h` for Latin, Greek and Cyrillic, a generated NFD table past them,
  Hangul by formula), mean pooling, no prefixes, 256 tokens. It runs on CPU, 2 threads, in every
  compute choice: the same vector on every machine, and a query in ~2 ms. fp32 rather than the
  int8 export: int8 quantises each batch's activations together, so a passage's vector moved with
  its batch (cosine 0.998 against ORT-Python), for 10 % more speed and no gain in the spike.
  Its own pass (`track::meaning`, progress under the model's spec `all-minilm-l6-v2/fp32/pre1`)
  takes a document once its words are read, 16 passages a run between the viewer's busy spells,
  and commits the document whole (`passage_vecs`, schema 10: int8 with a scale, as frames, and
  each passage's best cosine against five generic prompts). A stop embeds it again. Like the
  words pass it does not wait for the picture towers to load or move to an accelerator. The
  vectors are a third matrix in memory (`meanings_`, the passage id as its "pts"), loaded at
  start and kept as passes commit; a passage dropped (a changed file, a resumed read) takes its
  vector. `mv_ai_status` appends `meaning_total`, `meaning_done` and sets
  `MV_AI_STATUS_DOCS_READY`; `settings_json` has `docs_ready` and `docs_model`. Without the
  piece nothing changes: Words matches as typed. Removing it keeps the vectors for when it comes
  back (they are dropped with the documents).
- **The spike** (D3, `tools/ai-bench/doc-meaning`): 478 public documents (141 IRS forms and
  publications, 164 arXiv papers on 15 subjects, 173 Wikipedia articles on 12 subjects written
  as DOCX or PDF, 29 of them in French, German or Spanish), 50 queries written from the titles
  before any model ran (3 not in English), each document scored by its best passage, the first 64
  passages a document. Targets fixed first: P@5 >= 0.70, R@10 >= 0.80, both above keyword
  BM25 on the same passages. P@5 counts relevant results in the top 5 over min(5, relevant).

  | Model (Xenova ONNX) | Licence | Size | P@5 fp32 / int8 | R@10 fp32 / int8 | passages/s, CPU 2 threads (M5) |
  |---|---|---|---|---|---|
  | **all-MiniLM-L6-v2** | Apache-2.0 | 90 / 23 MB | **0.810** / 0.806 | 0.827 / 0.832 | **112-119** / 118-132 |
  | snowflake-arctic-embed-s | Apache-2.0 | 133 / 34 MB | 0.810 / 0.794 | 0.831 / 0.834 | 57-63 / 59-66 |
  | bge-small-en-v1.5 | MIT | 133 / 34 MB | 0.780 / 0.757 | 0.838 / 0.802 | 59-61 / 62-69 |
  | multilingual-e5-small | MIT | 470 / 118 MB | 0.766 / 0.755 | 0.818 / 0.793 | 57-58 / 53-61 |
  | BM25 (keywords) | | | 0.662 | 0.639 | |

  On the 3 other-language queries e5 led (P@5 0.53 against MiniLM's 0.40, n = 3); on the 47
  English ones MiniLM did (0.836 / 0.851). The speed runs were alternated over three rounds on a
  machine other work kept at load 10-36. MiniLM met the targets with the best P@5, at twice the
  others' speed and a quarter of e5's size; the English-only cost is what Whisper's speech and
  the keyword path already cover in other languages.
- **Ranking by meaning** is CLAP's rule: a passage is a result when it beats the generic prompts
  ("a document.", "some text.", "a page of a document.", "this is a paragraph of text.",
  "information.", embedded as queries) by `result_margin` 0.25, and a query answers only when one
  of its ten best passages beats them by `query_margin` 0.415 ("nothing found"), both scaled by
  Precision. Calibrated in the spike, the way CLAP's margin was: at 0.415, 47 of the 50 queries
  answer (the other 3 within 0.025) and 1 of 20 queries about nothing in the corpus ("a gym
  membership contract"); the 0.25 row floor costs no P@5 or R@10. A hit is a row at its passage's
  page with the passage as its snippet (`MV_AI_MATCH_TEXT`, as a word match), merged per document
  with the words. The margins gate; the **cosine orders** (by margin the spike lost 0.03 of P@5
  and R@10): a row's rank is 2 x (cosine - `rank_offset` 0.096, the mean generic cosine) /
  `query_margin`, so a close passage (~2.4) leads a document that only holds most of the words
  (0.6-1.6).
- **Measured end to end** (M5, `ai-bench --documents` over the spike's 478 documents with every
  passage embedded, 93,726, the 50 queries as typed, words and meaning merged; DEVELOPMENT, slice
  D3): **P@5 0.731, R@10 0.767** (English 0.748 / 0.784), against 0.304 / 0.272 for the words
  alone (no piece; 19 queries answer nothing) and 0.612 / 0.631 for BM25 on the same passages. The spike's 0.810 / 0.827 was over the first 64 passages a document: uncapped, the same
  model scores 0.757 / 0.782 alone, because the long IRS publications' thousands of passages
  outrank the forms a query is about. **The R@10 target (0.80) is not met** on the whole corpus.
  One query ("turning recorded speech into written text automatically") meets "nothing found".
  Embedding all 478 took 838 s of the run (about 112 passages a second on the workers, with the
  machine at load 10-36); queries took up to 94 ms.
- **Search.** Candidates come from the FTS5 index; the engine's own rule decides each, so the
  index only narrows. A **quoted phrase** matches a passage's words in order (anywhere, as it
  matches speech, cues and text in pictures); `text:` / `says:` matches documents only; `said:`
  never a document. **Plain words** match by the speech rule (most of the words; a word of four
  letters or more also matches its longer forms) when `MV_AI_FIND_TEXT` is asked (none set: all),
  beside the picture search. Each matching passage is a row at its **page**: `pts_ms` is the
  0-based page, `kind` `MV_AI_KIND_DOCUMENTS`, `match` `MV_AI_MATCH_TEXT`, the snippet the part of
  the passage around the first word, shown unquoted. `clip_matches` returns a document's matching
  pages, one each. A page picture is a picture row like a clip's moment (`MV_AI_MATCH_PICTURE`,
  `pts_ms` the page, under `MV_AI_FIND_PICTURES` and the "nothing found" rule); a document matched
  both ways is one row at its better page. `is:document` / `is:doc`, a leading or trailing `documents` / `docs` / `pdf` /
  `pdfs`; `is:pdf` and `is:docx` (`is:word`) name the format by extension.
- **Results.** The tile is the matched page: page 0 is the viewer's own thumbnail, any other is a
  moment row of the JPEG-512 cache (`path#t=<page>`), drawn by `video_frame_rgb` with `pts_ms` the
  page (the host's renderer, so the tile is what the viewer shows). The chromes show "p. 3" where a
  clip shows its time. Enter opens the result list with the page as the item's moment; both hosts
  open a document there (`present_lab_mac::open_item(..., page)`; Windows `mv_folder_select_page`
  from `edit_item_opened`), and Page Down goes on from it.
- **Settings and status.** Both chromes: a Documents section (Off / Words / Words and pages, the privacy line "an
  index of documents holds their text on this computer; it never leaves it"), a per-folder
  "Documents: Default ▾" menu, the folder's document count beside its photos and videos (counted
  apart: `roots_json` `documents`, `documents_done`, `docs`; "Up to date" covers the picture pass
  only), and the panel's **Documents** segment and **Text** token while documents are shown.
  `mv_ai_status` appends `docs_total`, `docs_done` (failed and locked count as done), `docs_locked`
  and `docs_failed`; while documents are short the state is not idle, and the line reads
  "Reading documents 12 of 200". `settings_json` has `document_files`, `doc_index` and
  `pdf_available`.
- **Privacy.** A document's text is the most sensitive thing the index holds. It stays in
  `index.db` beside the transcripts, never in a log, a crash report or telemetry, and in an
  export only when Documents is ticked for it (D3, off by default); Clear index, removing a root, a folder's Off and Show documents off delete it.
- **Never waits for the models.** Every other pass waits while the picture towers load or move
  to an accelerator (about two minutes for ViT-L/14 on the M5's Neural Engine at each pack start,
  more on a first compile). The document pass reads words and needs no model, so while they do,
  `claim` hands the workers documents alone, straight from the index (`wait_turn(false)`); the
  pause, the viewer's yield, a clear and an import still hold it.
- **Measured** (Apple M5, `ai-bench --documents`, a synthetic 200-document folder, three runs):
  every document read within 3 s of the pack loading, the 500-page PDF included; a quoted phrase
  from the first and from the last page of each of the 196 readable ones found that page (392 of
  392, the slowest query 4.1 ms); plain words 70 ms over 190 matches. The Mac PR 1 gate held with
  4,000 documents being read (0 dropped, p99 16.90 ms against 16.95 alone). Windows is not
  measured.

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
the merge. The same settle, without reading any asset again, is owed when faces.db's people were
filed under older clustering rules (meta `rules`, `faces_db::settle_due`; rules 2 = the quality
floor, 2026-10-06): ~13 s once on the control thread for the owner's 36 k faces. Pinned faces never move. A new face model in a pack update starts the same re-run by
itself (faces.db notes the spec it last opened with). Progress: `MV_AI_STATUS_PEOPLE_RERUN` /
`_SETTLING`, `people_scan_total` / `_done`, `people_model_utf8`. Settle cost: ~0.3 s for 6 k
faces on the control thread.

### Online clustering

`faces.cpp`. Scores are in pairwise cosine units everywhere — assign, "this person",
consolidate — never cosine to a normalised centroid (which inflates a stranger by ~√ρ). A new face
is matched in two steps. Every person is scored by mean pairwise cosine (sum · v / n), and the
best `shortlist` (3) at or above `same_person − 2 × margin` are kept. Then each shortlisted person
the user **confirmed** (it has a pinned face) is rescored by its **exemplars**: the refinement's
`support`, the mean of the top 3 cosines to its pinned faces and good core (≤ 12, built by
`build_person_proto`, the refinement's own `build_proto`). Every other person keeps its mean.
The face joins the best person at `same_person`. If a second shortlisted person is within
`ambiguous` of the best, it waits **unassigned** (`person_id` NULL). "This person"
(`nearest_person`) uses the same scorer without the ambiguity rule. **Quality gates it:** an
*unusable* face (quality < 0.10) joins nobody. A *weak* one (< 0.35) may join a person but never
starts one, and it is matched by the mean alone, because the refinement never admits a weak face
by exemplars either. The idle consolidate merges at mean pairwise `merge_at`, an average that
stays an average after a merge (no chaining). The user's faces are **pinned**: faces they split
out, the cover of a person when they name it, and both covers when they merge. Covers prefer
pinned faces.

**Corrections are what new faces meet (2026-10-15, #434).** Before this change a person's
corrections counted only in the refinement. Online, a new face met the average of every member,
so a varied person ("Tristan", front-on and side-on) scored *lower* as he grew. Now the faces the
user confirmed are anchors for every new photo: a side-on face whose mean to him is below the
threshold joins once a side-on cover of his is pinned. The exemplars are cached per person in
`faces_db` and rebuilt lazily, under the lock, only for a shortlisted confirmed person. Only the
first `max_refs` (256) refs' vectors are read. The cache is dropped on a rename, merge, split or
reject, when faces leave or are re-analysed, and on an import. A refinement commit replaces it
with the protos it just built. Ordinary growth rebuilds it only once the person is a quarter
larger or smaller than when it was built. **Unconfirmed people keep the mean.** On LFW, scoring
every person by exemplars let a newcomer whose mean was just under the threshold join a lookalike
(IR-50 online BCubed F 0.9975 → 0.9945, P 0.9991 → 0.9933). Restricted to confirmed people, LFW
is unchanged (`[.people-bench]`: IR-50 online F 0.9975, +merge 0.9980, settled 0.9988; SFace
0.9899 / 0.9893 / 0.9938). The bench's corrections phase merges each person's fragments and names
everyone with 3+ photos after 60 % of the faces, then files the remaining 40 % online. There, SFace's
remaining 40 % rises from F 0.9964 (P 0.9983, R 0.9946) to 0.9969 (P 0.9972, R 0.9966), and
IR-50's is unchanged at 0.9989. On a copy of the owner's library (`[.people-api]`), the 3,284 unfiled usable faces offered
again cost 0.28–0.30 ms per `add` against 0.27–0.33 ms before (runs alternated), and one more of
them files to a named person (556 against 555). The settle (`[.people-settle]`) does not use
this path and is unchanged (263 people in 2+ photos, 76 in 5+).

**Cost at a large library (2026-10-06, owner: "a lot of other UI seems to lag").** Every People
read holds the engine's model lock, which a search, a rename and the status counts wait on, so
none may grow with the library. `faces_cover (person_id, pinned, quality, score·w·h, asset_id)`
answers the grid's covers and photo counts without the face rows (each carries a 2 KB vector);
the idle merge looks only at pairs with a person changed since its last pass; a face landing sets
a flag the control loop turns into one `AI_PEOPLE` a second, not one per frame; the Mac's People
re-reads only while Settings shows them; `note_folder_opened` (a host's UI thread) hands the
folder to the next claim instead of waiting on the work queue's lock. On the owner's library
(36 k faces, 2 k people; `"[.people-api]"` on a copy): people list 69 → 3 ms, scoped 83 → 4 ms,
the per-second person count 34 → 1.7 ms, the minutely merge 550–770 → 8–11 ms.

**Quality** = min(size, sharpness, frontal) × (0.6 + 0.4 × detector score): size ramps 40 → 112
px, sharpness is the Laplacian variance of the aligned crop's luma on a log ramp 20 → 300,
frontal is the nose's offset from the eye midline against half the eye distance. Weak = quality
< 0.35; unusable = quality < 0.10 (`refine_params::unusable_quality`). Rows without a recorded
quality get a proxy from score and box size.

**Why the floor (2026-10-06).** On the owner's library (36 k faces, 31 k of them clip frames)
54 % of faces scored below 0.1: blurred frames, 25–60 px strangers, hoods and ears. Their vectors
resemble each other more than anyone, so online clustering made "people" of them — the largest
was 297 frames of one clip, every one below 0.1 — and 424 of the 710 people in two or more photos
were more than 80 % weak faces. The same library settled under the floor (`mv_ai_tests
"[.people-settle]"` on a copy): 284 people in two or more photos, 6 of them mostly weak (named
ones), mean face quality per person 0.21 → 0.58; named people kept 7,645 of 7,767 faces. LFW
(`[.people-bench]`, every face clear) is unchanged: F 0.9988, 426 people.

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
| **evict** to unassigned | support(own) < `keep` (`keep_weak` for a weak face); always when unusable, or when its person is unnamed, unpinned and has no face that is not weak (`weak_only`: nothing vouches for it, and it is no candidate) |
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
photo or clip under `scope_dir` by the same scopes `search_text` takes. `faces` counts the
**photos and clips** they are in there (not faces: a clip sampled every few seconds gave one
stranger hundreds, 2026-10-06) and the cover is the clearest of those (pinned, then quality), so a
card's count and face are the folder's. *Who
qualifies at all* (minimum faces, or a name) is still judged over the whole index. The minimum
is two photos or clips, or **one when fewer than 100 photos and clips are in view** (the scoped folder's,
or the whole index for Everywhere and the status count): in a small folder nobody may appear
twice, and the grid said "No people found yet" over five clear faces (owner, 2026-10-05). A
large library keeps two so a stranger in one photo does not fill it (`people_min_faces`).
`faces_db::people(min, assets)` does one indexed pass per person against the scope's asset set.

Chrome: a "People in · This folder | + Subfolders · <folder>" control at the top of the People
grid (Settings → People on both platforms). Default **+ Subfolders**; it
follows the folder the viewer opens. Everyone shows only when no folder is open (the viewer's
home): the control is then absent. A scope
of Everywhere left from before reads as + Subfolders when a folder opens. "Show photos" on a card
and the empty-grid wording search the same scope. The choice is not persisted. The status bar's
people count stays the whole index's. A pack without the entry shows everyone. The search panel
keeps all three scopes.

**The first 24 (2026-10-06, owner: "200 people … the settings screen now scrolls quite slowly").**
Settings shows the first 24 people (named, then most photos) and "Show all N people". On the Mac
the base Settings list is not lazy, so every card in it is live: "Show all" opens a sheet whose
grid is in its own scroll view, where only the cards on screen exist (`AllPeopleSheet`;
`PeopleGridBench`: a re-sorted list costs 16 ms of CPU with 24 shown against 89 ms with 200). On
Windows the repeater is virtualised already, so "Show all" expands the grid in place.

## Sharing an index

Index a library once and carry it to another machine (`src/addons/ai/transfer.*`,
`export_index` / `inspect_export` / `import_index`, both chromes).

**The file** (`.mvindex`, SQLite, `info.format = "mediaviewer.index"`, version 2; a version 1 file
still imports) holds the chosen
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

**Documents** (v2, #354 D3; `MV_AI_TRANSFER_DOCUMENTS`): off by default, as People is, because
the file then holds what the documents say. Ticked, it carries the documents (kind 3), their
passages and words (`passages`), their passage vectors (`passage_vecs`, with `info.docs_spec` the
text model) and their page pictures (frames with the page flag, whose progress is
`docpage/<picture spec>`). Unticked, no document travels, nor its name, as in v1. An import takes
them only when ticked there too and documents are shown on that machine; the passages get new ids
and their vectors follow them. Vectors come only with the passages they belong to: a document
whose words this machine had already read keeps its own and embeds them itself. `inspect_export`
reports `documents` (`count`, `shown`, `meaning_match`: the same text model is loaded here).

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
  progress are in the status. The status line treats the queue as the rest of the same run
  (issue #417, 2026-10-15): while the pictures pass still has this Mac's files, its count and ETA
  are that pass only, so the line says "Indexing pictures 24,442 of 24,443 on this Mac · under a
  minute" and the note under it carries on with "Then 1,833 clips from iCloud · downloading the
  next (42%), each indexed as it lands" (or why the fetch waits: for indexing to catch up, power,
  an unmetered network, 10 GB free). Once nothing local is left the line is "Downloading from
  iCloud · 1,847 clips left". Clips put off to the next launch are not counted as this run.
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

### Model tiers: the picture towers (#482, 2026-10-18)

`tools/ai-eval/calibrate.py` on COCO-1k (val2017's first 1,000 images, five captions each),
ONNX Runtime 1.30 on CPU, Apple M-series, the pack's preprocessing. Bar: R@10 ≥ 0.88 and
P@5 ≥ 0.8. Calibrated thresholds are a quantile match against B/32's hand-set ones.

| Tower | Tier | R@1 | R@5 | R@10 | P@5 | Passes | img/s, 2 CPU threads |
|---|---|---|---|---|---|---|---|
| CLIP ViT-B/32 | Default fast, Intel, Potato | — | — | 0.881 | 0.943 | yes (reference) | 25.6 |
| CLIP ViT-L/14 | Default high | 0.553 | 0.813 | 0.904 | 0.914 | yes | — |
| OpenCLIP ViT-H/14 | Maxxed high | 0.687 | 0.894 | 0.946 | 0.971 | yes | — |
| TinyCLIP ViT-8M/16 | (Potato candidate) | 0.415 | 0.731 | 0.838 | 0.886 | **no** | 23.4 |
| TinyCLIP ViT-39M/16 | (Potato candidate) | 0.592 | 0.852 | 0.922 | 1.000 | yes, but slower than B/32 | 6.2 |

The img/s column was timed while another eval ran, so read it as a ratio. On L/14 the method
gives query_margin 0.033 and result_margin 0.010 against the 0.04 and 0.015 set by hand in PR
20: it lands near the hand calibration, so ViT-H/14's (0.023, -0.015, similar_min 0.449) are
used as computed. **Not measured yet:** per-tier img/s on an accelerator, Whisper RTF, time to
first searchable result and size on disk per tier, on either platform, and both present-loop
gates while indexing on every tier (plan T8).

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
  one core (`mv_ai_tests "[.perf-bench]"`, which fails over 100 ms in optimised builds); with the ~30 ms
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

- Documents' page pictures (D2), meaning search over passages with a text model (D3, the
  `ai-docs` piece, `.mvindex` v2 carrying passages) and OCR of scanned pages (D4)
  ([document-search](../plans/document-search.md)).

- Hardware decode (D3D11VA / VideoToolbox) for the frame sampler; it decodes in software.
- An OpenVINO piece (the code path exists, nothing is published).
- A memory-mapped search matrix (it is loaded into memory).
- Local search on Intel Macs (no x86_64 ORT).
- Place names in the index and query language (places are the Places add-on's, answered at query time).
- Reading a clip's text from the picture pass's own decode: each moment is seeked and decoded
  again, which is most of a moment's cost ("Text in pictures").
- A "nothing found" z calibration for CLAP.
- Background (unrequested) People refinement; the incremental refinement path
  (`refine_begin(false)`) exists in faces.db and its tests but has no caller.
