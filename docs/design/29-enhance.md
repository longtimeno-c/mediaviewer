# Enhance

The Enhance add-on (#297): a restored copy of the photo, clip or audio file on screen, made by a
model that runs on this computer and is downloaded only when the person asks for it. The original
is never the output (rule 5). Built: model downloads and the sheet (slices 1–2), and sound — an
audio file's, or a clip's (slice 4). Not built: photos (slice 3) and a clip's picture (slice 5);
[the plan](../plans/enhance.md) has what is left.

Code: `src/addons/enhance` (`cmake/enhance.cmake`), the host's `edit/sound_writer.h` and the
sound rows of the host table ([14](14-abi.md)). Decisions: [12](12-decision-log.md), "Enhance is
its own add-on" and "Enhance's first audio model" (2026-10-26).

## What it is

One slot add-on with no chrome of its own ([25](25-open-addons.md) §10): both hosts draw its two
screens from its view trees.

- **Its page in Settings → Add-ons** (place `settings`): one card per kind (photos, video,
  audio) with that kind's model, its credit and size, and Download / Stop / Resume / Remove.
- **The sheet over the viewer** (place `sheet`): `Enhance…` in the palette and the context menu
  (no default key, [16](16-commands.md)), for the file on screen.

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
| `knobs` | `slider` / `choice` / `toggle`, each with the kinds of target it applies to |
| `io` | the runner's own section (for sound: `runner: stream`, rate, frame, state, delay) |

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

The host writes the result (`edit/sound_writer.h`) beside the original: `<stem>-enhance.m4a`
(AAC, FFmpeg's own encoder, about 96 kb/s a channel) for an audio file; for a clip, its picture
stream-copied packet for packet with the new sound in place of its own, in its own MP4 / MOV, else
Matroska. It is a hidden part file until finished, then published under the first free name
("name (2)"); a cancel or a failure leaves nothing. Cover art and other tracks are not carried.
The write runs in the app's process, not the clip helper ([08](08-video-editing.md)): the sound
comes from the add-on, and an AAC encode has no driver to crash.

## Measured (2026-10-26, Apple M-series, CPU, one thread)

| | |
|---|---|
| Speech under white noise and 50 Hz hum at 5 dB SNR (11 s, `say`), SI-SDR | 5.0 → 9.7 dB |
| A tone under noise (3 s, the hidden `[.enhance-model]` case), error energy | 481 → 13.7 |
| Speed, one channel | 0.13 × real time (1.45 s for 11 s) |
| Speed, stereo, through the host table (read, model, AAC, write) | 1.5 s for 5 s |
| Memory: ONNX Runtime + the session + runs | about 18 + 40 MB (`run_bytes` 150 MB) |

Not measured: Windows, a long file's memory over time, and the viewer's own gates with Enhance
installed (it loads no runtime until a run, and runs nothing at launch).

## Not built

- Photos (slice 3) and a clip's picture (slice 5): the sheet names them; Run stays greyed.
- Loudness: an "even out loudness" knob needs a second pass over the result.
- Keeping an MP3 an MP3: there is no MP3 encoder in the LGPL build; the result is an M4A.
- A live preview on the playback clock (not in #297).
