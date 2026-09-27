# 22 — The Editor add-on: an advanced video editor with colour grading

**Status: proposed 2026-09-26 (owner asked for the complete plan), from issue #40. Post-v1,
optional, Windows and macOS together (D9). Milestone K, PRs 32–47. Not a D-decision. It needs
the owner's sign-off, and it does not start before the base Video Editor (PRs 30–31,
[21](21-video-editor.md)) holds on both platforms.** This document supersedes the add-on
sections of [21](21-video-editor.md) (its "Add-on", "Host function table v2", "Voice isolation"
and "Install" sections); 21 keeps the base editor.

## 1. What it is

The base app edits a clip: cut, join, export ([21](21-video-editor.md)). **The Editor add-on
turns the Video Editor window into a finishing tool** for what a camera dump actually holds —
phone clips, action cameras, mirrorless log footage, drone clips — with:

- **Colour:** a colour-managed pipeline (camera log in, SDR or HDR out) and grading in the class
  of DaVinci Resolve's Color page — primaries and log wheels, curves, HSL qualifiers, power
  windows with tracking, a node graph, LUTs, shot matching, noise reduction and film looks — plus
  broadcast-grade scopes.
- **Editing:** a multi-track timeline in the class of Final Cut Pro — ripple / roll / slip /
  slide, J and L cuts, compound clips, keyframed transforms, speed ramps, transitions, titles and
  captions, stabilisation, proxies.
- **Audio:** a Fairlight-lite mixer — busses, EQ, dynamics, noise reduction, loudness to a
  target, ducking — and, as its own optional model download, voice isolation.
- **Delivery:** presets for the web, archive and intermediates, on hardware encoders, SDR and
  HDR, with a background render queue.

**Who it is for:** someone with a folder of footage who wants a finished film without leaving
the viewer, and wants the grade to be real (log footage looks right, skin tones hold, the export
matches the preview). **Who it is not for:** a post house. There is no collaboration, no
control-surface support, no OFX / VST ecosystem, no broadcast I/O and no Fusion-style node
compositor.

### Parity map (what "highly advanced" means here)

| Area | In the add-on | Later (own plan) | Never |
|---|---|---|---|
| Colour management | Per-clip input transform (auto from metadata), scene-linear + log working spaces, output transforms for Rec.709, sRGB, P3-D65, Rec.2100 PQ / HLG | ACES 2 output transforms, custom OCIO configs loaded by the user | — |
| Primaries | Lift / gamma / gain / offset wheels, log wheels (shadow / mid / highlight with ranges), contrast + pivot, temperature / tint, saturation, colour boost, shadows / highlights, white-balance picker | — | — |
| Curves | Custom (Y + RGB), hue vs hue / sat / lum, lum vs sat, sat vs sat | — | — |
| Secondaries | HSL / RGB / luma qualifier with matte finesse (denoise, clean black / white, blur, shrink / grow), power windows (circle, linear, polygon, Bézier, gradient) with softness, inside / outside | AI masks (subject / sky / face) as a model pack | — |
| Tracking | Point and planar tracker for windows, stabiliser (optical, and gyro from GoPro / DJI metadata) | Face / object tracking from a model pack | — |
| Node graph | Serial, parallel, layer mixer (blend modes, opacity), outside nodes, per-clip + group pre / post + timeline nodes | Splitter / combiner per channel | Fusion-style compositing |
| Looks | LUT import / export (.cube 1D / 3D), film grain, halation, glow, vignette, sharpen, spatial + temporal noise reduction | Film emulation packs | — |
| Grade management | Versions per clip, stills gallery with wipe, copy / paste / append grades, shot match (auto), grade groups | Remote grades | — |
| Scopes | Waveform (Y, RGB overlay), RGB parade, vectorscope (with skin line), histogram, CIE 1931 chromaticity, false colour, HDR nits waveform | — | — |
| Timeline | Tracks V1–V8 / A1–A16, magnetic *and* free placement, ripple / roll / slip / slide, blade, J / L cuts, markers, compound clips, nested sequences | Multicam | Collaboration |
| Motion | Transform (position, scale, rotation, anchor, crop, opacity) with keyframes and easing, blend modes | Motion paths with Bézier handles on canvas | 3D |
| Time | Constant speed, speed ramps, freeze frame, reverse, frame blending | Optical-flow retiming (S8) | — |
| Text | Titles (styled text, backgrounds, safe areas), lower thirds, captions (SRT / VTT import, export, burn-in) | Auto captions from the Voice pack's recogniser | Animated title templates marketplace |
| Transitions | Cross-dissolve, dip to colour, wipes, push / slide, audio crossfades | — | — |
| Audio | 48 kHz float engine, busses, per-clip gain / pan / fades, 4-band EQ + filters, compressor, limiter, gate, de-esser, spectral NR, de-hum, loudness (R128), ducking, meters | Voice isolation (model pack, §9), sync by waveform | VST3 / AU hosting |
| Media | Proxies (ProRes on Mac, HEVC 8-bit on Windows), render cache, relink, consolidate | — | Cloud |
| Delivery | H.264 / HEVC / AV1 (hardware), ProRes (Mac hardware), image sequences, audio-only, HDR10 / HLG metadata, captions sidecars, render queue | DNxHR, OpenEXR sequences | Software H.264 / HEVC encoders (licence) |

## 2. Rules, and how the add-on holds them

