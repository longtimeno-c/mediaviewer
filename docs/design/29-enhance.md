# Enhance

The Enhance add-on: a restored copy of the photo, clip or audio file on screen, made by a model
that runs locally and is downloaded only on request. The output is always a new file (rule 5).
What is left: [the plan](../plans/enhance.md).

Code: `src/addons/enhance` (`cmake/enhance.cmake`), the host's `edit/sound_writer.h`,
`edit/picture_io.h`, `edit/export.h` (`full_picture`, `export_pixels`), and the sound, picture and
photo rows of the host table ([14](14-abi.md)). Decisions: [12](12-decision-log.md).

## What it is

One slot add-on with no chrome of its own ([25](25-open-addons.md) §10); both hosts draw its
screens from its view trees.

| Surface | Place | Opened by |
|---|---|---|
| Settings → Add-ons page: Performance card, one card per kind (model, credit, size, Download / Stop / Resume / Remove) | `settings` | Settings |
| The sheet (root a `scroll`; host header has title and Done) | `sheet` | `Enhance…` in palette / context menu, `Ctrl+Shift+R` / `⌘⇧R` ([16](16-commands.md)) — audio files and clips only |
| The sheet over the Video Editor | `editor_items` (contribution API 11) | `Enhance…` in the editor's bar |
| The Enhance tab's cards | `edit` (contribution API 13) | the Edit pane's Enhance tab — every photo tool ("One click") |

Not in the `ai` family. Offered on Windows and Apple silicon only (no x86_64 macOS ONNX Runtime);
one-click install via `SlotAddonInstallOffer` (SlotItems.swift), `IslandHost.Enhance.cs`.

## Models

Each kind ships up to eight `models/<kind>/<id>.json` in the signed zip (id `[a-z0-9_-]{1,32}`).
`model.json` is used until the person picks another (a `Model` choice on the card / sheet section,
shown only when the kind has more than one; kept as `use` in `enhance.json`). Each model downloads,
stops and is removed on its own, keyed `<kind>.<id>`; a run keeps the model it started with. Core ML
cache: `models/<kind>-cache` for `model.json`, `models/<kind>-cache-<id>` otherwise.

| Field | |
|---|---|
| `kind`, `name`, `credit` | kind, display name, weights' author and licence |
| `summary` | optional: what choosing it means |
| `weights` | files, pinned under the manifest's `remote` at `<kind>/<file>` |
| `run_bytes` | free memory a run needs (the run gate) |
| `cpu` | one unit of work fits a measured CPU budget |
| `knobs` | `slider` / `choice` / `toggle` / `text` (≤ 200 bytes), each with the target kinds it applies to |
| `io` | runner section (sound: `runner: stream`, rate, frame, state, delay; pictures: `runner: tiles`, scale, tile, overlap, free dims; also `diffusion`, `inpaint`, `face`) |

Weights are never in the zip. The signed manifest pins each file under `remote` (Hugging Face URL at
a fixed revision, size, SHA-256, licence; `src/addons/enhance/remote.json` is what `addon-pack.py`
signs in). Download uses the AI pack's `mv_model_fetch` ([17](17-local-ai-search.md) "Model tiers")
into the add-on's data folder: plain GET, no query/cookies/token, Range-resumed (also across
restarts, but only for a kind with partial files whose pin is unchanged), kept only on SHA-256
match. Refused up front: a pin over **5 GB** (also refused by `addon-pack.py`), or a disk without
room for the rest plus a tenth.

