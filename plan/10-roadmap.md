# 10 — Roadmap

**Release cut (2026-09-14): finish PR 7, then package and ship the viewer in PR 8.**
v1 is the Windows viewer delivered by PRs 1–7, on the camera-dump format set (D5), with
C# WinUI 3 chrome over a C++ core (D1) and FFmpeg video on one present path (D2).
Metadata panes/writes, photo editing/export, video trimming, and additional Windows
integration ship in future updates; they do not block v1 (D4/D7 amended in
[01-decisions.md](01-decisions.md), rationale in [12](12-decision-log.md)).

**Numbering:** former PR 15 becomes PR 8; former PRs 8–14 become PRs 9–15 respectively.
PRs 1–7 (including 5a/5b/5c) and Mac PRs 16–20 keep their numbers. Historical decision-log
entries retain their original numbers. Milestone C is now the Windows release;
D/E are later feature updates and F remains the Mac host (**D9**).

**Dual-track from PR 9 (2026-09-24):** Windows PRs 1–8 and Mac PRs 16–20 are in the tree.
From PR 9 on, **every PR lands on Windows and macOS together**: one shared core change,
a WinUI half and a SwiftUI half, and a verify line on each platform. A PR is done only
when both halves hold. See [Dual-track updates](#dual-track-updates--prs-915-on-windows-and-macos-together-2026-09-24)
and D9 in [01-decisions.md](01-decisions.md). PR 26 (Ingest) is planned as a separate lane
after PR 9.

Work is sliced into **independently runnable PRs, each with a verify line**. Do not start PR N+1
until N's verify holds **and PR 1's present-loop verify still holds** (from PR 9, on both
platforms, plus PR 16's Metal gate on Mac). That second clause is what
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
Folder listing + sort (**name, mtime, size, type** — EXIF date-taken waits for PR 9),
`ReadDirectoryChangesW` watcher (portable `io/dir.h`, Windows
impl in `io/dir_win.cpp`), second XAML island with an `ItemsRepeater` filmstrip,
SQLite + on-disk **JPEG-512** thumbnail cache keyed by `(path, mtime, size, spec)`
with spec `jpg512.1`, visible-first generation, directional prefetch of ±2 decoded
textures into a five-slot GPU LRU with generation-counter cancellation.

The **gallery** (`G`) is a third island: a full-client thumbnail grid between the command
bar and the client bottom, over the same listing, the same `Items`, and the same thumbnail
cache as the filmstrip. Opening a single image lists its folder the same way a folder open
does — arrows and the gallery work — but the filmstrip is a preference per open mode
(`Settings`, persisted to `%LocalAppData%\MediaViewer\settings.ini`), defaulting to on for
a folder open and off for a single image.

PR 3's island is a **top strip**. The filmstrip is a **bottom strip** on the same
HWND — not a full-client island, not `SwapChainPanel`, not thumbs blitted onto the
photo swapchain. Completions are drained by C# once the island is attached
([12](12-decision-log.md) 2026-09-07, [14](14-abi.md)). On-disk BC7 waits until a
thumb has to be GPU-resident; DirectXTex is not a PR 4 dependency.

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
position, A-B loop, SMTC + media keys, scrub-preview thumbnails. Bindings in
[16-commands.md](16-commands.md): Space play/pause on a clip, `J` `K` `L`, `,` `.`.

**Verify:** scrubbing feels instant; frame step lands on exact frames in both directions; media keys
and the OS overlay work; resume returns to the right position.

### PR 6 — Viewer completeness
Fullscreen, slideshow **as a mode** (no transition pass), fit/100 %/fill, animated GIF/APNG/WebP
on the QPC frame clock, Recycle Bin delete with confirm, drag-and-drop in **and out**, argv
handling.

This is also the PR that makes the app **keyboard-complete for browse**. One key router, a
default map, `?` overlay, marks, copy-to / move-to (`F7`/`F8`), status,
typeahead, sticky zoom, companion hiding, loupe, hold-previous, display-referred clipping
blinkies, pixel grid, canvas background/checkerboard, always-on-top, fullscreen chrome
hide. Folder tree as a **third island, left, hidden by default** — slip to PR 9 if this
slice overruns, but `chrome_left_px` and the command id still land here. (2026-09-13: the
tree slipped to the metadata slice, now PR 9, and companion hiding to PR 7 — [12-decision-log.md](12-decision-log.md).)
Space becomes next-image (play/pause on video/animation); the lab sweep does not ship.
Full spec: [16-commands.md](16-commands.md). **Settings remaps the live table in this PR**
(`Ctrl+,`); JSON import/export and named layouts stay v1.1.

**Verify:** keyboard-only browse of a real folder — open, next/prev, zoom/fit/100 %, mark,
copy-to a destination, delete to Recycle Bin, fullscreen, slideshow start/stop — without
the mouse, with `?` listing those bindings; a file dropped into the folder appears without
restart; animation timing matches a browser; PR 1's present-loop still holds.

### PR 7 — The camera-dump formats
TIFF, WebP, ICO, **HEIC/HEIF**, **AVIF** — bundled, OS-codec-probed first (D3, D5). **RAW** via
LibRaw with embedded-preview-as-first-pixel, then full decode into the same texture slot. Tiled
pyramid for images above ~64 MP. **RAW+JPEG pairing** and **Live Photo pairing** land here
(one filmstrip stop, JPEG/still as first pixel — [04-image-pipeline.md](04-image-pipeline.md)).
Broken-file corpus and per-decoder libFuzzer harnesses land in CI
here.

Crash reporting (Crashpad, out-of-process) lands here too — this is the PR where hostile real-world
files first meet your decoders ([13-updates-and-telemetry.md](13-updates-and-telemetry.md)).

**Verify:** iPhone HEIC opens on a clean VM with no Store packs; a CR2/NEF/ARW shows in preview
time comparable to a JPEG and the full decode replaces it without a visible pop; **original RAW
bytes unchanged**; a RAW+JPEG pair is **one** filmstrip entry and one arrow-key stop; an
iPhone Live Photo is one entry and `;` plays the motion; nothing in the broken corpus
crashes or hangs; a deliberately-corrupted RAW produces a minidump containing **no path,
filename, or pixel data**.

## Milestone C — It ships (PR 8)

### PR 8 — Package & ship
Ship the feature set completed through PR 7. Metadata panes, edit/export, trim,
the folder tree, and additional Explorer integration are future updates.
PR 7 and all inherited viewer/present-loop verify gates must pass before release.

**Identity moves here from the former Windows integration slice:** one `.ico`
(16 / 20 / 24 / 32 / 40 / 48 / 64 / 256) for the window, taskbar, wizard, Start Menu,
and shortcuts, plus its PNG in About. PR 15 later reuses it for file associations.
The recorded UI-thread settings-write follow-up also belongs to PR 8 release hardening
([12](12-decision-log.md), 2026-09-13/14).

A **short first-install wizard** (Inno Setup) that lays down a **per-user** Velopack tree
under `%LocalAppData%\MediaViewer`, then **Velopack** for every later update (staged
rollout, signed manifest, rollback). Azure Trusted Signing on the wizard, the binaries,
and the update manifest. About dialog + `THIRD-PARTY.md` + per-release LGPL source offer.
Store MSIX is **not** a channel — the app is GPL-2.0-or-later ([11](11-licensing.md)).
Full design, including the wizard pages: [13-updates-and-telemetry.md](13-updates-and-telemetry.md).

The wizard is the one-time download-and-setup. Updates never re-open it. It does **not**
ask to become the default photo viewer (that is PR 15's in-app prompt) and it does **not**
ask for telemetry (that is the first-run screen in the app). Finish page: Launch,
[GitHub](https://github.com/longtimeno-c/mediaviewer), Licence.

**Release sequencing** — see that doc's sequencing note: the **updater ships in PR 8
before the first external build** (testers without an update path are stranded on whatever they installed), and
**crash reporting lands in PR 7** with the format long tail, which is exactly when other people's
RAW and HEIC files first hit decoders you've never tested against them.

**Verify:** clean VM → run the wizard (no UAC) → Start Menu shortcut shows the app icon →
Launch from the finish page → open a real camera dump → browse photos, play video with
audio and transport, pan/zoom, fullscreen, and slideshow using the PR 1–7 feature set,
with no SmartScreen block and **no missing-codec dialog anywhere**. The running window
and taskbar use the same app icon. PR 7 format/pairing checks and PR 1's present-loop
verify still hold for the installed build. The finish page's GitHub
link opens the repo. An update downloads and stages without showing the wizard. Uninstall
from Apps & features removes the shortcuts and install directory. PR 8 creates no
PR 15 associations or handlers; once those arrive, uninstall must remove them too.
Exercise update signature rejection and rollback, and confirm telemetry stays off
unless explicitly enabled.

## Dual-track updates — PRs 9–15 on Windows and macOS together (2026-09-24)

**Owner's call, 2026-09-24 ([12](12-decision-log.md), D9 amended in
[01-decisions.md](01-decisions.md)).** Windows v1 is packaged (PR 8) and the Mac host
exists (PR 16–20). From PR 9 on, **each PR lands on both platforms**. These are no
longer "future Windows updates" with a Mac catch-up later. There is one PR number, one
design and one shared core change. It has two host halves and a verify line for each
platform.

How a dual-track PR is shaped:

- **Shared core, once.** `meta/`, `edit/`, `codec/`, `image/`, `canvas/`, `core/` and the
  C ABI change a single time and build on both targets. A core change that only compiles
  on one OS is a D9 leak. Fix it before either host half lands.
- **Two host halves.** Windows: C# WinUI 3 chrome, `shell/`, `*_win.cpp` backends, HLSL.
  macOS: SwiftUI in the AppKit window, `*_mac.cpp`/`*_mac.mm` backends, MSL. **Chrome is
  written twice (D1).** It is never shared by moving it into C++, and never through Qt,
  Flutter, MAUI or Catalyst.
- **Shader twins in the same PR.** Every new HLSL kernel gets its line-for-line MSL twin
  in the same PR, not in a later catch-up.
- **Same command ids.** New rows go into `command_table.cpp` once. Each host adds its own
  default binding ([16-commands.md](16-commands.md)): `⌘` for `Ctrl`, and no numpad
  assumption on Mac laptops.
- **Two verify lines, both required.** The PR's shared verify runs on both platforms,
  plus each platform's own additions. **PR N is done only when both halves hold.** Do
  not start PR N+1 on either platform until then. That keeps the two apps at the same
  PR instead of letting one run ahead.
- **Two present-loop gates, inherited every PR.** PR 1's gate on Windows (DXGI, `frametime`)
  and PR 16's gate on Mac (Metal / display link, `frametime` on Darwin). Neither is
  evidence for the other.
- **One host half may lag inside a PR, but only in its own branch.** Merge a PR when both
  halves are green. A Windows-only or Mac-only merge of a PR 9–15 feature is a
  partial release, and D9 does not allow one any more.

**Entry state for PR 9** (the owner confirmed the basic Mac setup is complete on
2026-09-24). These items were recorded as owed in [12](12-decision-log.md). They stay
tracked, but they do not block PR 9 from starting:

- PR 19: the real-iPhone HLG check (only a synthetic HLG clip was checked).
- PR 20: Quick Look precedence over Finder's own generator; state carried across an
  update restart (zoom, pan, clip position); rollback after two failed starts.
- **Crashpad on Mac.** This is still the owner's call: either PR 17 still owes it or PR 20
  takes it. It must be settled before the first stable Mac release that includes PR 9.
  Mac now takes new decoders (Exiv2, libavformat metadata) in the same PR as Windows.

No release version is promised for any slice. Each merged PR may ship to both
platforms through the two-platform release flow (`RELEASING.md`,
[12](12-decision-log.md) 2026-09-23).

## Milestone D — Viewer and editing updates (PR 9–12, both platforms)

### PR 9 — Metadata (read)
**Shared:** Exiv2 + libavformat, one unified property model, per-stream video inspection,
AF-point quads from maker notes, sort-by-date-taken in the folder model, and the
eyedropper sample (a canvas-side read of the displayed texture). The info-overlay fill
(the exposure triangle) is computed in the core. Folder-tree *data* (a directory
enumeration that follows the watcher) is shared.

**Windows:** WinUI summary card + searchable full tree + video stream inspector. Folder
tree as the third island (left, hidden by default), using the existing command id and the
`chrome_left_px` inset deferred from PR 6. `Ctrl+Shift+E` stops beeping.

**macOS:** the SwiftUI metadata pane with the same summary card, tree and inspector. Folder
tree as a SwiftUI sidebar in the AppKit window, over the same enumeration. The canvas
inset is published through the same input snapshot field. Commands that the 2026-09-23
Mac key-routing entry hid from the remap list (folder tree, go-to) come back as they gain
an effect.

`I` focuses the pane on both ([16-commands.md](16-commands.md)).

**Verify (both platforms):** JPEG with EXIF, PNG with XMP, HEIC, a CR2/NEF/ARW and an MP4
all populate; missing metadata renders as empty fields, never an error; toggling AF points
and the info overlay does not re-read the file; sort by date taken orders a mixed folder
identically on both platforms. The folder tree opens from the keyboard and navigates without
the mouse. The present-loop gate for each platform still holds with the pane open.

### PR 10 — Geometry edits + export
**Shared:** `EditStack`, rotate/flip/crop/straighten/resize op chain, export with a
metadata preservation policy, **lossless JPEG** rotate and MCU-aligned crop (libjpeg-turbo
`transupp`, platform-neutral). Atomic output (write a new file, never touch the original)
goes through the `io` replace port: `io/replace_win.cpp` (`ReplaceFileW`) and
`io/replace_mac.cpp` (`rename`/`renamex_np` in the same directory after `F_FULLFSYNC`).

**Windows:** crop mode and export dialog in WinUI; HLSL geometry kernels.

**macOS:** SwiftUI crop mode and export sheet; **MSL twins** of the geometry kernels in
this PR.

`[` `]` from the viewer invoke lossless rotate without opening the adjust pane; `H`/`V`
flip; crop is a mode (`Enter` commit, `Esc` cancel).

**Verify (both platforms):** crop + export a JPEG, and the on-disk dimensions and EXIF
orientation match; reset returns the original pixels exactly; lossless rotate produces a
file with no recompression; keyboard-only rotate of a JPEG in the viewer writes that file.
The same crop on the same JPEG exports **byte-identical** on Windows and Mac: the op graph
is shared, so a difference is a bug.

### PR 11 — Colour adjusts (first editing set)
**Shared:** the exposure/contrast/saturation/temperature/tint math, histogram and clipping
reduction, and full-resolution export bake. The working space is linear FP16 (D6).

**Windows:** HLSL shaders on the live preview; WinUI adjust pane.

**macOS:** MSL twins of every adjust kernel, **in this PR**; SwiftUI adjust pane.

Viewer `C` blinkies become accurate on RAW once the full decode exists. `E` focuses the
adjust pane.

**Verify (both platforms):** dragging a slider is shader-only with no re-decode, updating
within one refresh interval on a 45 MP RAW; export matches the preview within 8-bit
rounding. **The adjust pane stays disabled until LibRaw's full decode completes**
([07-photo-editing.md](07-photo-editing.md)). A fixed slider set exported from the same
source on both platforms matches within 8-bit rounding. HLSL and MSL twins that disagree
fail the PR.

### PR 12 — Metadata (write) — narrow on purpose
**Rating, orientation, and user comment only.** **Shared:** the Exiv2 writer, a snapshot
before the first write in a session, maker notes preserved, and an **XMP sidecar for RAW
that never rewrites the original**. The atomic replace goes through the PR 10 `io` port
(`ReplaceFileW` on Windows; a same-directory temp file, `F_FULLFSYNC` and `rename` on Mac).

**Windows:** numpad `0`–`5` (or `Ctrl+Shift+0`–`5`) write the rating; the user comment is
edited in the WinUI pane.

**macOS:** `⌘⇧0`–`5` write the rating, and keypad `0`–`5` where a keypad exists. A laptop has
none, so the chord is the primary binding. The user comment is edited in the SwiftUI pane.
Number-row `0`/`1` remain zoom on both ([16-commands.md](16-commands.md)).

Batch date-shift, copy-metadata, strip-on-share, and filename templating are **v1.1**. Do
not build a batch engine before the pane has been read in anger.

**Verify (both platforms):** write-then-read round-trips preserve maker notes byte-for-byte
across the corpus; a process killed mid-write leaves the original intact; rating a JPEG from
the keyboard round-trips without opening the pane. A rating written on one platform reads
back identically on the other (the same file, or its XMP sidecar, copied across).

## Milestone E — Video and OS integration updates (PR 13–15, both platforms)

### PR 13 — Two-path trim
**Shared:** in/out model, keyframe index for the scrub-bar grid, Path 1 keyframe trim
(FFmpeg stream copy, platform-neutral), the cancellable job queue and the A–B loop preview.
**Smart cut is v1.1** (D7).

**Path 2 is a hardware re-encode behind an `encode` port.** Windows: NVENC / Quick Sync /
AMF / MF. macOS: VideoToolbox (`VTCompressionSession`). No x264/x265, no software HEVC
encoder, and no `--enable-gpl` on either platform ([11-licensing.md](11-licensing.md)).
Label it as slower on both.

**Windows:** WinUI job panel and trim mode. **macOS:** SwiftUI job panel and trim mode.
Trim mode takes `[` `]` for in/out on both.

**Verify (both platforms):** keyframe trim of a 1 GB MP4 completes in seconds with
proportional output size; the re-encode path is frame-accurate; **the source file is never
modified**; cancelling leaves no partial output. On Mac, the re-encode shows VideoToolbox
active, and no software encoder is linked.

### PR 14 — Extract & remux
**Shared:** lossless rotate (container matrix, no re-encode), split, remove-middle,
MKV ↔ MP4 remux, frame → PNG/JPEG, audio extract, and clip → GIF/WebP with a two-pass
palette. Everything is in the core; the hosts only add UI.

**Windows:** WinUI job panel entries. **macOS:** SwiftUI job panel entries.

**Verify (both platforms):** each operation round-trips; lossless rotate does not re-encode.

### PR 15 — OS integration
The Mac already has part of this from PR 20: UTIs, the "Open with" registration, Quick Look
in a separate process, and the first-launch default-viewer sheet. So this PR's Mac half is
smaller, and **not** a redo of PR 20.

**Windows:** file associations via `ProgId`/`OpenWithProgids` + a Default Apps deep link
(never a silent hijack). The default-viewer prompt follows the 2026-09-24 decision-log
entry (offered from the wizard's Finish page, pre-checked, confirmed by the user in
Settings). `IThumbnailProvider` and property handler **out-of-process (`DllSurrogate`)**
with timeouts. Jump list, taskbar transport buttons, drag-out via `CFSTR_FILEDESCRIPTOR`,
single-instance-with-tabs. **Reuse the identity shipped in PR 8** for each still `ProgId`'s
`DefaultIcon`.

**macOS:** the twins that PR 20 did not land. A Dock menu of recent folders (the jump list
twin). Window tabs through `NSWindow` tabbing (single-instance-with-tabs). Now Playing /
`MPRemoteCommandCenter` transport (the taskbar-button twin), if PR 19 has not already
provided it. Drag-out of a flattened view as a file promise (`NSFilePromiseProvider`).
Share through `NSSharingServicePicker`. **Open:** a Spotlight importer as the twin of the
property handler. It is out-of-process by construction, but whether it is worth its own
bundle is the owner's call. Do not build it silently.

Keyboard twins of drag-out, on both: `Ctrl+C` / `⌘C` (files), `Ctrl+Shift+C` / `⌘⇧C`
(path), `Ctrl+Alt+C` / `⌘⌥C` (flattened view), `Ctrl+Shift+S` / `⌘⇧S` (Share), and
`Ctrl+E` / `⌘E` (reveal in Explorer / Finder) ([16-commands.md](16-commands.md)).

**Verify (Windows):** double-clicking a HEIC in Explorer opens the app, and Explorer shows
your thumbnail and the MediaViewer file-type icon; a **deliberately corrupted** HEIC in a
browsed folder leaves Explorer running; uninstall removes every association. The running
window and taskbar button use the app icon, not the default exe. Accepting the default-app
offer opens Default Apps rather than writing `UserChoice`; declining leaves existing
defaults unchanged.

**Verify (macOS):** PR 20's Finder verify still holds. The Dock menu lists recent folders
and opens one. A second open of a file goes to the running instance as a tab. Transport
from the Now Playing controls drives the clip. `⌘C` of a still pastes as a file in Finder.
Share opens the system picker with the file. Dragging the app to the Trash still removes
every extension.

## Ingest — PR 26 (both platforms, planned 2026-09-24)

**Owner's call, 2026-09-24 ([12](12-decision-log.md)).** This moves "card ingest with verify"
out of the backlog. It is **not a faster copy engine.** Bytes still move at the speed of the
card, bus and disk, and Explorer and Finder already copy close to that. The wins come from
copying less, copying safely, and sorting while you copy.

PR 26 is numbered after Milestone G so PRs 9–25 keep their numbers. **It may start once
PR 9 holds on both platforms.** It needs PR 9's date taken, and otherwise builds on PR 6's
marks and `F7`/`F8` (Mac: PR 18). It touches `io/`, the folder model and the copy path, not
`edit/` or `player/`, so it runs beside PRs 10–15 as its own lane. Its verify gates only
itself, and both present-loop gates still hold throughout.

**Shared (core):**

- **Duplicate skip by content, never by name.** Skip a file only when a destination file
  has the same size **and** the same BLAKE3-256 hash. A different name with the same bytes
  counts as a duplicate. The same name with different bytes is **not** a duplicate: it is
  copied under the PR 6 collision-safe name. Size is compared first, so most files are
  never hashed. Destination hashes are cached in SQLite (`ingest.db`, keyed by volume,
  path, size and mtime), so a second ingest into the same library does not re-read it.
  Every skip is listed in the result, with the file it matched.
- **Verify after copy.** Hash while reading the source (no second pass over the card),
  flush the destination, read it back uncached where the OS allows
  (`FILE_FLAG_NO_BUFFERING` on Windows, `F_NOCACHE` on Mac), and compare. A mismatch
  deletes the bad copy, retries once, and then reports the failure. It never reports
  success.
- **Move is copy, then verify, then delete.** `F8` across volumes deletes the source
  **only after** the verify passes (this tightens PR 6's copy+delete). Same-volume move
  stays a rename. **Nothing is ever deleted from a source during ingest, and the app never
  formats or erases a card.** Rule 5 in spirit: the only original of a photo is on that card.
- **Overlap, not "parallel" for show.** One reader per physical source device and one
  writer per destination device, double-buffered with large sequential I/O, so the card
  is read while the SSD writes. Two sources on different devices (two card readers) run
  at the same time. Parallel reads from one card are **not** added: they make a card
  slower, not faster.
- **Sort while copying.** An optional destination layout by date taken (PR 9):
  `YYYY/YYYY-MM-DD/` from EXIF / container dates, with file mtime as the labelled fallback.
  This is one fixed layout. **User filename templating stays v1.1** (the Metadata backlog
  row).
- **Pairs travel together.** A RAW+JPEG pair and a Live Photo (PR 7 pairing) are copied,
  verified, skipped and sorted as one unit. A pair is never split across date folders.
- **Cull first, then copy.** The source is whatever is marked (else the current file), as
  in `F7`/`F8` today, so culling in the viewer decides what gets copied.
- The job runs on the I/O workers, is cancellable, and resumes a half-done ingest by
  hash. Cancelling leaves no partial files. **Never on the UI or render thread** (rule 1).
- Hashes, paths and filenames stay on the machine (rule 6). Nothing about an ingest goes
  into telemetry beyond counts, if telemetry is on at all.

BLAKE3 is offered under CC0-1.0 or Apache-2.0. **Use it under CC0.** Apache-2.0 alone
does not combine with GPL-2.0, and the app is GPL-2.0-or-later. Add it to `THIRD-PARTY.md`
and the vcpkg manifest in this PR ([11-licensing.md](11-licensing.md)).

**Windows:** an Ingest pane in WinUI (source, destination, the date-layout toggle, and a
progress list with copied / skipped-duplicate / failed rows). `Shift+F7` gains
"Ingest…". Removable-volume arrival (`WM_DEVICECHANGE`) can offer the pane, but **never
starts a copy by itself**.

**macOS:** the same pane in SwiftUI. Volume arrival comes from `NSWorkspace` mount
notifications. The same rule applies: offer the pane, never auto-copy.

**Not in this PR:** a catalogue or library database, a duplicate finder across a
whole existing library, near-duplicate or burst detection (burst-stack grouping stays in
the backlog), renaming templates, backup to a second destination, cloud anything.

**Verify (both platforms):**

- Ingest a 64 GB card dump of mixed RAW, JPEG, HEIC and video to an empty folder. Every
  file's destination hash matches its source. Total time is within 10 % of the OS file
  copy of the same set to the same drive (verification must not cost a second read of the
  card).
- Ingest the same card again. **Zero bytes are written**, every file is reported as a
  duplicate, the destination is not re-read (the hash cache holds), and each card file
  is read at most once.
- Rename files on the card and ingest again: still zero copies. Change one byte of a
  JPEG, keeping its name: it is copied under a collision-safe name, not skipped.
- A copy corrupted in flight (fault injection in the writer) is detected, retried, and
  either fixed or reported. It is never counted as done.
- `F8` from the card to another volume: pull the destination drive mid-job, and no source
  file whose copy was not verified is gone.
- A RAW+JPEG pair and a Live Photo land in the same date folder. Date layout puts a clip
  and a still shot on the same day in the same folder.
- The UI and canvas stay responsive during ingest, and **both present-loop gates hold
  while ingesting**.

## Milestone F — It opens on a Mac (PR 16–20)

**Status (2026-09-24):** landed. The owner confirmed that the basic Mac setup is complete
and that PRs 9–15 are now dual-track. The items still owed from F are listed under
[Dual-track updates](#dual-track-updates--prs-915-on-windows-and-macos-together-2026-09-24).
The sections below are kept as the record of what F was built to. Mac is a **host of the
same core**, not a UI-only port (D9). Decode, colour, EditStack, metadata, and
the C ABI transfer. Present, hardware decode, audio, I/O, chrome, and the installer do not.
Full split and the hostable-core rule: [15-platforms.md](15-platforms.md).

**Parity note (2026-09-17, [12](12-decision-log.md)):** the original PR 16–20 split only
reached Windows PR 1/2/3/5abc/8 parity — Windows PR 4 (folder/filmstrip/gallery), PR 6
(keyboard-complete browse), and PR 7 (camera-dump formats + crashpad), all already shipped
on Windows, had no assigned Mac PR. The owner chose to fold that scope into the existing
PR 17/18/20 slots rather than add new PR numbers — PR 17 and PR 18 are wider than their
Windows twins as a result, each still with its own verify line below.

F no longer waits for PR 8's verify (widened 2026-09-13 → 2026-09-17 below), but **PR 1's
present-loop verify must still hold on Windows** and F has its own present-loop gate on Mac —
both are checked independently, not waived by the other.

**Sequencing exception (2026-09-13, widened 2026-09-17):** the owner started **PR 16** in
parallel with Windows v1 because multiple agents can take the Metal present lab without
blocking Windows PRs. On 2026-09-17 the owner widened the exception to **PRs 17–20**
as well, working from a Mac day to day and ahead of Windows PR 8 shipping. D9's product
call is unchanged — Mac is a host, not a UI port — and Milestone F's own internal
sequencing (16 → 17 → 18 → 19 → 20, each verify line before the next starts) still
applies. See [12](12-decision-log.md).

### PR 16 — Metal present lab
AppKit window, `CAMetalLayer`, display-link pacing, idle → stop presenting, F3 overlay,
`frametime` on Darwin. **No SwiftUI yet** — this is the Mac instrument, kept as a debug
harness the way the Win32 lab is.

**Verify:** presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle,
**on Apple Silicon, measured from the Metal / display-link side**. A Windows DXGI soak is
not this verify.

### PR 17 — Still decode + pan/zoom, on Metal
Same ABI and decoders as PR 2 (JPEG/PNG/BMP) to start. Immutable Metal texture upload from
the worker pool, fit / wheel-zoom-toward-cursor / drag-pan. First blit shader gets its MSL
twin.

**Folded in (2026-09-17, [12](12-decision-log.md)):** the rest of the Windows PR 7 camera-dump
format set — TIFF, WebP, ICO, HEIC/HEIF, AVIF, RAW via LibRaw with embedded-preview-as-first-pixel
— and Crashpad + the Mac minidump scrub, landing here for the same reason PR 7 paired them on
Windows: this is the PR where hostile real-world files first meet Mac decoders. RAW+JPEG and Live
Photo pairing *detection* (`io/pairing.h` logic) can land here too, decode-side; surfacing a pair as
one filmstrip stop is PR 18's job once a filmstrip exists.

**Verify:** everything PR 2's verify line asked, plus: iPhone HEIC opens with no extra codec
install; a CR2/NEF/ARW shows a preview in JPEG-comparable time and the full decode replaces it
without a visible pop; **original RAW bytes unchanged**; nothing in the broken-file corpus
crashes or hangs; a deliberately corrupted RAW produces a minidump containing no path, filename,
or pixel data. A 12 MP JPEG pans at refresh with zero decode on mouse move; a tagged AdobeRGB
JPEG renders correctly and an untagged one is treated as sRGB, with **no tone-map applied
to either** (D6).

### PR 18 — SwiftUI chrome, hosted in the AppKit window
**The canvas is not ported to SwiftUI.** PR 16's AppKit window and `CAMetalLayer` stay;
SwiftUI chrome is hosted inside them. Command bar and window chrome only.

**Folded in (2026-09-17, [12](12-decision-log.md)):** the Windows PR 4 and PR 6 scope that has
nowhere else to go once chrome exists — folder listing + sort, `kqueue`/`FSEvents` dir watch
(`io/dir_mac.cpp`), filmstrip + gallery as SwiftUI views over the same folder model and the same
JPEG-512 thumbnail cache spec (`jpg512.1`) Windows uses, and PR 6's keyboard-complete browse: one
key router with a Mac default map (`⌘` not `Ctrl`), `?` overlay, marks, copy-to/move-to, Trash
(not Recycle Bin) with confirm, drag-and-drop in and out, argv handling, fullscreen, slideshow as
a mode, fit/100%/fill, animated GIF/APNG/WebP on the display-link frame clock. Same command ids as
Windows ([16-commands.md](16-commands.md)); the Mac host writes its own default map rather than
importing the XAML one.

**Verify:** zero dropped frames while panning a cached image at display refresh, unchanged
from PR 17 now that chrome is on screen. Focus and keyboard traversal cross the SwiftUI /
canvas boundary; a popover opens over the canvas without clipping. 2000 mixed JPEGs — filmstrip
scrolls without a hitch, second folder visit has near-instant thumbnails; keyboard-only browse of
a real folder — open, next/prev, zoom/fit/100%, mark, copy-to, delete to Trash, fullscreen,
slideshow start/stop — without the mouse, with `?` listing those bindings; a file dropped into the
folder appears without restart; animation timing matches a browser.

### PR 19 — VideoToolbox + Core Audio
FFmpeg + VideoToolbox on *your* `MTLDevice`, copy out of the decoder pool into a
presentation ring you own, NV12 and 10-bit sample paths, HDR→SDR in the MSL twin. Core
Audio is the master clock. **`AVPlayer` is forbidden.**

**Verify:** 4K 10-bit HEVC plays at full rate with VideoToolbox active, on a clean Mac with
no extra codec packs; an iPhone HLG clip looks correct; A/V drift flat over 30 minutes;
photo → video → photo leaks no textures.

### PR 20 — Finder + notarized ship
UTIs for the D5 still set, never a silent default-app hijack. Quick Look / thumbnails in a
**separate process**. Notarized Sparkle, Apple Silicon only. Crash reporting already exists
from PR 17 (folded in 2026-09-17, mirroring Windows PR 7) — this PR does not add it.
First install is a branded drag-install `.dmg` with the GPL on mount — the Mac twin of the
PR 8 wizard, not a `.pkg`
([13](13-updates-and-telemetry.md#macos-first-install--a-branded-disk-image-pr-20)).

**Verify:** double-clicking a HEIC in Finder opens the app; a deliberately corrupted HEIC
in a browsed folder leaves Finder running; a clean Mac → mount the notarized image (GPL
shown) → drag to Applications → open a real camera dump, with no Gatekeeper block and no
codec dialog; Sparkle takes N → N+1 silently and refuses an appcast signed with any other
key; dragging the app to the Trash removes the Quick Look extension.

---

## Milestone G — Local AI search (PR 21–25, post-v1, proposed)

Windows-first, opt-in, and a **downloadable extra installed from Settings** — the base
installer and updates never carry it. Full design, models, index, yield policy and verify
lines: [17-local-ai-search.md](17-local-ai-search.md). Proposed 2026-09-24
([12](12-decision-log.md)); not a D-decision. Every slice inherits PR 1's present-loop verify,
re-run **while indexing**.

- **PR 21 — Inference host and the AI pack.** `src/infer`, ORT CPU + a vendor provider,
  signed pack download/verify, per-piece Install/Remove and an Auto / provider / CPU-only
  toggle in Settings. Starts with a measured spike.
- **PR 22 — Video sampler and index.** Keyframe sampling with gap limits, embedding dedupe,
  resumable background `index.db`, yield-to-playback policy.
- **PR 23 — Search and results.** Natural-language query ("man on broom") over photos and
  video, results in the gallery island, Enter seeks to the moment, match markers on the
  scrub bar, keyboard-complete.
- **PR 24 — Find-similar, index management, hardening.**
- **PR 25 — Faces.** Local, opt-in, deletable people index; stricter biometric handling.

Numbers continue after Milestone F to avoid renumbering PRs 9–20. AI culling, cloud
inference, and inference in the base installer stay out.

**Open (owner, 2026-09-24):** G was proposed Windows-first, before PRs 9–15 went
dual-track. Whether G follows the dual-track rule is not decided. A Mac half would need an
ORT Core ML provider, which plan/17 does not specify. Decide before PR 21 starts.

---

## Further backlog — after the first feature updates

Older specs use **v1.1** for this backlog. It remains deferred beyond its prerequisite
feature slices; that label does not promise everything in one release.

| Area | Deferred work |
|---|---|
| Photo | Curves, per-channel HSL, colour grading, highlights/shadows, sharpen, NR, dehaze, vignette |
| Photo (local) | Radial/linear gradients, brush masks, healing/clone, red-eye |
| RAW | Full develop: highlight recovery, lens profiles, dual-illuminant WB, GPU demosaic |
| Video | **Smart cut** (D7), subtitle rendering beyond plain text, HDR passthrough |
| Formats | JPEG XL, OpenEXR, HDR, PSD, SVG, DDS, JPEG 2000, VVC (D5) |
| Display | HDR output + FP16 swapchain (D6), wide-gamut |
| Metadata | Batch date-shift, copy-metadata, strip-on-share, filename templating (PR 26's fixed date layout is not templating), colour labels, keywords |
| Viewer | JSON keymap import/export and named layouts (FastStone / IrfanView / vim); **theme**: colour scheme for chrome + canvas + F3 overlay, and a user font (TTF/OTF copied into `%LocalAppData%\MediaViewer\fonts`, never off-machine; CozetteVector remains the default and the fallback). Side-by-side compare, burst-stack grouping, print/contact sheet, GPS map, quick-export presets, PiP/compact overlay, focus peaking / zebras / channel isolation |
| Security | AppContainer decode process (D8) |
| Distribution | Per-machine MSI for enterprise (Store MSIX remains excluded by the licence decision) |
| Platform | Windows ARM64, Intel Macs. **Apple Silicon macOS is Milestone F, not v1.1.** |

## Sequencing advice

- **Build the frame-time harness in PR 1, and re-run it in every PR.** Retrofitting performance is
  a rewrite; defending it from day one is nearly free. This is why every verify line is additive.
- **PR 3 is the real go/no-go on D1.** If `SwapChainPanel` can't hold the pacing gate, you find out
  before any chrome is built on it — that's the entire reason PR 3 exists as its own slice.
- **Formats (PR 7) before editors.** Coverage is what makes a viewer worth switching to; editing is
  what makes people stay.
- **Resist the NLE, and resist the develop module.** Both are real products; neither is this one.
  Resist JSON keymap packs and the compare workspace in v1 the same way: the default map,
  Settings remap, and hold-previous are the daily path ([16-commands.md](16-commands.md)).
- **PRs 1–7 define the first release feature set; PR 8 makes it installable and updatable.**
  Milestones D/E add features in later updates, on both platforms at once (from PR 9).
- **Dual-track is a cost, paid on purpose.** Every PR from 9 writes chrome twice and every
  kernel twice (HLSL + MSL). Plan the SwiftUI half and the MSL twin into the PR, not as a
  follow-up. A PR that is green on one platform only is not done.
- **Do not plan v1 as "3 months full-time."** That number was the original overconfidence surviving
  the scope cuts. PR 5b (the A/V clock), PR 7 (HEIC + RAW + tiles + fuzzing), and PR 15 (shell
  integration and out-of-process handlers) are each multi-week for one person on their own.
  Size the milestones, ship them in order, and let the calendar report itself rather than being
  promised up front.
- **Keep Win32 / D3D11 out of `image/`, `player/`, `edit/`, and `meta/`, and keep Cocoa /
  Metal out of them too.** From PR 9 a leak does not wait for a later port to surface. It
  breaks the other platform's half of the same PR. Chrome is written twice (D1). The present
  paths stay one per OS (D3D11 on Windows, Metal on Mac).
