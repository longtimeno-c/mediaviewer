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

One slot add-on with no chrome of its own ([25](25-open-addons.md) §10): both hosts draw its three
screens from its view trees.

- **Its page in Settings → Add-ons** (place `settings`): one card per kind (photos, video,
  audio) with that kind's model, its credit and size, and Download / Stop / Resume / Remove.
- **The sheet over the viewer** (place `sheet`): `Enhance…` in the palette and the context menu
  (no default key, [16](16-commands.md)), for an audio file or a clip on screen. A photo never
  gets it (one-click, below): the hosts send those to the Edit pane's Enhance tab.
- **`Enhance…` in the Video Editor's bar** (`editor_items`, contribution API 11): the same sheet,
  over the editor, for the edit ("In the Video Editor" below). It has no button on the viewer's
  command bar.
- **The Enhance tab's cards** (place `edit`, contribution API 12; owner 2026-11-05, "One click"
  below): each photo tool's compact choices, the time it will take and one button, drawn inside the
  Edit pane's cards; one click runs it with a glow over the photo and opens the new photo. No sheet
  and no panel for a photo.

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
Range (also after a restart: the click is remembered in `enhance.json`, and a start resumes a
kind only when part of its download is here, so a kind whose pin changed since, as `still`'s did
in S7, waits for a new click), kept only when the SHA-256
matches. Refused before any request: a pin over **5 GB**, and a disk without room for what is
still to come (plus a tenth). `addon-pack.py` refuses a pin over 5 GB too.

| Kind | Model | Weights | Licence |
|---|---|---|---|
| audio | DeepFilterNet3 (Schröter et al.), streaming ONNX export (`kimtos-labs/denoiser-dfn3` @ `888f33c4`) | 16.1 MB | MIT (dual MIT / Apache-2.0) |
| video | Real-ESRGAN general x4v3 (Wang et al.), ONNX export (`jonathanst29/tinier-upscale-models` @ `899dc1e4`) | 4.9 MB | BSD-3-Clause |
| still | **Sharpen & restore**: Real-HAT-GAN SRx4 "sharper" (HAT, Chen et al., XPixelGroup), ONNX export `MahoCommerce/Real_HAT_GAN_sharper-ONNX` @ `f0c8c3a4`, `onnx/model.onnx` saved as `Real_HAT_GAN_sharper.onnx` | 161 MB | Apache-2.0 |
| natural | **Natural**: 4xRealWebPhoto v4 (DAT-2) by Philip Hofmann, `Phips/4xRealWebPhoto_v4_dat2` @ `95e9cd50` | 48.8 MB | CC-BY-4.0 (credited in the spec and the notices) |
| denoise | **Remove noise**: SCUNet real-PSNR (Zhang et al., KAIR), `deepghs/image_restoration` @ `77885239` | 91.3 MB | Apache-2.0 |
| deblur | **Remove blur**: NAFNet-REDS-width64 (Chen et al., MEGVII), the same repo and revision | 275 MB | MIT |
| motion | **Remove motion blur** (not offered, `kOfferMotionBlur`): NAFNet-GoPro-width64, the same repo and revision | 275 MB | MIT |
| remove | LaMa (Suvorov et al.), ONNX port `Carve/LaMa-ONNX` @ `c3c0c9e4`, `lama_fp32.onnx` | 208 MB | Apache-2.0 |
| restyle | InstructPix2Pix (Brooks et al., `timbrooks/instruct-pix2pix` @ `31519b5c`), exported by `tools/package/restyle-export.py`: fp32 text encoder and VAE, fp16 UNet | about 2.5 GB | CreativeML OpenRAIL-M (owner, 2026-11-03); its use restrictions are shown with the credit |
| face | RestoreFormer++ (Wang et al., `wzhouxiff/RestoreFormerPlusPlus`), the ONNX export `facefusion/models-3.0.0` @ `728b9659`, `restoreformer_plus_plus.onnx`; and GFPGAN v1.4 (Wang et al.), `neurobytemind/GFPGANv1.4.onnx` @ `e3fd467e`, `GFPGANv1.4.onnx` | 294 + 340 MB (both are the kind's files) | Apache-2.0 (owner call C, 2026-11-04) |

Restyle's files are pinned in `remote.json` (ForserX's export); its card offers them as any
tool's: "Download (2.5 GB) and restyle".

## The sheet

Built from the installed models, never a fixed form. What the item is comes from its first bytes
(`sniff.h`: magic, never the extension), then the host's own decoders for a container
(`sampler_open` for a picture, `sound_open` for sound).

