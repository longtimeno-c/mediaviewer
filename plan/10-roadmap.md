# 10 — Roadmap

Re-cut against the decisions in [01-decisions.md](01-decisions.md): **v1 is a viewer with light
edits** (D4), on the **camera-dump format set** (D5), with a **C# WinUI 3 shell over a C++ core**
(D1) and **FFmpeg video on one present path** (D2).

Work is sliced into **independently runnable PRs, each with a verify line**. Do not start PR N+1
until N's verify holds **and PR 1's present-loop verify still holds** — that second clause is what
stops smoothness eroding one feature at a time. Week numbers are deliberately absent; they are
fiction.

## Milestone A — It draws (PR 1–3)

### PR 1 — Present lab
Win32 + DComp + Dear ImGui host, D3D11 device, flip-model waitable swapchain, per-monitor-v2 DPI,
clear to a colour, F3 frame-time overlay reading real present-to-present intervals. Job system,
result types, ETW tracepoints. **No WinUI yet** — this is the instrument, and it stays in the tree
as a debug harness.

Also in PR 1, because all three are unrecoverable later:

- the **licence decision**, framed as "do we need the Store?", with an owner
  ([11-licensing.md](11-licensing.md));
- the **C ABI**, specified and round-tripping end to end ([14-abi.md](14-abi.md)) — a header,
  `mv_guard`, one call, a `SafeHandle`, a completion drain. Roughly a day, and it decides whether
  PR 3-4 are pleasant or miserable;
- the **platform floor**: Windows 10 21H2, so `OVERLAPPED` rather than Win11-only `IoRing`.

**Verify:** presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle.

### PR 2 — Still decode + pan/zoom, in the lab
JPEG/PNG/BMP. Decode on the pool, immutable-texture upload from the worker, fit-to-window,
spring-based wheel-zoom-toward-cursor, drag-pan, `0`/`1`. **Linear FP16 working space, 8-bit sRGB
swapchain, ICC via LCMS from the start** (D6).

**Verify:** a 12 MP JPEG pans at refresh with zero decode on mouse move; dragging the window while
a 60 MP PNG loads stays smooth (TIFF is not in until PR 7); a tagged AdobeRGB JPEG renders
correctly and an untagged one is treated as sRGB, with **no tone-map applied to either** (D6).

### PR 3 — WinUI chrome, hosted in the native shell
**The canvas is not ported.** PR 1's Win32 window and D3D11 swapchain stay exactly as built; WinUI 3
chrome is hosted inside them as XAML content islands (`DesktopWindowXamlSource`), authored in C#,
talking to the core over the PR 1 ABI. Command bar and window chrome only — no panes yet.

This is the **D1 amendment**. The earlier plan had this PR re-implement presentation through
`SwapChainPanel` to discover whether it paced well enough, when the native-island fallback was
always the destination. One present path, owned by C++, never re-implemented.

**Verify:** **zero dropped frames while panning a cached image at the display's refresh rate**,
unchanged from PR 2 now that chrome is on screen — if hosting chrome costs frames, that is the bug.
Focus and tab traversal cross the island boundary correctly; a flyout opens over the canvas without
clipping. If islands prove unworkable, fall back to a WinUI app with `SwapChainPanel` and an
accepted composed frame *before* any panes are built on it.

## Milestone B — It's a viewer (PR 4–7)

*PR 5 is split into 5a/5b/5c. Numbering after it is unchanged.*

### PR 4 — Folder, filmstrip, thumbnails
Folder listing + sort, `ReadDirectoryChangesW` watcher, `ItemsRepeater` filmstrip, SQLite + on-disk
BC7 thumbnail cache keyed by `(path, mtime, size, spec)`, visible-first generation, directional
prefetch of ±2 decoded textures with generation-counter cancellation.

**Verify:** 2000 mixed JPEGs — filmstrip scrolls without a hitch, second folder visit has
near-instant thumbnails, arrow-key browse shows the next image in < 40 ms warm.

### PR 5a — Silent video into the same swapchain
The architectural win, isolated. FFmpeg demux + `avcodec` + D3D11VA behind `IVideoSource`, copied
out of the decoder pool into your own presentation ring, **NV12 and P010 sample paths**, YUV→RGB
plus **HDR (HLG/PQ) → SDR tone-mapping** in the shader
([05-video-pipeline.md](05-video-pipeline.md)). No audio, no clock — present each frame on its PTS
against QPC. Photos and videos share one folder navigation model.

**Verify:** 4K 10-bit HEVC and AV1 play at full rate with GPU video decode > 0 in Task Manager, **on
a clean VM with no Store codec packs**; an iPhone HLG clip looks correct rather than washed out; the
decoder never stalls waiting for a surface over a 10-minute play; photo → video → photo leaks no
textures.

