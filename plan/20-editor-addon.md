# 20 — Editor (an optional video/audio editing add-on)

**Status: proposed 2026-09-26, post-v1, from issue #40. Milestone J, PRs 29–34. Windows and
macOS together (D9, amended 2026-09-24). Not a D-decision, and it needs the owner's
sign-off before PR 30 starts ([Open decisions](#open-decisions-owner)). PR 29 is spikes only.**
Nothing here changes the PR 1–15 viewer or the PR 13/14 clip tools. The Editor is a separate
download, installed from Settings → Add-ons through PR 16's mechanism
([18](18-import.md#add-ons-how-import-is-installed)).

## What it is

A **short-form editor for the clips in a camera dump**. Put a few clips on one timeline, cut
them, grade them, clean up the sound, and export one new file. The timeline has thumbnails and
waveforms, a precise playhead and in/out points, and split and ripple delete. Grading has
curves, a `.cube` LUT, scopes and a before/after view. Audio has gain, EQ, loudness, noise
reduction and, as an optional model download, voice isolation. Preview runs on the viewer's own
canvas. Renders go to a cancellable background queue.

It is **not a studio NLE**. It has one video track and one linked audio track (plus one music
bed from PR 33), cuts and cross-dissolves, and no titles, keyframed effects, compositing,
multicam or third-party plug-ins. Those limits are what make it shippable (see *Later, or never*).

## Contradictions this plan has to resolve (read before building)

1. **"Not an NLE"** (CLAUDE.md, [plan/README](README.md), [08](08-video-editing.md) scope line,
   [10](10-roadmap.md) "Resist the NLE"). That line is about the **viewer**: its installer, its
   updates and what a new user sees. It stands. The Editor follows the Import precedent. It is an
   add-on the base app never carries, and with it absent the install tree is byte-identical and no
   Editor command, menu or key exists. The core changes it needs (below) are generalisations the
   base Edit workspace (issue #39) uses too. They are not an NLE hiding in the base app. **Owner
   decision required**: this is a scope expansion of the product, even though the viewer's scope
   does not change.
2. **[08](08-video-editing.md) mentions "an x264/x265 software fallback"** for full re-encode.
   That line predates [11](11-licensing.md) and CLAUDE.md "Video". This plan follows the licence
   rule: **hardware / OS encoders only**. There is no software H.264/HEVC fallback, and a machine
   with no hardware encoder exports ProRes / lossless intermediates only (S1). 08 is corrected
   in the same change.
3. **AAC.** [11](11-licensing.md) says "never bundle a software AAC encoder; prefer the OS one".
   S1 shows FFmpeg's native `aac` is in the build (Path 2 stream-copies audio and never used it).
   The Editor re-encodes audio whenever gain, EQ or clean-up runs, so it must pick `aac_at`
   (AudioToolbox, Mac) and `aac_mf` (Media Foundation, Windows). Native `aac` stays unused, and a
   test asserts the export path never opens it.

## Rules, and how the Editor holds them

| Rule | How it holds |
|---|---|
| 1 — nothing blocking on UI/render | Timeline thumbnails, waveforms, scopes readback, model inference and renders all run on workers or in the render helper process. The UI thread edits a POD project model and posts it |
| 2 — the canvas is ours | Preview is **the main canvas**, not a second player. The Editor is a workspace mode of the main window, not a new window with a `MediaPlayerElement` or `AVPlayerView` |
| 4 — zero dropped frames | Both present-loop gates are re-run **while a render is running** and **while a model processes audio** |
| 5 — never modify an original | Sources are read-only. A project is a separate file. Exports are new files published through `.mvpart` staging |
| 6 — nothing leaves the machine | No cloud render, no cloud model. The model download is a plain GET of a fixed URL. Project files, caches, stems and waveforms are user data and never go into telemetry or crash reports (minidump filter excludes the audio and frame heaps, as for images) |
| 7 — no required pack | The base app never requires the Editor, and the Editor never requires the voice model. Absent means hidden |

## Where it starts (with issue #39)

Issue #39 gives the base app one **Edit** workspace with a visible *Edit image / Edit video*
button. Its video tab is PR 13's trim lane. The Editor **does not add a second entry point**:

- Editor absent: *Edit video* opens the base trim lane (PR 13/14, unchanged).
- Editor installed: the same button and key open the same workspace. It shows the Editor's
  timeline instead of the trim lane, and a *Simple trim* toggle keeps the base lane one click away.
  The current clip is on the timeline, and nothing is exported until the user asks.
- Edit workspace absent (issue #39 not landed yet): the Editor adds one command, *Open in
  Editor* (`Ctrl+Shift+T` / `⌘⇧T`, currently unbound; checked against the live table when PR 30
  lands).

Issue #39 decides whether the Edit workspace is itself an add-on. This plan assumes it is not:
the workspace and crop are base features, and the Editor plugs its panes into the workspace.

## The workspace

```
┌ Edit video ─ Holiday cut ● ───────────────────────────────────────────── [Simple trim] [Export ▾] ┐
│ ┌ Media ─────────┐ ┌──────────────── canvas (the viewer's swapchain) ────────────┐ ┌ Inspector ────┐│
│ │ ▶ IMG_4411 0:42│ │                                                             │ │ Clip  Grade  ││
│ │ ▶ IMG_4412 1:10│ │            before │ after   (\ toggles, drag the divider)  │ │ Audio         ││
│ │ ▶ IMG_4415 0:09│ │                                                             │ │ Exposure  +0.3││
│ │  (this folder) │ │                                                             │ │ Curves  [╱ ]  ││
│ │ + Add from…    │ └─────────────────────────────────────────────────────────────┘ │ LUT  Rec709 ▾ ││
│ └────────────────┘  00:01:12.18 / 00:02:01.00   ◀◀ ▶ ▶▶   1.0×   I 00:00:31  O 01:12 │ Scopes ▾      ││
├───────────────────────────────────────────────────────────────────────────────────┴───────────────┤
│ V │▕▔IMG_4411▔▔▔▔▔▔▔▔▏▕▔IMG_4412▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▔▏▕IMG_4415▔▏     thumbnails every ~1 s at this zoom      │
│ A │ ▁▃▅▇▅▃▁▁▃▅▃▁▁▁▂▃ ▁▂▅▇▇▅▂▁▁▂▃▅▃▂▁▁▂▃▅▇▅▃▁▁ ▂▃▂▁▂   waveform, loudness colour-coded            │
│ M │ ♪ music bed (PR 33)                                                                            │
│   0:00        0:15        0:30   ▼playhead   0:45        1:00        1:15        1:30             │
├───────────────────────────────────────────────────────────────────────────────────────────────────┤
│ Render queue: Holiday cut.mp4 · HEVC · VideoToolbox · 42 % · 0:38 left      [Cancel] [Reveal]      │
└───────────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Media** lists the open folder's clips (the viewer's thumbnails and posters). *Add from…*
  adds clips from other folders. Clips are referenced, never copied.
- **Canvas**: the viewer's canvas plays the timeline. Before/after is a split or a toggle on the
  same frame, drawn by the blit, so it costs no second decode.
- **Inspector**: per-clip Clip (speed 0.5–2× from PR 31, rotation, audio on/off), Grade (PR 32)
  and Audio (PR 33/34). **Scopes** dock here: histogram, luma waveform, RGB parade, vectorscope.
- **Timeline**: a V track, a linked A track, and an M track for one music bed (PR 33).
  Thumbnails and waveform are generated in the background and cached. Zoom runs from the whole
  sequence down to single frames, and at frame zoom a keyframe grid shows where cuts are lossless.
- **Render queue**: the Jobs pane (PR 13), with Editor renders as jobs. Closing the workspace
  keeps a render running.

UX mockups for both hosts (WinUI 3 and SwiftUI, light/dark, 100 % and 200 % scale) are produced
in PR 30 before the chrome is written, as issue #39 asks for its workspace.

## Architecture

**The split: what draws on the canvas or writes the output file lives in the core; what only
the Editor needs lives in the add-on.** The canvas is one present path per OS that C++ owns
(rule 2), and the add-on links nothing of the core ([18](18-import.md)). A timeline preview
therefore cannot be drawn by the add-on. The core has to play a *sequence*.

### Core (base app, shared with issue #39's Edit workspace)

| Piece | What changes | Why in the core |
|---|---|---|
| `edit/sequence.h` | A POD **sequence**: ordered segments of `{source, in_ns, out_ns, speed, grade, audio}` on a rational timeline. A sequence of one segment is today's trim | The Edit workspace's trim lane and the Editor share one model. Versioned JSON, the same on both OSes |
| `player/` sequence source | An `IVideoSource` that plays a sequence gaplessly: pre-rolls the next segment's decoder before the cut, one audio clock across cuts, silence for a clip with no audio | The present path is the player's. Seams must be as smooth as a single clip |
| `gfx/` grade stage | Generalises PR 11's single-source kernel (`adjust_kernel.h` → HLSL, MSL and C++). Adds a 1D curves LUT texture (RGB + per-channel), a 3D LUT texture (33³, `.cube`) and a split/before-after uniform | One kernel for preview and export, compiled three ways, so the preview is the output to within 8-bit rounding (the `bake.h` pattern). The photo backlog's *curves* and *colour grading* reuse it |
| `gfx/` scopes | Async readback of a ¼-res frame (staging texture, mapped without waiting, one or two frames late), reduced on a worker to histogram, waveform, parade and vectorscope | The render thread never waits (rule 1). The histogram exists already (`edit/histogram`) |
| `player/` audio DSP | Per-segment gain, fades, a 5-band biquad EQ and a limiter in the audio sink's mix callback, sample-accurate across cuts | Must run on the audio clock in real time. Cheap and deterministic, and the same code runs in the export |
| `edit/clip` render | A new op, `render_sequence`, run in `MediaViewerClipJob` (the helper process): decode each segment, grade (C++ kernel), mix audio (same DSP), encode through the encode port, mux. **Lossless where possible**: a segment with no grade, speed or audio change, cut on keyframes and matching the output codec, is stream-copied, and the export label says how many segments were | The helper already isolates decoder and encoder crashes, and cancel is a kill (plan/12 2026-09-25). One render engine for trim and Editor |
| `edit/hwencode` | Adds `prores_videotoolbox` (Mac) and the `av1_*` hardware encoders (Windows) as intermediate / export options | S1 below |

Headers in `edit/`, `player/` and `gfx/` consumers stay free of `d3d11.h` and Metal (D9). The
HLSL and MSL twins are written in the same PR.

### Add-on (`editor`, downloaded)

- **Native** `mv_editor.dll` / `libmv_editor.dylib` (`mv_addon_get`, interface `mv.editor.1`):
  the multi-clip project model and its undo history, project files (`.mvedit`, JSON, sources by
  path plus size, mtime and a BLAKE3 of the first and last 1 MiB, for relinking), the thumbnail
  strip and waveform cache builders, loudness analysis (EBU R128 via libavfilter `ebur128`), the
  clean-up pre-passes, and the model runner for voice isolation.
- **Chrome**: `MediaViewer.Editor.Chrome.dll` (its own `AssemblyLoadContext`) and
  `Editor.bundle` (SwiftUI), which provide the timeline, media, inspector and scopes panes docked
  into the Edit workspace.
- **Voice model** (optional, a separate sub-pack, PR 34): the model files and their runtime.

### Host function table v2 (spike S4)

The add-on reaches the core only through `mv_host_api` ([mediaviewer_addon.h](../src/abi/include/mediaviewer/mediaviewer_addon.h)).
v1 was built for Import (io, pairing, thumbnails). The Editor needs these appended fields and a
`MV_ADDON_HOST_API` bump (additive, and `struct_size` keeps Import loading):

- `probe_clip` (duration, keyframes, streams, VFR flag): wraps `clip::probe`.
- `preview_set_sequence(json)` and `preview_seek / play / pause`: the canvas plays what the
  add-on built. The host copies the JSON, so no pointer outlives the call.
- `set_grade(segment, uniforms, curve_lut, cube_lut)`: POD uniforms plus two small tables, never
  a texture handle.
- `read_scopes(out)`: the latest reduced scope buffers (POD, fixed size).
- `decode_audio_pcm(path, range, rate, cb)`: float PCM on a worker, for waveforms, loudness and
  clean-up. The add-on never opens FFmpeg itself.
- `submit_render(sequence_json, output_opts) → job id` and `job_snapshot`: the existing clip
  queue and helper.
- `cache_dir(bytes_wanted, out)`: the Editor's cache root, with the free space on it.

**The add-on never receives a device, texture, swapchain or FFmpeg context.** That keeps
the D9 hostability and the "links nothing of the core" rule, and it is why grading and preview
live in the core.

**No third-party plug-in API** in this milestone. OFX, VST3 and CLAP hosting would run arbitrary
code in the render path and put their licence and crash risk in front of users. The only code
the Editor loads is signed with the add-on key. If ever reopened, it is a separate plan, and the
plug-ins run out of process.

### Preview and render stay in sync

- The project is the one source of truth. The UI thread edits it and posts a new immutable
  sequence snapshot to the core (**generation counter**, as for navigation). Preview picks it up
  on the next frame. A render **snapshots the sequence at submit**, so editing during a render
  changes the preview and never the file being written.
- Clean-up pre-passes (noise reduction, voice isolation) are **offline**. They write processed
  audio to the cache keyed by `(source hash, range, settings, model id)`, and preview plays the
  processed audio once it is ready (a progress chip until then; the original plays meanwhile).
  A render waits on, or reuses, the same cached result. Models never run in the real-time path.
- Render priority: background by default. The helper runs at below-normal priority and yields
  between GOPs while the present loop is busy (`should_yield`, as Import does). Fast renders are
  opt-in and run while the viewer is idle.

## Time, frame rate and audio (spike S5)

Camera dumps are the hard case: phone clips are **variable frame rate**, clips run long
(2 h dashcam and GoPro chapters), and one timeline mixes 44.1 and 48 kHz, mono and stereo, and
clips with no audio.

- **The timeline is in rational time**, never frame numbers. Cuts are PTS on the source's own
  timeline (`clip.h`'s `time_ns`). A frame at the playhead is "the frame displayed at t".
- **Sequence frame rate**: taken from the first clip, rounded to a standard rate (23.976 / 25 /
  29.97 / 30 / 50 / 59.94 / 60), editable. Export conforms to it by **frame selection** (nearest
  displayed frame, no interpolation). A VFR source is labelled *VFR → 30 fps*. An option keeps
  source timing when every segment is stream-copied.
- **Sequence resolution**: the first clip's, editable. Other clips are fitted with a letterbox or
  pillarbox (never stretched) and rotated by their display matrix.
- **Audio**: mixed in float at 48 kHz stereo (swresample per segment). Mono is centred and 5.1
  is downmixed with ITU coefficients. A clip with no audio contributes silence. Multiple tracks:
  the first by default, selectable per clip. The export default is AAC (OS encoder) at 256 kb/s,
  with ALAC / FLAC / PCM in MOV/MKV as lossless options.
- **HDR sources** (iPhone HLG/PQ): preview tone-maps to SDR as the player already does. Export
  is SDR Rec.709 in PR 32. HDR export waits for HDR output (D6 backlog) and is labelled.
- **Long clips**: thumbnail strips and waveforms are built lazily for the visible range first,
  then the rest in the background, cached, with memory bounded by zoom level and not by clip
  length. The sequence source keeps at most two decoders open.

## Exports that preserve the source

- **Never overwrite.** The default name is `<first clip>_edit.mp4` beside the first source, or
  in the last export folder, and a clash is numbered, as `clip::run` does. Replacing an existing
  file is not offered.
- **Staged, then published**: `.mvpart` beside the output, flushed, renamed. Cancel, failure or a
  helper crash removes it (`sweep_temporaries`).
- **Lossless where possible, and honest about it**: the export sheet says "3 of 5 segments
  copied without re-encoding; 2 re-encoded on NVENC (HEVC, 40 Mb/s)". A cut inside a GOP on an
  untouched segment is re-encoded whole in the MVP. Head/tail smart cut is D7's v1.1 work, not
  this.
- **Presets with units**: *Same as source* (codec, size, rate, bitrate +10 %), *Share (H.264
  1080p, 16 Mb/s)*, *Archive (HEVC 10-bit, source size)*, *Intermediate (ProRes 422 on Mac)*.
  Bitrates are shown, and there is no unitless "quality" slider ([08](08-video-editing.md)).
- **Metadata**: the creation date is taken from the first segment, rotation is baked, and
  location is **off by default** (a checkbox "Keep location from the first clip"), following
  [06](06-metadata.md)'s policy for exports.
- **Project files are the user's documents.** Saved where the user chooses (default beside the
  first source), never deleted by removal or cache cleanup.

## Temp space and caches

- **One cache root per user**: `%LocalAppData%\MediaViewer\cache\editor` /
  `~/Library/Caches/MediaViewer/Editor`, holding thumbnail strips, waveforms, loudness, processed
  audio stems and (later) proxies. Nothing is written beside sources except the export's
  `.mvpart` and the project file.
- **Cap**: default 10 GB or 10 % of the volume's free space, whichever is smaller, and editable
  in Settings → Add-ons → Editor, which shows the current size and a **Clear cache** button.
  Eviction is LRU by project last-opened.
- **Before a render**, the needed space is estimated (bitrate × duration × 1.1, plus
  intermediates) and checked on the output volume. If it is short, the render is refused up front
  and never fails at 90 %.
- **Crash hygiene**: the cache writes a journal. At the next start, entries with no completed
  marker and orphan `.mvpart` files from Editor renders are removed. Removing the add-on deletes
  the cache (with its size shown) and keeps project files.

## Voice isolation and clean-up (spike S3)

Three tiers, so the common case costs no download:

| Tier | What | Ships as | Real-time? |
|---|---|---|---|
| Clean-up | High-pass, gate, `afftdn` spectral noise reduction, `anlmdn`, de-hum (notch at 50/60 Hz and harmonics), loudness normalise (R128, −16 LUFS default for share, −23 for archive) | The Editor add-on itself (FFmpeg filters present in the LGPL build, S1) | Offline pre-pass; fast |
| Voice isolation (speech) | Keep speech, suppress wind, traffic, crowd and music under dialogue | **Optional model sub-pack** `editor-voice`, with its size shown before download | Offline pre-pass |
| Stem separation (music / vocals / drums / bass) | Remix a music bed | **Later**, a separate sub-pack only if S3's licence and size check passes | Offline, slow |

S3 decides the voice model. **No model is assumed.** Candidates to measure:

- **DeepFilterNet (v2/v3)**: speech enhancement at 48 kHz, small and CPU-real-time by its
  authors' account. Code and weights are reported as MIT / Apache-2.0, to be confirmed file by
  file.
- **RNNoise-family models through FFmpeg's `arnndn`**: already in the build, and tiny. The
  licence of each model file has to be checked.
- **Demucs (v4 / htdemucs)**: the stem tier. The code is MIT. The terms of the pretrained weights
  and their training data have to be checked before they can be redistributed. It is large and
  slow on CPU.

Gate (the same as [17](17-local-ai-search.md)): the sub-pack manifest names a licence for every
model file, and CI fails on non-commercial or research-only weights. Runtime: ONNX Runtime CPU,
plus the Core ML provider on Mac, dynamic-linked inside the sub-pack. Its ORT copy is its own
(sharing one runtime with the AI pack needs add-on dependencies; see Open decisions).

**S3 measures on both platforms**: sub-pack download and installed size (a ceiling of 300 MB for
`editor-voice`, stated in the manifest and enforced like the AI pack's 3 GB), seconds of
processing per minute of audio on CPU and on Core ML, peak memory, and quality on a local eval
set (DNSMOS and SI-SDR on speech with added noise: wind, traffic, café and music). The eval set
lives outside git, like the RAW corpus. **No-go** if nothing licence-clean beats the no-model
clean-up tier by a clear margin. In that case voice isolation is dropped and PR 34 closes as
not built.

## Install, sign, update, remove

The same mechanism as Import, and nothing new to trust:

- **Settings → Add-ons → Editor**: "Install Editor, N MB". Then, inside the Editor's settings,
  "Voice isolation model, N MB download, N MB on disk". Nothing is pre-ticked, and the sizes come
  from the signed manifest.
- **Signed and verified**: the Ed25519 manifest is checked with the update key before the archive
  is fetched, every file is hashed before install **and at every load**, Windows binaries are
  Authenticode-signed, the Mac bundle is Developer ID-signed and notarized and loaded under library
  validation. The request is a fixed URL with no identifier.
- **Sub-packs need one addition to the manifest**: `requires: {"id": "editor", "min": "1.0.0"}`.
  `editor-voice` is refused without a compatible `editor`, and removing `editor` offers to
  remove `editor-voice` too. The AI pack needs the same field for its provider sub-packs ([17](17-local-ai-search.md)),
  so it is added once, in S4.
- **Updates** arrive with the app on the stable feed. A host-API mismatch shows "Editor needs an
  update" and falls back to the base trim lane. Sideloading works offline.
- **Remove**: the add-on unloads after its render jobs are cancelled (staged output swept), then
  its folder and cache are deleted. Project files stay, and the base trim lane returns.
- **Absent means absent**: the base tree is byte-identical, and there is no Editor command, key
  or pane.

## Accessibility

- **Keyboard-complete** ([16](16-commands.md)). The Editor layers on the existing video keys
  instead of inventing new ones: `J` `K` `L` shuttle, `,` `.` frame step, `Q` `E` skip, `[` `]`
  in/out (as in trim), `Ctrl+B` split at the playhead, `Ctrl+X` ripple-delete in–out, `Ctrl+Z` /
  `Ctrl+Shift+Z` undo/redo, `Ctrl+J` the render queue, `\` before/after. Inside the timeline,
  `Tab` moves between tracks, `←` `→` select the previous/next clip, `Alt+←` `Alt+→` move it, and
  `Enter` opens it in the Inspector. Mac uses the `⌘` equivalents. The keys are checked against
  the live table in PR 30, and the table wins over this list.
- **Screen readers** (UI Automation / NSAccessibility): the timeline is exposed as a list of
  clips ("IMG_4411, 0:00 to 0:42, graded, gain −3 dB"). The playhead announces its time on pause
  and step, in/out changes are announced, and every scope has a text readout (clipping %, peak
  and average luma, loudness in LUFS). A waveform or a colour is never the only cue.
- **Contrast and motion**: markers and clip colours pass 3:1 against the track in light, dark
  and high-contrast themes, and are distinguishable without hue (shape and pattern). Reduced
  motion turns off animated zoom (springs snap).
- **Scale**: 100–300 % DPI and Dynamic Type. The track height and hit targets have a 24 DIP
  minimum.

## Spikes first (PR 29)

| # | Question | Output | State |
|---|---|---|---|
| **S1** | Which hardware encoders open at 1080p30, 2160p30 and 2160p60 10-bit, how fast; which OS audio encoders and LGPL filters exist | `tools/encprobe` report per machine | **Mac done** (below). Windows owed: an NVENC box, a Quick Sync box, an AMF box |
| **S2** | Can the sequence source play a cut between two 4K HEVC clips, grade kernel on, at display refresh with no dropped frame? And the export-side C++ grade kernel's speed | A lab branch of the sequence source; frametime soak across 100 cuts; C++ kernel ms per 4K frame | Owed, both platforms |
| **S3** | Voice model size, licence, speed, quality | Table above, filled; go / no-go | Owed |
| **S4** | Host table v2 and manifest `requires` against a stub add-on, including an old Import loading against the v2 table | Header diff plus a stub add-on in `tests/` | Owed |
| **S5** | VFR, long-clip and mixed-audio behaviour of the sequence model | Test fixtures from `tools/testmedia` (VFR phone clip, 2 h chaptered clip, 44.1 + 48 kHz + silent + 5.1 mix); A/V drift measured | Owed |

### S1 results — Apple M5, macOS 26.6, the app's FFmpeg (libavcodec 63.1.101), 2026-09-26

Synthetic frames, encoder only (no decode, grade or mux), so these are upper bounds:

| Encoder | 1080p30 8-bit | 2160p30 8-bit | 2160p60 10-bit |
|---|---|---|---|
| `h264_videotoolbox` | 257 fps (8.6×) | 71 fps (2.4×) | n/a (no 10-bit H.264) |
| `hevc_videotoolbox` | 250 fps (8.3×) | 69 fps (2.3×) | 69 fps (**1.1×**, P010) |
| `prores_videotoolbox` | 1812 fps (60×) | 494 fps (16×) | 437 fps (7.3×, P210) |

- Audio: `aac_at` and `alac_at` (OS) open. `flac` and PCM are available as lossless. Native
  `aac` is present but must not be used (contradiction 3). The native `opus` encoder is
  experimental, so it is not offered. `libopus` is not in the build.
- Filters: `volume`, `equalizer`, `highpass`, `acompressor`, `alimiter`, `loudnorm`, `ebur128`,
  `afftdn`, `anlmdn`, `arnndn`, `showwavespic`, `amix`, `lut3d`, `tonemap` and `colorspace` are
  present. `zscale` is absent (no zimg), so HDR conversion stays in our kernel. The GPL-only
  `eq`, `hqdn3d` and `delogo` are absent, which confirms the LGPL build.
- Hardware decode: `videotoolbox` creates a device.
- **Reading:** 4K60 10-bit HEVC export is only just faster than real time on the encoder alone,
  so a 4K60 export will run slower than real time once decode and grading are added. That is
  acceptable, and it is labelled with a measured ETA. ProRes is fast enough to be the Mac
  intermediate and proxy codec. Windows needs the same table before the export presets are
  final.

Run it: `tools/encprobe/CMakeLists.txt` (opt-in, not part of the app build).

## Roadmap slices (both platforms each)

Sequenced **after PR 28** and after issue #39's Edit workspace. Each slice has a Windows half
and a Mac half, a verify line per platform, and both present-loop gates **while rendering**.

### PR 29 — Spikes (S1–S5), no user-visible change
**Verify (both platforms):** S1 through S5 filled in above with numbers from real machines
(Windows: NVENC, Quick Sync and AMF; Mac: Apple Silicon), each ending in a go / no-go line. A
no-go on S2 (seams drop frames) stops PR 30 until it is fixed. A no-go on S3 drops PR 34.

### PR 30 — MVP 1: the add-on, one clip, a real timeline
Add-on install/remove (with `requires`), host table v2, the core sequence source (one segment),
the render op, and the workspace inside issue #39's Edit workspace: a timeline with thumbnails
and waveform, playhead, in/out, split, ripple delete, per-clip gain and fades, the render queue,
export presets, the cache with its cap and Clear button, and project save/open.

**Verify (both platforms):** base tree byte-identical and no Editor key with the add-on absent;
the add-on refused with a tampered file. A 10-minute 4K clip: thumbnails for the visible range in
< 1 s, and the whole strip and waveform in the background. Split, delete, then export: the output
has exactly the expected frames (first and last checked by content). An untouched segment is
stream-copied (packets byte-identical), the source is byte-identical afterwards, and cancelling
at 50 % leaves no file. The cache stays under its cap. Keyboard only: open, cut, export. **Both
present-loop gates hold while a render runs.**

### PR 31 — MVP 2: multi-clip composition
Several sources on V + A, reorder, add from other folders, cross-dissolve (video and audio),
speed 0.5–2×, sequence frame rate and size, VFR conform, the mixed-audio mix, and relinking a
moved source.

**Verify (both platforms):** a timeline of a VFR iPhone clip, a 25 fps camera clip, a clip with
no audio and a 44.1 kHz clip exports at the sequence rate with A/V drift ≤ 1 frame at the end.
A 2-hour chaptered clip opens and scrubs, and memory is flat over time. The preview plays 20
cuts between 4K HEVC clips with **no dropped frame at the cut** (frametime harness). The same
project renders on Windows and Mac to outputs within PSNR ≥ 45 dB of each other (encoders
differ, so hashes do not match).

**MVP = PRs 30 + 31.** This is where the Editor becomes worth installing, and a user can stop
here.

### PR 32 — Grade
Curves (master + RGB), `.cube` LUT import (33³, validated, stored in the project), PR 11's
exposure/contrast/saturation/WB per clip, copy/paste grade, before/after split, and scopes
(histogram, luma waveform, RGB parade, vectorscope) with text readouts. HLSL + MSL + C++ kernel
twins.

**Verify (both platforms):** preview vs export of a graded frame within 1 code value (8-bit)
everywhere; a LUT known to be identity leaves the export bit-exact with the ungraded one; scopes
update at ≥ 15 Hz during playback with no dropped present frames; screen reader reads scope
readouts.

### PR 33 — Audio
5-band EQ, compressor/limiter, the no-model clean-up tier (high-pass, gate, `afftdn`, de-hum),
loudness analyse + normalise to a target, meters (peak, short-term LUFS), and one music bed on
M with ducking under speech clips.

**Verify (both platforms):** normalise to −16 LUFS gives an export measured at −16 ± 0.5 LUFS,
with the true peak ≤ −1 dBTP. Noise reduction on a noisy fixture improves DNSMOS on the local
eval set. The EQ and gain the user hears in the preview match the export sample for sample. A
clip edited with the network off works end to end.

### PR 34 — Voice isolation (optional sub-pack), only if S3 says go
The `editor-voice` sub-pack (signed, size shown, CPU + Core ML) and a per-clip *Isolate voice*
control with a strength amount, processed offline into the cache and previewed when ready.

**Verify (both platforms):** installing shows the manifest's size, and the installed size is
under the ceiling. Removing it leaves the Editor working with the clean-up tier. A 10-minute
clip processes within the time S3 measured, and **both present-loop gates hold while it
processes**. The eval-set quality meets S3's go line. Nothing is fetched except the fixed pack
URL (checked with a proxy log).

## Later, or never

- **Later (own plans):** proxies for 8K / long-GOP editing, titles and captions (the subtitle
  pipeline first, [08](08-video-editing.md)), a second video track (picture-in-picture),
  keyframed parameters, stem separation, HDR export (after the FP16 swapchain, D6), smart cut
  on untouched segments (D7), and a shared ONNX runtime between the AI pack and the Editor.
- **Not planned:** third-party plug-ins (OFX/VST3/CLAP), multicam, motion tracking, cloud
  anything, software H.264/HEVC encoders, and AI generation of any kind.

## Open decisions (owner)

1. **Approve the Editor at all.** It stays out of the viewer, but it is a second product surface
   with a real maintenance cost (every kernel ×3, every pane ×2).
2. **Order against Milestones H and I.** As written it follows PR 28. The MVP (PRs 30–31) could
   come before AI search, since it needs no model.
3. **Voice sub-pack size ceiling** (300 MB proposed) and whether an ONNX runtime shared with the
   AI pack is worth add-on dependencies.
4. **Issue #39:** whether the Edit workspace is itself an add-on. This plan assumes it is base.