| Rule / decision | How it holds |
|---|---|
| 1 — nothing blocking on UI / render | The engine runs on its own threads; GPU work is recorded off the render thread and executed there under a time budget (§4.4); decode, analysis and models are workers or processes |
| 2 — the canvas is ours | The preview is the viewer's canvas in the Video Editor window ([21](21-video-editor.md)); the engine's output is a texture the host blits on the same swapchain. No second present path, no `MediaPlayerElement` / `AVPlayerView` |
| 4 — zero dropped frames panning a cached image | The viewer's gates still hold with the add-on loaded. The editor has its own gate: **real-time playback at the timeline rate with no dropped frame** for the reference timelines in §11 |
| 5 — never modify an original | Sources are read-only; projects, caches, proxies, analysis and renders are new files. Relink never rewrites media |
| 6 — nothing leaves the machine | No cloud render, no cloud AI, no telemetry of projects. Model downloads are plain GETs of fixed URLs. Crash reports carry node types and sizes, never paths, names, frames or grades |
| 7 — no required pack | The base app never requires the add-on, and the add-on never requires a model pack; each piece is optional and hides cleanly |
| D2 — FFmpeg + hardware decode on our device | Decode is the host's (§4.5), so every clip decodes the way the viewer already does, onto our device |
| D6 — linear FP16 working space; 8-bit sRGB swapchain in v1 | The engine works in FP16 / FP32 and scene-linear (§5). The preview still presents 8-bit sRGB until the FP16 swapchain lands; HDR grading shows a tone-mapped preview labelled as such, and HDR *output files* are exact (§5.5) |
| D9 — dual track, no Win32 / Cocoa below the hosts | The engine is portable C++; GPU work goes through the port (§4.3) with HLSL and MSL twins built in the same PR; chrome is WinUI and SwiftUI |
| Licensing ([11](11-licensing.md)) | FFmpeg LGPL, hardware encoders only, no software AAC, OS text engines, BSD / MIT / Apache libraries only (§13) |

## 3. Contradictions this plan has to resolve (read before building)

1. **"Not an NLE" / "Resist the NLE"** ([10](10-roadmap.md), CLAUDE.md). Narrowed on
   2026-09-26 to allow the base cut editor; this add-on goes further, into a real NLE with a
   grading suite. It stays out of the viewer (a separate download, absent means absent), but it
   is a second product. **Owner decision required** before PR 32.
2. **"The add-on never receives a device"** ([21](21-video-editor.md), [18](18-import.md)).
   Kept: the add-on gets **opaque handles** to textures, buffers and pipelines that the host
   creates on its device and executes for it (§4.3). It brings its own compiled shaders; the
   host validates, loads and runs them. This replaces 21's idea of putting the grade stage in
   the core: an engine this size in the base app would make every viewer install carry it.
3. **D6: 8-bit sRGB swapchain in v1.** Grading HDR on an SDR preview is only honest if the
   preview says so. §5.5 defines the tone-mapped HDR preview and makes the *file* exact. An HDR
   preview needs the FP16 swapchain (the D6 backlog item); when it lands the editor uses it.
4. **CLAUDE.md "Do not introduce … D3D12".** Kept. The port is D3D11 compute (cs_5_0 /
   SM 5.0 DXBC, compiled with FXC; DXIL would need D3D12) and Metal compute. Anything that needs
   D3D12 (mesh shaders, DirectML) is out.
5. **[08](08-video-editing.md) "x264/x265 fallback"**. Superseded long ago by
   [11](11-licensing.md): hardware / OS encoders only. A machine with no hardware encoder can
   still render ProRes (Mac) or a lossless intermediate, and says so.
6. **No third-party plug-ins.** Same position as 21: OFX, VST3, AU and CLAP are out. Every
   effect is first-party, signed with the add-on key.

## 4. Architecture

### 4.1 Pieces and where they run

```
┌ MediaViewer (host) ────────────────────────────────────────────────────────────────────┐
│ Video Editor window (base, plan/21)                                                      │
│  ├ preview = the viewer's canvas ── presents the engine's output texture (host blit)    │
│  └ add-on chrome: pages Edit / Colour / Audio / Deliver (WinUI | SwiftUI)                │
│                                                                                          │
│ host function table v2 ──► GPU port · decode service · audio service · text service ·    │
│                            files / cache · jobs · events · scheduling                    │
│                                   ▲                                                      │
│ mv_editor (add-on native) ────────┘ project model · render graph · colour pipeline ·     │
│   engine threads: scheduler, analysis, audio graph           GPU kernels (DXBC + metallib)│
└──────────────────────────────────────────────────────────────────────────────────────────┘
┌ MediaViewerRender (helper process, one per export) ─────────────────────────────────────┐
│ its own device + the same GPU port · loads mv_editor · renders frames · hardware encode   │
└──────────────────────────────────────────────────────────────────────────────────────────┘
┌ MediaViewerModel (helper process, model packs only) ────────────────────────────────────┐
│ ONNX Runtime (CPU / Core ML / vendor EP) · voice isolation, captions, masks               │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

- **`mv_editor`** (the add-on's native library, `mv_addon_get`, interfaces `mv.editor.1`):
  everything the add-on knows — project, timeline, render graph, colour science, audio graph,
  analysis, delivery presets. Portable C++20 under the core's rules (no exceptions on hot paths,
  no RTTI, POD across the line). It links nothing of the core; it links OCIO (§5.1) and its own
  small libraries (§13).
- **Kernels:** every GPU node is one HLSL source and one MSL source, compiled at build time to
  DXBC (FXC, cs_5_0) and a `.metallib`, plus a C++ reference implementation for the golden tests
  (§12). They ship inside the add-on and are hashed in its signed manifest like any other file.
- **Chrome:** `MediaViewer.Editor.Chrome.dll` (its own `AssemblyLoadContext`) and
  `Editor.bundle` (SwiftUI). They add pages to the Video Editor window; they never draw video
  (the canvas does) and never touch files (the engine does).
- **`MediaViewerRender`:** exports run here, out of process, as `MediaViewerClipJob` does for
  the base editor: a crash in a driver or a kernel ends one job, never the viewer. It hosts the
  same port over its own device, so preview and export run the same kernels.
- **`MediaViewerModel`:** only when a model pack is installed (§9). Inference never shares the
  render device and never runs in the viewer's process.

### 4.2 Threads (the five roles, extended)

| Thread | Owns | Never |
|---|---|---|
| UI (host) | chrome, edits to the project model (POD commands), posting snapshots | waits on the engine, the GPU or a file |
| Render (host) | presents; executes the engine's recorded GPU work under a per-frame budget (§4.4) | records engine work, decodes, blocks on a fence |
| Engine scheduler (add-on) | turns the playhead + project snapshot into per-frame graphs, requests decodes, records GPU work, manages caches | touches the swapchain |
| Workers (host pool) | decode requests, analysis (tracking, shot match, loudness), proxy and cache jobs | — |
| Audio (host device thread) | pulls the engine's mixed audio from a lock-free ring; the master clock | allocates, locks or decodes |

Edits are **commands on an immutable project snapshot**: the UI thread applies a command and
publishes a new snapshot with a bumped **generation** (the viewer's navigation pattern); the
scheduler drops work tagged with an older generation. Undo / redo is the command history.

### 4.3 The GPU port (host function table v2)

The add-on never sees `ID3D11Device`, `MTLDevice` or a swapchain. It sees handles:

```c
/* appended to mv_host_api; MV_ADDON_HOST_API 2 (struct_size keeps v1 add-ons loading) */
typedef uint64_t mv_gpu_tex;      /* opaque */
typedef uint64_t mv_gpu_buf;
typedef uint64_t mv_gpu_kernel;
typedef uint64_t mv_gpu_list;     /* a recorded command list */

