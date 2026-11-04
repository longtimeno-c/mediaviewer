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
- **The sheet over the viewer** (place `sheet`): `Enhance…` in the palette and the context menu
  (no default key, [16](16-commands.md)), for the file on screen.
- **`Enhance…` in the Video Editor's bar** (`editor_items`, contribution API 11): the same sheet,
  over the editor, for the edit ("In the Video Editor" below). It has no button on the viewer's
  command bar.

It is not part of Local search and not in the `ai` family: search reads pixels and writes an
index, Enhance writes a new file. Settings → Add-ons lists it on Windows and on Apple silicon
(its models run on ONNX Runtime, which has no x86_64 macOS build), with a one-click install
(`SlotAddonInstallOffer` in SlotItems.swift, `IslandHost.Enhance.cs`).

## Models

Each kind Enhance can run ships `models/<kind>/model.json` in its signed zip:

| Field | |
|---|---|
| `kind`, `name`, `credit` | `audio`, "Speech cleanup", who made the weights and their licence |
| `weights` | the files it needs, pinned under the manifest's `remote` at `<kind>/<file>` |
| `run_bytes` | free memory a run needs: the run gate |
| `cpu` | one unit of work finishes inside a measured budget on CPU |
| `knobs` | `slider` / `choice` / `toggle` / `text` (a typed request, up to 200 bytes), each with the kinds of target it applies to |
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
| video | Real-ESRGAN general x4v3 (Wang et al.), ONNX export (`jonathanst29/tinier-upscale-models` @ `899dc1e4`) | 4.9 MB | BSD-3-Clause |
| still | the same file, pinned again under `still/` (each kind downloads its own) | 4.9 MB | BSD-3-Clause |
| remove | LaMa (Suvorov et al.), ONNX port `Carve/LaMa-ONNX` @ `c3c0c9e4`, `lama_fp32.onnx` | 208 MB | Apache-2.0 |
| restyle | InstructPix2Pix (Brooks et al., `timbrooks/instruct-pix2pix` @ `31519b5c`), exported by `tools/package/restyle-export.py`: fp32 text encoder and VAE, fp16 UNet | about 2.5 GB | CreativeML OpenRAIL-M (owner, 2026-11-03); its use restrictions are shown with the credit |

Restyle is **not pinned yet**: its files must be hosted at a fixed revision before
`remote.json` lists them, and until then no kind offers it.

## The sheet

Built from the installed models, never a fixed form. What the item is comes from its first bytes
(`sniff.h`: magic, never the extension), then the host's own decoders for a container
(`sampler_open` for a picture, `sound_open` for sound).

- **An audio file**: the audio model's knobs.
- **A clip**: picture, sound or both (only what it holds); each from its own model.
- **A still**: the photo model's (none yet).
- **A document, an animation, a file it cannot read**: greyed, saying why.

A kind with no installed model is named ("not installed — Download it in Settings → Add-ons →
Enhance"), never offered. Knob values are remembered per kind of target, and a clip's target
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
model's `run_bytes`. **The photo being edited carries its edit** ([plan](../plans/edit-adjust.md)
slice 5): the UI thread hands the edit of the item on screen to `addon_media::set_still_edit`
whenever it changes (a copy of a few hundred bytes under a mutex, never held across a decode), and
`full_still` for that path returns `edit::edited_picture` (detail, geometry, colour, as an export
bakes them) instead of the plain picture; any other path, or no edit, gets the plain picture. The
host table and the add-on are unchanged. The Edit strip offers **Enhance…** beside Save copy…
while Enhance is loaded: it runs Enhance's own `open` row (`find_addon_command`).

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

## Restyle: a typed request on a photo

With Restyle's model downloaded, the sheet's **Photo** choice offers *More detail* (the still
model) or *Restyle with words*, which adds a text field ("make it a snowy winter day"). An empty
request is refused before a run. The run (`diffusion.h`, `io.runner: diffusion`):

