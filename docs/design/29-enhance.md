# Enhance

The Enhance add-on (#297): a restored copy of the photo, clip or audio file on screen, made by a
model that runs on this computer and is downloaded only when the person asks for it. The original
is never the output (rule 5). Built: model downloads and the sheet (slices 1–2), photos (slice 3),
sound — an audio file's, or a clip's (slice 4) — and a clip's picture (slice 5). [The
plan](../plans/enhance.md) has what is left.

Code: `src/addons/enhance` (`cmake/enhance.cmake`), the host's `edit/sound_writer.h`,
`edit/picture_io.h` and `edit/export.h`'s `full_picture` / `export_pixels`, and the sound, picture
and photo rows of the host table ([14](14-abi.md)).
Decisions: [12](12-decision-log.md), "Enhance is its own add-on", "Enhance's first audio model"
and "Enhance's first video model" (2026-10-26).

## What it is

One slot add-on with no chrome of its own ([25](25-open-addons.md) §10): both hosts draw its two
screens from its view trees.

- **Its page in Settings → Add-ons** (place `settings`): one card per kind (photos, video,
  audio) with that kind's model, its credit and size, and Download / Stop / Resume / Remove.
- **The sheet over the viewer** (place `sheet`): `Enhance…` in the palette and the context menu,
  and `Ctrl+Shift+R` / `⌘⇧R` ([16](16-commands.md)), for the file on screen. The settings page says
  so. No button in the viewer's bar: no contribution point puts an add-on there yet.

It is not part of Local search and not in the `ai` family: search reads pixels and writes an
index, Enhance writes a new file. Settings → Add-ons lists it on Windows and on Apple silicon
(its models run on ONNX Runtime, which has no x86_64 macOS build), with a one-click install
(`SlotAddonInstallOffer` in SlotItems.swift, `IslandHost.Enhance.cs`).

## Models

