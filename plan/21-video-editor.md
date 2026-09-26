# 21 — The Video Editor (its own window) and the Editor add-on

**Status: 2026-09-26, from issue #40 and the owner's review of PR 29. Milestone J, PRs 30–35,
Windows and macOS together (D9). PR 30 — the Video Editor window, one clip — is written and run on
the Mac (below, "What was built"); its Windows half is owed. PRs 32–35, the optional Editor add-on
(grading, audio clean-up, voice isolation), are proposed and need the owner's sign-off.**

## What it is

Video is edited in a **window of its own**, with a timeline, the way iMovie, Final Cut and
DaVinci Resolve do it, not in a side pane (owner, 2026-09-26; [20](20-edit-workspace.md)
"Owner review"). *Edit video* (or `Enter`) on a clip opens it:

```
┌ Video Editor — GOPR0412.MP4 ─────────────────────────────────────────────────────────────┐
│                                                                                           │
│                     preview: the viewer's own canvas, moved into this window              │
│                                                                                           │
├───────────────────────────────────────────────────────────────────────────────────────────┤
│ ◁| ❚❚ |▷  0:07.57 / 0:10.70 │ Split  Delete  Set in  Set out │ Undo  Redo    Export  Export exact │ Done │
│ 0:00      0:01      0:02      0:03      0:04      0:05      0:06    ▼ 0:07      0:08      0:09   │
│ [▣▣▣▣▣▣ piece 1 (thumbnails) ▣▣▣▣▣▣][▣▣▣▣ piece 2, selected ▣▣▣▣▣▣▣▣▣▣]                        │
│ [∿∿∿∿∿∿∿ waveform ∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿][∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿]                        │
│ Space play · ← → frame · I O in / out · ⌘B split · ⌫ delete piece · ⌘Z undo · ⌘E export        │
└───────────────────────────────────────────────────────────────────────────────────────────┘
```

Two layers, split by payload rather than by feature count:

- **The Video Editor (base app, PRs 30–31).** The window, the timeline, cutting, several clips,
  and export. It is built on PR 13/14's clip core and adds no library, model or download, so it
  costs the installer nothing.
- **The Editor add-on (optional, PRs 32–35; issue #40).** The parts that do add payload or
  maintenance weight: grading (curves, LUTs, scopes), audio clean-up and mixing, and voice
  isolation with its model download. It docks into the Video Editor window when it is installed.

Still **not a studio NLE**: one video track and its audio, cuts (dissolves from PR 31), no titles,
keyframed effects, compositing, multicam or third-party plug-ins.

## Contradictions this plan has to resolve (read before building)

1. **"Not an NLE"** (CLAUDE.md, [plan/README](README.md), [08](08-video-editing.md),
   [10](10-roadmap.md) "Resist the NLE"). The owner asked for a timeline editor in its own
   window. The line now reads as the limits above: a cut editor for camera clips is in, a
   studio NLE is not. Recorded in [12](12-decision-log.md) 2026-09-26.
2. **[08](08-video-editing.md) mentions "an x264/x265 software fallback"** for full re-encode.
   That predates [11](11-licensing.md). The Editor uses **hardware / OS encoders only**; *Export
   exact* is Path 2's encoder rules.
3. **AAC.** [11](11-licensing.md): never a bundled software AAC encoder. The base editor copies
   audio (no re-encode). The add-on's audio features re-encode through `aac_at` / `aac_mf` (the
   OS encoders; S1 below), never FFmpeg's native `aac`.

## What was built (PR 30, Mac, 2026-09-26)

| Piece | Code |
|---|---|
| The cut list: pieces of one clip, split / delete / set in / set out, undo / redo, source ↔ program clocks, where playback jumps | `shell/video_timeline.{h,cpp}` (shared, tested) |
| The timeline strip: keyframe thumbnails (sRGB, display rotation, HDR tone-mapped like the canvas) and an audio peak envelope | `edit/clip_strip.h`, in `edit/clip_encode.cpp` (tested) |
| Export: `clip::op::keep_ranges` — the pieces in one file, cut on keyframes (packets copied, instant), or *exact* (every piece decoded and re-encoded on the hardware encoder, in the `MediaViewerClipJob` helper) | `edit/clip_run.cpp`, `clip_encode.cpp` (the Path 2 loop, generalised to pieces); ABI 0.11 appends `ranges_ns` / `range_count` to `mv_clip_request` |
| The window: the canvas **moves into its preview** while it is open (one canvas, one CAMetalLayer, one present path, rule 2) and back when it closes; the viewer shows where it went | `main_mac.mm` (`setEditorOpen:`, `moveCanvasToEditor:`) |
| Playback over the edit: a 60 Hz main-thread tick jumps the player over each cut and pauses at the end | `main_mac.mm` (`editorFollowPlayback`) |
| The timeline UI: transport, timecode, cut tools, thumbnail and waveform tracks, a draggable playhead, piece selection, Export / Export exact, keyboard-complete | `VideoEditorView.swift` |

Run with the `MV_EDIT_SELFTEST` rig on a 16 s clip: open → split at ⅓ and ⅔ → delete the middle →
export both ways. The program is 10.71 s; the keyframe-cut file is 11.13 s (each cut on its
nearest keyframe, as labelled) and the exact file 10.69 s (VideoToolbox), and the source is
untouched. Tests: `test_video_timeline`, and the `[pr30]` cases in `test_clip` (pieces with and
without B-frames, exact pieces frame for frame, the helper wire, the strip).