1. The edited still is scaled to the long edge its **Quality** names (`io.qualities`: Standard
   512 px, High 768 px, Best 1024 px), each side a multiple of 64. Each quality has its own
   memory gate (12, 18 and 32 GB); a request over it is refused with a lower quality suggested.
   A larger copy changes finer things, takes longer, and compiles once on Core ML per size.
2. CLIP's tokenizer and the text encoder turn the words (and an empty prompt) into the conditions.
3. The VAE encodes the small picture; 20 Euler-ancestral steps (linspace timesteps, the
   reference schedule, a fixed seed) run the UNet over a batch of three — text and picture,
   picture only, neither — combined with the *Follow the words* (7.5) and *Keep the photo*
   (1.5) guidances.
4. The VAE decodes, and `carry_back` applies the blurred ratio of after to before onto the
   full-resolution photo, scaled by *Strength*, so the photo keeps its own detail.
5. The result is written as a new file, like every Enhance run.

The UNet opens on Core ML with its dims fixed to the run's size (a size is compiled once, then
cached), else the CPU. The model is not `cpu`-capable, so a machine with no GPU provider
(Windows today) is refused rather than left running for minutes.

Restyle changes the look of the whole picture: season, light, colour, style. It cannot remove an
object ("remove the person" tints it; nothing is filled in). That needs an inpainting model and
a mask ([plan](../plans/edit-adjust.md) slice 5b).

## In the Video Editor (2026-11-02)

`Enhance…` sits in the editor's bar before Export, on both hosts (`EditorAddonItems` in
VideoEditorView.swift; `RebuildEditorAddons` in IslandHost.VideoEditor.cs), drawn from the
add-on's `editor_items` like any slot item, so the app never names Enhance. A click hands the
add-on the clip on screen and, when anything is cut, the edit's kept pieces (`(in, out)` source
nanoseconds; `mv_slots_editor_item`, ABI 0.35; the Mac calls the registry's
`invoke_editor_item`). The sheet opens over the editor (Mac: a sheet on the editor window, the
main one while it is open; Windows: centred over it, still owned by the viewer) and says so: "The
edit from the Video Editor: 3 pieces, 1:24 in all. The new clip is the edit, enhanced; the original
is not changed." With nothing cut it is the whole clip, as from the viewer. `Enhance…` again on an
open sheet moves it to the new file and edit.

The cut is applied by the host as it reads, not by the add-on: two rows appended to the table,
`frames_keep` and `sound_keep` (`src/core/keep_ranges.h`, in the thunks of `addon/host.cpp`),
called once after `frames_open` / `sound_open`. Frames and samples outside the pieces are dropped;
what is kept comes back on the edit's own timeline (each piece follows the last), so the writer
gets the program's times and sound lines up with picture as it does for the whole clip (sound that
starts after the first piece is padded with silence). A frame is kept when `in <= pts < out`, as
the editor's Export cuts.

A cut clip's sound cannot be packet-copied whole, so every run on a cut goes through the
picture writer (`run_picture`): with the picture model, as before; with the sound alone, the
picture is written again unchanged (same size, hardware encoder); with the picture alone, the
edit's own sound is read through the cut at 48 kHz and encoded again (the OS's AAC), not run
through the sound model. One generation of re-encode, as Export exact.

## Brighten dark scenes (2026-11-02)

A choice on the clip's picture, in the video model's `model.json` (`brighten`: Off, A little,
More), so it shows with the model's other knobs. No model and no download of its own: a tone lift
fitted to each frame (`brighten.h`), applied before the Detail model, which then sees the
lifted frame.

- Each frame's brightness is the geometric mean of its linear luminance (every fourth pixel each
  way). A scene darker than the target (0.09 for A little, 0.15 for More) gets a gain up to 3× or
  6×, through `f(x) = g x / (1 + (g − 1) x)` in linear light on R, G and B alike: the shadows rise
  by g, white stays white, nothing clips. A scene at or above the target is untouched.
- The gain eases over time (its log, half a second's time constant on the frames' own times), so a
  passing light does not pump the picture; a cut (half the brightness histogram moved) takes the
  new scene's gain at once.