Each kind Enhance can run ships one or more `models/<kind>/<id>.json` in its signed zip (at most
eight; the id is the file's name, `[a-z0-9_-]{1,32}`). **`model.json` is the one a kind uses until
the person chooses another**, like Local search's tiers: a `Model` choice on the kind's card in
Settings and on the sheet's section, shown when the kind has more than one. Each model downloads,
stops and is removed on its own (keyed `<kind>.<id>` in `enhance.json`, which also keeps the
choice per kind as `use`; an older file's bare kind means its `model.json`). The page names the
kind's other models that hold files, each with its own Remove. A run keeps the model it started
with. Core ML's compiled cache is per model: `models/<kind>-cache` for `model.json`'s, as before,
`models/<kind>-cache-<id>` for another, removed with it.

| Field | |
|---|---|
| `kind`, `name`, `credit` | `audio`, "Speech cleanup", who made the weights and their licence |
| `summary` | optional: what choosing it means ("Finer detail; about six times as long"), on the page |
| `weights` | the files it needs, pinned under the manifest's `remote` at `<kind>/<file>` |
| `run_bytes` | free memory a run needs: the run gate |
| `cpu` | one unit of work finishes inside a measured budget on CPU |
| `knobs` | `slider` / `choice` / `toggle`, each with the kinds of target it applies to |
| `io` | the runner's own section (sound: `runner: stream`, rate, frame, state, delay; pictures: `runner: tiles`, scale, tile, overlap, the graph's free dims) |

The weights are never in the zip or a release asset. The signed manifest pins each file under
`remote` (Hugging Face URL at a fixed revision, size, SHA-256, licence; `src/addons/enhance/
remote.json` is the list `addon-pack.py` signs in). A click on a kind's Download fetches only that
kind's files, with the AI pack's downloader (`mv_model_fetch`, [17](17-local-ai-search.md) "Model
tiers"), into the add-on's data folder: a plain GET with no query, cookies or token, resumed with
Range (also after a restart: the click is remembered in `enhance.json`), kept only when the SHA-256
matches. Refused before any request: a pin over **5 GB**, and a disk without room for what is
still to come (plus a tenth). `addon-pack.py` refuses a pin over 5 GB too.

| Kind | Model | Weights | Licence |
|---|---|---|---|
| audio | DeepFilterNet3 (Schröter et al.), streaming ONNX export (`kimtos-labs/denoiser-dfn3` @ `888f33c4`) | 16.1 MB | MIT (dual MIT / Apache-2.0) |
| video | "Detail, fast" (`model.json`): Real-ESRGAN general x4v3 (Wang et al.), ONNX export (`jonathanst29/tinier-upscale-models` @ `899dc1e4`) | 4.9 MB | BSD-3-Clause |
| video | "Detail, best" (`x4plus.json`): Real-ESRGAN x4plus, the same repo and revision; same contract (`input` [1, 3, h, w] → `output` [1, 3, 4h, 4w]) | 67.1 MB | BSD-3-Clause |
| still | the same two files, pinned again under `still/` (each kind downloads its own) | 4.9 / 67.1 MB | BSD-3-Clause |

## The sheet

Built from the installed models, never a fixed form. What the item is comes from its first bytes
(`sniff.h`: magic, never the extension), then the host's own decoders for a container
(`sampler_open` for a picture, `sound_open` for sound).

- **An audio file**: the audio model's knobs.
- **A clip**: picture, sound or both (only what it holds); each from its own model.
- **A still**: the photo model's (none yet).
- **A document, an animation, a file it cannot read**: greyed, saying why.

A kind whose chosen model is not downloaded says so and offers its **Download** (or Resume) and
then its progress and Stop right on the sheet, never knobs it cannot run yet. Knob values are remembered per kind of target, and a clip's target
too. Changing a knob starts nothing: Enhance runs on its button. Closing the sheet cancels a run
(leaving the item); Cancel does too.

## A run

One job at a time, on the runner's own thread: never the UI or render thread, never the player's
decoder (the host's sound rows open their own), never the render device. Before it starts, the
model's `run_bytes` against the memory available now (`memory_*.cpp`); short of it, the sheet
says how much is needed and nothing starts.

**Sound.** The host reads the file's sound at the model's rate with the source's own channels
(`clip::audio_reader`, now able to keep them). The model runs on each channel, one 480-sample
frame (10 ms at 48 kHz) at a time with the state it carries (`sound_job.h`); its 30 ms lag is
dropped at the start and flushed with silence at the end, so the result lines up with the original
sample for sample. "Noise reduction" mixes the model's output with the time-aligned original
(1 = the model alone): the model's own attenuation-limit input did not behave monotonically in
measurement. ONNX Runtime is loaded from the add-on's own folder on the first run (CPU, one
thread: a 10 ms frame is too small for a GPU round trip).

The host writes the result (`edit/sound_writer.h`) beside the original: `<stem>-enhance.m4a` for
an audio file; for a clip, its picture stream-copied packet for packet with the new sound in place
of its own, in its own MP4 / MOV, else Matroska. The sound is AAC from the operating system's own
encoder (AudioToolbox on the Mac, Media Foundation on Windows; about 96 kb/s a channel), never
FFmpeg's ([11](11-licensing.md): no software AAC encoder anywhere). Where the OS has none it is
FLAC, lossless and royalty-free: a `.flac`, or a clip in Matroska (`edit/clip_audio_enc.h`). It is a hidden part file until finished, then published under the first free name
("name (2)"); a cancel or a failure leaves nothing. Cover art and other tracks are not carried.
The write runs in the app's process, not the clip helper ([08](08-video-editing.md)): the sound
comes from the add-on, and its picture is copied, never encoded.

**A photo.** The host decodes it at full resolution as an export does (`edit::full_picture`: a
RAW's own develop, ICC to sRGB, upright; never the first-pixel preview), the add-on runs it through
the same tiles as a clip's frame (below), reporting each tile, and the host writes
`<stem>-enhance.jpg` (quality 92; `.png` for a PNG) with the source's metadata carried as an
export carries it (`edit::export_pixels`: Orientation 1, the written size, no stale thumbnail;
Exiv2 for HEIC, TIFF, RAW and WebP), under the first free name. Same size, twice or four times, at
most 100 megapixels (16384 on the long edge); the run gate adds the picture in and out to the
model's `run_bytes`. **Not yet:** a crop or colour change made on screen is not in the copy — the
edit stack lives in the host's UI-thread edit session and is not handed to add-ons.

**A clip's picture.** The host decodes every frame in display order on a decoder of its own
(`edit/picture_io.h`'s `frame_reader`), to 8-bit sRGB through the same colour path as frame
export (`clip_pixels.h`); HDR clips are refused for now ("cannot enhance HDR video yet"). Each
frame is cut into 256-pixel tiles overlapping by 16 pixels a side (edges replicated) and run
through Real-ESRGAN (4×); each tile's centre is averaged down to the size asked for as it is
placed (`picture_job.h`), so a 4× frame is never held whole. **Size**: the same size (more
detail), twice or four times, never past 4K (3840 × 2160; the sheet says so before anything
runs). **Strength** mixes the model's picture with a plain bilinear enlargement. A fixed tile lets
Core ML compile the graph (it refuses unbounded dimensions): Apple silicon runs it on Core ML,
compiled once into `models/video-cache` in the add-on's data folder; elsewhere on the CPU with all
but one core.

**The preview.** While a clip's picture or a photo runs, the sheet shows the picture split down
the middle: the original on the left, the enhanced on the right, with a thin light divider
(`preview.h`). The runner hands over small copies (960 pixels on the long edge, box-averaged):
a clip's first frame and then about one a second; a photo's original at the start and both at
the end. The engine letterboxes them to the sheet's width at its scale (at most 2048 × 1024) and
posts them to a `surface` with a `"glow"` edge, which turns while the run goes and holds still
once it ends; the progress bar takes the glow's colours (`"look": "glow"`). Both need
contribution API 10 ([25](25-open-addons.md)), so this Enhance asks for 10. The copies are made on
the runner's thread from the frame it already has; nothing is decoded twice, and nothing touches
the canvas or the render device. A sound-only run shows no preview.

The host's `picture_writer` encodes the new frames on a **hardware** encoder, as Path 2 does
(`edit/hwencode.h`; HEVC first for an HEVC source; docs/design/11: no software encoder), at a
bitrate scaled with the picture's area, tagged BT.709, with the source's frame times and
rotation. The sound is the source's, packet-copied — or, for **picture and sound**, the cleaned
sound, made by the audio model on a thread of its own and fed to the writer ahead of each frame
through a bounded queue (five seconds), so neither is held whole. With no hardware encoder at
that size the sheet says so and nothing is written. The sheet shows the time left once a run is
a few percent in.

## Measured (2026-10-26, Apple M-series; sound on CPU, one thread)

| | |
|---|---|
| Speech under white noise and 50 Hz hum at 5 dB SNR (11 s, `say`), SI-SDR | 5.0 → 9.7 dB |
| A tone under noise (3 s, the hidden `[.enhance-model]` case), error energy | 481 → 13.7 |
| Speed, one channel | 0.13 × real time (1.45 s for 11 s) |
| Speed, stereo, through the host table (read, model, AAC, write) | 1.5 s for 5 s |
| Memory: ONNX Runtime + the session + runs | about 18 + 40 MB (`run_bytes` 150 MB) |
| Real-ESRGAN, 256-px tile, Core ML (Python, pinned dims) vs CPU 4 threads | 0.05 s vs 0.30 s a tile (about 1.3 vs 0.37 Mpx/s in) |
| Tiled (4 tiles) against one pass over a 300 × 250 picture, 4×, away from the border | mean 0.02, worst 4 (of 255) |
| A 640 × 360 clip, 30 frames, to 1280 × 720 with its sound, Core ML + VideoToolbox | 6.2 s (0.21 s a frame) |
| Peak memory of that run, both models and a reference session | about 330 MB (`run_bytes` 600 MB) |
| A 4032 × 3024 photo (EXIF-turned) to 6048 × 8064, Core ML, read to written | 8.7 s, about 850 MB peak (`run_bytes` 700 MB + the picture in and out) |
| One 256-px tile, x4plus vs x4v3 (Python, 2026-10-29): Core ML / CPU 7 threads | 0.135 / 3.4 s vs 0.023 / 0.20 s; peak about 470 MB / 1.8 GB vs 220 / 150 MB |

Not measured: Windows, a long file's memory over time, and the viewer's own gates with Enhance
installed (it loads no runtime until a run, and runs nothing at launch).

## Not built

- A photo's on-screen crop and colour in its enhanced copy (#297 asks for it): needs the host's
  edit stack handed to the add-on.
- HDR clips: the picture would come back SDR; refused for now.
- A GPU on Windows: the bundled ONNX Runtime is the CPU build, so a clip's picture there runs on
  the CPU (about a quarter of Core ML's speed on this Mac's CPU; not measured on Windows).
- Loudness: an "even out loudness" knob needs a second pass over the result.
- Keeping an MP3 an MP3: there is no MP3 encoder in the LGPL build; the result is an M4A.
- A live preview on the playback clock (not in #297); the preview above is the run's own frames.