| Kind | Model | Weights | Licence |
|---|---|---|---|
| audio | DeepFilterNet3, streaming ONNX (`kimtos-labs/denoiser-dfn3` @ `888f33c4`) | 16.1 MB | MIT / Apache-2.0 |
| video | "Detail, fast" (`model.json`): Real-ESRGAN general x4v3 (`jonathanst29/tinier-upscale-models` @ `899dc1e4`) | 4.9 MB | BSD-3-Clause |
| video | "Detail, best" (`x4plus.json`): Real-ESRGAN x4plus, same repo/revision, `input` [1,3,h,w] → `output` [1,3,4h,4w] | 67.1 MB | BSD-3-Clause |
| still | **Sharpen & restore**: Real-HAT-GAN SRx4 sharper (`MahoCommerce/Real_HAT_GAN_sharper-ONNX` @ `f0c8c3a4`, `onnx/model.onnx` saved as `Real_HAT_GAN_sharper.onnx`) | 161 MB | Apache-2.0 |
| still | "Real-ESRGAN" (`x4plus.json`): x4plus pinned again under `still/` | 67.1 MB | BSD-3-Clause |
| natural | **Natural**: 4xRealWebPhoto v4 DAT-2 (`Phips/4xRealWebPhoto_v4_dat2` @ `95e9cd50`) | 48.8 MB | CC-BY-4.0 |
| denoise | **Remove noise**: SCUNet real-PSNR (`deepghs/image_restoration` @ `77885239`) | 91.3 MB | Apache-2.0 |
| deblur | **Remove blur**: NAFNet-REDS-width64, same repo/revision | 275 MB | MIT |
| motion | **Remove motion blur** (not offered, `kOfferMotionBlur`): NAFNet-GoPro-width64, same repo/revision | 275 MB | MIT |
| remove | LaMa (`Carve/LaMa-ONNX` @ `c3c0c9e4`, `lama_fp32.onnx`) | 208 MB | Apache-2.0 |
| restyle | InstructPix2Pix (`timbrooks/instruct-pix2pix` @ `31519b5c`) via `tools/package/restyle-export.py`: fp32 text encoder and VAE, fp16 UNet | ~2.5 GB | CreativeML OpenRAIL-M (restrictions shown with the credit) |
| face | RestoreFormer++ (`facefusion/models-3.0.0` @ `728b9659`, `restoreformer_plus_plus.onnx`) and GFPGAN v1.4 (`neurobytemind/GFPGANv1.4.onnx` @ `e3fd467e`) | 294 + 340 MB | Apache-2.0 |

## The sheet

Built from the installed models. The item's type comes from its first bytes (`sniff.h`), then the
host's decoders (`sampler_open`, `sound_open`).

- Audio file: the audio model's knobs. Clip: picture, sound or both (what it holds). Document,
  animation, unreadable file: greyed, with the reason.
- A kind whose model is missing offers Download / Resume, then progress and Stop, never knobs.
- Knob values are remembered per target kind. Changing a knob runs nothing; the button runs.
  Closing the sheet or Cancel cancels the run.

## A run

One job at a time on the runner's own thread — never UI, render, the player's decoder or the render
device. Gate: `run_bytes` against available memory (`memory_*.cpp`); short, the sheet says how much
is needed.

**Sound.** Read at the model's rate, source channels (`clip::audio_reader`); per channel,
480-sample frames (10 ms at 48 kHz) with carried state (`sound_job.h`); the 30 ms lag is trimmed and
flushed so output aligns sample for sample. "Noise reduction" mixes with the original (1 = model
only). ONNX Runtime loads from the add-on folder on first run (CPU, one thread).

`edit/sound_writer.h` writes beside the original: `<stem>-enhance.m4a`, or for a clip its picture
packet-copied with the new sound (MP4 / MOV, else Matroska). AAC from the OS (AudioToolbox / Media
Foundation, ~96 kb/s a channel), never FFmpeg's ([11](11-licensing.md)); else FLAC
(`edit/clip_audio_enc.h`). Hidden part file, then the first free name ("name (2)"); cancel leaves
nothing; cover art and extra tracks dropped. Runs in-process, not in the clip helper.