- One 256-entry table a frame over 8-bit sRGB: about 1.6 ms for a 1080p frame on one core of this
  Mac (100 frames, three runs: 1.72, 1.56, 1.55 ms), against about 0.2 s a frame for the model.
- **Strength 0 at the same size skips the model**, so Brighten alone runs at decode-and-encode
  speed. The model's 4.9 MB is still the door to the knob (it lives in the video model's sheet).
- Lifting dark footage lifts its noise, and an 8-bit frame can band in lifted shadows; the help
  text says the first. HDR clips are still refused.

## Remove: what was brushed in Edit, filled in

Edit → Adjust → **Remove** paints over the photo ([07](07-photo-editing.md)); **Remove…** opens
this sheet with the dabs held by the host (`still_mask`). A sheet opened on a photo with something
brushed offers **Remove what you brushed** and starts on it. The run (`remove.h`,
`io.runner: inpaint`):

1. The dabs drawn as a mask of the full-resolution edited picture.
2. Its box: the brushed bounds grown by half their longer side (at least 128 px a side), square
   where the photo allows.
3. The box's crop and mask scaled to the model's side (512), filled by **LaMa** (big-lama, Suvorov
   et al., Apache-2.0; the ONNX port `Carve/LaMa-ONNX` @ `c3c0c9e4`, `lama_fp32.onnx`, 208 MB,
   CPU: `cpu: true`, so Windows runs it too), scaled back.
4. Only the brushed pixels pasted, feathered outward (2–16 px with the box's size); a pixel more
   than twice the feather from the brush is the photo's own, bit for bit.
5. A new file, as every run.

Measured 2026-11-03 (M-series, CPU, the hidden `[.remove-model]` case): an 832 × 832 box
filled in 1.73 s. It fills from the surroundings; it adds nothing and takes no words. A box much larger than 512 px
is filled at that size and scaled up, so a large fill is softer than the photo around it.

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

Restyle (2026-11-03, the same Mac, the hidden `[.restyle-model]` case on a 512 × 320 picture):
14–16 s a request on Core ML at Standard once the size is compiled. The first run at a new size
took about 200–230 s (Core ML compilation) on a quiet machine. "Black and white" took colourfulness from 83.4 to 1.9. fp16 output looks the same as fp32
by eye.

Measured again on the exported files (2026-11-03, a 24 GB M-series Mac, `MV_RESTYLE_EDGE`, peak
memory footprint from `time -l`): **Standard** 16 s a request warm, peak 12.1 GB; **High** (768 ×
448) 43 s warm, peak 17.4 GB; Core ML could not place High on the Neural Engine and ran it on the
GPU. A first request at a size compiled for 14–18 minutes while the machine was busy. The gates
(12 and 18 GB) are those peaks. **Best is not measured**: extrapolated to 25–30 GB, it would swap
this machine, so its 32 GB gate is an estimate, and machines like this one are refused it.

Not measured: Windows (Restyle is refused there without a GPU provider), a long file's memory over time, and the viewer's own gates with Enhance
installed (it loads no runtime until a run, and runs nothing at launch).

## Not built

- Hosting Restyle's weights and pinning them in `remote.json` (the owner's upload).
- Restyle on Windows: needs a DirectML or CUDA ONNX Runtime piece; measure before offering it.

- HDR clips: the picture would come back SDR; refused for now.
- A GPU on Windows: the bundled ONNX Runtime is the CPU build, so a clip's picture there runs on
  the CPU (about a quarter of Core ML's speed on this Mac's CPU; not measured on Windows).
- Loudness: an "even out loudness" knob needs a second pass over the result.
- Keeping an MP3 an MP3: there is no MP3 encoder in the LGPL build; the result is an M4A.
- A live preview on the playback clock (not in #297); the preview above is the run's own frames.
- In the Video Editor, frames inside a cut are decoded and dropped rather than skipped by a seek
  (the frame reader has no seek): a short edit of a long clip reads the whole clip.
- Brighten dark scenes on photos (the still model has no `brighten` knob), and a denoise pass
  for what it lifts.
- Not measured: an editor run on Windows (C# compile-checked only), and a long clip's cut.