- **An audio file**: the audio model's knobs.
- **A clip**: picture, sound or both (only what it holds); each from its own model.
- **A still**: *Enhance image*'s chosen model (below), or Restyle, Remove, Extend.
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
the chosen Enhance image model (`restore_picture`, "Enhance image" below), reporting each tile, and the host writes
`<stem>-enhance.jpg` (quality 92; `.png` for a PNG) with the source's metadata carried as an
export carries it (`edit::export_pixels`: Orientation 1, the written size, no stale thumbnail;
Exiv2 for HEIC, TIFF, RAW and WebP), under the first free name. Same size, twice or four times, at
most 100 megapixels (16384 on the long edge); the run gate adds the picture in and out, and a
tile-high band of float sums, to the model's `run_bytes`. **The photo being edited carries its edit** ([plan](../plans/edit-adjust.md)
slice 5): the UI thread hands the edit of the item on screen to `addon_media::set_still_edit`
whenever it changes (a copy of a few hundred bytes under a mutex, never held across a decode), and
`full_still` for that path returns `edit::edited_picture` (detail, geometry, colour, as an export
bakes them) instead of the plain picture; any other path, or no edit, gets the plain picture. The
host table and the add-on are unchanged. **A Photos-library photo** (`photos:<id>`, Mac) is read
through the host's resolve hook and its result becomes a new photo in the library
([26](26-photos-library.md) "Edits of a library photo"): `still_write` then writes the new
photo's key, and the sheet says *Added to your Photos library as a new photo.* (a key is never
shown); a refused Photos permission says how to allow it. The Edit strip offers **Enhance…** beside Save copy…
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
once it ends; under it, the one progress line (docs/design/20 "Craft rules"): "Enhancing · 40 % ·
about 1 min left", its time left estimated by the host (`"timed"`, contribution API 13; the
`"look": "glow"` it still names is drawn as every bar). This Enhance asks for 13
([25](25-open-addons.md)); a model's download is the same line with its amounts (`"detail"`). The copies are made on
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

## Enhance image: a photo restored (S7, owner 2026-11-04)

The photo's **Enhance image** has five models, each its own kind (`spec.h` `kStillKinds`), so
each is downloaded only when it is chosen: **Sharpen & restore** (`still`, the default), **Natural**
(`natural`), **Remove noise** (`denoise`), **Remove blur** (`deblur`) and **Remove motion blur**
(`motion`). **Remove motion blur is not offered** (2026-11-05, `spec.h` `kOfferMotionBlur` false):
its pinned NAFNet-GoPro export blows up on about a third of photos, so it is not a choice, not a
Settings card unless its files are already here (so they can be removed), never resumed or
installed; its spec, pin and code stay for a sound export. The sheet's *Improve* choice (`"detail"`, kept in `enhance.json`) lists those this build
has a spec and a pin for; one not downloaded yet shows its size and a **Download** button in the
sheet itself (`install.<kind>`, then progress and *Stop the download*), and Settings → Add-ons →
Enhance lists the four offered. The docked panel ("Docked in Edit") draws the same choice, Download
and credit under the Enhance image card, and a restorer's note on parts it left as they were under
its result row. The two upscalers have a **Size** (the same size, twice, four times),
run **shrunk to the detail** (below). The three restorers keep the size and have no Size. Each has **Strength** (the model's picture mixed
with the photo, or with its plain bilinear enlargement) and shows its credit line.