### PR 5b — Audio and the clock
WASAPI render client, `swresample`, audio-master clock, drop/duplicate on drift, silent-clip QPC
fallback, device-change recovery, drift instrumentation in the F3 overlay.

**This is the 2-4 week job, and it is its own PR precisely so the escape hatch stays usable.** If it
overruns, dropping `IMFMediaEngine` in behind `IVideoSource` is a considered decision with a clean
boundary. Buried inside a larger PR, that same fallback becomes a panic merge under deadline
pressure.

**Verify:** A/V drift flat over 30 minutes, with the overlay to prove it; unplugging the audio
device mid-playback recovers without stopping video; a clip with no audio track plays at correct
speed.

### PR 5c — Transport
Dual-mode seek, frame step, speed 0.25x-4x (chained `atempo`), volume, track selection, resume
position, A-B loop, SMTC + media keys, scrub-preview thumbnails.

**Verify:** scrubbing feels instant; frame step lands on exact frames in both directions; media keys
and the OS overlay work; resume returns to the right position.

### PR 6 — Viewer completeness
Fullscreen, slideshow, fit/100 %/fill, animated GIF/APNG/WebP on the QPC frame clock, Recycle Bin
delete with confirm, drag-and-drop in, argv handling.

**Verify:** keyboard-only browse of a real folder; a file dropped into the folder appears without
restart; animation timing matches a browser.

### PR 7 — The camera-dump formats
TIFF, WebP, ICO, **HEIC/HEIF**, **AVIF** — bundled, OS-codec-probed first (D3, D5). **RAW** via
LibRaw with embedded-preview-as-first-pixel, then full decode into the same texture slot. Tiled
pyramid for images above ~64 MP. Broken-file corpus and per-decoder libFuzzer harnesses land in CI
here.

Crash reporting (Crashpad, out-of-process) lands here too — this is the PR where hostile real-world
files first meet your decoders ([13-updates-and-telemetry.md](13-updates-and-telemetry.md)).

**Verify:** iPhone HEIC opens on a clean VM with no Store packs; a CR2/NEF/ARW shows in preview
time comparable to a JPEG and the full decode replaces it without a visible pop; **original RAW
bytes unchanged**; nothing in the broken corpus crashes or hangs; a deliberately-corrupted RAW
produces a minidump containing **no path, filename, or pixel data**.

## Milestone C — It's useful (PR 8–11)

### PR 8 — Metadata (read)
Exiv2 + libavformat, unified property model, summary card + searchable full tree + per-stream video
inspector.

**Verify:** JPEG with EXIF, PNG with XMP, HEIC, and an MP4 all populate; missing metadata renders as
empty fields, never an error.

### PR 9 — Geometry edits + export
`EditStack`, GPU op chain at viewport resolution, rotate/flip/crop/straighten/resize. Export with a
metadata preservation policy. **Lossless JPEG** rotate and MCU-aligned crop where applicable.

**Verify:** crop + export a JPEG — on-disk dimensions and EXIF orientation match; reset returns the
original pixels exactly; lossless rotate produces a file with no recompression.

### PR 10 — Colour adjusts (the v1 set)
Exposure, contrast, saturation, temperature/tint as GPU shaders on the live preview. Histogram and
clipping warnings. Export bakes the stack at full resolution.

**Verify:** dragging a slider is shader-only with no re-decode, updating within one refresh interval
on a 45 MP RAW; export matches the preview within 8-bit rounding. **The adjust pane stays disabled
until LibRaw's full decode completes** — sliders never act on the embedded preview, because editing
pixels you will not export is the kind of wrong that erodes trust in every other number the app
shows ([07-photo-editing.md](07-photo-editing.md)).

### PR 11 — Metadata (write) — narrow on purpose
**Rating, orientation, and user comment only.** Atomic write via `ReplaceFileW`, snapshot before the
first write in a session, preserve maker notes, **XMP sidecar for RAW — never rewrite the original**.

Batch date-shift, copy-metadata, strip-on-share, and filename templating are **v1.1**. Do not build
a batch engine before the pane has been read in anger.

**Verify:** write-then-read round-trips preserve maker notes byte-for-byte across the corpus; a
process killed mid-write leaves the original intact.

## Milestone D — It trims video (PR 12–13)

### PR 12 — Two-path trim
In/out markers with the **keyframe grid drawn on the scrub bar**. Path 1: keyframe trim, stream
copy, instant. Path 2: full re-encode, frame-accurate, **explicitly labelled slower**, using
hardware encoders only ([11-licensing.md](11-licensing.md)). Cancellable job queue panel. A–B loop
preview of the proposed range. **Smart cut is v1.1** (D7).