mv_status gpu_caps(void* host, mv_gpu_caps* out);            /* vendor class, VRAM budget, fp16/fp32 image support */
mv_status gpu_tex_create(void* host, const mv_gpu_tex_desc*, mv_gpu_tex* out);   /* RGBA16F/32F, R16F, R8, 3D (LUTs) */
mv_status gpu_buf_create(void* host, uint32_t bytes, uint32_t usage, mv_gpu_buf* out);
mv_status gpu_upload(void* host, mv_gpu_tex|buf, const void* bytes, ...);       /* LUTs, curves, parameters */
mv_status gpu_kernel_load(void* host, const char* name, const void* dxbc_or_metallib, uint32_t size,
                          const mv_gpu_kernel_sig* signature, mv_gpu_kernel* out);  /* validated, cached */
mv_status gpu_list_begin(void* host, mv_gpu_list* out);       /* any engine thread */
mv_status gpu_dispatch(void* host, mv_gpu_list, mv_gpu_kernel, const mv_gpu_binding*, uint32_t n,
                       uint32_t gx, uint32_t gy, uint32_t gz);
mv_status gpu_list_submit(void* host, mv_gpu_list, uint64_t generation, uint64_t* out_fence);
int32_t   gpu_fence_done(void* host, uint64_t fence);         /* poll; never waits */
mv_status gpu_readback(void* host, mv_gpu_buf, ...);          /* scopes: async, one frame late */
mv_status preview_present(void* host, mv_gpu_tex frame, int64_t pts_ns, uint64_t generation);
```

- **Recording off the render thread.** Windows: a D3D11 *deferred context* per engine thread
  records the dispatches; the render thread runs `ExecuteCommandList` for submitted lists,
  oldest first, until the frame's budget is spent. Mac: an `MTLCommandBuffer` per list from a
  compute queue on the same `MTLDevice`, committed from the engine thread; the render thread
  only waits on a shared event it already signalled. The render thread **never waits on
  a fence**; a frame whose list has not finished is shown next vsync (and counted).
- **Kernels are data.** `gpu_kernel_load` checks the blob's hash against the signed manifest,
  checks the declared signature (bindings, formats, thread-group size) against the blob's
  reflection, and caches the pipeline. An add-on cannot load a kernel it did not ship.
- **Budgets.** `gpu_caps` gives the VRAM the add-on may use (a share of what the viewer leaves);
  textures come from a host pool with an LRU the host can trim. Going over is a failed create,
  never a device reset.
- **Presenting.** `preview_present` hands the host a finished RGBA16F frame in the timeline's
  output colour space. The host's blit maps it to the swapchain (8-bit sRGB in v1), letterboxes
  it and draws the overlays (safe areas, wipe line). This is the same blit and swapchain the
  viewer uses.

The rest of v2 (appended, same rules as v1, [18](18-import.md)):

- **Decode service:** `decode_open(path) → source`, `decode_request(source, pts, flags) →
  mv_gpu_tex planes (NV12 / P010 / RGBA) + colour tags`, async with completion events, hardware
  where the viewer uses it, the same seek policy as the player; `decode_audio(source, range) →
  float PCM`. The add-on never opens FFmpeg itself, so a codec the viewer plays is a codec the
  editor edits.
- **Audio service:** `audio_open(rate=48000, channels) → ring`; the host's device thread pulls
  from the ring and reports the device clock back (the master clock, as in the player).
- **Text service:** `text_layout(font, size, styled runs, box) → glyph atlas + quads`, using
  DirectWrite and Core Text in the hosts, so titles need no font engine in the add-on and look
  native on both platforms (the same font can shape slightly differently per OS; §12 allows it).
- **Files / cache:** `cache_dir`, `write_new_file`, `project_dir`, free-space queries, and
  `watch_volumes` (relink when a card comes back).
- **Jobs:** `submit_render(project snapshot, delivery settings) → job` in `MediaViewerRender`;
  `submit_model(pack, request) → job` in `MediaViewerModel`; progress and completion as events.

### 4.4 The render graph

Each frame is a **DAG built from the project snapshot for one timeline time**:

```
per video layer (bottom → top):
  decode(src, pts) → input transform (IDT) → [clip nodes: the clip's colour graph]
                   → transform / crop / speed-blend → layer effects
compose layers (blend modes, opacity, transitions) → [timeline nodes] → output transform (ODT)
→ preview_present  |  → encode (in MediaViewerRender)
```

- **Resolution independence.** Node parameters are in normalised frame space; windows and
  tracks are relative. A 4K timeline previews at ½ or ¼ resolution (the scheduler picks it from
  the frame budget and says so) and renders at full.
- **Precision.** Every intermediate is RGBA16F; accumulation-heavy nodes (NR, blurs, grain,
  qualifier mattes) use FP32 internally. Sources arrive as 8, 10 or 12-bit planes and are
  expanded in the IDT kernel; dither happens once, at encode.
- **Caching.** Each node output is keyed by a hash of (its inputs' keys, parameters, time where
  it matters). A **frame cache** (VRAM, then RAM, then disk for render-cached ranges) holds the
  expensive intermediates: with a heavy clip graph, changing only the timeline node re-runs
  only the timeline node. The cache is bounded (§10) and evicts LRU.
- **Smart render / render cache.** A range the preview cannot play in real time can be rendered
  in the background to a cached intermediate (ProRes 422 HQ on Mac, a lossless 10-bit
  intermediate on Windows) and played from there, marked on the timeline (red → blue bar, as
  Resolve and FCP do). Invalidated by any edit under it.
- **Determinism.** Same graph + same kernels → the same pixels on the same GPU class. Across GPU
  vendors and HLSL vs MSL: within the ΔE tolerance of §12. Preview and export use the same
  kernels; export runs at full resolution with no drops.

### 4.5 Playback

- **Master clock:** the audio device (WASAPI shared / Core Audio), as in the player; with no
  audio, QPC / mach time.
- **Scheduling:** the scheduler keeps `N` frames ahead (N from the frame cost measured over the
  last second, 3 to 8), requests decodes earlier still (hardware decode latency plus seek), and
  pre-rolls the next clip's decoder before a cut so a seam costs nothing.
- **When it cannot keep up:** first it lowers the preview resolution (full → ½ → ¼, labelled);
  then it drops frames *but keeps audio*, and shows a dropped-frame counter and the ranges worth
  render-caching. It never stalls the UI or the viewer's render thread.
- **Scrubbing** uses non-exact seeks while dragging and an exact seek on release (the viewer's
  rule), with thumbnails from the strip cache while the decode catches up.

### 4.6 Project model

- **Project:** sequences, a media pool (references to files; nothing is copied unless asked),
  a stills gallery, looks, settings. **Timeline:** frame rate (rational, fixed per sequence),
  resolution, working and output colour spaces, audio layout. **Tracks:** video V1–V8, audio
  A1–A16 (mono / stereo / 5.1), a title track, a caption track. **Items:** clips (source, in, out,
  speed, transform, effects, colour graph, audio chain), gaps, compound clips (a nested
  sequence), transitions, markers, keyframes (per parameter, with easing).
- **Time** is rational ([21](21-video-editor.md) §S5 rules): cuts are source PTS; a VFR clip
  is conformed by frame selection to the timeline rate, labelled "VFR → 30 fps".
- **File:** `<name>.mvproj`, a folder: `project.json` (versioned schema, human-diffable),
  `history/` (autosaved snapshots every 60 s and on every save, the last 50 kept), `stills/`,
  `looks/`. Media are referenced by path plus (size, mtime, BLAKE3 of the first and last MiB) so
  a moved file relinks by content. Caches live elsewhere (§10) and can always be thrown away.
- **Recovery:** the project is saved as a new file and renamed into place (the io replace port);
  a crash loses at most the last autosave interval.

## 5. The colour pipeline

### 5.1 Colour management

- **Engine:** OpenColorIO 2 (BSD-3) for transform *definitions* and their CPU reference; the
  add-on **bakes each transform chain into a 3D LUT plus a shaper (or analytic kernel where OCIO
  offers GPU shader text, translated at build time into our HLSL / MSL twins)**. At run time the
  GPU only sees our kernels and LUT textures.
- **Config:** a built-in config derived from the ACES 1.3 / 2.0 studio config (colour spaces,
  looks, displays) plus camera formats; users can pick another OCIO config file (later).
- **Per-clip input (IDT), chosen automatically from metadata and editable:**

  | Source | Detection | Input |
  |---|---|---|
  | Phone / consumer SDR | BT.709 / sRGB tags or untagged | Rec.709 (scene-referred, inverse of the camera OETF) |
  | iPhone HDR | HLG, `mdcv`/`clli` | Rec.2100 HLG |
  | HDR10 | PQ tags | Rec.2100 PQ |
  | Apple Log | QuickTime colour tags + Apple metadata | Apple Log / Rec.2020 |
  | Sony S-Log3 | `S-Log3` in XAVC metadata | S-Log3 / S-Gamut3(.Cine) |
  | Panasonic V-Log | metadata | V-Log / V-Gamut |
  | Canon C-Log2/3 | metadata | C-Log / Cinema Gamut |
  | Fujifilm F-Log/F-Log2 | metadata | F-Log / F-Gamut |
  | Nikon N-Log, DJI D-Log / D-Log M, GoPro Protune Flat | metadata where present, else a clip-level pick | the vendor's published curves |

  A clip whose tags and content disagree (a log clip tagged Rec.709) keeps the tag and offers
  the likely IDT in the inspector; it is never switched silently.
- **Working spaces:** compositing, transforms, blurs and NR in **scene-linear** (ACES AP1 /
  ACEScg primaries); grading nodes in **ACEScct** (log), so wheels and curves behave the same on
  every camera. Each node declares which it wants; the graph inserts the conversions.
- **Timeline / output (ODT):** Rec.709 gamma 2.4 (broadcast), sRGB / Rec.709 gamma 2.2 (web),
  P3-D65, Rec.2100 PQ (1000 nits), Rec.2100 HLG. The output transform includes tone and gamut
  mapping (ACES 2 output transforms where available; a parameterised filmic transform otherwise).

### 5.2 Grading tools (the Colour page)

Each is a GPU node in the clip's graph, with keyframes on every parameter:

- **Primaries:** lift / gamma / gain / offset colour wheels and master sliders; log wheels
  (shadow / midtone / highlight with range controls); contrast with pivot; temperature and tint
  (in the working space, not a channel gain); saturation and colour boost (vibrance); shadows,
  highlights, midtone detail; white and black point pickers; auto balance.
- **Curves:** custom (Y + R, G, B with soft clip), hue vs hue, hue vs saturation, hue vs
  luminance, luminance vs saturation, saturation vs saturation. Splines are monotone-cubic, baked
  to 1D textures (4096 entries) per change.
- **Qualifiers (secondaries):** HSL, RGB, luma and 3D (colour-picker volume) keys; matte
  finesse: pre-filter, clean black / white, black / white clip, blur, in / out ratio,
  shrink / grow, denoise; "highlight" view (the matte over grey). Output: a matte feeding the
  node's alpha.
- **Power windows:** circle, linear, polygon, Bézier, gradient; inner and outer softness;
  invert; combine with qualifiers (intersect / subtract). Drawn and edited **on the canvas** with
  the host's overlay layer (handles are ≥ 24 DIP).
- **Tracking:** a point tracker (pyramidal KLT on the GPU) and a planar tracker (homography from
  tracked features, RANSAC on a worker) drive window transforms; per-frame keyframes stored in
  the project; analysis runs in the background and can be refined by hand.
- **Node graph:** serial nodes, parallel mixers, layer mixers (blend modes, opacity, composite
  order), outside nodes (the inverse matte of a node), per-clip graphs plus **group pre / post**
  graphs and a **timeline** graph. Node labels, bypass per node and for the whole grade.
- **Looks and LUTs:** load `.cube` (1D, 3D up to 65³) at input, per node or output; export the
  grade as a 33³ / 65³ `.cube` (for another app or the camera's monitor). **Film look** node:
  halation, grain (resolution-aware, temporally varying, in log), gate weave off by default.
- **Detail:** spatial and **temporal** noise reduction (motion-compensated, 2–5 frames, GPU;
  the most expensive node, so it is render-cached by default), sharpening, soften / glow,
  vignette, chromatic-aberration fix.
- **Grade management:** versions per clip (A / B / C…), copy / paste / append grades, **stills**
  (grab a frame + grade into the gallery) with a split / wipe compare against the current clip,
  **shot match** (match a clip's primaries to a still: statistics in the working space, then a
  primaries + curve fit; always editable after), grade groups.
- **Views:** before / after (bypass), split screen (vertical, horizontal, mosaic of versions),
  wipe against a still, highlight (qualifier matte), false colour for exposure.

### 5.3 Scopes

A reduction kernel (compute, per frame at ¼ resolution, read back asynchronously one frame late,
never on the render thread's wait path) feeds: **waveform** (luma, RGB overlay, YCbCr), **RGB
parade**, **vectorscope** (75 % / 100 % targets, skin-tone line, zoom 2×), **histogram**, **CIE
1931 chromaticity** (with the output gamut and the source plotted), **HDR waveform in nits**
(PQ), **false colour** and **clipping / zebra** overlays. Scopes show the **output** signal by
default and can show the node or source. Every scope has a text readout (min, max, average,
clipped %) for accessibility and for tests.

### 5.4 Accuracy targets

- The CPU reference and both GPU twins agree to **ΔE2000 ≤ 0.5** on the test charts (§12).
- Round trip: IDT → working space → inverse → ODT on an identity grade reproduces the source
  within 1 code value (10-bit) for SDR Rec.709 and within ΔE2000 ≤ 1 for log formats.
- A Rec.709 clip with no grade renders **bit-exact** to a direct transcode (the "do no harm"
  test), after the encoder.

### 5.5 HDR

- **Grading HDR:** timeline output Rec.2100 PQ or HLG; the grade happens in the working space
  as usual; scopes show nits.
- **Preview in v1 is SDR:** the host's swapchain is 8-bit sRGB (D6). The preview applies an
  HDR → SDR preview tone map (the same one the viewer uses for iPhone HDR) and shows an
  "HDR — preview tone-mapped" badge; the **nits waveform** is the ground truth.
- **Files are exact:** PQ / HLG encodes carry the right transfer, primaries and matrix tags,
  plus `mdcv` / `clli` (measured MaxCLL / MaxFALL from a render pass).
- **When the FP16 swapchain lands** (D6 backlog, a host PR), the editor presents HDR directly
  on HDR displays and drops the badge.

## 6. The editing tools (the Edit page)

- **Timeline:** magnetic by default (clips close gaps; connected clips ride with their parent,
  like Final Cut) with a **free** mode per track (like Resolve). Snapping to cuts, markers and the
  playhead; zoom from the whole sequence to single frames; track heights; lock, mute, solo,
  target tracks.
- **Trimming:** ripple, roll, slip, slide, and the blade; **J and L cuts** (audio edits offset
  from video); precision trimmer (two-up view of outgoing / incoming frames); trim by numbers
  (`+5` frames).
- **Structure:** compound clips (collapse a selection into a nested clip; edit in place),
  multiple sequences per project, markers (with notes and colours), chapters for export.
- **Transforms:** position, scale, rotation, anchor, crop, flip, opacity, blend modes — keyframed
  with easing curves; on-canvas handles; dynamic zoom (Ken Burns) for stills.
- **Speed:** constant speed (5 %–1000 %), speed ramps with ease, freeze frame, reverse; frame
  blending in v1; optical-flow retiming after spike S8.
- **Transitions:** cross-dissolve, dip to colour, wipes (edge, clock, iris), push / slide, and
  audio crossfades (constant power / gain). Transitions are GPU nodes with handles; the timeline
  warns when a clip has no media for the handle.
- **Titles and captions:** text layers (font, size, tracking, leading, fill, stroke, shadow,
  background box), lower-third templates, safe-area guides; caption tracks (SRT / WebVTT import,
  per-caption timing, burn-in or sidecar export). Laid out by the host text service (§4.3).
- **Stabilisation:** optical (feature tracking + smoothing, crop to fit), or **gyro** from
  GoPro GPMF / DJI / iPhone metadata when present (exact and cheap; rolling-shutter aware).
- **Media pool:** the viewer's folder as a pool, bins, search, and clips from anywhere;
  "used in timeline" badges; proxies and optimized media (§10).

## 7. Audio (the Audio page)

- **Engine:** 48 kHz 32-bit float, a graph of clip → track → bus → master, processed in
  1024-sample blocks ahead of the device clock in a lock-free ring (the audio service, §4.3).
  Sample-accurate edits and fades; resampling per source (soxr-quality polyphase, our code or
  FFmpeg's swr).
- **Per clip:** gain, pan, fades (curves), channel mapping, keyframed volume, **audio
  enhancement chain**: high-pass, spectral noise reduction (learned noise profile from a
  selection), de-hum (50 / 60 Hz + harmonics), de-esser.
- **Per track / bus:** 4-band parametric EQ + shelves + filters, compressor, limiter, gate,
  delay, reverb (small room / hall, for matching), meters (peak, RMS, LUFS short-term /
  integrated, true peak).
- **Mix tools:** ducking (music under dialogue, side-chain from dialogue tracks), **loudness
  normalise** to a target (−14 LUFS web, −16 podcast, −23 EBU broadcast), sync clips by waveform
  (cross-correlation on a worker), roles / stems (dialogue, music, effects).
- **Voice isolation:** a model pack (§9), processed offline into the cache and played when
  ready. Never in the real-time path.

## 8. Delivery (the Deliver page)

- **Presets with units, never a unitless quality slider:** Web (H.264 1080p / 4K,
  16 / 45 Mb/s, AAC 256 kb/s via the OS encoder), Web HDR (HEVC 10-bit HLG or PQ),
  Archive (HEVC 10-bit 4:2:0, source rate), Intermediate (ProRes 422 HQ on Mac hardware; on
  Windows a lossless intermediate), Audio only (WAV / FLAC / AAC), Image sequence (PNG / TIFF
  16-bit), Social (vertical 9:16 with safe-area reframe).
- **Encoders:** the encode port ([21](21-video-editor.md) S1): NVENC / Quick Sync / AMF / MF on
  Windows, VideoToolbox on Mac; AV1 where the GPU has it. The render helper opens the first that
  accepts the format and labels the output with what ran.
- **Render queue:** multiple jobs, background priority (yields to playback, as Import does),
  pause, resume after crash (per-range progress in the job file), notification on finish.
- **Smart render:** ranges with no effects and matching codec parameters are stream-copied (the
  base editor's `keep_ranges` logic, per range).
- **Metadata:** creation date from the first clip, chapters from markers, captions as sidecars
  or embedded, HDR mastering metadata; location off by default ([06](06-metadata.md)).
- **Checks before render:** free space (estimate × 1.1), offline media, missing fonts, caption
  overlaps, loudness out of target — listed, not blocking unless the render would fail.

## 9. Model packs (optional, local)

Each is its own signed sub-pack of the add-on, with its size shown before download, run in
`MediaViewerModel` on ONNX Runtime (CPU, plus Core ML on Mac and a vendor EP on Windows where
licence-clean, as the AI pack does, [17](17-local-ai-search.md)).

| Pack | What | Candidates (licence checked per file in the spike) | Ceiling |
|---|---|---|---|
| Voice isolation | Speech enhancement: keep dialogue, remove wind / traffic / crowd / music | DeepFilterNet 2/3; RNNoise-family via FFmpeg `arnndn` for a no-download tier | 300 MB |
| Captions | Speech-to-text for auto captions | Shares the Voice add-on's recogniser ([19](19-voice.md)) when installed; else a Whisper-family model under a permissive licence | 1.5 GB |
| Masks | Subject / sky / face masks for the Colour page | A licence-clean segmentation model (SAM-class only if its weights allow redistribution) | 800 MB |
| Stems | Music / vocals / drums / bass separation | Demucs-class, only if weights and training data allow | 1 GB |

**Gate (as [17](17-local-ai-search.md)):** the manifest names a licence for every model file;
CI fails on non-commercial or research-only weights. Each pack is spiked (size, speed per
minute on CPU / GPU, quality on a local eval set that lives outside git) before its PR, and is
dropped if nothing licence-clean clears its bar.

## 10. Performance and resources

**Reference machines:** Mac — M1 (8 GB) floor, M3 Pro target; Windows — GTX 1660 / RX 6600 /
Iris Xe floor, RTX 3060-class target.

| Workload | Floor machine | Target machine |
|---|---|---|
| 1080p30 H.264, 3 clip nodes + scopes | real time, full res | real time, full res |
| 4K30 HEVC 10-bit log, IDT + 4 nodes + ODT | real time at ½ res | real time, full res |
| 4K60 HEVC, 2 video tracks + cross-dissolve | real time at ½ res | real time, full res |
| Temporal NR (3 frames) on 4K | render-cache | real time at ½ res |
| Export 4K30 HEVC, 4 nodes | ≥ 0.5× real time | ≥ 1.5× real time |

- **Memory:** VRAM budget = the host's share (§4.3), default 60 % of what the viewer leaves,
  hard cap 6 GB; RAM frame cache 2 GB default; disk render cache 20 GB default or 10 % of the
  volume, LRU, with a Clear button showing size (as 21's cache rules).
- **Proxies:** generated in the background (ProRes 422 Proxy on Mac hardware, HEVC 8-bit ½ res
  on Windows), switched per clip or globally; export always uses originals.
- **Startup:** the add-on loads lazily on the first Video Editor open; kernels compile / load
  from a pipeline cache (first-run cost measured in S2).
- **The viewer is unaffected:** with the add-on installed and the editor closed, both
  present-loop gates are unchanged (measured).

## 11. The Editor's own gates

1. **Real-time gate:** the reference timelines in §10 play for 60 s on each target machine with
   **zero dropped frames** at the stated resolution (frametime harness reading the editor's
   present stamps), on both platforms.
2. **Colour gate:** §5.4's ΔE and bit-exact tests pass for every kernel on both GPU backends.
3. **Parity gate:** the same project renders on Windows and Mac within PSNR ≥ 45 dB and
   ΔE2000 ≤ 1 per frame (encoders differ, so files never hash-match).
4. **Do-no-harm gate:** an unedited Rec.709 clip exported with "Same as source" is visually
   lossless (VMAF ≥ 98 with a local libvmaf test tool, never shipped) and keeps its tags.
5. **Viewer gate:** both present-loop gates hold with the add-on installed and while it renders
   in the background.

## 12. Testing

- **Kernels:** golden frames per node from the C++ reference; HLSL (WARP / a real GPU on the CI
  runner where one exists) and MSL must match within tolerance. Colour charts: ColorChecker
  24 / SG in each camera log format, generated synthetically from published curves, plus a
  small licence-clean real-footage set outside git.
- **Colour science:** every IDT / ODT against OCIO's CPU processor at 4096 sample points.
- **Timeline:** property tests on the edit operations (ripple / roll / slip / slide never
  change the total media used except as asked; undo is exact), a project fuzzer on
  `project.json`, VFR / long / mixed-audio fixtures ([21](21-video-editor.md) S5).
- **Audio:** sample-exact renders of mixes against a reference; loudness meter against the EBU
  test set; A/V drift ≤ 1 frame over 2 h.
- **Resilience:** kill the render helper mid-job (resume works, nothing partial published);
  unplug media mid-playback (the clip goes offline, the app does not); GPU device removed
  (Windows TDR): the engine rebuilds its resources and the preview recovers.
- **Accessibility:** VoiceOver / Narrator walk every page; every control keyboard-reachable;
  scopes and meters have text readouts.
- **Performance:** the §11 gates in CI where hardware allows, nightly on the owner's machines
  otherwise, with p99 regression > 10 % failing the run (the viewer's rule).

## 13. Dependencies and licences

All in the add-on, none in the base app.

| Library | Licence | Use |
|---|---|---|
| OpenColorIO 2 | BSD-3 | Transform definitions, CPU reference, LUT baking |
| Imath / pystring / expat / yaml-cpp (OCIO's deps) | BSD / MIT | via OCIO |
| ONNX Runtime | MIT | Model packs only, in `MediaViewerModel` |
| GPMF parser (GoPro) | MIT / Apache-2.0 (check) | Gyro stabilisation |
| FFmpeg (the host's) | LGPL | Decode / encode through the host services only |
| DirectWrite / Core Text | OS | Titles |

Nothing non-commercial, no software H.264 / HEVC / AAC encoders. The app moved to
GPL-3.0-or-later on 2026-09-25 (plan/12-decision-log.md), so GPL-3 code combines cleanly;
GPL-2-only code still does not. The manifest names a licence per file and CI checks it,
as for the AI pack.

## 14. The UI

- **One window, four pages** — **Edit**, **Colour**, **Audio**, **Deliver** — as tabs across the
  Video Editor window's top, sharing the preview (the canvas) and the timeline. The base editor's
  single page stays as it is when the add-on is absent.
- **Colour page layout:** preview (with split / wipe), node graph, clip thumbnails for the
  timeline, the tools (wheels, curves, qualifier, windows, tracker, blur, key, sizing) as
  palettes, scopes docked or in their own window, the stills gallery.
- **Grading environment:** a neutral mid-grey UI option for the Colour page (with the canvas
  surround at 18 % grey), since the chrome's colours bias judgement.
- **Keyboard:** complete, with a Resolve-like and a Final Cut-like preset plus the app's own; the
  command table and `?` list them ([16](16-commands.md)). Wheels and sliders take arrow keys and
  typed values.
- **Accessibility:** every node, wheel and curve point is reachable and nameable; values are
  readable; colour is never the only signal (markers use shape too).
- **Mouse and trackpad:** wheels drag like Resolve's (ring = offset, centre = master), trackpad
  scroll zooms the timeline, pinch zooms the preview.

## 15. Install, update, remove

Through the add-on mechanism ([18](18-import.md)), nothing new to trust:

- **Pieces** (each with its size, installable separately): **Editor** (engine, kernels, chrome,
  OCIO; target ≤ 120 MB), **Voice isolation**, **Captions**, **Masks**, **Stems** (§9). A
  manifest `requires` field ties a piece to a compatible Editor version (added in PR 32, shared
  with the AI pack's sub-packs).
- **Signed and verified** before install and at every load (Ed25519 manifest, per-file SHA-256,
  Authenticode / Developer ID + notarization, library validation on Mac).
- **Updates** arrive with the app's stable feed; a host-API mismatch shows "Editor needs an
  update" and falls back to the base editor. Offline sideloading works.
- **Remove** cancels its jobs (partials swept), unloads, deletes the pieces and offers to delete
  caches (with their size); projects are the user's documents and are never deleted.
- **Absent means absent:** the base install is byte-identical; the Video Editor shows only its
  base page.

## 16. Roadmap (both platforms each; verify line per platform; viewer gates every PR)

Milestone K. Numbers continue from [21](21-video-editor.md) (30–31 base editor); 21's PRs
32–35 are replaced by these.

| PR | Slice | Verify (both platforms) |
|---|---|---|
| **32** | **Spikes S2–S8 + the add-on shell.** GPU port (§4.3) over D3D11 deferred contexts and Metal compute queues; kernel loading and validation; `MediaViewerRender`; host table v2; the add-on's install / pages skeleton | S-results recorded; a trivial kernel graph (IDT → ODT) plays a 4K clip in real time in the editor on the floor machines; a tampered kernel is refused |
| 33 | **Colour management.** OCIO-baked IDTs / ODTs, auto IDT from metadata, working spaces, output spaces, the HDR preview badge, scopes v1 (waveform, parade, vectorscope, histogram) | §5.4 round-trips; every camera IDT against OCIO; a Rec.709 clip exports bit-exact through an identity pipeline |
| 34 | **Primaries and curves.** Wheels, log wheels, contrast / pivot, temperature / tint, saturation, all curves, before / after, copy / paste grade, versions | Golden frames per tool, HLSL = MSL = C++ within ΔE 0.5; 4K log + 4 nodes real time (target machines) |
| 35 | **Secondaries.** Qualifiers with matte finesse, power windows on canvas, the point tracker | A tracked window holds on a panning fixture within 2 px; mattes match the reference |
| 36 | **Node graph and looks.** Serial / parallel / layer / outside nodes, group and timeline graphs, LUT import / export, film look, stills + shot match | A graph exported as a 65³ LUT reproduces the grade within ΔE 1; shot match lands within ΔE 3 of the still on the test set |
| 37 | **Multi-track timeline.** V1–V8 / A1–A16, magnetic + free, ripple / roll / slip / slide, J / L cuts, compound clips, markers | Property tests; keyboard-only trim session; 2 video tracks + dissolve 4K60 real time (target) |
| 38 | **Motion, transitions, titles, captions.** Keyframed transforms with easing, blend modes, transitions, titles via the host text service, caption tracks | Titles render the same layout on both OSes within tolerance; captions round-trip SRT / VTT |
| 39 | **Time and stabilisation.** Speed ramps, freeze, reverse, frame blending, optical + gyro stabilisation | A gyro-stabilised GoPro fixture's residual motion ≤ 1 px; ramps export frame-accurate |
| 40 | **Detail.** Spatial + temporal NR, sharpen, glow, render cache (smart render bars) | NR PSNR gain on a noisy fixture; a render-cached range plays with zero drops |
| 41 | **Audio engine and mixer.** Busses, EQ, dynamics, meters, loudness normalise, ducking, sync by waveform | A mix renders sample-exact to the reference; −14 LUFS target lands ± 0.5 |
| 42 | **Audio repair.** Spectral NR, de-hum, de-esser; the Voice isolation pack (if its spike passes) | NR / isolation DNSMOS gains on the eval set; nothing fetched but the pack URL |
| 43 | **Delivery.** Presets, render queue with resume, HDR10 / HLG metadata, smart render per range, pre-render checks | Parity gate; do-no-harm gate; MaxCLL / MaxFALL measured correctly |
| 44 | **Proxies and media management.** Proxies, relink by content, consolidate, offline handling | A 2 h multi-cam-dump project relinks after a folder move with no user action |
| 45 | **Captions pack.** Auto captions from speech (shares the Voice recogniser where installed) | WER on the local eval set under the spike's bar; captions editable before burn-in |
| 46 | **Masks pack** (if its spike passes). Subject / sky / face masks as window sources | Mask IoU on the eval set; masks track across a clip |
| 47 | **Optical-flow retiming** (if S8 passes). Smooth slow motion | Visual artefact score on the fixture set under the spike's bar |

**Sequencing:** 32 → 33 → 34 are the spine (nothing grades until colour management is right).
35–36 finish the Colour page; 37–39 the Edit page; 40–43 detail, audio and delivery; 44–47 in
any order. A slice is done only when both platforms hold its verify line and the viewer's gates.

### Spikes (PR 32)

| # | Question | Go line |
|---|---|---|
| S1 | Hardware encoder coverage | **Done on the Mac** ([21](21-video-editor.md)): HEVC 4K60 10-bit at 1.1×, ProRes 7–60×. Windows owed |
| S2 | GPU port overhead: deferred-context / command-buffer recording + budgeted execution vs direct | ≤ 10 % over direct dispatch for a 6-node 4K graph; no render-thread wait |
| S3 | Model packs: sizes, licences, speeds, quality | Per pack, §9 |
| S4 | Host table v2 against a stub add-on, with Import still loading on v1 | Both load; v1 add-on unaffected |
| S5 | VFR, long clips, mixed audio in the graph | A/V drift ≤ 1 frame / 2 h; memory flat |
| S6 | OCIO baking accuracy (LUT size / shaper choices per transform) | ΔE2000 ≤ 0.5 vs OCIO CPU for every IDT / ODT |
| S7 | Host text service layout parity (DirectWrite vs Core Text) | Glyph positions within 1 px at 1080p for the bundled fonts |
| S8 | Optical flow: OS (Vision `VNGenerateOpticalFlowRequest`, NVIDIA / Intel OF where licence-clean) vs our GPU KLT-dense | Artefact score and speed; else PR 47 is dropped |

## 17. Risks

| Risk | Mitigation |
|---|---|
| Scope: this is several products' worth of work | The spine (32–34) is useful on its own; every later slice is optional and independently shippable; the owner can stop after any slice |
| Twin kernels drift (HLSL vs MSL) | One source per node with shared parameter structs, a C++ reference, and golden tests on both backends in the same PR |
| D3D11 deferred contexts are slow on some drivers | S2 measures; the fallback is recording on the render thread's budget from pre-built parameter blocks (still never waiting) |
| Colour correctness on cheap panels | The app cannot fix a bad display; the preview says which output transform it uses, and the scopes are the truth |
| Log metadata missing or wrong | Auto IDT is a suggestion with the evidence shown; the user's pick is sticky per clip and per camera |
| Model licences | Per-file licence gate in CI; a pack with no clean candidate is not shipped |
| VRAM pressure beside the viewer | Host-owned pools with budgets; the viewer's own textures always win |

## 18. Open decisions (owner)

1. **Approve Milestone K** (the add-on as a real NLE with a grading suite), given
   "Resist the NLE".
2. **Which pieces are paid for first:** the spine (32–34) is required; after it, Colour-first
   (35–36) or Edit-first (37–39)?
3. **Model packs:** which of Voice isolation / Captions / Masks / Stems to pursue, and their
   size ceilings.
4. **Keyboard presets:** ship Resolve-like and FCP-like maps (key choices are not copyrightable,
   but names and layouts in docs should be our own), or only the app's map.
5. **Windows ProRes:** no hardware ProRes encoder exists on Windows; ship FFmpeg's `prores_ks`
   (LGPL, software) for intermediates, or keep a lossless intermediate only? ProRes is Apple's
   format; using an unlicensed encoder is a legal question, not a technical one.