**A photo.** Decoded like an export (`edit::full_picture`), run through the chosen model
(`restore_picture`), written as `<stem>-enhance.jpg` (q 92; `.png` for PNG) with metadata as export
carries it (`edit::export_pixels`). Size ×1/×2/×4, ≤ 100 MP and 16384 px a side; the gate adds the
picture in and out and one tile-high band of float sums. The photo being edited carries its edit:
the UI thread hands it to `addon_media::set_still_edit` (small copy under a mutex), and `full_still`
for that path returns `edit::edited_picture`. A Photos-library photo (`photos:<id>`) comes via the
resolve hook; the result is a new library photo ([26](26-photos-library.md) "Edits of a library
photo").

**A clip's picture.** Every frame decoded on the host's own decoder (`edit/picture_io.h`
`frame_reader`) to 8-bit sRGB (`clip_pixels.h`); HDR refused. 256-px tiles, 16-px overlap, through
Real-ESRGAN 4×, each tile's centre averaged down as placed (`picture_job.h`). Size ×1/×2/×4, never
past 3840 × 2160. **Strength** mixes with a bilinear enlargement. Apple silicon: Core ML (fixed tile
so it compiles; cache `models/video-cache`); else CPU, all cores but one.

**The preview.** During a clip run the sheet shows a split frame, original left / enhanced right
(`preview.h`; 960-px copies, first frame then ~1/s; surface 160–320 pt, ≤ 2048 × 1024, `"glow"`
edge: `GlowTiming` in ScreenView.swift, `GlowMath` on Windows). Below it the one progress line
([20](20-edit-workspace.md) "Craft rules"), time left by the host (`"timed"`, contribution API 14).
Sound-only runs show no preview.

The host's `picture_writer` encodes on a **hardware** encoder (`edit/hwencode.h`; HEVC first for an
HEVC source), bitrate scaled by area, BT.709, source frame times and rotation. Sound is packet-copied,
or for picture + sound, the cleaned sound from a parallel thread through a 5 s bounded queue. No
hardware encoder at that size: the sheet says so, nothing is written.

## Enhance image: a photo restored

Five models, each its own kind (`spec.h` `kStillKinds`), downloaded only when chosen: **Sharpen &
restore** (`still`, default), **Natural**, **Remove noise** (`denoise`), **Remove blur** (`deblur`),
**Remove motion blur** (`motion`, hidden by `kOfferMotionBlur = false`; its export blows up on about
a third of photos — shown in Settings only if its files exist, so they can be removed). The choice is
`"detail"` in `enhance.json`. The two upscalers (HAT, RealWebPhoto) have **Size** and run shrunk to
the detail; the three restorers keep the size. All have **Strength** (mix with the photo or its
bilinear enlargement) and a credit line.

The run (`picture_job.h` `restore_picture`): tiles at the model's side (HAT 192 → 768,
RealWebPhoto 256, SCUNet/NAFNet 512) kept inside the photo (last one pulled back to the edge; a
photo under one tile is mirrored out); overlap ⅓ tile for HAT, ¼ otherwise (`io.overlap`). Each
result Lanczos-3 downscaled, blended by a **tent weight** (1 mid-tile → 0 across the overlap, flat
at photo edges); rows written once no later tile reaches them. Progress and Cancel per tile. fp32
CPU (`io.backend: "cpu"`).

**Shrunk to the detail.** A ×4 model restores detail at its input's pixel scale, so a soft photo is
first shrunk to where its detail lives. `detail_shrink` (ms): on the three busiest 320-px crops of
nine, the largest d in 1.25…4 (quarter steps) whose Lanczos-3 down-up round trip loses ≤ 3 % more
gradient energy than a 1.15 round trip (or ≤ one 8-bit level² where nearly flat); 1 if even 1.25
loses detail. d = 1 runs as before; otherwise resize to 1/d, run at the least model scale covering
the asked size (`plan_shrink`), resize to source × Size (`restore_at_detail`; Strength mixes with the
photo itself enlarged). The gate adds the shrunk copy, the model's result and the resize rows.

**A tile the model blows up.** A result outside −1…2, non-finite, or with > 0.1 % of samples past
−0.15…1.15 is retried mirrored left-right, upside down and transposed (each turned back); if all fail
the tile is the photo itself, cross-faded. The result note says how many parts were left as they
were. Bad pixels never reach the file.

