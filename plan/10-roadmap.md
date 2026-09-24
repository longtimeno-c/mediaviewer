# 10 — Roadmap

**Release cut (2026-09-14): finish PR 7, then package and ship the viewer in PR 8.**
v1 is the Windows viewer delivered by PRs 1–7, on the camera-dump format set (D5), with
C# WinUI 3 chrome over a C++ core (D1) and FFmpeg video on one present path (D2).
Metadata panes/writes, photo editing/export, video trimming, and additional Windows
integration ship in future updates; they do not block v1 (D4/D7 amended in
[01-decisions.md](01-decisions.md), rationale in [12](12-decision-log.md)).

**Numbering (2026-09-14):** former PR 15 became PR 8; former PRs 8–14 became PRs 9–15.

**Numbering (2026-09-24): one number per feature, on both platforms.** The Mac host, built
as Milestone F (old PRs 16–20), is filed as the **Mac halves of PRs 1–8** (table under
[Mac halves](#mac-halves-of-prs-18--landed-formerly-milestone-f-prs-1620)). **Windows and
Mac are both at PR 9.** The order from here is:

| PR | Slice | Platforms | State (2026-09-24) |
|---|---|---|---|
| 1–8 | The viewer, packaged | Windows + Mac halves | Landed. Mac items still owed are listed under Dual-track |
| **9** | Metadata (read) | both | **In progress.** Mac half ahead; Windows owes the XAML pane, tree island and sort menu |
| **10** | Geometry edits + export | both | Mac half started on a branch; waits for PR 9 before merging |
| 11 | Colour adjusts, plus Mac crash reporting | both | Planned |
| 12 | Metadata (write) | both | Planned |
| 13 | Two-path trim | both | Planned |
| 14 | Extract & remux | both | Planned |
| 15 | OS integration (Explorer; the remaining Finder twins) | both | Planned |
| 16–19 | **Import add-on**, Milestone G ([18](18-import.md)) | both | Planned |
| 20–24 | Local AI search add-on, Milestone H ([17](17-local-ai-search.md)); was 21–25 | both | Proposed |

Decision-log entries, branches and commits keep the numbers they were written with. Old 16–20
→ Mac halves of 1–8. Old 21–25 → 20–24. The earlier same-day draft's "PR 26 Ingest" → 16–19.

**Dual-track from PR 9 (2026-09-24):** from PR 9 on, **every PR lands on Windows and macOS
together**: one shared core change, a WinUI half and a SwiftUI half, and a verify line on
each platform. A PR is done only when both halves hold. See
[Dual-track updates](#dual-track-updates--prs-915-on-windows-and-macos-together-2026-09-24)
and D9 in [01-decisions.md](01-decisions.md).

Work is sliced into **independently runnable PRs, each with a verify line**. Do not start PR N+1
(from PR 9: do not *merge* it; a host half may start ahead on a branch)
until N's verify holds **and PR 1's present-loop verify still holds** (from PR 9, on both
platforms: Windows PR 1's DXGI gate and Mac PR 1's Metal gate). That second clause is what
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

## Mac halves of PRs 1–8 — landed (formerly Milestone F, PRs 16–20)

**Renumbered 2026-09-24 ([12](12-decision-log.md)).** The Mac host was built as its own
milestone, F, numbered PRs 16–20. Since the D9 amendment, **one PR number means one feature on
both platforms**, so those five PRs are now filed as the **Mac halves of Windows PRs 1–8**.
Both platforms are at PR 9. Numbers 16–20 are reused by the Import add-on (16–19) and AI search
(20). Decision-log entries, branch names and commits from before 2026-09-24 keep the old
numbers. Use this table to read them:

| Old number | Is now | What it built |
|---|---|---|
| PR 16 | **Mac PR 1** | Metal present lab: AppKit, `CAMetalLayer`, display link, F3, `frametime` on Darwin |
| PR 17 | **Mac PR 2** + **Mac PR 7** | Still decode, pan/zoom, the MSL blit twin, and the rest of the D5 formats + pairing detection (Crashpad moved to Mac PR 11) |
| PR 18 | **Mac PR 3** + **Mac PR 4** + **Mac PR 6** | SwiftUI chrome in the AppKit window, folder/filmstrip/gallery + FSEvents, and keyboard-complete browse |
| PR 19 | **Mac PR 5** (5a/5b/5c) | VideoToolbox + Core Audio clock + transport strip |
| PR 20 | **Mac PR 8**, plus part of **Mac PR 15** | Notarized disk image, Sparkle, and (early) Finder types, Quick Look, the default-viewer sheet |

The sections below are the record of what each was built to, with their verify lines. Mac is a
**host of the same core**, not a UI-only port (D9). Decode, colour, EditStack, metadata and the
C ABI transfer. Present, hardware decode, audio, I/O, chrome and the installer do not
([15-platforms.md](15-platforms.md)). Each Mac half has its own present-loop gate (Mac PR 1's
Metal soak), checked independently of Windows PR 1's.

### Mac PR 1 — Metal present lab (was PR 16)
AppKit window, `CAMetalLayer`, display-link pacing, idle → stop presenting, F3 overlay,
`frametime` on Darwin. **No SwiftUI yet** — this is the Mac instrument, kept as a debug
harness the way the Win32 lab is.

**Verify:** presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle,
**on Apple Silicon, measured from the Metal / display-link side**. A Windows DXGI soak is
not this verify.

### Mac PR 2 + PR 7 — Still decode, pan/zoom and the camera-dump formats, on Metal (was PR 17)
Same ABI and decoders as PR 2 (JPEG/PNG/BMP) to start. Immutable Metal texture upload from
the worker pool, fit / wheel-zoom-toward-cursor / drag-pan. First blit shader gets its MSL
twin.

**Folded in (2026-09-17, [12](12-decision-log.md)):** the rest of the Windows PR 7 camera-dump
format set — TIFF, WebP, ICO, HEIC/HEIF, AVIF, RAW via LibRaw with embedded-preview-as-first-pixel
— and Crashpad + the Mac minidump scrub (**never landed; moved to the Mac half of PR 11,
2026-09-24**), planned here for the same reason PR 7 paired them on
Windows: this is the PR where hostile real-world files first meet Mac decoders. RAW+JPEG and Live
Photo pairing *detection* (`io/pairing.h` logic) can land here too, decode-side; surfacing a pair as
one filmstrip stop is Mac PR 3's job once a filmstrip exists.

**Verify:** everything PR 2's verify line asked, plus: iPhone HEIC opens with no extra codec
install; a CR2/NEF/ARW shows a preview in JPEG-comparable time and the full decode replaces it
without a visible pop; **original RAW bytes unchanged**; nothing in the broken-file corpus
crashes or hangs; a deliberately corrupted RAW produces a minidump containing no path, filename,
or pixel data. A 12 MP JPEG pans at refresh with zero decode on mouse move; a tagged AdobeRGB
JPEG renders correctly and an untagged one is treated as sRGB, with **no tone-map applied
to either** (D6).

### Mac PR 3 + PR 4 + PR 6 — SwiftUI chrome, folder/filmstrip/gallery, keyboard-complete browse (was PR 18)
**The canvas is not ported to SwiftUI.** Mac PR 1's AppKit window and `CAMetalLayer` stay;
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
from Mac PR 2 now that chrome is on screen. Focus and keyboard traversal cross the SwiftUI /
canvas boundary; a popover opens over the canvas without clipping. 2000 mixed JPEGs — filmstrip
scrolls without a hitch, second folder visit has near-instant thumbnails; keyboard-only browse of
a real folder — open, next/prev, zoom/fit/100%, mark, copy-to, delete to Trash, fullscreen,
slideshow start/stop — without the mouse, with `?` listing those bindings; a file dropped into the
folder appears without restart; animation timing matches a browser.

### Mac PR 5 — VideoToolbox + Core Audio + transport (was PR 19)
FFmpeg + VideoToolbox on *your* `MTLDevice`, copy out of the decoder pool into a
presentation ring you own, NV12 and 10-bit sample paths, HDR→SDR in the MSL twin. Core
Audio is the master clock. **`AVPlayer` is forbidden.**

**Verify:** 4K 10-bit HEVC plays at full rate with VideoToolbox active, on a clean Mac with
no extra codec packs; an iPhone HLG clip looks correct; A/V drift flat over 30 minutes;
photo → video → photo leaks no textures.

### Mac PR 8 — Notarized disk image, Sparkle, and early Finder integration (was PR 20)
UTIs for the D5 still set, never a silent default-app hijack. Quick Look / thumbnails in a
**separate process**. Notarized Sparkle, Apple Silicon only. Crash reporting already exists
from Mac PR 7 (folded in 2026-09-17, mirroring Windows PR 7) — this PR does not add it.
First install is a branded drag-install `.dmg` with the GPL on mount — the Mac twin of the
PR 8 wizard, not a `.pkg`
([13](13-updates-and-telemetry.md#macos-first-install--a-branded-disk-image-mac-pr-8)).

**Verify:** double-clicking a HEIC in Finder opens the app; a deliberately corrupted HEIC
in a browsed folder leaves Finder running; a clean Mac → mount the notarized image (GPL
shown) → drag to Applications → open a real camera dump, with no Gatekeeper block and no
codec dialog; Sparkle takes N → N+1 silently and refuses an appcast signed with any other
key; dragging the app to the Trash removes the Quick Look extension.

## Dual-track updates — PRs 9–15 on Windows and macOS together (2026-09-24)

**Owner's call, 2026-09-24 ([12](12-decision-log.md), D9 amended in
[01-decisions.md](01-decisions.md)).** Windows v1 is packaged (PR 8) and the Mac host
exists (the Mac halves of PRs 1–8). From PR 9 on, **each PR lands on both platforms**. These are no
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
  plus each platform's own additions. **PR N is done only when both halves hold, and PR
  N+1 does not merge until then.** A host half may be *started* ahead on a branch (the Mac
  half of PR 10 already is), but merges go in number order. That keeps the two apps at the
  same PR instead of letting one run ahead.
- **Two present-loop gates, inherited every PR.** PR 1's gate on Windows (DXGI, `frametime`)
  and Mac PR 1's gate on Mac (Metal / display link, `frametime` on Darwin). Neither is
  evidence for the other.
- **One host half may lag inside a PR, but only in its own branch.** Merge a PR when both
  halves are green. A Windows-only or Mac-only merge of a feature from PR 9 onward is a
  partial release, and D9 does not allow one any more.

**Entry state for PR 9** (the owner confirmed the basic Mac setup is complete on
2026-09-24). These items were recorded as owed in [12](12-decision-log.md). They stay
tracked, but they do not block PR 9 from starting:

- Mac PR 5: the real-iPhone HLG check (only a synthetic HLG clip was checked).
- Mac PR 8: Quick Look precedence over Finder's own generator; state carried across an
  update restart (zoom, pan, clip position); rollback after two failed starts.
- **Crashpad on Mac:** assigned to the **Mac half of PR 11** (owner, 2026-09-24). Until
  then Mac has no crash capture. Do not cut a stable Mac release that includes PR 9's new
  parsers before PR 11 lands, or say plainly in the release notes that crashes aren't captured.

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
*Status 2026-09-24: shared core and **both host halves written** on the PR 9 branch; the core is
unit-tested, the Windows and macOS host halves await their first compile and each platform's verify
run — see [12](12-decision-log.md) (which also records where this departs from the text below:
`transupp`, and crop mode's overlay living in the canvas).*

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
**Also Mac crash reporting** (owner call, 2026-09-24; owed since old PR 17). Crashpad,
out-of-process, as on Windows ([13](13-updates-and-telemetry.md)). The Mac minidump goes
through the same scrub before anything could be sent: no path, no filename, no username,
no pixel data, no EXIF. The Swift/AppKit side is its own capture path: an uncaught
`NSException` and a Swift runtime trap are recorded with the same correlation id as the
native report. There is still no upload endpoint, same as Windows. It lands here, before any
stable Mac release that carries PR 9's Exiv2 / libavformat metadata parsing or PR 11's
full-resolution RAW bake.

Viewer `C` blinkies become accurate on RAW once the full decode exists. `E` focuses the
adjust pane.

**Verify (both platforms):** dragging a slider is shader-only with no re-decode, updating
within one refresh interval on a 45 MP RAW; export matches the preview within 8-bit
rounding. **The adjust pane stays disabled until LibRaw's full decode completes**
([07-photo-editing.md](07-photo-editing.md)). A fixed slider set exported from the same
source on both platforms matches within 8-bit rounding. HLSL and MSL twins that disagree
fail the PR.

**Verify (macOS, crash reporting):** a deliberately corrupted RAW opened on Mac produces a
minidump from the out-of-process handler. The Mac twin of Windows' canary scan (a marked copy in
`PRIVATE_FOLDER_canary/SECRET_FILENAME_canary…`, plus a pixel-pattern companion) finds **no path,
filename, username or pixel data** in it. A forced `NSException` in the chrome is captured with
the correlation id of the native call in flight. The app relaunches cleanly after each crash.

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
The Mac already has part of this from Mac PR 8: UTIs, the "Open with" registration, Quick Look
in a separate process, and the first-launch default-viewer sheet. So this PR's Mac half is
smaller, and **not** a redo of Mac PR 8.

**Windows:** file associations via `ProgId`/`OpenWithProgids` + a Default Apps deep link
(never a silent hijack). The default-viewer prompt follows the 2026-09-24 decision-log
entry (offered from the wizard's Finish page, pre-checked, confirmed by the user in
Settings). `IThumbnailProvider` and property handler **out-of-process (`DllSurrogate`)**
with timeouts. Jump list, taskbar transport buttons, drag-out via `CFSTR_FILEDESCRIPTOR`,
single-instance-with-tabs. **Reuse the identity shipped in PR 8** for each still `ProgId`'s
`DefaultIcon`.

**macOS:** the twins that Mac PR 8 did not land. A Dock menu of recent folders (the jump list
twin). Window tabs through `NSWindow` tabbing (single-instance-with-tabs). Now Playing /
`MPRemoteCommandCenter` transport (the taskbar-button twin), if Mac PR 5 has not already
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

**Verify (macOS):** Mac PR 8's Finder verify still holds. The Dock menu lists recent folders
and opens one. A second open of a file goes to the running instance as a tab. Transport
from the Now Playing controls drives the clip. `⌘C` of a still pastes as a file in Finder.
Share opens the system picker with the file. Dragging the app to the Trash still removes
every extension.

## Milestone G — Import add-on (PR 16–19, both platforms)

An **optional add-on installed from Settings**: copy a card or folder into a library, skip
content duplicates (a size check, then a BLAKE3 hash, never the name), verify every copy,
sort by date taken, keep pairs and sidecars together, resume after an unplug, and back up to
a second drive from one read. The base viewer, installer and updates never carry it. It is not a
faster copy engine: the time saved comes from copying less and never stopping to ask. Full
design, GUI, settings, engine, add-on mechanism and verify lines:
[18-import.md](18-import.md). Planned 2026-09-24 ([12](12-decision-log.md)).

Sequenced **after PR 15**, because PRs 9 and 10 are already in flight on both platforms. Each
slice is dual-track, with a verify line on each platform, and both present-loop gates held
**while importing**.

- **PR 16 — Add-on mechanism + import engine.** Signed add-on install/remove in Settings,
  host function table, the engine (duplicates, library index, verify, overlap, units, resume)
  and a minimal sheet. The base app's `F8` stops deleting a source before its copy is verified.
- **PR 17 — The Import window.** Sources with "N new" counts, a day-grouped grid, a preset
  panel with a **Where files go** preview, ETA, pause/cancel, background running, summary,
  eject. Keyboard-complete.
- **PR 18 — Presets, backup, layouts, rename.** Named and per-card presets, opt-in auto-import,
  a second destination from one read, layouts, rename templates, camera sidecars.
- **PR 19 — Library tools.** Library-wide duplicate scope, import history, and
  verify-a-folder (silent-corruption check).

The add-on mechanism built in PR 16 is the one the AI pack (PR 20) installs through.

---

## Milestone H — Local AI search (PR 20–24, post-v1, proposed)

**Both platforms** (owner, 2026-09-24), like every PR from 9. Opt-in, and a **downloadable
add-on installed from Settings** through PR 16's add-on mechanism. The base installer,
disk image and updates never carry it. Windows runs ONNX Runtime on CPU plus the vendor
providers. Mac runs ONNX Runtime with the **Core ML provider** (Apple GPU / Neural Engine),
CPU underneath. Full design, models, index, yield policy and verify
lines: [17-local-ai-search.md](17-local-ai-search.md). Proposed 2026-09-24
([12](12-decision-log.md)); not a D-decision. Every slice inherits PR 1's present-loop verify,
re-run **while indexing**.

- **PR 20 — Inference host and the AI pack.** `src/infer`, ORT CPU + a vendor provider,
  signed pack download/verify, per-piece Install/Remove and an Auto / provider / CPU-only
  toggle in Settings. Starts with a measured spike.
- **PR 21 — Video sampler and index.** Keyframe sampling with gap limits, embedding dedupe,
  resumable background `index.db`, yield-to-playback policy.
- **PR 22 — Search and results.** Natural-language query ("man on broom") over photos and
  video, results in the gallery island, Enter seeks to the moment, match markers on the
  scrub bar, keyboard-complete.
- **PR 23 — Find-similar, index management, hardening.**
- **PR 24 — Faces.** Local, opt-in, deletable people index; stricter biometric handling.

Renumbered 2026-09-24 from 21–25 (it follows Import). AI culling, cloud
inference, and inference in the base installer stay out.

**Settled (owner, 2026-09-24): dual-track.** Each of PRs 20–24 has a Windows half and a Mac
half and a verify line on each, and both present-loop gates hold **while indexing**.

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
| Metadata | Batch date-shift, copy-metadata, strip-on-share, renaming files already in a library (Import's rename-on-import, PR 18, is not this), colour labels, keywords |
| Viewer | JSON keymap import/export and named layouts (FastStone / IrfanView / vim); **theme**: colour scheme for chrome + canvas + F3 overlay, and a user font (TTF/OTF copied into `%LocalAppData%\MediaViewer\fonts`, never off-machine; CozetteVector remains the default and the fallback). Side-by-side compare, burst-stack grouping, print/contact sheet, GPS map, quick-export presets, PiP/compact overlay, focus peaking / zebras / channel isolation |
| Security | AppContainer decode process (D8) |
| Distribution | Per-machine MSI for enterprise (Store MSIX remains excluded by the licence decision) |
| Platform | Windows ARM64, Intel Macs. **Apple Silicon macOS landed as the Mac halves of PRs 1–8; it is not v1.1.** |

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