**Verify:** keyframe trim of a 1 GB MP4 completes in seconds with proportional output size; the
re-encode path is frame-accurate; **the source file is never modified**; cancelling leaves no
partial output.

### PR 13 — Extract & remux
Lossless rotate (container matrix, no re-encode), split, remove-middle, MKV ↔ MP4 remux, frame →
PNG/JPEG, audio extract, clip → GIF/WebP with a two-pass palette.

**Verify:** each operation round-trips; lossless rotate does not re-encode.

## Milestone E — It ships (PR 14–15)

### PR 14 — Windows integration
File associations via `ProgId`/`OpenWithProgids` + a Default Apps deep link (never a silent
hijack). **One prompt after the first successful still open** — "Make MediaViewer your
default photo viewer?" — Yes opens Default Apps; No is remembered and never asked again.
Settings keeps the same action. Stills only, not video. Skip if already default.
`IThumbnailProvider` and property handler so **Explorer** gains your format support, jump
list, taskbar transport buttons, drag-out via `CFSTR_FILEDESCRIPTOR`, single-instance-with-tabs,
settings.

**The shell handlers run out-of-process (`DllSurrogate`), with timeouts and no state shared with the
app.** In-process, one malformed HEIC in a folder someone browses takes down Explorer
([09-build-and-test.md](09-build-and-test.md)). Treat this as the risky part of the PR, not the
boilerplate.

**Verify:** double-clicking a HEIC in Explorer opens the app and Explorer shows your thumbnail; a
**deliberately corrupted** HEIC in a browsed folder leaves Explorer running; uninstall removes every
association. First successful still open shows the default-app prompt; declining leaves existing
defaults unchanged and does not show it again; accepting opens Default Apps rather than writing
`UserChoice`; an install-and-quit with no image open never prompts.

### PR 15 — Package & ship
Signed per-user installer with Velopack auto-update (staged rollout, signed manifest, rollback),
code signing via Azure Trusted Signing, opt-in telemetry, `THIRD-PARTY.md` + About dialog +
per-release LGPL source offer. Store MSIX as a secondary channel if the licence permits.
Full design: [13-updates-and-telemetry.md](13-updates-and-telemetry.md).

**Two pieces move earlier** — see that doc's sequencing note: the **updater ships before the first
external build** (testers without an update path are stranded on whatever they installed), and
**crash reporting lands in PR 7** with the format long tail, which is exactly when other people's
RAW and HEIC files first hit decoders you've never tested against them.

**Verify:** clean VM → install → open a real camera dump → browse, crop, trim, export, with no
SmartScreen block and **no missing-codec dialog anywhere**.

---

## v1.1 and beyond — same architecture, more of it

| Area | Deferred work |
|---|---|
| Photo | Curves, per-channel HSL, colour grading, highlights/shadows, sharpen, NR, dehaze, vignette |
| Photo (local) | Radial/linear gradients, brush masks, healing/clone, red-eye |
| RAW | Full develop: highlight recovery, lens profiles, dual-illuminant WB, GPU demosaic |
| Video | **Smart cut** (D7), subtitle rendering beyond plain text, HDR passthrough |
| Formats | JPEG XL, OpenEXR, HDR, PSD, SVG, DDS, JPEG 2000, VVC (D5) |
| Display | HDR output + FP16 swapchain (D6), wide-gamut |
| Metadata | Batch date-shift, copy-metadata, strip-on-share, filename templating |
| Security | AppContainer decode process (D8) |
| Distribution | Store MSIX as a secondary channel, per-machine MSI for enterprise |
| Platform | ARM64, compare/side-by-side, keymap customization |

## Sequencing advice

- **Build the frame-time harness in PR 1, and re-run it in every PR.** Retrofitting performance is
  a rewrite; defending it from day one is nearly free. This is why every verify line is additive.
- **PR 3 is the real go/no-go on D1.** If `SwapChainPanel` can't hold the pacing gate, you find out
  before any chrome is built on it — that's the entire reason PR 3 exists as its own slice.
- **Formats (PR 7) before editors.** Coverage is what makes a viewer worth switching to; editing is
  what makes people stay.
- **Resist the NLE, and resist the develop module.** Both are real products; neither is this one.
- **PR 1-7 is the app you would use daily**, and that is a believable target on a tight calendar for
  one person. Milestone C is what makes it worth other people switching to.
- **Do not plan v1 as "3 months full-time."** That number was the original overconfidence surviving
  the scope cuts. PR 5b (the A/V clock), PR 7 (HEIC + RAW + tiles + fuzzing), and PR 14 (shell
  integration, out-of-process handlers, packaging) are each multi-week for one person on their own.
  Size the milestones, ship them in order, and let the calendar report itself rather than being
  promised up front.