## Restyle: a typed request on a photo

A text request ("make it a snowy winter day"); empty is refused. `diffusion.h`, `io.runner:
diffusion`:

1. Scale the edited still to **Quality**'s long edge (`io.qualities`: Standard 512, High 768, Best
   1024; sides multiples of 64). Memory gates 12 / 18 / 32 GB; over it, refused with a lower quality
   suggested.
2. CLIP tokenizer + text encoder for the words and an empty prompt.
3. VAE encode; Euler-ancestral steps by performance level (linspace timesteps, fixed seed), UNet over
   a batch of three (text+picture, picture, neither) with *Follow the words* 7.5 and *Keep the photo*
   1.5.
4. VAE decode; `carry_back` applies the blurred after/before ratio to the full-resolution photo,
   scaled by *Strength*.
5. Written as a new file.

UNet on Core ML with dims fixed per size (compiled once, cached), else CPU; the model is not `cpu`,
so a machine with no GPU provider (Windows today) is refused. Restyle changes the whole look; it
cannot remove objects (that is Remove).

## The performance level

A **Performance** card on the settings page: *Automatic* (`addons/enhance/tier.h` `recommend_tier`,
Local search's rule), *Potato*, *Intel*, *Default*, *Maxxed*, kept in `enhance.json`; node
`tierlevel` shows the level in use. `tier_policy` bounds a run:

| Level | Threads | Restyle steps | Restyle quality (default = highest) |
|---|---|---|---|
| Potato | 2 | 20 | Standard |
| Intel | half the cores | 25 | Standard |
| Default | all cores but one | 40 | High |
| Maxxed | every core | 50 | Best |

A quality above the level is listed disabled with the level it needs (choice option `"disabled"`,
[25](25-open-addons.md)); a chosen quality is used while the level allows it, else the level's
highest (`screens.h` `level_quality`). A request naming a quality above the level is refused. The
Enhance tab shows the level as a header chip ([20](20-edit-workspace.md) "The Enhance tab").

## In the Video Editor (2026-11-02)

`Enhance…` sits before Export (`EditorAddonItems` in VideoEditorView.swift; `RebuildEditorAddons` in
IslandHost.VideoEditor.cs), from the add-on's `editor_items`. A click hands the add-on the clip and,
if cut, the kept pieces (`(in, out)` source ns; `mv_slots_editor_item`, ABI 0.35; Mac
`invoke_editor_item`). The sheet opens over the editor and describes the edit ("3 pieces, 1:24 in
all"); again on an open sheet moves it to the new edit.

The host applies the cut as it reads: rows `frames_keep` / `sound_keep` (`src/core/keep_ranges.h`,
thunks in `addon/host.cpp`), called once after `frames_open` / `sound_open`; output is on the edit's
timeline; a frame is kept when `in <= pts < out`. Every cut run goes through `run_picture`: picture model as usual; sound only →
picture re-encoded unchanged; picture only → the edit's sound re-read at 48 kHz and re-encoded (OS
AAC). One generation of re-encode.

## Brighten dark scenes

A clip-picture knob in the video `model.json` (`brighten`: Off, A little, More); no model of its own
(`brighten.h`), applied before the Detail model.

- Frame brightness = geometric mean of linear luminance (every 4th pixel each way). Below target
  (0.09 / 0.15), gain up to 3× / 6× via `f(x) = g x / (1 + (g − 1) x)` on linear R, G, B (white stays
  white).
- Gain eased in log space, 0.5 s time constant on frame times; a cut (half the histogram moved)
  jumps.
- One 256-entry table per frame over 8-bit sRGB (~1.6 ms at 1080p).
- **Strength 0 at the same size skips the model.** HDR clips still refused.

## One click

Every photo tool runs from the Enhance tab's cards — no sheet, no panel.

**The cards.** The host opens the `tool` screen (place `edit`) while the Enhance tab shows and points
it at each photo (`enhance/tool {"path": …}`; a run stops on photo change). Root `cards`
(`screens.h` `cards_tree`): one `card.<word>` per tool with a pinned model, drawn inside the host's
card.

| Card | Choices | Estimate | Button |
|---|---|---|---|
| Enhance image (`detail`) | Quality (the four models), Size (upscalers) | "About 2 min · 6048 × 8064" | *Enhance* |
| Restyle | the words (Return runs), Quality | "About 40 seconds" (+", more the first time" above Standard) | *Restyle* |
| Restore faces (under Faces) | Look, Strength | "About 2 seconds a face" | *Restore faces* |
| Remove, Extend | none (tool bar's Remove ↵ / Fill ↵ runs them) | the run's own | only *Download (208 MB)* |

A missing model makes the button *Download (161 MB) and enhance*: it downloads, the tool bar shows
progress, and the run starts itself (`engine::go`); Stop the download is on the card.

**The estimate** (`eta.h`): tiles counted as `restore_picture` would on the picture it sees (shrunk
by `detail_shrink`, Size via `plan_shrink`), × measured s/tile at 8 threads (HAT 5.4 s / 192 px,
RealWebPhoto 10 s / 256, SCUNet 4 s and NAFNet 1.25 s / 512), × 8 / level threads (clamped 0.5…4),
+ ~1.5 s per 12 MP read and written. The photo is read on the engine's own thread while no run goes
(*Estimating…* until then). Restyle: steps × 0.8 / 2.15 / 5.4 s (Standard / High / Best). Restore
faces: 1.6 s (RestoreFormer++) or 1 s (GFPGAN) a face, scaled the same.

**The run.** The tree's hidden `run` node (`idle | running | downloading | done | failed |
cancelled`, `run.id`, `run.progress`, `run.verb`, `run.line`, `run.result`) drives the host
(`shell/enhance_run.h`):

- **Glow over the photo** while working (not while only downloading): edge light coloured by six
  drifting blobs, 700 ms in, 300 ms out (`tool_overlay.h` `draw_work_glow`, one ImGui body for Metal
  and D3D11; Mac lab `--work-glow` holds it). Presents only while moving; Reduce Motion: still.
- **Tool bar line** ([20](20-edit-workspace.md) "Craft rules"): "Enhancing · 40 % · about 1 min
  left" + Cancel, composed by the host from `run.verb`/`run.progress`; then "Saved as
  IMG_0007-enhance.jpg" + Move to Trash / Recycle Bin (`recycle_file`, only the written file), or the
  reason + Try again (not for Remove/Extend), and ×.
- **The result opens** once per run the host saw working: navigates to the new file (library photo by
  key), Edit stays on the Enhance tab, result cross-fades in; Compare compares with the source
  ([20](20-edit-workspace.md) "Compare", `note_result`). Failure or cancel: glow stops, photo stays.

Hosts: Mac `EnhanceRun.swift` (`mv_chrome_edit_enhance_run`), `EnhanceHubView.swift`,
`ToolBarView.swift`, `main_mac.mm` `-enhanceCardsSync` / `-enhanceRunState:`; Windows
`IslandHost.EnhanceDock.cs` (`chrome_edit_action::enhance_run`), `IslandHost.EnhanceHub.cs`,
`IslandHost.ToolBar.cs`, `main.cpp` `enhance_cards_sync` / `enhance_run_state`. Palette rows, the
context entry and the strip's Enhance… on a photo open the Enhance tab; audio, clips and the Video
Editor keep the sheet. Tests: `mv_import_tests "[enhance][tool]"`, `mv_tests "[enhance-run]"`.

## Docked in Edit (2026-11-05)

Replaced by "One click"; what remains is the `tool` screen mechanism it introduced. The `tool`
screen is in place `edit` (contribution API 13), opened only by a command
(`slot_registry::screen_command`, `mv_slots_screen_command`, ABI 0.38), never an item or
`screen_show`; at most one per add-on. The command hands Enhance `enhance/tool
{"photo": "remove|restyle|detail|extend|faces", "run": true?}` (plus `"faces"` boxes, see below);
Remove first takes the brush dabs (`still_mask`). Mac `DockedToolPanel` (`EnhanceHubView.swift`),
Windows `IslandHost.EnhanceDock.cs`.

## Remove: what was brushed in Edit, filled in

Edit → Adjust → **Remove** paints over the photo ([07](07-photo-editing.md)); the run gets the dabs
(`still_mask`). `remove.h`, `io.runner: inpaint`:

1. Dabs drawn as a mask of the full-resolution edited picture.
2. Boxes (`remove_boxes`): blobs on a ≤ 256-cell grid (dilated 2, 8-connected), bounds grown by half
   the longer side (≥ 128 px), square where possible; merge two when the union < 1.3× their summed
   area. > 8 boxes or > 64 blobs: one box over all.
3. Each box top to bottom: crop + mask scaled to 512, filled by LaMa (CPU, `cpu: true`), scaled back.
   The mask holds this box's blobs and later boxes' holes; earlier fills count as photo.
4. **Second pass**: a box over 768 px (`needs_refine`) is refilled at full resolution in
   half-overlapping 512 tiles (`refine_tiles`, `refine_fill`; tiles already filled or ≤ 1/10 known
   skipped). Only detail **above the first pass's scale** is added, weighted tent × known fraction
   (ramp 1/10 → 2/5); the first pass keeps shape and colour.
5. Only the box's brushed pixels pasted, feathered outward (`remove_feather`: longer side / 256,
   2–16 px); everything else is the photo bit for bit.
6. A new file.

Cancel is checked before every fill; progress counts fills.

**Extend** uses the same model (`extend_tiles`, `extend_fill`): photo centred, margins seeded by
mirroring, square tiles 3× the margin depth and ≥ `kExtendMinTile` = 512 (≤ the shorter side), half
overlapping so every margin pixel is in two (`kExtendCover`), tent-weighted. Where tiles exceed the
model side, a **detail pass** (`extend_detail`) adds full-resolution detail as Remove's second pass
does. Photo pixels copied back unchanged.

## Restore faces

Edit → Enhance tab → Faces card → **Restore faces** (also the palette's `faces` command). Knobs:
**Look** — *Faithful* (RestoreFormer++, default) or *Natural* (GFPGAN v1.4) — and **Strength**
(default 0.7). `face_restore.h`, `io.runner: face`, CPU fp32, the level's threads:

1. **Faces and five landmarks** from the AI pack (People's YuNet) via the host row `still_faces`
   (after `still_mask`; pack `mv_ai_api faces_json`, after `eyes_json`). Photo asked at ≤ 2048 px
   long side and, from 2048 px short side, also in overlapping half-size tiles; duplicates merged.
   No Local search / People off: the note says what to enable.
2. **Alignment**: least-squares similarity (no reflection) to the FFHQ 5-point template at 512;
   sampled with up to 8 × 8 averaging; refitted once from landmarks on the crop. < 10 px between the
   eyes: left alone.
3. **Model** on the 512 crop (RGB −1..1; RestoreFormer++ output 0 only via `infer::session::run_named`;
   GFPGAN output `1288`, clipped).
4. **People guard**: pasted-as-will-be crop vs original via the pack's embedding (`still_faces` with
   `other`). Cosine below **`kFaceGuard` = 0.55** (or face not found again): face keeps its pixels.
5. **Paste back** through the inverse warp under a feathered ellipse (solid to 75 % radius), at
   Strength; a face larger than 512 gets only the model's change added.

Outside the masks the photo is bit-exact. Output `<stem>-restored.jpg` (or a new library photo); if
no face changed nothing is written and the note says why; Strength 0 is refused. Cancel is checked
before each pack call and between faces.

## Faces in the Enhance tab (edit-refresh S8)

The **Faces** card arms the host's Faces tool ([20](20-edit-workspace.md) "Canvas tools"), not this
add-on: host pixel work (`edit/eyes.h` via `addon::media::fix_faces`, donors from People), **Open
eyes**, **Improve face**, **Do all**, writing `<stem>-eyes.jpg` / `<stem>-faces.jpg`; only chosen
faces change (`mv_ai_tests "[faces-mask]"`). The fix table (`addon::media::face_fix`,
`shell::kFaceActions`) has a third row, **Restore face**, which is this add-on's: it runs `faces`
with the chosen boxes (`"faces"` in `enhance/tool`, 0..1 quads), keeping faces each box overlaps
most (`chosen_faces`, IoU ≥ 0.3); no match says "The chosen faces were not found again".

## Extend: new edges around the photo

Served by Remove's model (`remove`, `mode: extend`). Margins come from:

- **The frame dragged on the canvas** ([20](20-edit-workspace.md) "Extend"): host row `still_extend`
  ([14](14-abi.md)) gives left/top/right/bottom as fractions of the edited picture. **Fill** runs
  Extend (`enhance/tool {"photo":"extend","run":true}`). The frame wins over presets and is tied to
  the edit it was set over (`addon_media`); an undo or later geometry change zeroes it, as do Cancel,
  a new Extend and a new item. `extend_from_fractions` (`remove.h`) rounds to pixels and shrinks
  evenly to fit the caps.
- **A preset**: Wider, Taller (an eighth a side), All round (a tenth), Square, 16:9, 4:5 (one axis,
  centred).

Caps: ≤ 2× the picture each way, 16384 px a side, 100 MP (a preset past it is refused). The bar's
size uses the same sums (`shell/edit_workspace.h` `extend_size`, `extend_preset_draft`;
`"[enhance][extend]"` checks agreement). Output `<stem>-extended.jpg` (`run_port::still.suffix`).

## Measured

Apple M-series, CPU unless noted; loaded machines, so times are upper bounds.

| | |
|---|---|
| Sound: SI-SDR at 5 dB SNR; speed; memory | 5.0 → 9.7 dB; 0.13 × real time; ~58 MB (`run_bytes` 150 MB) |
| 640 × 360, 30 frames → 1280 × 720 with sound | 6.2 s, ~330 MB (`run_bytes` 600 MB) |
| 4032 × 3024 → 6048 × 8064, Core ML | 8.7 s, ~850 MB |
| Restyle Standard / High, warm | 16 s, 12.1 GB / 43 s, 17.4 GB (Best not measured; 32 GB gate is an estimate) |
| Enhance image per tile, 8 threads, fp32 | HAT 5.4 s / 192, RealWebPhoto 10 s / 256, SCUNet 3.3–4.6 s / 512, NAFNet 0.9–1.6 s / 512 |
| Soft 1500 × 1500, d = 4 | 9 tiles, 138 s, Laplacian variance 2.13 → 18.71 |
| Remove, 832 × 832 box | 1.73 s |
| Restore faces per face | 1.1 s RestoreFormer++, 0.6 s GFPGAN |

`kFaceGuard` = 0.55: above every other-person blend (max 0.322) and the 1e-5 impostor point
(0.384), below every real restoration (min 0.685) — `tools/enhance/face_guard.py` on LFW. Extend
tilings: `[.extend-compare]`, `tools/enhance/extend_metrics.py`. Windows not measured.

## Not built

- Enhance image / Restore faces on Core ML or a GPU (CPU fp32 only).
- Remove motion blur: hidden until a sound export or another model (owner call).
- Sweeping unpinned files (e.g. old `still/realesr-general-x4v3.onnx`) before Remove.
- Restore faces: downloading only the chosen Look; guarding against People's references.
- Restyle on Windows (needs DirectML or CUDA ONNX Runtime); hosting Restyle's weights.
- HDR clips; a GPU on Windows for clips; loudness levelling; MP3 output (results are M4A).
- A live preview on the playback clock; seeking past cut-out frames in the Video Editor.
- Brighten on photos, and denoise for what it lifts.
