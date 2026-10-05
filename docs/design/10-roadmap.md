# 10 — Feature inventory by PR

What each numbered PR delivered, on Windows and on macOS, and the verify line that is its
acceptance test. Code comments cite these as "PR N"; the numbers are labels, not a schedule.

One PR number means one feature on both platforms. PRs 1–8 were built on Windows first; their
Mac halves ("Mac PR N") were built afterwards and are listed in
[Mac halves of PRs 1–8](#mac-halves-of-prs-18). From PR 9 each PR is one shared core change
with a WinUI half and a SwiftUI half ([Platforms](15-platforms.md)).

Every verify line is additive: each PR's acceptance also re-runs the present-loop gate of
[PR 1](#pr-1--present-lab) on Windows (DXGI, `frametime`) and [Mac PR 1](#mac-pr-1--metal-present-lab)
on the Mac (Metal / display link). Which verify runs have actually been carried out, and on what
hardware, is recorded in [DEVELOPMENT, "Where this actually is"](../DEVELOPMENT.md#where-this-actually-is).

**Old numbers.** Commits, branches and decision-log entries keep the numbers they were written
with: old PRs 16–20 are the Mac halves of PRs 1–8 (table below); old PRs 8–14 are PRs 9–15 and
old PR 15 is PR 8; old AI-search PRs 21–25 are 20–24. PR 25 is unused.

| PR | Feature | Platforms |
|---|---|---|
| 1–8 | The viewer, packaged | Windows, Mac halves |
| 9 | Metadata (read) | both |
| 10 | Geometry edits + export | both |
| 11 | Colour adjusts; Mac crash reporting | both |
| 12 | Metadata (write) | both |
| 13 | Two-path trim | both |
| 14 | Extract & remux | both |
| 15 | OS integration | both |
| 16–19 | Import add-on ([18](18-import.md)) | both |
| 20–24 | Local AI search add-on ([17](17-local-ai-search.md)) | both |
| 26 | Folder tiles, breadcrumb, up | both |
| 29 | Edit workspace ([20](20-edit-workspace.md)) | both |
| 30 | Video Editor window, one clip ([21](21-video-editor.md)) | both |
| 49 | Network-speed copy engine ([24](24-transfer.md)) | both |
| 54 | Find duplicates in the Import add-on ([18](18-import.md#find-duplicates-pr-54)) | both |
| 55 | Open add-ons: packages and themes ([25](25-open-addons.md)) | both |

Local search inside Final Cut Pro and FCPXML export (issue #71) has no PR number:
[23](23-nle-search.md).

## Milestone A — It draws (PR 1–3)

### PR 1 — Present lab
Win32 + DComp + Dear ImGui host, D3D11 device, flip-model waitable swapchain, per-monitor-v2
DPI, F3 frame-time overlay reading real present-to-present intervals. Job system, result types,
ETW tracepoints. The lab stays in the tree as the debug harness (`tools/frametime`). Also in
PR 1: the C ABI round-tripping end to end ([ABI](14-abi.md): header, `mv_guard`, a call, a
`SafeHandle`, a completion drain), and the platform floor of Windows 10 21H2 (`OVERLAPPED` I/O,
not `IoRing`). Licensing: [11](11-licensing.md).

**Verify:** presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle.

### PR 2 — Still decode + pan/zoom, in the lab
JPEG/PNG/BMP decoded on the pool, immutable-texture upload from the worker, fit-to-window,
spring-based wheel-zoom-toward-cursor, drag-pan, `0`/`1`. Linear FP16 working space, 8-bit sRGB
swapchain, ICC via LCMS.

**Verify:** a 12 MP JPEG pans at refresh with zero decode on mouse move; dragging the window
while a 60 MP PNG loads stays smooth; a tagged AdobeRGB JPEG renders correctly and an untagged
one is treated as sRGB, with no tone-map applied to either.

### PR 3 — WinUI chrome, hosted in the native shell
PR 1's Win32 window and D3D11 swapchain stay as the canvas; WinUI 3 chrome is hosted in them as
XAML content islands (`DesktopWindowXamlSource`), written in C#, talking to the core over the
C ABI. Command bar and window chrome. One present path, owned by C++.

**Verify:** zero dropped frames while panning a cached image at the display's refresh rate,
unchanged from PR 2 with chrome on screen. Focus and tab traversal cross the island boundary
correctly; a flyout opens over the canvas without clipping.

## Milestone B — It's a viewer (PR 4–7)

### PR 4 — Folder, filmstrip, thumbnails
Folder listing + sort (name, mtime, size, type; date taken arrives in PR 9), directory watcher
behind portable `io/dir.h` (`io/dir_win.cpp`: `ReadDirectoryChangesW`), SQLite + on-disk
JPEG-512 thumbnail cache keyed by `(path, mtime, size, spec)` with spec `jpg512.1`,
visible-first generation, directional prefetch of ±2 decoded textures into a five-slot GPU LRU
with generation-counter cancellation.

The filmstrip is a second XAML island (an `ItemsRepeater`), a bottom strip on the same HWND;
PR 3's island is the top strip. The gallery (`G`) is a third island: a full-client thumbnail
grid between the command bar and the bottom of the client area, over the same listing, items
and thumbnail cache. Opening a single image lists its folder the same way a folder open does.
The filmstrip is a per-open-mode preference (`%LocalAppData%\MediaViewer\settings.ini`), on for
a folder open and off for a single image. C# drains completions once the island is attached.

**Verify:** 2000 mixed JPEGs — filmstrip scrolls without a hitch, second folder visit has
near-instant thumbnails, arrow-key browse shows the next image in < 40 ms warm.

### PR 5a — Silent video into the same swapchain
FFmpeg demux + `avcodec` + D3D11VA behind `IVideoSource`, copied out of the decoder pool into a
presentation ring the app owns, NV12 and P010 sample paths, YUV→RGB plus HDR (HLG/PQ) → SDR
tone-mapping in the shader ([Video pipeline](05-video-pipeline.md)). Frames present on their
PTS against QPC. Photos and videos share one folder navigation model.

**Verify:** 4K 10-bit HEVC and AV1 play at full rate with GPU video decode > 0 in Task Manager,
on a clean VM with no Store codec packs; an iPhone HLG clip looks correct rather than washed
out; the decoder never stalls waiting for a surface over a 10-minute play; photo → video →
photo leaks no textures.

### PR 5b — Audio and the clock
WASAPI render client, `swresample`, audio-master clock, drop/duplicate on drift, QPC fallback
for silent clips, device-change recovery, drift instrumentation in the F3 overlay. It is a
separate slice so `IMFMediaEngine` behind `IVideoSource` remains a clean escape hatch.

**Verify:** A/V drift flat over 30 minutes, with the overlay to prove it; unplugging the audio
device mid-playback recovers without stopping video; a clip with no audio track plays at
correct speed.

### PR 5c — Transport
Dual-mode seek, frame step, speed 0.25x–4x (chained `atempo`, `player/transport.cpp`), volume,
track selection, resume position, A-B loop, SMTC + media keys, scrub-preview thumbnails.
Bindings ([Commands](16-commands.md)): Space play/pause on a clip, `J` `K` `L`, `,` `.`.

**Verify:** scrubbing feels instant; frame step lands on exact frames in both directions; media
keys and the OS overlay work; resume returns to the right position.

### PR 6 — Viewer completeness
Fullscreen, slideshow as a mode (no transitions), fit/100 %/fill, animated GIF/APNG/WebP on the
QPC frame clock, Recycle Bin delete with confirm, drag-and-drop in and out, argv handling.

Keyboard-complete browse: one key router, a default map, `?` overlay, marks, copy-to / move-to
(`F7`/`F8`), status, typeahead, sticky zoom, loupe, hold-previous, display-referred clipping
blinkies, pixel grid, canvas background/checkerboard, always-on-top, fullscreen chrome hide.
Space is next-image (play/pause on video and animation). Settings (`Ctrl+,`) remaps the live
table. The `chrome_left_px` inset and the folder-tree command id exist from here; the tree
itself is PR 9 and companion hiding is PR 7. Full spec: [Commands](16-commands.md).

**Verify:** keyboard-only browse of a real folder — open, next/prev, zoom/fit/100 %, mark,
copy-to a destination, delete to Recycle Bin, fullscreen, slideshow start/stop — without the
mouse, with `?` listing those bindings; a file dropped into the folder appears without restart;
animation timing matches a browser; PR 1's present-loop still holds.

### PR 7 — The camera-dump formats
TIFF, WebP, ICO, HEIC/HEIF, AVIF — bundled, with the OS codec probed first. RAW via LibRaw with
the embedded preview as first pixel, then the full decode into the same texture slot with a
cross-fade. Tiled pyramid for images above ~64 MP. RAW+JPEG pairing and Live Photo pairing
(`io/pairing.h`): one filmstrip stop, JPEG/still as first pixel
([Image pipeline](04-image-pipeline.md)). Broken-file corpus and per-decoder libFuzzer
harnesses (`tools/fuzz`) in CI. Crash reporting: Crashpad, out-of-process, with the minidump
scrub (`shell/crash_reporter_win.cpp`, `tools/minidump-scrub`;
[Updates and telemetry](13-updates-and-telemetry.md)).

**Verify:** iPhone HEIC opens on a clean VM with no Store packs; a CR2/NEF/ARW shows in preview
time comparable to a JPEG and the full decode replaces it without a visible pop (gated by
`frametime --no-pop`); original RAW bytes unchanged; a RAW+JPEG pair is one filmstrip entry and
one arrow-key stop; an iPhone Live Photo is one entry and `;` plays the motion; nothing in the
broken corpus crashes or hangs; a deliberately corrupted RAW produces a minidump containing no
path, filename, or pixel data.

## Milestone C — It ships (PR 8)

### PR 8 — Package & ship
Packages the PR 1–7 viewer. App identity: one `.ico` (16 / 20 / 24 / 32 / 40 / 48 / 64 / 256)
for the window, taskbar, wizard, Start Menu and shortcuts, its PNG in About, and `VERSIONINFO`;
PR 15 reuses it as each still ProgId's `DefaultIcon`. `settings.ini` writes run off the UI
thread.

A short first-install wizard (Inno Setup) lays down a per-user Velopack tree under
`%LocalAppData%\MediaViewer`, bundling the Windows App SDK and .NET runtimes so a clean machine
needs no prerequisite; Velopack handles every later update (staged rollout, signed manifest,
rollback). The signing seam covers the wizard, the binaries and the update manifest. About
dialog, `THIRD-PARTY.md` and the per-release LGPL source offer; the app is GPL-3.0-or-later, so
there is no Store MSIX channel ([Licensing](11-licensing.md)). Updates never re-open the wizard.
The wizard registers "Open with" / Default Apps candidacy and removes it on uninstall; it does
not ask about telemetry, which is opt-in, default off, and set from the app. Finish page:
Launch, GitHub, Licence. Every push to `main` is a release published to GitHub
([Updates and telemetry](13-updates-and-telemetry.md), [DEVELOPMENT](../DEVELOPMENT.md#package-and-install-pr-8)).

**Verify:** clean VM → run the wizard (no UAC) → Start Menu shortcut shows the app icon →
Launch from the finish page → open a real camera dump → browse photos, play video with audio
and transport, pan/zoom, fullscreen, and slideshow using the PR 1–7 feature set, with no
SmartScreen block and no missing-codec dialog anywhere. The running window and taskbar use the
same app icon. PR 7 format/pairing checks and PR 1's present-loop verify still hold for the
installed build. The finish page's GitHub link opens the repo. An update downloads and stages
without showing the wizard. Uninstall from Apps & features removes the shortcuts, the install
directory, and every association and handler. Update signature rejection and rollback work;
telemetry stays off unless explicitly enabled.

## Mac halves of PRs 1–8

The Mac host is a second host of the same core: decode, colour, EditStack, metadata and the C
ABI are shared; present, hardware decode, audio, I/O, chrome and the installer are per-OS
([Platforms](15-platforms.md)). It was built as five PRs, old numbers 16–20:

| Old number | Mac half of | What it built |
|---|---|---|
| PR 16 | PR 1 | Metal present lab: AppKit, `CAMetalLayer`, display link, F3, `frametime` on Darwin |
| PR 17 | PR 2 + PR 7 | Still decode, pan/zoom, the MSL blit twin, the rest of the D5 still formats, pairing detection |
| PR 18 | PR 3 + PR 4 + PR 6 | SwiftUI chrome in the AppKit window, folder/filmstrip/gallery + FSEvents, keyboard-complete browse |
| PR 19 | PR 5 (5a/5b/5c) | VideoToolbox + Core Audio clock + transport strip |
| PR 20 | PR 8, part of PR 15 | Notarized disk image, Sparkle, Finder types, Quick Look, the default-viewer sheet |

### Mac PR 1 — Metal present lab
AppKit window, `CAMetalLayer`, display-link pacing, idle → stop presenting, F3 overlay,
`frametime` on Darwin. Kept as the Mac debug harness.

**Verify:** presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle, on
Apple Silicon, measured from the Metal / display-link side.

### Mac PR 2 + PR 7 — Still decode, pan/zoom and the camera-dump formats, on Metal
Same ABI and decoders as PR 2; immutable Metal texture upload from the worker pool; fit /
wheel-zoom-toward-cursor / drag-pan; the MSL twin of the blit shader. The PR 7 still set: GIF /
APNG, TIFF, WebP, ICO, HEIC/HEIF, AVIF, RAW via LibRaw with embedded preview as first pixel.
RAW+JPEG and Live Photo pairing detection (`io/pairing.h`). Mac crash reporting is PR 11.

**Verify:** a 12 MP JPEG pans at refresh with zero decode on mouse move; a tagged AdobeRGB JPEG
renders correctly and an untagged one is treated as sRGB, with no tone-map applied to either;
iPhone HEIC opens with no extra codec install; a CR2/NEF/ARW shows a preview in JPEG-comparable
time and the full decode replaces it without a visible pop; original RAW bytes unchanged;
nothing in the broken-file corpus crashes or hangs.

### Mac PR 3 + PR 4 + PR 6 — SwiftUI chrome, folder/filmstrip/gallery, keyboard-complete browse
Mac PR 1's AppKit window and `CAMetalLayer` stay as the canvas; SwiftUI chrome is hosted in
them. Folder listing + sort, `FSEvents` dir watch (`io/dir_mac.cpp`), filmstrip and gallery as
SwiftUI views over the same folder model and the same `jpg512.1` thumbnail cache, and PR 6's
keyboard-complete browse: one key router with a Mac default map (`⌘` for `Ctrl`), `?` overlay,
marks (badge on cells, count in the command bar), copy-to/move-to, Trash with confirm,
drag-and-drop in and out, argv handling, fullscreen, slideshow as a mode, fit/100 %/fill,
animated GIF/APNG/WebP on the display-link frame clock. Same command ids as Windows
([Commands](16-commands.md)); the Mac host has its own default map.

**Verify:** zero dropped frames while panning a cached image at display refresh, unchanged
from Mac PR 2 with chrome on screen. Focus and keyboard traversal cross the SwiftUI / canvas
boundary; a popover opens over the canvas without clipping. 2000 mixed JPEGs — filmstrip
scrolls without a hitch, second folder visit has near-instant thumbnails; keyboard-only browse
of a real folder — open, next/prev, zoom/fit/100 %, mark, copy-to, delete to Trash, fullscreen,
slideshow start/stop — without the mouse, with `?` listing those bindings; a file dropped into
the folder appears without restart; animation timing matches a browser.

### Mac PR 5 — VideoToolbox + Core Audio + transport
FFmpeg + VideoToolbox on the app's `MTLDevice`, copied out of the decoder pool into an owned
presentation ring, NV12 and 10-bit sample paths, HDR→SDR in the MSL video shader, poster
thumbnails. Core Audio is the master clock. No `AVPlayer`. SwiftUI transport strip.

**Verify:** 4K 10-bit HEVC plays at full rate with VideoToolbox active, on a clean Mac with no
extra codec packs; an iPhone HLG clip looks correct; A/V drift flat over 30 minutes; photo →
video → photo leaks no textures.

### Mac PR 8 — Notarized disk image, Sparkle, and early Finder integration
`MediaViewer.app` as a universal app (Apple Silicon and Intel). UTIs for the D5 still set and
an "Open with" registration, never a silent default-app change; a first-launch default-viewer
sheet. Quick Look thumbnails in a separate process (`shell/quicklook_mac.mm`). Notarized
Sparkle updates. First install is a branded drag-install `.dmg` that shows the GPL on mount
([Updates and telemetry](13-updates-and-telemetry.md#macos-first-install--a-branded-disk-image)).

**Verify:** double-clicking a HEIC in Finder opens the app; a deliberately corrupted HEIC in a
browsed folder leaves Finder running; a clean Mac → mount the notarized image (GPL shown) →
drag to Applications → open a real camera dump, with no Gatekeeper block and no codec dialog;
Sparkle takes N → N+1 silently and refuses an appcast signed with any other key; dragging the
app to the Trash removes the Quick Look extension.

## Milestone D — Viewer and editing updates (PR 9–12, both platforms)

### PR 9 — Metadata (read)
**Shared:** Exiv2 + libavformat into one property model, per-stream video inspection, AF-point
quads from maker notes, sort by date taken in the folder model, the eyedropper sample (a
canvas-side read of the displayed texture), the info-overlay fill (exposure triangle) computed
in the core, and folder-tree data (a directory enumeration that follows the watcher). ABI 0.6.

**Windows:** WinUI summary card, searchable full tree and video stream inspector; the folder
tree as a left island, hidden by default; sort menu; `Ctrl+C` copy. The panes float over the
canvas (`chrome_left_px` stays 0). **macOS:** the SwiftUI metadata pane with the same card,
tree and inspector; the folder tree as a SwiftUI sidebar over the same enumeration.

`I` focuses the metadata pane on both ([Commands](16-commands.md)). The tree is rooted at the
open folder and navigates from the keyboard.

**Verify (both platforms):** JPEG with EXIF, PNG with XMP, HEIC, a CR2/NEF/ARW and an MP4 all
populate; missing metadata renders as empty fields, never an error; toggling AF points and the
info overlay does not re-read the file; sort by date taken orders a mixed folder identically on
both platforms. The folder tree opens from the keyboard and navigates without the mouse. The
present-loop gate for each platform still holds with the pane open.

### PR 10 — Geometry edits + export
**Shared:** `EditStack` (`edit/edit_stack.h`), the rotate/flip/crop/straighten/resize op chain,
export with a metadata preservation policy that carries HEIC/TIFF/RAW/WebP metadata, and
lossless JPEG rotate and MCU-aligned crop via libjpeg-turbo `transupp`
(`edit/lossless_jpeg.cpp`). Output is atomic and never touches the original, through the `io`
replace port: `io/replace_win.cpp` (`ReplaceFileW`) and `io/replace_mac.cpp` (same-directory
temp, `F_FULLFSYNC`, `rename`). Crop mode's overlay is drawn by the canvas.

**Windows:** crop mode and export dialog in WinUI. **macOS:** SwiftUI crop mode and export
sheet. Geometry runs through the shared op graph on both.

`[` `]` in the viewer run lossless rotate without opening the adjust pane; `H`/`V` flip; crop is
a mode (`Enter` commit, `Esc` cancel).

**Verify (both platforms):** crop + export a JPEG, and the on-disk dimensions and EXIF
orientation match; reset returns the original pixels exactly; lossless rotate produces a file
with no recompression; keyboard-only rotate of a JPEG in the viewer writes that file. The same
crop on the same JPEG exports byte-identical on Windows and Mac.

### PR 11 — Colour adjusts (first editing set), plus Mac crash reporting
**Shared:** exposure/contrast/saturation/temperature/tint (`edit/adjust.cpp`), histogram and
clipping reduction (a CPU histogram on a worker, `edit/histogram.cpp`), RAW linear develop, and
the full-resolution export bake (`edit/bake.cpp`), in the linear FP16 working space. One kernel
source feeds the HLSL shader, the MSL shader and the CPU bake.

**Windows:** HLSL on the live preview; WinUI adjust pane. **macOS:** MSL twin; SwiftUI adjust
pane. `Shift+A` (`⇧A`) opens and focuses the adjust pane (`E` is the clip transport). Viewer `C`
blinkies are accurate on RAW once the full decode exists. The adjust pane stays disabled until
LibRaw's full decode completes ([Photo editing](07-photo-editing.md)).

**Mac crash reporting:** Crashpad, out-of-process (`shell/crash_reporter_mac.mm`), with the same
minidump scrub as Windows (no path, filename, username, pixel data or EXIF). Uncaught
`NSException`s and Swift runtime traps are recorded with the correlation id of the native call
in flight. There is no upload endpoint on either platform.

**Verify (both platforms):** dragging a slider is shader-only with no re-decode, updating
within one refresh interval on a 45 MP RAW; export matches the preview within 8-bit rounding.
A fixed slider set exported from the same source on both platforms matches within 8-bit
rounding; HLSL and MSL that disagree fail.

**Verify (macOS, crash reporting):** a deliberately corrupted RAW opened on Mac produces a
minidump from the out-of-process handler. The canary scan (a marked copy in
`PRIVATE_FOLDER_canary/SECRET_FILENAME_canary…`, plus a pixel-pattern companion) finds no path,
filename, username or pixel data in it. A forced `NSException` in the chrome is captured with
the correlation id of the native call in flight. The app relaunches cleanly after each crash.

### PR 12 — Metadata (write)
Rating, orientation and user comment. **Shared:** the Exiv2 writer, a snapshot before the first
write in a session (revert), maker notes preserved, an XMP sidecar for RAW that never rewrites
the original, and pending writes flushed at exit. The atomic replace uses PR 10's `io` port.
From PR 29 the same writer, sidecar rule and snapshot back single-tag edits in the Edit
workspace.

**Windows:** numpad `0`–`5` (or `Ctrl+Shift+0`–`5`) write the rating; the comment is edited in
the WinUI pane. **macOS:** `⌘⇧0`–`5` write the rating, and keypad `0`–`5` where a keypad
exists; the comment is edited in the SwiftUI pane. Number-row `0`/`1` stay zoom on both.

**Verify (both platforms):** write-then-read round-trips preserve maker notes byte-for-byte
across the corpus; a process killed mid-write leaves the original intact; rating a JPEG from
the keyboard round-trips without opening the pane. A rating written on one platform reads back
identically on the other (the same file, or its XMP sidecar, copied across).

## Milestone E — Video and OS integration updates (PR 13–15, both platforms)

### PR 13 — Two-path trim
**Shared** (`edit/clip*`, ABI 0.10 `mediaviewer_clip.h`, `shell/trim_state`): in/out model,
keyframe index for the scrub-bar grid, Path 1 keyframe trim (FFmpeg stream copy), the
cancellable job queue and the A–B loop preview. Encode and decode jobs run in the
`MediaViewerClipJob` helper process (`tools/clipjob`).

Path 2 is a hardware re-encode behind the `encode` port (`edit/hwencode_{win,mac,none}.cpp`),
built on FFmpeg's hardware encoder wrappers: NVENC / Quick Sync / AMF / MF on Windows,
VideoToolbox on the Mac. Audio is stream-copied on Path 2. No software H.264/HEVC encoder is
linked. Both paths are labelled in the UI, Path 2 as slower.

**Windows:** WinUI transport overlay, trim mode and Jobs pane. **macOS:** the SwiftUI twins.
Trim mode takes `[` `]` for in/out.

**Verify (both platforms):** keyframe trim of a 1 GB MP4 completes in seconds with proportional
output size; the re-encode path is frame-accurate; the source file is never modified;
cancelling leaves no partial output. On Mac, the re-encode shows VideoToolbox active, and no
software encoder is linked.

### PR 14 — Extract & remux
**Shared, in the core:** lossless rotate (container matrix, no re-encode), split,
remove-middle, MKV ↔ MP4 remux, frame → PNG/JPEG, audio extract, and clip → GIF/WebP with a
two-pass palette over two decodes. Frame, GIF and WebP pixels are sRGB (HDR tone-mapped, P3
converted). The Windows flyout and the Mac sheet share one packed request.

**Verify (both platforms):** each operation round-trips; lossless rotate does not re-encode.

### PR 15 — OS integration
**Both hosts:** `Ctrl+C` / `⌘C` (files), `Ctrl+Shift+C` / `⌘⇧C` (path), `Ctrl+Alt+C` / `⌘⌥C`
(the still flattened to a PNG with its ICC in the app's own `clipboard` folder, one folder per
copy, newest four kept), `Ctrl+Shift+S` / `⌘⇧S` (Share: `IDataTransferManager` /
`NSSharingServicePicker`), `Ctrl+E` / `⌘E` (reveal in Explorer / Finder). `Ctrl+Alt+drag` /
`⌘⌥-drag` drags the edited copy out (Windows bakes first, then a `CF_HDROP` drag; the Mac uses a
file promise). Soak and scripted runs never record recent folders.

**Windows:** recent folders in the jump list; taskbar thumbnail transport buttons; one
AppUserModelID (`MediaViewer.Viewer`) for the process and its shortcuts. Associations via
`ProgId` / `OpenWithProgids` and a Default Apps deep link, never `UserChoice`; the default-viewer
offer is on the wizard's Finish page and confirmed in Settings. Explorer thumbnails:
`MediaViewerThumbs.dll` (`src/shellext/`), an `IThumbnailProvider` over `IInitializeWithStream`
on the `MediaViewer.Image` ProgId, run in the system `dllhost` surrogate from a versioned copy
under `<root>\shellext` that carries the DLL's full PE import closure; at most four decodes run
at once and an abandoned decode pins the DLL. Its request path is portable, tested on macOS and
fuzzed (`fuzz_thumbnail`). Single instance: a named pipe claimed right after option parsing, so
a second start (including Explorer's one-process-per-file launch) forwards its paths to the
running window (`shell/single_instance_win.cpp`).

**macOS:** builds on Mac PR 8's UTIs, Quick Look and default-viewer sheet. Dock menu of recent
folders; Now Playing / `MPRemoteCommandCenter` transport; a second open goes to the running
instance via Launch Services. `MediaViewerSpotlight.mdimporter` in `Contents/Library/Spotlight`
indexes MKV / WebM / AVI / TS, the containers macOS does not index itself
(`shell/spotlight_importer_mac.mm`, `shell/spotlight_fields.cpp`).

**Verify (Windows):** double-clicking a HEIC in Explorer opens the app, and Explorer shows its
thumbnail and the MediaViewer file-type icon; a deliberately corrupted HEIC in a browsed folder
leaves Explorer running; uninstall removes every association. The running window and taskbar
button use the app icon, not the default exe. Accepting the default-app offer opens Default
Apps rather than writing `UserChoice`; declining leaves existing defaults unchanged. Opening a
second file from Explorer while the app runs opens it in the running window; no second window
or process stays.

**Verify (macOS):** Mac PR 8's Finder verify still holds. The Dock menu lists recent folders
and opens one. A second open of a file goes to the running instance and opens in its window.
Transport from the Now Playing controls drives the clip. `⌘C` of a still pastes as a file in
Finder. Share opens the system picker with the file. Dragging the app to the Trash still
removes every extension. The Spotlight importer runs inside `mdworker` only for the installed,
Developer ID-signed app.

## Milestone G — Import add-on (PR 16–19, both platforms)

An optional add-on installed from Settings: copy a card or folder into a library, skip content
duplicates (size, then BLAKE3, never the name), verify every copy, sort by date taken, keep
pairs and sidecars together, resume after an unplug, and back up to a second drive from one
read. The base installer and updates never carry it. Code: `src/addon` (signed manifests,
install / remove, loader, host function table), `src/addons/import` (scanner, planner, engine,
`mv.import.1`), the io ports (`io/verified_copy`, file, `io/volume_*`), WinUI chrome
(`IslandHost.Addons.cs`, `MediaViewer.Import.Chrome`), SwiftUI chrome (`AddonsView.swift`,
`src.swift/ImportChrome` → `Import.bundle`), and `tools/package/addon-pack.py`. Tests:
`mv_import_tests`. Full design and verify lines: [Import](18-import.md).

- **PR 16 — Add-on mechanism + import engine.** Signed add-on install/remove in Settings, the
  host function table, the engine (duplicates, library index, verify, overlap, units, resume)
  and a minimal sheet. The base app's `F8` never deletes a source before its copy is verified.
- **PR 17 — The Import window.** Sources with "N new" counts, a day-grouped grid, a preset panel
  with a **Where files go** preview, ETA, pause/cancel, background running, summary, eject.
  Keyboard-complete.
- **PR 18 — Presets, backup, layouts, rename.** Named and per-card presets, opt-in auto-import,
  a second destination from one read, layouts, rename templates, camera sidecars.
- **PR 19 — Library tools.** Library-wide duplicate scope, import history, verify-a-folder.
- **PR 54 — Find duplicates.** A folder and every folder under it, every file type, grouped by
  identical bytes (size, then BLAKE3); open, show, or move a copy to the Recycle Bin / Trash,
  never the last. Verify line: [Import, "Find duplicates"](18-import.md#find-duplicates-pr-54).

The AI pack (PR 20) installs through the same add-on mechanism. Each slice's verify includes
both present-loop gates held while importing.

## Milestone H — Local AI search (PR 20–24, both platforms)

A downloadable add-on installed from Settings (Add-ons → Local search) through PR 16's
mechanism; the base installer, disk image and updates never carry it. Installed pieces total up
to 3 GB (the index is separate user data). Windows runs ONNX Runtime on CPU plus vendor
providers; the Mac runs ONNX Runtime with the Core ML provider. Code: `src/infer`,
`src/addons/ai`, `MediaViewer.Ai.Chrome`, `src.swift/AIChrome`. Full design, models, index,
yield policy and verify lines: [Local AI search](17-local-ai-search.md).

- **PR 20 — Inference host and the AI pack.** `src/infer`, ORT CPU + a vendor provider, signed
  pack download/verify, per-piece Install/Remove/Update and an Auto / provider / CPU-only toggle.
- **PR 21 — Video sampler and index.** Keyframe sampling with gap limits, embedding dedupe,
  remembered roots, incremental/resumable background `index.db`, **Index this folder and
  subfolders**, live progress/ETA, yield to playback.
- **PR 22 — Search and results.** Natural-language query over photos and video, results in the
  gallery, `Enter` seeks to the moment, match markers on the scrub bar, a query syntax for
  people, words said, `file:` and filters; keyboard-complete.
- **PR 23 — Find-similar, index management, hardening.** Includes index export/import.
- **PR 24 — Faces.** A local, opt-in, deletable People index with merge and refinement.
- **Audio.** A separate `ai-audio` piece: what a clip sounds like (CLAP) and what is said in it
  (Whisper), Pictures / Sound / Both per folder.

On the Mac the Photos library is also an index source (issue #72). Each slice's verify includes
both present-loop gates held while indexing.

## PR 26 — Multi-folder browsing

### PR 26 — Folder tiles, breadcrumb, up
The gallery shows the open folder's child folders as tiles. A folder of only folders uses big
tiles and opens the gallery on its own; a folder that also holds photos keeps one short row of
folders above them. A cover is the folder's own first photo, or one borrowed from a descendant
(bounded to depth 3 / 48 directories); the tile says when the photo came from further down,
when the level is only more folders, and when the look stopped early. Click or `Enter` opens a
tile. The path stays on screen while a photo is open; a long middle collapses until asked for.
`Ctrl/Cmd+Up` goes up and selects the folder left; `Ctrl/Cmd+Left/Right` opens the previous or
next sibling folder; `/` on the folder row finds a tile by name prefix. Synology `@eaDir`,
`#recycle`, `$RECYCLE.BIN` and dot-folders are never tiles; names sort naturally
(`Trip 2` < `Trip 10`).

Core: `io::list_subfolders` / `summarize_dir` (`io/dir_tree.cpp`, platform primitive
`scan_subdirs`), `shell::browse_path` (`shell/browse_path.h`, trail arithmetic),
`command_id::folder_up`. Chrome: SwiftUI and the WinUI gallery and breadcrumb.

**Verify:** open a root whose only contents are subfolders → the gallery opens on folder tiles
with covers and counts, no file I/O on the UI thread; click through three levels, the
breadcrumb and `Ctrl/Cmd+Up` walk back; a mixed folder shows tiles above images and Up/Down
crosses between them in the same column; keyboard-only (`Ctrl/Cmd+Up`, arrows, `Enter`, `Esc`)
reaches every folder; `test_dir_tree` and `test_browse_path` pass; PR 1's present-loop verify
still holds.

## PR 29–30 — Edit workspace and Video Editor (both platforms)

- **PR 29 — Edit workspace** (issue #39): the Edit button / `Enter`, a docked Edit pane, crop
  presets, and editing any single metadata tag of the file on screen through PR 12's writer
  (`shell/edit_workspace.cpp`). Design and verify: [Edit workspace](20-edit-workspace.md); the
  self-test rig is in [DEVELOPMENT](../DEVELOPMENT.md#edit-workspace-self-test-pr-29-macos-and-windows).
- **PR 30 — Video Editor window, one clip** (issue #40): its own window with a timeline, strip,
  waveform, split / delete / in / out, and keyframe or exact export (`shell/video_timeline.cpp`,
  `IslandHost.VideoEditor.cs`, `VideoEditorView.swift`). Design and verify:
  [Video Editor](21-video-editor.md).

## PR 49 — Network-speed copy engine

Positional I/O in the file port; a deep path in `io::verified_copy` (several requests in flight
per file, hashed in order, read-back the same) and several files at once, used only when an end
is a network share. `F8` and Import use it; cards and local disks keep the sequential path.
`tools/copybench` measures it. Design: [Transfer](24-transfer.md).

**Verify (both platforms):** on a 10 GbE SMB share, `copybench --mode auto` with verify (big and
RAW-sized sets) is >= 2x `--mode seq` on the same files, runs alternated; unverified auto is
>= 90 % of `robocopy /MT:16 /J` (Windows) / `ditto` (Mac); local `seq` within noise of the base
build; `mv_import_tests "[io]"` passes (the deep cases under ThreadSanitizer too); both
present-loop gates hold while an `F8` move to the share runs.

## Standalone PR 51 — The Photos library as a folder, its backup, and upright clips

Design: [26-photos-library.md](26-photos-library.md). Mac for the library (D9, like the source);
the rotation fix on both platforms. Once added in Settings, the library is a *Photos Library* row
in the folder tree and File → Open Photos Library, listing virtual items resolved as shown; the
backup copies every original, verified, into `YYYY/YYYY-MM-DD` with a manifest; clips with a
display matrix play upright and their posters turn with them (`jpg512.3`).

**Verify:** `mv_tests "[poster]" "[write_guard]" "[photos_backup]" "[folder][photos]"`; a 90°
fixture clip shows portrait and upright; with the library added, the row opens it in the gallery,
an iCloud-only item becomes its original after a moment, writes are refused, Copy To copies the
original; a backup to a folder, then a second run that writes zero bytes; the Mac PR 1
present-loop gate still holds.

## PR 55 — Open add-ons: packages and themes

Add-ons anyone can make, installed from a file or an `https` link (issue #79). One `.mvaddon`
file, a strict stored-only ZIP, signed by its publisher's own Ed25519 key; manifest schema 2;
the key pinned per add-on at first install, no downgrades, consent bound to the package's
SHA-256; a sheet in Settings → Add-ons → From others that says what the add-on adds, can and
cannot do, and that MediaViewer has not checked it. Contribution API 1 is themes: seven colour
tokens per palette and a font family for the chrome, one table both hosts read, behind a WCAG
contrast floor. A manifest that names code is refused. `tools/addon-sdk/mvaddon.py` (MIT) packs
and checks; `docs/ADDONS.md` is the author's guide. `.mvaddon` is registered with both OSes.
Code: `src/addon/package.*`, `open_manifest.*`, `theme.*`, `open_store.*`, `open_json.*`; ABI
0.17 `mv_open_addon_*` / bridge `mv_open_addons_*`; `Theme.swift`, `OpenAddons.swift`;
`IslandHost.ThemePack.cs`, `IslandHost.OpenAddons.cs`. Design, calls and measurements:
[25](25-open-addons.md).

**Verify (both platforms):** a package from the SDK with a fresh key installs from a file and
from an `https` link, shows publisher and fingerprint, themes the chrome, and Default restores
it exactly; each refusal is shown with its reason; an installed add-on changed on disk is not
used; the link request carries no cookie, query or identifier; launch → first pixel and → full
resolution within noise with the add-on installed and its theme on; 0 presents idle; both
present-loop gates; with none installed, no folder is created and nothing of an add-on is read
at start. Where each part stands:
[25 "Implementation notes"](25-open-addons.md#implementation-notes-pr-55-2026-09-29).

## Not built

- **PR 31** — Video Editor: several clips, zoom, dissolves ([21](21-video-editor.md)).
- **PRs 32–47** — the Editor add-on ([22](22-editor-addon.md)); only a design and an encoder
  spike exist.
- **PRs 27–28** — Voice query add-on ([19](19-voice.md)).
- **PR 50** — Transfer, the general copier, and the SMB link check (no `mv_transfer_*` ABI).
- **PRs 56–60** — the rest of the open add-ons ([25](25-open-addons.md)): declarative
  contributions (settings pages, keymap packs), a sandboxed script runtime (Lua), screens as
  data, slots and search providers, files. The owner's calls for them were made 2026-10-03
  ([25 §17](25-open-addons.md#17-decisions-owner-2026-10-03)). PR 56's first half is built
  (2026-10-04): Import's commands, keys, Settings line and card hint come from its manifest
  (`contributes`, eight `addon_cmd_*` slots, ABI 0.18), with an older Import kept working
  through the built-in rows.
- Windows grouped as tabs / `Ctrl+Tab` (split out of PR 15; Mac windows have tabbing disabled).
- The Windows property handler (split out of PR 15; it needs HKLM).
- Backlog: further photo adjusts and local edits, full RAW develop, smart cut, HDR output,
  formats outside the D5 set (JPEG XL, EXR, PSD, SVG, DDS, JPEG 2000, VVC), batch metadata
  tools, JSON keymap import/export (planned as keymap packs, PR 56), a canvas and F3-overlay
  colour scheme and a user font file (the chrome's themes landed as add-on themes in PR 55),
  AppContainer decode process, per-machine MSI, Windows ARM64.