**Known limits of PR 30:** a join during playback can show a frame of the cut while the exact
seek lands (the tick runs a frame ahead). Export exact copies audio packet-accurately, as Path 2
does, so a join can carry up to a packet of the cut's audio. The timeline fits the whole program
(zoom is PR 31). No VoiceOver pass yet. Windows: nothing yet.

## Architecture (PRs 31–35; PR 30's pieces are in the table above)

Rules held throughout: nothing that decodes, encodes or reads a file runs on the UI or render
thread (the strip and exports are worker jobs; encodes run in the helper process); the preview
is the viewer's canvas and never an `AVPlayerView` / `MediaPlayerElement` (rule 2); sources are
never written (rule 5); nothing about the files leaves the machine (rule 6).

**The split: what draws on the canvas or writes the output file lives in the core; what only
the Editor add-on needs lives in the add-on.** The canvas is one present path per OS that C++ owns
(rule 2), and the add-on links nothing of the core ([18](18-import.md)). A timeline preview
therefore cannot be drawn by the add-on. The core has to play a *sequence*.

### Core (base app; `edit/sequence` grows out of PR 30's `video_timeline` in PR 31)

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
  into the Video Editor window.
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
  the live table when each PR lands, and the table wins over this list.
- **Screen readers** (UI Automation / NSAccessibility): the timeline is exposed as a list of
  clips ("IMG_4411, 0:00 to 0:42, graded, gain −3 dB"). The playhead announces its time on pause
  and step, in/out changes are announced, and every scope has a text readout (clipping %, peak
  and average luma, loudness in LUFS). A waveform or a colour is never the only cue.
- **Contrast and motion**: markers and clip colours pass 3:1 against the track in light, dark
  and high-contrast themes, and are distinguishable without hue (shape and pattern). Reduced
  motion turns off animated zoom (springs snap).
- **Scale**: 100–300 % DPI and Dynamic Type. The track height and hit targets have a 24 DIP
  minimum.

## Spikes (PR 32; S1 done)

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

Each slice has a Windows half and a Mac half, a verify line per platform, and both present-loop
gates **while the editor plays and while it renders**.

### PR 30 — The Video Editor window, one clip *(Mac written and run; Windows owed)*
The window, the canvas hand-off, the cut list, the strip, the timeline UI, playback over the
edit, Export / Export exact (above).

**Verify (both platforms):** `Enter` on a clip opens the window with the clip in the preview and
the strip drawn; closing it puts the canvas back. Cut the middle third: playback skips it and
stops at the end. Export writes a new file beside the source whose duration matches the program
(within a GOP for keyframe cuts, a frame for exact), and the source is byte-identical. Keyboard
only: open, split, delete, export, close. Both present-loop gates hold with the editor open.

### PR 31 — Several clips, zoom, dissolves (base)
Add clips from the folder (the viewer's gallery as the media list), reorder pieces (which needs
`keep_ranges` over several sources: the exact path first, then the copy path where codecs match),
a zoomable timeline with a keyframe grid at frame zoom, cross-dissolves (exact export only), and
the project saved as a small JSON file beside the first clip.

**Verify (both platforms):** three clips of different frame rates and one without audio export as
one file at the sequence rate with A/V drift ≤ 1 frame; a 2-hour clip opens and scrubs with flat
memory; 20 cuts between 4K HEVC clips play with no dropped frame at a cut.

### PR 32 — The Editor add-on: spikes and the mechanism
S2–S5 below (S1 is done on the Mac), then the add-on through PR 16's mechanism (signed, sized,
removable), host function table v2 so it can drive the editor's preview and render, and the
Settings entry. Nothing user-visible beyond Install / Remove.

### PR 33 — Grade (add-on)
Curves, `.cube` LUTs, PR 11's exposure / contrast / saturation / white balance per piece,
before/after, and scopes (histogram, waveform, parade, vectorscope) with text readouts. One
kernel source compiled to HLSL, MSL and C++ so preview and export match.

### PR 34 — Audio (add-on)
Gain and fades per piece, a 5-band EQ, compressor / limiter, noise reduction with no model
(`afftdn`, de-hum), loudness normalise, meters, and one music bed with ducking.

### PR 35 — Voice isolation (add-on sub-pack), only if S3 says go
The model as its own signed download with its size shown, processed offline into the cache and
previewed when ready.

## Later, or never

- **Later (own plans):** proxies for 8K / long-GOP editing, titles and captions (the subtitle
  pipeline first, [08](08-video-editing.md)), a second video track (picture-in-picture),
  keyframed parameters, stem separation, HDR export (after the FP16 swapchain, D6), smart cut
  on untouched segments (D7), and a shared ONNX runtime between the AI pack and the Editor.
- **Not planned:** third-party plug-ins (OFX/VST3/CLAP), multicam, motion tracking, cloud
  anything, software H.264/HEVC encoders, and AI generation of any kind.

## Open decisions (owner)

1. **The add-on at all** (PRs 32–35): grading, audio and voice isolation are a second product
   surface with a real maintenance cost (every kernel ×3, every pane ×2). The base editor
   (PRs 30–31) was asked for; the add-on is proposed.
2. **Base or add-on for PR 31.** Several clips, zoom and dissolves add no payload, so they are
   planned as base. If the owner wants all multi-clip editing optional, they move behind the
   add-on with no design change.
3. **Voice sub-pack size ceiling** (300 MB proposed) and whether an ONNX runtime shared with the
   AI pack is worth add-on dependencies.