The run (`picture_job.h` `restore_picture`): tiles of the model's side (Real-HAT-GAN is fixed at
192 → 768; RealWebPhoto runs at 256, SCUNet and NAFNet at 512) start inside the photo, the last
in a row or column moved back to end at its edge, so a model only ever sees the photo's own
pixels (a photo smaller than one tile is mirrored out to it). Neighbours share a third of a tile
for HAT and a quarter for the rest (`io.overlap`: quality over speed). Each tile's result is
brought to the size asked for by a Lanczos-3 downscale of the model's own pixels (built once a
run), then blended by a **tent weight**: 1 in the tile's middle, falling linearly to 0 across the
overlap toward a neighbour, flat at the photo's edges, so two tiles cross-fade and a seam never
shows. Rows are written as soon as no later tile reaches them; only a band of float sums one tile
high is held. Progress is per tile (the sheet's time left), and Cancel is honoured between tiles.
The models run in fp32 on the CPU (`io.backend: "cpu"`): measured, they are not worth Core ML's
fp16.

**Shrunk to the detail** (owner 2026-11-05: "when I run Enhance to sharpen the image it doesn't
look any sharper"; [plan](../plans/edit-refresh.md)). A ×4 model restores detail at its own input's
pixel scale. A soft photo (a small picture blown up, an out-of-focus shot) has none there, so ×4 on
the photo itself and a downscale back changed almost nothing (Laplacian variance 2.4 → 3.1 with
HAT). So the upscalers (HAT, RealWebPhoto) first measure how far the photo can be shrunk without
losing its detail (`detail_shrink`, milliseconds): on its three busiest 320 px crops of nine, at
full resolution, the largest d in 1.25…4 (quarter steps) whose Lanczos-3 down-and-up round trip
loses at most 3 % more gradient energy than a 1.15 round trip does (that finest band holds only
grain, JPEG blocks and an enlargement's kinks on a soft photo), or one 8-bit level squared where
the photo is nearly flat; 1 when even 1.25 loses detail. Sharp photos get 1 and run as before
(`restore_picture` on the photo as it is); a soft one is resized to 1/d (Lanczos-3), run tiled at
the least of the model's scales that covers the size asked for (`plan_shrink`), and resized
(Lanczos-3) to exactly the source size times the Size. Same size, ×2 and ×4 all shrink alike;
Strength mixes with the photo itself enlarged, never the shrunk copy (`restore_at_detail`). A
6× enlargement reads d = 4, a 2× one about 2, a sharp scene ≤ 1.5 (unit tests). It is also much
faster: the model sees 1/d² of the pixels. The run gate adds the shrunk copy, the model's result
before its resize and the resize's float rows.

**A tile the model blows up.** NAFNet's ONNX exports (GoPro worst, REDS sometimes) return values
in the thousands on some tiles of ordinary photos (11 of 30 COCO crops for GoPro, 4 of 30 for
REDS; SCUNet none), and half-broken tiles stay in range but speckle. A result outside −1…2,
not finite, or with more than 0.1 % of its samples past −0.15…1.15 (on 40 tiles of 20 COCO
photos, sound results reached at most 0.01 %: HAT 0.004 %, SCUNet and RealWebPhoto none; GoPro
failed 17 of the 40, REDS 5) is asked again with the tile mirrored
left to right, upside down and transposed (each answer turned back); if every view fails, that
tile is the photo itself, cross-faded like any other. The sheet then says how many parts were
left as they were ("…could not restore 3 of 40 parts…", or that the photo is unchanged). Pixels
from a blown-up tile never reach the file.

## Restyle: a typed request on a photo

With Restyle's model downloaded, the sheet's **Photo** choice offers *Enhance image* (above)
or *Restyle with words*, which adds a text field ("make it a snowy winter day"). An empty
request is refused before a run. The run (`diffusion.h`, `io.runner: diffusion`):

1. The edited still is scaled to the long edge its **Quality** names (`io.qualities`: Standard
   512 px, High 768 px, Best 1024 px), each side a multiple of 64. Each quality has its own
   memory gate (12, 18 and 32 GB); a request over it is refused with a lower quality suggested.
   A larger copy changes finer things, takes longer, and compiles once on Core ML per size.
2. CLIP's tokenizer and the text encoder turn the words (and an empty prompt) into the conditions.
3. The VAE encodes the small picture; Euler-ancestral steps by the performance level (Potato 20,
   Intel 25, Default 40, Maxxed 50: linspace timesteps, the reference schedule, a fixed seed) run
   the UNet over a batch of three — text and picture,
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

## The performance level (2026-11-04)

Settings → Add-ons → Enhance opens on a **Performance** card, as Local search's: *Automatic*
(recommended for this computer by Local search's rule, `addons/enhance/tier.h` `recommend_tier`),
*Potato*, *Intel*, *Default* or *Maxxed*, kept in `enhance.json`; under it, *In use: Default level*
(node `tierlevel`). A level bounds a run (`tier_policy`): the CPU models' threads, Restyle's steps
and its largest quality. The owner prefers quality over speed for every Enhance result
(docs/plans/edit-refresh.md "Quality over speed"), so each level takes as many steps as it bears
and Restyle starts at the best quality its level allows:

| Level | Threads | Restyle steps | Restyle quality (the default is the highest) |
|---|---|---|---|
| Potato | 2 | 20 | Standard |
| Intel | half the cores | 25 | Standard |
| Default | all cores but one | 40 | High |
| Maxxed | every core | 50 | Best |

The words' guidance stays 7.5 at every level. In the sheet a quality above the level is listed,
disabled, with the level it needs (*Best (needs Maxxed level)*; a choice option's `"disabled"`,
[25](25-open-addons.md), contribution API 12 with the docked panel's place `"edit"`), and a
change to it is not kept. A quality the person chose is theirs while the level allows it; above
it, or with none chosen, the sheet shows and runs the level's highest (`screens.h`
`level_quality`), and the chosen one comes back when the level does. A request that still names a quality above the level is refused with the level it needs. Potato
shows Restyle with Standard only (the owner's default; "Needs Intel or higher" was the
alternative). The Edit pane's Enhance tab shows the level in effect as a chip in its header
([20](20-edit-workspace.md) "The Enhance tab"). Per-level model files are not built (owner call F:
no new downloads for them).

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
`frames_keep` and `sound_keep` (`src/addon/keep_ranges.h`, in the thunks of `addon/host.cpp`),
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

## One click (owner 2026-11-05)

"Why does 'enhance' open this window? It should just show quality / ETA and when clicked use an
Apple Intelligence pattern over the image while it works, then open the new image." The sheet and
S5's docked panel are gone from the photo flow ([12](12-decision-log.md), 2026-11-05).

**The cards.** The `tool` screen (place `edit`) is opened by the host while the Edit pane's Enhance
tab shows, on the photo on screen, and pointed at each photo after it (`enhance/tool
{"path": …}`; a run on the last photo stops, as leaving it always did). Its tree's root is `cards`
(`screens.h` `cards_tree`): one stack `card.<word>` per tool this Enhance has a pinned model for
(never gated on a palette command), which the host draws inside its own card:

| Card | Choices | Estimate | Button |
|---|---|---|---|
| Enhance image (`detail`) | Quality (the four models), Size (×1/×2/×4 for the upscalers) | "About 2 min · 6048 × 8064" | *Enhance* |
| Restyle | the words (Return runs), Quality | "About 40 seconds", ", more the first time" above Standard | *Restyle* |
| Restore faces (under Faces) | Look (RestoreFormer++ / Natural), Strength | "About 2 seconds a face" | *Restore faces* |
| Remove, Extend | none (the tool bar's Remove ↵ / Fill ↵ runs them) | the run's own | only *Download (208 MB)* |

A model not here yet makes the button *Download (161 MB) and enhance*: one click fetches it, the
tool bar says "Downloading Sharpen & restore… 30 %", and the run starts on its own (`engine::go`;
no room, a bad checksum or a stop ends it with the reason). Stop the download is on the card while
it goes.

**The estimate** (`eta.h`, `[enhance][tool][eta]`): the model's tiles counted as `restore_picture`
counts them on the picture it sees (the photo shrunk to its detail by `detail_shrink`, so time
scales with W·H/d²; `plan_shrink` for the Size), times doc 29's measured seconds a tile at 8
threads (HAT 5.4 s / 192 px, RealWebPhoto 10 s / 256, SCUNet 4 s and NAFNet 1.25 s / 512: loaded
runs, so upper bounds), times 8 / the level's threads (held to 0.5…4), plus about 1.5 s per 12 MP
read and written. The photo is read as a run reads it (the edited picture) on a thread of the
engine's own, never the host's worker, and only while no run goes; the card says *Estimating…*
until then. Restyle: its level's steps × 0.8 / 2.15 / 5.4 s a step (Standard, High, Best; Best
extrapolated). Restore faces: 1.6 s (RestoreFormer++) or 1 s (GFPGAN) a face, scaled the same.

**The run.** One click (the card's button, Return in Restyle's words, the tool bar's Remove ↵ /
Fill ↵) runs as before. The tree's hidden `run` node (`idle | running | downloading | done |
failed | cancelled`, `run.id`, `run.progress`, `run.verb`, `run.line`, `run.result`) is what the host follows (`shell/enhance_run.h`, `[enhance-run]`):

- **The glow over the photo** while it works (not while only downloading): soft blue, purple,
  pink and orange flowing round the photo's edges with a sheen sweeping across, in the present
  path on both labs (`tool_overlay.h` `draw_work_glow`, Metal and D3D11 through the same ImGui
  body). The labs present each frame only while it moves, then once to clear; it fades out in
  300 ms the moment the run ends; Reduce Motion draws it still and presents nothing for it.
- **The tool bar under the photo** (the canvas tool bar's slot and band, shown for the run even
  with no tool armed), the one progress line ([20](20-edit-workspace.md) "Craft rules"): the bar
  and "Enhancing · 40 % · about 1 min left" with Cancel (the host composes it from `run.verb` and
  `run.progress` and says the time left itself; Restoring faces, Removing, Extending, Restyling),
  then in the same fixed slot "Saved as IMG_0007-enhance.jpg" with Move to Trash (Recycle Bin on
  Windows; the host table's `recycle_file`, never a hard delete, never the photo it was made from),
  or the one-line reason with Try again (the card's own button; not for Remove and Extend, which
  run from the bar), and a quiet ×. When the result opens it cross-fades in over the photo.
- **The result opens**: once per run, and only a run the host saw working, it navigates to the new
  file (a library photo by the key the add returned), Edit kept on the Enhance tab; the cards follow
  it and keep its line. Compare then compares it with its source ([20](20-edit-workspace.md)
  "Compare", `note_result`). On a failure or a cancel the glow stops and the photo stays.

Hosts: Mac `EnhanceRun.swift` (reads the tree; `mv_chrome_edit_enhance_run`), `EnhanceHubView.swift`
(the cards), `ToolBarView.swift` (the line), `main_mac.mm` `-enhanceCardsSync` / `-enhanceRunState:`;
Windows `IslandHost.EnhanceDock.cs` (the cards and the run node; `chrome_edit_action::enhance_run`),
`IslandHost.EnhanceHub.cs`, `IslandHost.ToolBar.cs`, `main.cpp` `enhance_cards_sync` /
`enhance_run_state`. Enhance's palette rows, its context entry and the strip's Enhance… on the photo
on screen open the Enhance tab; an audio file, a clip and the Video Editor keep the sheet (there is
no Edit pane for them). Tests: `mv_import_tests "[enhance][tool]"`, `mv_tests "[enhance-run]"`.

## Docked in Edit (2026-11-05; replaced by "One click" the same day)

What follows is S5's panel, kept for its history; the cards above replaced it.

docs/plans/edit-refresh.md S5 (owner call B). The manifest's third screen, `tool`, is in place
`edit`: only the Edit workspace opens it, with a command (`slot_registry::screen_command`, ABI
`mv_slots_screen_command`, 0.36), never an item, a status click or `screen_show` (an add-on has at
most one, and a manifest asks for contribution API 12 to name it). The command opens it on the
photo on screen when it is not open, then hands Enhance `enhance/tool`
`{"photo": "remove|restyle|detail|extend|faces", "run": true?}`: the panel switches to that tool (its
knobs reset no result but its own) and, with `run`, starts it. Remove reads the brush again first:
the host hands its dabs over (`still_mask`) in the same click, as the sheet's Remove did.

`tool_tree` is the sheet's `sheet_tree` without the photo choice or Close: the tool's knob cards,
the note, the preview surface (as wide as the inspector's column, at its backing scale), the glowing
progress and the time left, **Cancel**, and the run button (*Restyle*, *Enhance*, *Extend*,
*Restore faces*; after a result, *… again*). Its root is `tool.<word>`, which is how the host knows
the card to draw it under (`tool.faces` under the Faces card, whose Open closed eyes stays). Remove has no run button in the panel, before or after a result: the card above it already
has *Remove* (and Return in the brush runs it), which hands the strokes on screen over first, so a
second button could only run on dabs handed over earlier. Remove shows its knobs and *Brush over
what to remove, then press Remove (or Return).*

A finished run shows the **result row**: a thumbnail of the new file, *Saved as
IMG_0007-removed.jpg* (or *Added to your Photos library* for a library photo, whose key is never
shown), and three buttons:

- **Show**: the host's. Edit closes and the viewer selects the new file (a library photo by the key
  the add returned, once PhotoKit has listed it beside its source).
- **Keep editing**: the host's. The same, with Edit reopened on the Enhance tab.
- **Move to Trash / Move to Recycle Bin**: Enhance's. The host table's `recycle_file`, never a
  permanent delete; only the file the run wrote, never the original and never a library photo
  (Photos asks before it deletes; that is done in Photos). A folder with no bin keeps the file and
  says so under the row.

The sheet keeps its own *Saved as …* status; the docked panel shows the row instead. The host
closes the panel when Edit closes or another photo shows (a run still going stops, as closing the
sheet does); its own × does the same. Mac: `DockedToolPanel` in `EnhanceHubView.swift`
(`ScreenButtonHook` takes Show and Keep editing); Windows: `IslandHost.EnhanceDock.cs`
(`TakeScreenButton`; the path goes to native as `OpenPath`'s does). Test: `"[enhance][tool]"`.

## Remove: what was brushed in Edit, filled in

Edit → Adjust → **Remove** paints over the photo ([07](07-photo-editing.md)); **Remove** hands
the dabs held by the host (`still_mask`) to the docked panel and runs ("Docked in Edit"; the sheet
with an Enhance that has no docked panel). A sheet opened on a photo with something
brushed offers **Remove what you brushed** and starts on it. The run (`remove.h`,
`io.runner: inpaint`):

1. The dabs drawn as a mask of the full-resolution edited picture.
2. Its boxes (`remove_boxes`, edit-refresh S4b): the mask split into blobs (a grid of at most
   256 cells a side, dilated 2 cells, 8-connected), each blob's bounds grown by half their longer
   side (at least 128 px a side), square where the photo allows. Two boxes whose union is under
   1.3× their summed area become one, so strokes close together share a box. More than 8 boxes
   (or more than 64 blobs): one box over everything brushed.
3. Each box in turn, top to bottom: its crop and mask scaled to the model's side (512), filled by
   **LaMa** (big-lama, Suvorov et al., Apache-2.0; the ONNX port `Carve/LaMa-ONNX` @ `c3c0c9e4`,
   `lama_fp32.onnx`, 208 MB, CPU: `cpu: true`, so Windows runs it too), scaled back. The model's
   mask holds the box's own blobs and any later box's (still holes); a later box sees the earlier
   fills as photo. Cancel is checked before and after every fill.
4. **The second pass** (quality over speed, owner 2026-11-04; edit-refresh Q): a box over 1.5× the
   model's side (`needs_refine`, 768 px at 512) is filled again at full resolution
   (`refine_tiles`, `refine_fill`). Tiles of the model's side (smaller only where the photo is),
   half overlapping, cover the box's own brushed pixels and their feather's reach plus a quarter
   tile of context; a tile whose brushed pixels are all filled by earlier boxes, or that is a
   tenth known or less (deep in a large hole), is left out before the run, so every tile counted
   is a fill. Each
   tile is filled against the photo (its holes the box's open mask), and what it has **above the
   first pass's scale** (the fill less the fill shrunk by the box-to-512 ratio and grown back) is
   added to the first pass, the weighted mean over the tiles (a tent, highest in a tile's middle,
   times how much of the tile was known: rising from a tenth known, full from two fifths). The first pass keeps the shape and colour, since it saw the whole
   box; the tiles give the grain the 512-px fill lost. Filling the tiles' whole result in was
   tried and dropped (2026-11-05): a 512-px tile deep in a large hole sees only what is near it,
   and on a 770 × 500 hole it smeared the next rock's texture over a horizon the first pass had
   continued, and averaging tiles that disagree left translucent ghosts. Cancel is checked before
   every tile; progress counts fills (the first passes and every tile), so the time left is the
   work left.
5. Only the box's own brushed pixels pasted, feathered outward (`remove_feather`: the box's longer
   side / 256, 2–16 px); a pixel outside every box, or more than twice the feather from the brush,
   is the photo's own, bit for bit.
6. A new file, as every run.

**Extend** (owner 2026-11-04) runs on the same model: the photo in the middle of the bigger
picture, its margins started as the photo mirrored, then filled tile by tile (`extend_tiles`,
`extend_fill`). Tiles are square, three times the margin's depth and at least the model's side
(`kExtendMinTile` = 512, so a shallow margin is filled at full resolution; at most the picture's
shorter side), half overlapping along the margin (and across it where it is deeper than half a
tile), so every margin pixel is in a tile and, away from a margin's ends, in two (`kExtendCover`).
Each is filled at the model's side and every margin pixel is the tent-weighted mean of the tiles
over it. Where the tiles were larger than the model's side, a **detail pass** (`extend_detail`,
Remove's detail transfer) fills model-side tiles over those margins at full resolution and adds
what each has above the first pass's scale, tent-weighted by how much of the tile was known: the
first pass keeps the shapes and colours, the second gives back the grain. The photo's own pixels
are copied back unchanged. Progress counts every fill of both passes; cancel is checked between them.

Measured 2026-11-03 (M-series, CPU, the hidden `[.remove-model]` case): an 832 × 832 box
filled in 1.73 s. It fills from the surroundings; it adds nothing and takes no words. Things
brushed far apart are each filled at the model's detail in their own box (one model run each, so
two spots take about twice as long as one), not in one photo-sized box.

Measured 2026-11-05 (the same Mac, CPU, the hidden `[.remove-quality]` case through the runner, on
a 3840 × 2160 lake photo, the machine shared with about seven builds, so times are wide; base and
new runs alternated): **Remove** of a rock and a tree (two boxes; the second pass adds 12 tiles)
took 4.0, 6.2 and 14.1 s before and 18.7, 24.4 and 45.0 s after. The fine detail
inside the removed rock (mean |picture − its 2-px blur|) went from 1.79 to 2.60, against 4.94 for
the untouched water beside it; no tile seam is visible, and shapes are the first pass's. On a
770 × 500 hole over a horizon the large fill is still the first pass's guess, now with grain.
**Extend** with Q's first tiling (tiles of at least 768 px, two-thirds overlapping, every margin
pixel in three) took 158–170 s "all" on that photo, against 47–121 s before, and its margins came
out visibly softer. It was measured against the other tilings the same day (the hidden
`[.extend-compare]` case with the real LaMa, `tools/enhance/extend_metrics.py`; the libheif example
photo, 1280 × 854, and a 3840 × 2562 Lanczos enlargement of it standing in for a camera file;
sharpness is the variance of the luma Laplacian in the margins, 8 px clear of the photo and the
edge, the photo's own border band 3,400–5,500 and 117 for the enlargement; "edge" is the step
across the line where a margin meets the photo over the steps beside it, 1 for no line; "seam" the
worst column-to-column peak along a margin over its local median):

| Margins sharpness / edge / seam | "all", 1280 | "square", 1280 | "all", 3840 |
|---|---|---|---|
| Before Q: 512-px tiles at least, half overlapping | 1837 / 0.79 / 5.3 | 83 / 1.66 / 2.5 | 16.5 / 1.96 / 3.8 |
| Q: 768-px tiles at least, three deep | 61 / 2.99 / 4.8 | 17 / 5.63 / 1.9 | 7.0 / 2.28 / 3.6 |
| Q's tiles, then the detail pass | 1210 / 0.91 / 4.8 | 317 / 1.19 / 1.6 | 19.1 / 1.75 / 3.0 |
| Three deep at 512 (no 768) | 401 / 1.10 / 9.1 | 27 / 3.04 / 1.9 | |
| **Shipped:** 512-px tiles at least, two deep, then the detail pass | **1832 / 0.79 / 5.3** | **273 / 1.17 / 1.8** | **28.5 / 1.70 / 3.0** |

Averaging three different fills, not the 768-px size alone, took most of the detail (three deep at
512 kept a fifth of two deep's). The shipped tiling is the sharpest in every case and the best or
level on the edge and the seams; on the 1280 photo "all" its tiles are the model's side, so it is the
old fill (no detail pass, 12 fills, 13.7 s); "square" took 18.1 s (16 fills) and the 3840 photo
99 s (86 fills: 32 tiles and 54 detail tiles) against Q's 66 s and the old 36 s, on a loaded
machine. Q's "colour fringes" at the half-overlapping tiles' edges did not show as seams in these
numbers; the sky over a deep "square" margin can still show a faint step at the photo's edge (the
photo is copied back bit for bit; the fill is not matched to it).

## Restore faces (docs/plans/edit-refresh.md S10, owner call C)

A sharper, clearer face for each person in a photo, from a face model, guarded by People so it
stays the same person. Edit → Enhance tab → Faces card → **Restore faces…** (both hosts; also the ✦
popover / `Flyout`) opens Enhance's docked panel on it under the Faces card (the `tool` screen,
`{"photo": "faces"}`, root `tool.faces`; "Docked in Edit"), or its sheet on it when that screen is
not there; the palette's *Restore faces…* (Enhance's `faces` command) opens the sheet. Knobs: **Look** — *Faithful* (RestoreFormer++, the default: the best identity of the methods
compared in PMRF, ICLR 2025) or *Natural* (GFPGAN v1.4, smoother skin) — and **Strength** (default
0.7). The run (`face_restore.h`, `io.runner: face`; CPU fp32 for quality, the performance level's threads):

1. **The faces and their five landmarks from the AI pack** (People's YuNet), through the host
   table's `still_faces` row (appended after `still_mask`; the pack's `mv_ai_api faces_json`,
   appended after `eyes_json`). Pixels go in, JSON comes out; no path, nothing kept. The whole
   photo is asked at most 2048 px on its long side and, from 2048 px on its short side, also in
   overlapping half-size tiles, so a small face in a group is found; a face found twice is kept once.
   Without Local search the sheet says to add it; with People off, to turn it on.
2. **Alignment**: a least-squares similarity (rotation, uniform scale, shift; never a reflection)
   from the landmarks to the FFHQ 5-point template at 512, the frame both models were trained in.
   The crop is sampled with up to 8 × 8 averaging where it shrinks the photo. The pack is asked
   again on that crop and the alignment refitted once from the finer landmarks. A face with fewer
   than 10 px between the eyes is left alone.
3. **The model** restores the 512 crop (RGB scaled to -1..1; RestoreFormer++'s output 0 only —
   `infer::session::run_named` with an output list, so its 14 other heads are not computed; GFPGAN's
   `1288`, clipped).
4. **The People guard**: the crop as it will be pasted (the original plus Strength × mask × the
   change) against the original crop, through the pack's embedding (`still_faces` with `other`:
   each face's cosine to the face at the same place). Under **`kFaceGuard` = 0.55** the face
   keeps its own pixels, as does a face the pack no longer finds.
5. **The paste back** through the inverse warp under a feathered ellipse (forehead to chin, cheek to
   cheek; whole inside 75 % of its radius, falling smoothly to 0, never the crop's edge), at
   Strength. A face larger in the photo than 512 keeps its own detail: only the model's change is
   added, never its lower resolution.

Every pixel outside the faces' masks is the photo's own (tested to the bit with a stand-in model).
The result is `<stem>-restored.jpg` beside the original, or a new Photos-library photo; when no face
changed (all kept by the guard, too small, or none found) nothing is written and the sheet says why;
Strength 0 is refused before the photo is read. Cancel is checked before every call to the pack
(the photo's tiles, each crop, each guard) and between faces, so it waits at most for one face's model.

**The guard's threshold** (`tools/enhance/face_guard.py`, LFW through the People model, AdaFace
IR-50, on this Mac 2026-11-05; 240 LFW faces, one per person, every other one degraded as the
plan's test crop is: 2× up, blurred, noisy, JPEG 35):

| Cosine to the original face | min | 1st pct | median |
|---|---|---|---|
| RestoreFormer++, Strength 0.7 | 0.822 | 0.886 | 0.953 |
| RestoreFormer++, Strength 1 | 0.795 | 0.852 | 0.931 |
| GFPGAN, Strength 0.7 | 0.726 | 0.836 | 0.942 |
| GFPGAN, Strength 1 | 0.685 | 0.783 | 0.909 |
| Another person's restored face blended in at 0.7 (478 cases), max | 0.322 | | 0.08 |
| Different people (17.7 M LFW pairs) at a false accept of 1e-5 / 1e-6 | 0.384 / 0.678 | | |

0.55 is above every other-person blend and the 1e-5 impostor point, and below every one of the 960
real restorations (none was kept). The pack lost no original face.

**Speed** (M-series CPU, fp32, the hidden `[.face-model]` case, warm, on a machine shared with
other builds): a face aligned, refitted, restored, blended and pasted in **1.1 s with RestoreFormer++**
and **0.6 s with GFPGAN** (the model alone 1.3 s / 0.6 s on the first run). The pack's own calls
(three per face: the photo once, the crop, the guard) are not in those numbers. Under heavy load
(load average 40) the Python guard run took a median 4.4 s / 2.2 s for the model alone. Windows is
not measured.

## Faces in the Enhance tab (edit-refresh S8)

The Enhance tab's **Faces** card is not this add-on: it arms the host's Faces tool
([20](20-edit-workspace.md) "Canvas tools"), whose fixes are the host's own pixel work on real
photos (`edit/eyes.h` through `addon::media::fix_faces`, with donors from the AI pack's People),
never a generated face. A box per face on the canvas, a click chooses which faces change, and the
Faces panel lists each face (thumbnail, People's name, "from 37 photos of Anna") with **Open eyes**
(only where they look shut), **Improve face** and **Do all**. Only the chosen faces change: every
pixel outside their boxes is the original's (`mv_ai_tests "[faces-mask]"`). Results are
`<stem>-eyes.jpg` / `<stem>-faces.jpg`, new files like every Enhance result. The fixes are a table
(`addon::media::face_fix`, `shell::kFaceActions`); the model-based **Restore face** (S10, owner
call C) is the third row, and it is this add-on's: the panel's per-face button opens the docked
panel on `faces` and runs it with the chosen boxes (`"faces"` in the `enhance/tool` command, 0..1
quads). The run reads the faces itself as below and keeps only those each box overlaps most
(`chosen_faces`, intersection over union ≥ 0.3, each face once); a box that matches no face says
"The chosen faces were not found again". The palette's Restore faces… and the Faces card's row
still restore every face.

## Extend: new edges around the photo

The sheet's photo choice **Extend the picture** (owner 2026-11-04) is served by Remove's model with
no second download (`remove` in its `mode: extend`). The margins come from one of two places:

- **The frame dragged on the canvas** (edit-refresh S6, [20](20-edit-workspace.md) "Extend"). The
  host table's `still_extend` row ([14](14-abi.md)) answers the margins past the edges of the photo
  being edited: left, top, right and bottom, each a fraction of the edited picture `still_open`
  returns (the part of the frame inside the photo is already a crop on the edit). The canvas's
  **Fill** runs Extend in the docked panel under the Extend card ("Docked in Edit", `enhance/tool
  {"photo":"extend","run":true}`), or in the sheet with an Enhance that has no docked screen. A
  sheet or panel opened on a photo with a frame starts on Extend (not saved as the photo's choice), and the frame wins over
  the preset: the sheet or panel then shows "The new frame you dragged…" instead of the preset choice. The
  frame is measured against the edit it was set over (`addon_media` keeps that geometry): an undo
  of Fill's crop, or any later turn or crop, and it answers all 0 again (the presets come back).
  Cancel, a new Extend and a new item clear it. `extend_from_fractions` (`remove.h`) rounds each to whole pixels and holds them to
  the caps below, shrinking the margins evenly to fit 100 MP rather than refusing.
- **A preset** otherwise: Wider and Taller (an eighth each side), All round (a tenth), Square,
  16:9 and 4:5 (grown on one axis only, centred).

The caps: at most twice the picture each way, 16384 px a side, 100 MP in all (a preset past 100 MP
is refused). The host's bar reads the size from the same sums (`shell/edit_workspace.h`
`extend_size` and `extend_preset_draft`; `"[enhance][extend]"` checks the two agree), so the file is
the size the bar showed. The run: the photo mirrored into the new margins as a start, overlapping
square tiles along the margins filled at the model's side and blended by weight, then **the photo's
own pixels copied back bit for bit**. The result is `<stem>-extended.jpg` (`run_port::still.suffix`).

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

Enhance image (2026-11-05, the same M-series Mac, CPU fp32 with 8 threads, the hidden
`[.enhance-image-model]` case; **the machine was shared with four other builds**, load average
30–100, so these are upper bounds). Per tile, from the least loaded run (320 × 240): Real-HAT-GAN
5.4 s a 192 tile, RealWebPhoto 10 s a 256 tile, SCUNet 3.3–4.6 s and NAFNet 0.9–1.6 s a 512 tile.
With the overlaps above that is, per input megapixel at steady state, about **330 s** (Sharpen &
restore), **280 s** (Natural), **25 s** (Remove noise) and **6–11 s** (Remove blur, motion blur):
a 12 MP photo is 744 HAT tiles, about an hour. One whole 1024 × 768 run of Sharpen & restore took
1601 s (48 tiles) at load 30–90. Results on two COCO crops and a synthetic scene: HAT took
Laplacian variance 16 → 231 and PSNR to the sharp original 30.5 → 33.6 dB on a blurred crop;
SCUNet took a σ = 15 noisy crop from 24.5 to 36.5 dB; NAFNet-REDS doubled to quadrupled the
Laplacian variance (PSNR −0.4 to −1 dB: it sharpens past the original); NAFNet-GoPro blew up on
the synthetic scene in every view (kept as the photo) and on 11 of 30 COCO crops in Python.

Shrunk to the detail (2026-11-05, the owner's soft 1500 × 1500 photo, Sharpen & restore at the
same size, the hidden `[.enhance-sharp-check]` case, CPU fp32, load average 10–12 from other
builds): d = 4.00 (estimated in 90 ms), 9 HAT tiles, **138 s**, Laplacian variance **2.13 →
18.71** (the plan's Python run: 17.6). The whole photo at d = 1 would be about 170 tiles.

Not measured: Windows (Restyle is refused there without a GPU provider), a long file's memory over time, and the viewer's own gates with Enhance
installed (it loads no runtime until a run, and runs nothing at launch).

## Not built

- Enhance image on Core ML or a GPU: fp32 on the CPU only; Sharpen & restore takes about an hour
  for a sharp 12 MP photo (a soft one is shrunk first and takes 1/d² of that). **Remove motion
  blur is hidden** (`kOfferMotionBlur`): its NAFNet-GoPro export is defective, blowing up on about a
  third of photos (the guard then keeps them as they were, so the tool does nothing); a sounder
  export or another model, then the flag on, wants an owner call.
  A file a changed pin no longer names (the earlier `still/realesr-general-x4v3.onnx`, 4.9 MB)
  stays in the data folder until Remove; nothing sweeps unpinned files yet.
- Restore faces: Core ML / a GPU provider (it runs on CPU fp32 by the quality call); downloading
  only the chosen Look's file (the kind fetches both, 634 MB); the guard against the person's
  People references rather than the face itself (it compares the face before and after).

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
