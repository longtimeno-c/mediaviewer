Build, test and status notes for contributors. The product overview is in the [README](../README.md); links below are relative to the repo root.

# MediaViewer: development notes

A Windows viewer for a real camera dump — photos and video in one folder. Opens everything
instantly and pans without a dropped frame. The first release ships the viewer through
PR 7, packaged in PR 8; metadata tools, photo edits/export, video trimming, and additional
Windows integration follow in future updates. **v1 is Windows.** From PR 4 the native core is
kept hostable; macOS is Milestone F (the Mac halves of PRs 1–8), a host of the same core, not a UI-only
port — see [docs/design/15-platforms.md](design/15-platforms.md).

**One PR number per feature, on both platforms (2026-09-24).** The Mac host, first built as
PRs 16–20, is now filed as the Mac halves of PRs 1–8, so **Windows and Mac are both at PR 9**.
From here every PR lands on both, with one shared core change, a WinUI half and a SwiftUI half,
and a verify line on each platform (D9, amended). The order is:

| PR | What | State |
|---|---|---|
| 9 | Metadata read: pane, overlays, AF points, eyedropper, folder tree, date-taken sort | **In progress** (Mac ahead; Windows owes the XAML pane, tree island, sort menu) |
| 10 | Geometry edits + export, lossless JPEG rotate | Mac half started on a branch |
| 11 | Colour adjusts, plus Mac crash reporting (Crashpad + the same scrub as Windows) | Planned |
| 12–15 | Metadata write · two-path trim · extract & remux · OS integration | Planned |
| 16–19 | **Import add-on**: copy cards with content-hash duplicate skip, verify, date folders, backup, resume ([docs/design/18-import.md](design/18-import.md)) | In main, optional download; release packing is `tools/package/release-addon.patch`, to apply; hardware verify owed |
| 54 | **Find duplicates** in the Import add-on: a folder tree compared by content, one copy at a time to the Recycle Bin / Trash, never the last ([docs/design/18](design/18-import.md#find-duplicates-pr-54)) | Engine tested on the Mac (Release, TSan); Swift and C# compile; Windows native build, both live windows and both gates while scanning owed |
| 20–24 | Local AI search add-on, both platforms (Core ML on Mac) ([docs/design/17-local-ai-search.md](design/17-local-ai-search.md)) | Built and tested on both with the real pack (PR #59); quiet-machine gates and the Mac in-app walk-through owed. Mac Photos library as a source (issue #72): in main (PR #86). [docs/design/26](design/26-photos-library.md) (2026-10-03): the library as a folder (virtual list items), its backup to a folder or NAS, and the display-matrix fix for clips on both platforms; engine tests pass, the in-app Photos walk-through needs a build with Photos access |
| 27–28 | **Voice query add-on**: speak a Local search query, on-device, as its own download ([docs/design/19-voice.md](design/19-voice.md)) | Proposed |
| 29 | **Edit workspace**: an Edit image / Edit video button, a docked Edit pane, crop presets, every metadata tag editable ([docs/design/20-edit-workspace.md](design/20-edit-workspace.md)) | Both halves written and run on their platform (PR 54); Mac build of the merged tree, the quiet-machine present-loop gates, Narrator / VoiceOver owed |
| 30 | **Video Editor**: its own window with the viewer's canvas as the preview, a timeline (thumbnails, waveform), Split / Delete, marked ranges (`I` `O`, Delete, `X`), Trim start / end (`[` `]`), draggable piece edges, `J K L` shuttle, frame timecode, Undo, Export as keyframe cuts or exact on the hardware encoder; ABI 0.13 `keep_ranges` ([docs/design/21-video-editor.md](design/21-video-editor.md)); the Editor add-on is proposed ([docs/design/22-editor-addon.md](design/22-editor-addon.md)) | Both halves written and run on their platform (PR 55, `MV_EDIT_SELFTEST`, a key walk on Windows); present-loop gates with the editor open, an interactive-desktop pass, Narrator / VoiceOver and encoder spike S1 on Windows owed |
| 49 | **Fast network copies**: F8 and Import to or from a share keep several requests and files in flight, still verified; `copybench` measures it ([docs/design/24-transfer.md](design/24-transfer.md)) | Engine and Mac F8 built; `mv_import_tests "[io]"` passes, TSan-clean; Windows half compiled by CI only; the 10 GbE share run and the present-loop gates owed |
| 50 | **Transfer**: a general copier (copy/move any files and folders, verified, resumable) and an SMB link check | Planned |
| 55 | **Open add-ons**: add-ons anyone can make, one `.mvaddon` file signed by its publisher, installed from a file or a link with a sheet that says what it is; themes as the first thing one can contribute; the author's tool and guide ([docs/design/25-open-addons.md](design/25-open-addons.md), [ADDONS.md](ADDONS.md)); ABI 0.17 | Shared core and SDK tested on the Mac (ASan / UBSan) and cross-checked against each other; both halves run in the app (Mac: `MV_ADDON_SELFTEST`; Windows: by hand, 2026-10-03), launch and pacing measured against the base on both; install from a link, `.mvaddon` double-click on a rebuilt installer, VoiceOver / Narrator and the idle clause of both gates (which fails on the base too) owed. PRs 56–60 are planned; the owner's calls for 57–60 were made 2026-10-03 (docs/design/25 §17) |
| 56 | **Import described by its manifest** (the first half of [docs/design/25](design/25-open-addons.md)'s declarative contributions): its commands, keys, Settings line and card hint come from `manifest.json`; both chromes drive it through one generic run-command call; an older installed Import keeps working through the built-in rows; ABI 0.18 | Mac: shell and manifest suites, the packer cross-check, the full suite and the rig pass; a signed Import with the new manifest has not been loaded in the app. Windows: both C# projects compile; the native side is CI's |

See [docs/design/10-roadmap.md](design/10-roadmap.md). Old Mac numbers in the history below map as
PR 16 → Mac PR 1, 17 → Mac PR 2/7, 18 → Mac PR 3/4/6, 19 → Mac PR 5, 20 → Mac PR 8.

**Status: PR 7's slices are all merged and pass locally. Its clean-VM HEIC, real
Live Photo and on-screen no-pop checks now have gates around them
([Where this actually is](#where-this-actually-is)); what remains of those three
is a clean VM, a phone and one look at a RAW opening. PR 8 — packaging — is in progress:
the icon, About, the Inno wizard, the Velopack updater, opt-in telemetry and the bundled
runtimes are in the tree and build, the wizard installs and uninstalls cleanly on this
machine, and the signature-rejection suite passes. It has **not** been through the
clean-VM run its verify line asks for, and no artefact is signed — see
[Package and install](#package-and-install-pr-8). PRs 9–15 are next, on both platforms at once.
The owner widened the 2026-09-13 sequencing exception on 2026-09-17
([docs/design/12-decision-log.md](design/12-decision-log.md)) so Mac work (the Mac halves of PRs 1–8) no longer waits
on Windows PR 8 shipping; Mac PR 1 (Metal present lab), Mac PR 2 (decode + pan/zoom, folded in the
PR 7 formats/Crashpad scope) and Mac PR 3 (SwiftUI chrome, folded in the PR 4/PR 6
folder/filmstrip-backend/keyboard scope) are all in the tree. The Darwin target configures,
builds and links with a real toolchain (`cmake`+`ninja`+`vcpkg`+`swift build`) and its Catch2
suite passes (210 assertions, 60 cases). It has been run on a real Mac with a display
(2026-09-19): the 60 s present-loop gate passes with the chrome on screen, and the window,
menu bar, filmstrip, gallery and `?` sheet were driven by hand. Still unproven on Mac:
drag-and-drop, copy/move/Trash and slideshow on real folders, a *by-eye* check of animated
GIF/APNG/WebP playback (the render loop is traced cycling a 4-frame GIF at its 500 ms delays,
looping forever, but a screen capture of the Metal layer is not a reliable instrument, so
nobody has yet watched it), and the tonal step when a RAW's embedded preview is replaced
by the full decode — see [macOS](#macos-mac-prs-16) below. Mac PR 8 (MediaViewer.app: Finder open,
Quick Look thumbnails, Sparkle updates, the notarized disk image) is written but **not yet
built or run on a Mac** — see [MediaViewer.app](#mediaviewerapp-and-a-shippable-mac-build-mac-pr-8).**
The Windows present lab still owns
the Win32 window and D3D11 swapchain. WinUI 3 chrome is XAML islands on that
window: command bar (top) and filmstrip (bottom). Open a folder of JPEG/PNG/BMP/GIF/WebP,
TIFF/ICO/HEIC/AVIF/camera RAW **or video**; the strip virtualizes, thumbs come from a SQLite + JPEG-512 disk
cache, arrow keys move the selection. PR 5 puts video on that same swapchain —
FFmpeg demux and decode, D3D11VA on the lab's own device, NV12/P010 sampled and
tone-mapped in one shader, a WASAPI audio master clock, and a transport (seek,
frame step, speed, A-B loop, resume, media keys). Photos and clips are one
folder and one present path. PR 6 makes browse keyboard-complete: one key
router over a live command table, `?` shortcuts built from that table, a
Settings screen that remaps it, marks, copy/move-to, Recycle
Bin delete, fullscreen, slideshow as a mode, fit/fill/100 %, gallery drag-and-drop,
and animated GIF/APNG/WebP on the frame clock. PR 7 (in progress) adds the camera-dump
formats — TIFF and ICO (libtiff), HEIC/HEIF (libheif + libde265; the Windows HEIF codec
is used for plain HEIC stills only when the Store HEVC pack is present), AVIF still and
animated (libavif + dav1d), and camera RAW (LibRaw: the embedded preview is first pixel,
then the full decode) — plus RAW+JPEG and Live Photo pairing, and out-of-process
Crashpad crash reporting whose dumps are scrubbed of paths, filenames and the
username before anything could be sent (no upload endpoint exists yet), a
preview→full refinement that keeps the view and cross-fades instead of refitting,
and a tiled pyramid for images above 64 MP or wider than 16384 px. A broken-file
corpus (`mv_broken_tests`) runs in every CI build leg, and per-decoder libFuzzer
harnesses (`-DMV_FUZZ=ON` under clang-cl, `tools/fuzz/run.ps1`) run briefly on
pull requests and for longer nightly.

macOS is Milestone F ([docs/design/15-platforms.md](design/15-platforms.md)), a later
host of the same core — not a UI-only port. Mac PR 1 is the Metal present lab
(AppKit + `CAMetalLayer` + `CAMetalDisplayLink`). Mac PR 2 adds JPEG/PNG/BMP decode,
immutable Metal texture upload, fit / wheel-zoom-toward-cursor / drag-pan, and an MSL
twin of the blit shader, plus (folded in from Windows PR 7) the rest of the D5 still
formats and RAW+JPEG/Live Photo pairing detection. Crashpad and the Mac minidump scrub,
planned there, never landed with it; they are in the Mac half of PR 11 (below).
Mac PR 3 hosts SwiftUI chrome in the same AppKit window (the canvas stays Metal, never
ported): a command bar, a bottom filmstrip, and a full-grid gallery overlay, all driven
by an FSEvents-backed folder model and a JPEG-512 SQLite thumbnail cache sharing
Windows' `jpg512` spec (now `.2`, PR 10), lazy-loading thumbnails so a large folder doesn't stall the
scroll. Real folder navigation (argv, drag-and-drop-in, arrow keys and the rest of
docs/design/16-commands.md's Browse table), marks, copy/move-to, Trash delete, fullscreen,
a stills-only slideshow, and drag-out round out the folded-in Windows PR 4/PR 6 scope.
It also carries the **PR 9 metadata read** (macOS and Windows): `I` opens a pane with a summary card, a searchable tree of every EXIF/IPTC/XMP tag and, for clips, a per-stream inspector; `O` adds camera, exposure and date lines to the on-canvas info; `Shift+O` draws AF points; `Shift+I` is a one-pixel eyedropper; `⌘⇧E` shows a folder tree; View ▸ Sort By adds date taken. It does **not** yet handle rating/metadata *writes* (PR 12) or RAW-pairing UI. On Windows the same features are in: `I` (or View ▸ Metadata pane) opens the pane on the right, `Ctrl+Shift+E` (or View ▸ Folder tree) the folder tree on the left rooted at the open folder, `O` adds the camera/exposure/date lines, `Shift+O` draws AF points, `Shift+I` is the eyedropper, and `Ctrl+C` copies the eyedropper colour (or, with it off, the marked/current file(s) as a file drop). View ▸ Sort by and Settings offer name, date modified, size, type and EXIF date taken, ascending or descending; the choice is saved. Both panes float over the photo, so opening one never refits it.
On top of that, the **PR 10 geometry edits** (Windows and macOS, same core): `[` `]` rotate and `H` `V` flip a still — on a JPEG the file itself is rewritten *losslessly* (DCT coefficients rearranged, never re-encoded; atomic swap) — `Shift+C` crops and straightens, `Ctrl+Z` / `Ctrl+R` (`⌘` on Mac) undo / reset, and `Ctrl+S` opens an export dialog (format, quality, size, metadata) that writes `<name>-edit.jpg` beside the original with its metadata carried over (orientation and dimensions corrected). JPEGs are now displayed through their EXIF orientation, so thumbnails regenerate once. Neither host half has been compiled yet — see [docs/design/12](design/12-decision-log.md) 2026-09-24.
Then the **PR 11 colour adjusts** (Windows and macOS, same core): `Shift+A` (`⇧A` on Mac) opens an adjust pane with exposure, contrast, saturation, temperature and tint, a histogram and a clipped-highlights / crushed-shadows readout. Slider drags only change shader uniforms — nothing is re-decoded — and the colour is worked in linear light from an FP16 working image; for a RAW the sliders stay disabled ("Preparing…") until LibRaw's full linear develop is ready, never the embedded preview. Export (`Ctrl+S`) bakes the same maths at full resolution. The Mac half also brings **crash reporting**: Crashpad out of process, the Windows privacy scrub (now aware of `/Users/…`-style paths), and uncaught `NSException`s recorded with the id of the native call they happened in. PR 11's host halves are not verified on hardware yet — see [docs/design/12](design/12-decision-log.md) 2026-09-24 (PR 11).
Then the **PR 12 metadata writes** (shared core and the **macOS half**; the Windows half is written but not yet compiled or run): keypad `0`–`5` (or `⌘⇧0`–`5`, `Ctrl+Shift+0`–`5` on Windows once built) rate the photo on screen, and `⌘I` puts the keyboard in the metadata pane's comment field. A plain JPEG is rewritten in place, checked against the original before it replaces anything; every other format (RAW, HEIC, PNG, video, …) gets an `IMG_1234.xmp` sidecar beside it and the original is never opened for writing. The pane has clickable stars, the comment and a "Revert metadata" button. **On a Mac, `⌘⇧3`/`4`/`5` are the system's screenshot shortcuts and never reach the app; use the keypad or turn those shortcuts off.** See [docs/design/12](design/12-decision-log.md) 2026-09-25.

And **PR 13 / 14 clip editing** (Windows and macOS, same core), not yet built on either platform: on a clip, `Ctrl+T` (`⌘T`) arms trim — `[` `]` set in and out, the scrub bar shows the keyframe grid and what will be kept, `P` previews the cut as a loop, `Enter` saves an instant keyframe cut (stream copy, no quality loss) and `Shift+Enter` a frame-accurate re-encode on the GPU's hardware encoder (NVENC / Quick Sync / AMF / Media Foundation, VideoToolbox on Mac; labelled slower). `Ctrl+S` on a clip opens the clip tools: lossless rotate, split, remove in–out, MP4 ↔ MKV remux, save the frame as PNG / JPEG, extract the audio (copy, WAV or FLAC), and GIF / WebP. Every result is a new file beside the clip (`<name>_trimmed.mp4`, …); the original is never touched, and jobs run in a Jobs pane (`Ctrl+J`) where they can be cancelled without leaving a partial file. Anything that decodes or encodes runs in a separate helper process (`MediaViewerClipJob`), so a crash in a GPU driver fails that one job and never the viewer. The shared core is tested on Linux ([tools/portable](../tools/portable/README.md)); what is owed on each platform is in [docs/design/12](design/12-decision-log.md) 2026-09-25.

**PR 15 OS integration** is written on a branch (Windows and macOS, same command rows): `Ctrl+Shift+C` (`⌘⇧C`) copies the marked or current file's path as text; `Ctrl+Alt+C` (`⌘⌥C`) copies the photo as you see it, edits applied, as a PNG (both a file and an image, so it pastes into Explorer / Finder and into Word, Keynote or a chat; no EXIF rides along); `Ctrl+Shift+S` (`⌘⇧S`) opens the system Share sheet. The folders you open show up as **Recent folders** in the taskbar jump list and in the Dock icon's menu, in File ▸ Open Recent on the Mac, and as clickable rows on the empty window's welcome card (up to six, as many as the window has room for; a folder that has gone is dropped when clicked, and the × on a hovered row removes a folder from every recent list without touching it on disk). The card and the host share one layout (`src/shell/welcome_layout.h`), so the row drawn and the row hit are the same. The taskbar thumbnail gains previous / play-pause / next buttons, and on the Mac, Control Centre, the media keys and AirPods drive a clip through Now Playing. Explorer gets MediaViewer's thumbnails (HEIC, AVIF, RAW and the rest) for the file types you make MediaViewer the default for; the handler runs outside Explorer, so a damaged file can't take Explorer down. Explorer's Details-pane properties need a machine-wide install and are deferred. On the Mac, Spotlight learns the length, size and codecs of MKV, WebM, AVI and TS clips (macOS already indexes photos and MP4/MOV itself). `Ctrl+Alt`-drag (`⌘⌥`-drag) drags out the edited copy, and opening a file while MediaViewer is running opens it in the running window instead of starting a second one (`--new-instance` overrides). `Ctrl+N` (`⌘N`, File ▸ New Window) opens another window as its own process. Several windows grouped as tabs come in a later update ([docs/design/10](design/10-roadmap.md) PR 15). The macOS half is built, its tests pass and the Metal present-loop gate holds; the Windows half builds in CI. Neither platform's hands-on verify (Explorer / Finder, installed builds) has been run yet.
Windows DXGI soak is not that verify.

PR 1's present-loop verify and PR 3's island-on-screen verify are inherited and
not yet demonstrated on a quiet GPU runner, and PR 5's and PR 6's own verify
lines are only partly demonstrated — read
[Where this actually is](#where-this-actually-is) before believing any of it.
The keys below come from the command table specified in
[docs/design/16-commands.md](design/16-commands.md); press `?` in the app for the ones
that apply to what you are doing.

The Windows and macOS UI follows the system's light or dark appearance,
including Settings, browsing, editing panels and Import. Appearance changes
apply while the app is open. The empty welcome screen and dinosaur runner follow
the system window colour too. The photo/video **Canvas background** now defaults
to System; Grey, White, Checkerboard and Dark remain fixed choices. Build and
interactive checks are documented in [System appearance verification](system-theme-verify.md).

**Licence: GPL-3.0-or-later** ([LICENSE](../LICENSE)). Settled in PR 1; the reasoning is in
[docs/design/11-licensing.md](design/11-licensing.md).

---

## What is here today

| | |
|---|---|
| **`mediaviewer_lab.exe`** | A Win32 + DirectComposition window with a flip-model D3D11 swapchain. Open a folder, drop a JPEG/PNG/BMP/GIF/WebP or a clip, or pass a path on the command line. Animated GIF, APNG and WebP play on the render thread's frame clock, frame 0 first. Wheel-zoom toward the cursor, drag-pan, `0` fits, `1` is 100 %, `+`/`-` zoom, Left/Right browse. Video plays on the same swapchain as photos — never a `MediaPlayerElement`. Decode and ICC convert run on the worker pool; pan never re-decodes. `F` / `F3` toggles the frame-time overlay, which grows codec, decoder, A/V drift and present-counter lines while a clip is up. WinUI command bar (top) and filmstrip (bottom) are `DesktopWindowXamlSource` islands; the canvas is not a `SwapChainPanel`. |
| **`mediaviewer_core.dll`** | The native core behind a flat C ABI: job system, JPEG/PNG/BMP/GIF/WebP decode (giflib, libwebp), TIFF/ICO (libtiff), HEIC/HEIF (libheif + libde265), AVIF (libavif + dav1d) and camera RAW (LibRaw, embedded preview first), scan-time RAW+JPEG / Live Photo pairing, with animated GIF/APNG/WebP fed a frame at a time into a small texture ring, LCMS colour, immutable GPU upload, pan/zoom camera, folder listing, thumbnail cache, ±2 prefetch LRU, and the PR 5 video surface (open, transport, position/state/info/stats, magic-byte video probe). |
| **`MediaViewer.Chrome.dll`** | C# WinUI 3 chrome, loaded by the lab through hostfxr. Open (image or folder), View (zoom in/out, fit, 50 / 100 / 200 / 400 %, overlay), About, `ItemsRepeater` filmstrip, load indicator. Flyouts are supposed to open over the canvas without clipping — that is part of PR 3's verify. |
| **`frametime.exe`** | The frame-time regression harness. Runs a soak, writes a JSON report, compares against a rolling baseline, and fails on a dropped frame. |
| **`mediaviewer_lab` (Darwin)** | Mac PRs 1–6 Metal present lab. AppKit window, `CAMetalLayer` (max drawable 2 — Metal's minimum, see docs/design/12 — 8-bit sRGB), `CAMetalDisplayLink` wait-before-encode, idle → stop presenting, F3 overlay. Decodes a JPEG/PNG/BMP (plus the rest of the D5 stills) onto an immutable Metal texture; wheel-zoom-toward-cursor, drag-pan, `0`–`4` zoom presets, `+`/`-`, keyboard pan when zoomed, hold `Z` loupe, hold `\` previous, `C` clipping blinkies. Real folder browsing: argv/drag-drop opens a folder or a file (selecting it), `←`/`→`/`A`/`D`/`Space`/`Home`/`End`/`PageUp`/`PageDown` navigate it (every key goes through the same command table and key router as Windows, with `⌘` standing for `Ctrl` and the Mac Delete key for `Delete`), an FSEvents watch keeps the listing live. SwiftUI chrome hosted in the same window via a C bridge into the render thread's `input_snapshot`: a Windows-style command bar (Open / View / Settings / About, `?` at the right), a Settings screen (`⌘,`: filmstrip/wrap/sticky-zoom/background preferences and remappable keys, persisted in `NSUserDefaults`), a bottom filmstrip (`T` toggles) and a full-grid gallery overlay (`G` toggles), both lazy-loading JPEG-512 thumbnails from a shared SQLite cache. **Nested folders (PR 26):** child folders show as tiles. A folder of only folders uses big tiles; one that also holds photos keeps a short folder row above them. A tile says when photos were found further down, when it is only more folders, and when that look stopped early. The path stays on screen while a photo is open. `⌘↑` goes up and returns to the folder you left; `⌘←` / `⌘→` open the folder beside it; `/` on the folder row finds a tile by name. The Windows host matches that chrome. Marks (`Insert`/`Shift+Space`/`Ctrl+A`/`Ctrl+D`), copy/move to a chosen folder (`F7`/`F8`, collision-safe), Trash delete with confirm (`Delete`), fullscreen (`F11`/`F`), a stills-only slideshow (`F5`; `.` blackout, `R` shuffle), and drag-out (`⌘`+drag on the canvas; a plain drag from a gallery, filmstrip or search-result cell, the marks when that cell is marked, as the original files). **Video (Mac PR 5):** FFmpeg + VideoToolbox decode, copied out of the decoder pool into a presentation ring of our own Metal textures, an MSL twin of the video shader (NV12/P010, the stream's matrix/range/transfer, HLG/PQ tone-mapped to SDR), Core Audio as the master A/V clock (no `AVPlayer`), a SwiftUI transport strip and the docs/design/16 video keys, and poster thumbnails for clips. Metadata read (PR 9, see above); no rating/metadata writes or RAW-pairing UI yet. Not on the Mac yet: `⌘G` go to, `;` Live Photo motion, Open RAW / Open JPEG of a pair ([docs/design/16](design/16-commands.md) "Not built"). Builds on Apple Silicon and Intel, macOS 14+ (Intel: unverified for frame pacing). |
| **`MediaViewer.Interop`** | The C# side of the ABI — `SafeHandle`, struct layouts, completion drain. The filmstrip island borrows the session and drains folder/thumb completions. |
| **Import add-on** (Milestone G) | An optional add-on installed from Settings → Add-ons ([docs/design/18](design/18-import.md)): copy a card or folder into a library, skip what is already there by content (size, then BLAKE3), verify every copy by reading it back, sort into dated folders with RAW+JPEG / Live Photo pairs and camera sidecars kept together, resume after an unplug, back up to a second drive from one read, verify an old folder for silent corruption, and **find duplicates** in a folder tree (every file, grouped by identical bytes; PR 54), where one copy at a time can be moved to the Recycle Bin / Trash, never the last. Apart from that, never deletes from, formats or overwrites anything. `mv_import.dll` / `libmv_import.dylib` plus its chrome (`MediaViewer.Import.Chrome.dll` / `Import.bundle`) are built beside the app in `build/addons/import` and shipped as a separate signed download that a stable run of the release workflow packs beside the app once `tools/package/release-addon.patch` is applied (RELEASING.md); the base install does not contain them. Settings → Add-ons offers Install only when that download exists and verifies for the running app. With it absent, `Ctrl+Shift+I` / `Ctrl+Shift+F7` do not exist. The shared engine is tested; the Windows and Mac hosts are written but their first platform builds and every hardware verify line are owed (docs/design/10). Also from this work, in the base app: `F8` across drives now deletes the source only after a verified copy. |

## Build

You need **Visual Studio 2022** (or Build Tools) with the C++ workload, the **Windows 10/11
SDK**, **CMake ≥ 3.28**, **vcpkg**, the **.NET 8 SDK**, and the **Windows App SDK 2.4
runtime** (the command-bar island is unpackaged). The native core still builds and tests
with no .NET present; without `dotnet` on `PATH` the lab runs as it did in PR 2
(`--no-chrome`).

```powershell
# once
git clone https://github.com/microsoft/vcpkg $env:USERPROFILE\vcpkg
& $env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
./tools/install-windows-app-runtime.ps1   # unpackaged WinUI 2.4 runtime

# configure and build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

CMake finds vcpkg from `VCPKG_ROOT`, or from `%USERPROFILE%\vcpkg`, or from an explicit
`-DCMAKE_TOOLCHAIN_FILE`. Through PR 5 the manifest pulls `imgui`, `catch2`,
`libjpeg-turbo`, `libspng`, `lcms`, `sqlite3` and `ffmpeg` (LGPL only —
`avcodec`, `avformat`, `avfilter`, `swresample`, `swscale`, `dav1d`; no
`--enable-gpl`, no x264/x265, enforced by `tools/licence-check.ps1`). The rest of
the v1 set arrives with the PR that needs it, listed in [`vcpkg.json`](../vcpkg.json).
The first configure after PR 5 builds FFmpeg, which is not quick.

Other configurations:

```powershell
cmake -S . -B build-asan -A x64 -DMV_ASAN=ON     # AddressSanitizer
cmake -S . -B build-clang -A x64 -T ClangCL      # clang-cl, the CI second opinion
```

`-DMV_ASAN=ON` needs the **C++ AddressSanitizer** component
(`Microsoft.VisualStudio.Component.VC.ASAN`) in the Visual Studio Installer — the
ASan runtime is a DLL that ships beside `cl.exe` and nowhere else. The build
copies it next to every executable, so `ctest` and a double-click both work
outside a developer prompt. Configure says so and stops if the component is
missing, rather than producing a tree whose every test hangs for its full
timeout with nothing in the log.

### macOS (Mac PRs 1–6)

Apple Silicon or Intel, macOS 14+, CMake ≥ 3.28, vcpkg, a full Xcode install (Command Line
Tools alone are not enough — `swift build`'s SwiftUI target and `xcrun metal` both
need it), Swift 6. Build natively on the Mac you are on: the snippets below use the
`arm64-osx` triplets; on an Intel Mac use `x64-osx` and `x64-osx-dynamic` instead. The release
is one universal app made by building both and joining them (RELEASING.md). Intel builds
launch but the frame-pacing gate has not been measured on Intel hardware yet (docs/design/12,
2026-09-24). This path builds the native Mac app and its dynamic FFmpeg libraries; it does
not build WinUI or the Windows lab.

```sh
export VCPKG_ROOT=/path/to/vcpkg   # bootstrapped
brew install libomp
# Dynamic LGPL codecs use a separate manifest/tree; CMake installs the static
# permissive dependencies from the root manifest. See RELEASING.md.
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic \
  --overlay-triplets="$PWD/tools/mac/triplets" \
  --x-manifest-root="$PWD/tools/mac/dependencies" \
  --x-install-root="$PWD/build/vcpkg_dynamic"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DMV_VCPKG_DYNAMIC_PREFIX="$PWD/build/vcpkg_dynamic/arm64-osx-dynamic"
cmake --build build
./build/bin/mediaviewer_lab --open some.jpg
./build/bin/frametime --seconds 60 --lab ./build/bin/mediaviewer_lab
ctest --test-dir build --output-on-failure
```

This has been built and linked for real (not just reviewed) with `cmake` + `ninja` +
a manifest-mode `vcpkg` install (`imgui[metal-binding]`, `libjpeg-turbo`, `libspng`,
`lcms`, `sqlite3`, `catch2`) and `swift build` for the SwiftUI chrome — `mv_tests`
passes (210 assertions, 60 cases). **The macOS SDK matters**: once the filmstrip/gallery
pulled in SwiftUI's `Lazy*Stack`, linking against an SDK that doesn't match the Swift
toolchain's own SDK failed with "cannot link directly with 'SwiftUICore'" — use
`xcrun --show-sdk-path` (or the SDK your installed Xcode ships) rather than an older one
you may have lying around for a lower deployment target; `CMAKE_OSX_DEPLOYMENT_TARGET`
stays 14.0 either way. On a real display (2026-09-19) `frametime --seconds 60` passes with the SwiftUI chrome
on screen (3600 frames, 0 dropped, p99 16.9 ms, idle 0.14 % of one core, 0 presents). Not yet
exercised by hand: drag-and-drop, copy/move/Trash and slideshow on real folders — do a manual
pass before trusting them. Thumbnails are cached in `~/Library/Caches/MediaViewer/thumbs`,
never in the folder being browsed.

The whole D5 still set decodes on Mac: JPEG, PNG, BMP, GIF, APNG, TIFF, WebP, ICO, HEIC/HEIF,
AVIF, and camera RAW (CR2/CR3/NEF/ARW/DNG through LibRaw). Verified on real files (the CC0
RAW set in `tools/testmedia/raw-manifest.json`, the libheif example HEIC) plus generated
samples: every format thumbnails and opens on the canvas, and `mv_tests` runs the codec tests
including the RAW ones ("original bytes unchanged", cancel latency) when
`tools/testmedia/raw/` and `heif/` are populated (they skip otherwise; `fetch-raw.ps1` is the
Windows fetcher, the manifests are plain JSON). A JPEG or RAW shows its preview first (JPEG
DCT 1/4, or the RAW's embedded JPEG: about 100 ms on a 42 MP ARW) and the full decode
(about 5 s for that ARW) replaces it in place. HEIC always uses the bundled libheif here; an
ImageIO fast path is a later change to `codec/os_decode_mac.cpp`.

The Mac has a real menu bar (File / View / Go / Window / Help), `?` opens a shortcuts sheet,
and in the gallery `↑`/`↓`/`W`/`S` move by row, `Enter` opens the selection and `+`/`-` resize the
thumbnails.

**Video on Mac (Mac PR 5).** FFmpeg is LGPL and dynamic-link only. The dynamic manifest
installed above includes it alongside libheif/LibRaw; no extra install is needed.

Open a folder with clips in it. `Space`/`K` play/pause, `,` `.` frame step, `Q`/`E` ±2 s,
`J`/`L` ±10 s, `Shift+Q`/`Shift+E` speed 0.25–4×, `Shift+M` mute (`?` lists them; the transport
bar floats above the filmstrip while a clip is on screen and fades after 2.5 s of playback
with no activity — see the transport paragraph below). `F3` names the decoder that is
*actually* running (`SOFTWARE` is spelled out, never silent), the clock source, the A/V error
and the counters. H.264 and HEVC (8- and 10-bit) decode in hardware; MPEG-2 and MPEG-4 fall
back to software on Apple Silicon and say so.

`playprobe` is the headless pipeline check — it plays a clip on the system `MTLDevice` against
a 60 Hz timer and prints the decoder used, the presenter counters and the drift slope:

```sh
./build-darwin/bin/playprobe clip.mov --seconds 30 --expect hw --mute
```

Verified on an Apple Silicon Mac with generated clips (H.264, 4K60 10-bit HEVC, an HLG-tagged
HEVC, MPEG-2 TS, MKV, AVI, no audio): 4K60 10-bit HEVC plays at full rate through VideoToolbox
(P010 path) with 0 dropped frames; skip and frame-step land on the exact frame against a
burned-in timecode; repeated photo ↔ video navigation leaves memory and thread count flat.
Not yet verified: an iPhone HLG capture (only a synthetic HLG-tagged clip), VP9/AV1/WebM (no
encoder in the LGPL build to make a clip), and audio-device hot-swap.

`frametime` on Darwin requires `drop_source` `Metal display-link`. Copying a
Windows DXGI JSON report over is a failed gate, not a pass.

`F3` toggles the overlay, `Space` advances the folder (or pauses a running slideshow),
`R` resets the frame-time measurement, `Esc` closes the gallery / leaves fullscreen or
slideshow / quits, `0`–`4` are the zoom presets, `T`/`G` toggle the filmstrip/gallery,
`F11`/`F` fullscreen, `F5` starts a stills slideshow. Idle (`--static`) must park the
cursor off the window.

### MediaViewer.app and a shippable Mac build (Mac PR 8)

`mediaviewer_lab` stays the bare instrument `frametime` drives. The same sources also
build `MediaViewer`, the executable inside **MediaViewer.app**:

```sh
# a local, ad-hoc-signed bundle (no updater unless a key is given)
cmake --build build-darwin --target mediaviewer_app
open build-darwin/MediaViewer.app
```

The bundle holds the app, `MediaViewerThumbnails.appex` (Finder thumbnails for the D5
still set, run by Quick Look in its own sandboxed process, never inside Finder), the LGPL
dylibs in `Contents/Frameworks`, and, when configured, Sparkle. It registers the D5 still
and video types at rank *Alternate*: MediaViewer shows up in Finder's **Open With** and never
makes itself the default merely by being installed. First launch shows a setup sheet with
*Use MediaViewer for all supported photos and videos* checked. Continue applies that choice
through macOS; untick it or choose Not Now to keep current defaults. The MediaViewer menu
has the same command for later, and updates preserve the previous setup choice.

#### Runbook: build, sign, release, update (macOS)

Everything below was run end to end on Apple Silicon except notarization and Sparkle,
which need your Apple credentials — those steps say so. `docs/design/13` is the design; this is
the procedure.

**Universal (Apple Silicon + Intel).** The shipped app is universal, but each build below is
for the Mac you run it on. The release workflow builds once on each architecture and joins
the two apps with `python3 tools/mac/lipo_merge.py --arm64 <arm64 app> --x86_64 <x86_64 app>
--out <universal app>`, then runs `macpack.py release` on the result (RELEASING.md). To
reproduce that locally you need one build per architecture; a single build stays single-arch.
Intel has been built but not measured for frame pacing (docs/design/12, 2026-09-24).

**0. Prerequisites (once per machine)**

| Need | Notes |
|---|---|
| Full Xcode, macOS 14+ | Command Line Tools alone are not enough (Swift/SwiftUI, `xcrun metal`, `notarytool`) |
| CMake ≥ 3.28 | `python3 -m pip install --user cmake` puts it in `~/Library/Python/3.x/bin` — add that to `PATH` |
| Ninja | vcpkg downloads one under `$VCPKG_ROOT/downloads/tools`; or `brew install ninja` |
| OpenMP runtime | `brew install libomp`; the dynamic codec triplets below enable bounded parallel RAW decoding. Packaging bundles the runtime. |
| vcpkg, bootstrapped | `export VCPKG_ROOT=…`. The baseline is pinned in `vcpkg.json`; do not float it |
| Python 3.10+ | for `dmgbuild==1.6.7` (`pip install -r tools/mac/requirements.txt`). On 3.9, `dmgbuild==1.6.5` works for a local dry run only |
| Apple Developer Program | needed for Developer ID signing and notarization; without it you can only build the ad-hoc bundle |

**1. Build the app**

```sh
export VCPKG_ROOT=/path/to/vcpkg
brew install libomp
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic \
  --overlay-triplets="$PWD/tools/mac/triplets" \
  --x-manifest-root="$PWD/tools/mac/dependencies" \
  --x-install-root="$PWD/build-darwin/vcpkg_dynamic"

cmake -S . -B build-darwin -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DMV_VCPKG_DYNAMIC_PREFIX="$PWD/build-darwin/vcpkg_dynamic/arm64-osx-dynamic"
cmake --build build-darwin --target mediaviewer_app
open build-darwin/MediaViewer.app
```

That is an ad-hoc-signed bundle: fine on this Mac, but **it will not launch once
re-signed ad hoc with the hardened runtime** (dyld rejects the bundled dylibs with
"different Team IDs"). Only a real Developer ID signature, which gives every binary in the
bundle the same Team ID, satisfies that check. `python3 tools/mac/check_plists.py` and
`python3 tools/mac/test_macpack.py` need no Mac and run anywhere.

**2. One-time signing and update setup**

1. *Developer ID Application certificate.* In Xcode: Settings → Accounts → your team →
   Manage Certificates → **+** → *Developer ID Application*. (By hand: create a CSR in
   Keychain Access, upload it at developer.apple.com → Certificates choosing **Developer ID
   Application** and the **G2 Sub-CA**, then double-click the downloaded `.cer`.) Keep the
   private key on this Mac. Never commit `.cer`, `.p12` or CSR files (they are gitignored).
2. *Apple's G2 intermediate.* If `security find-identity -v -p codesigning` shows "0 valid
   identities" although the certificate is installed, the chain is missing:
   `curl -LO https://www.apple.com/certificateauthority/DeveloperIDG2CA.cer` and
   `security import DeveloperIDG2CA.cer -k ~/Library/Keychains/login.keychain-db`.
   The identity string it then prints — `Developer ID Application: <Team name> (<TEAMID>)` —
   is what `--identity` takes.
3. *Notarization profile.* Create an app-specific password at appleid.apple.com, then
   `xcrun notarytool store-credentials mediaviewer-notary --apple-id <id> --team-id <TEAMID>`
   (it prompts for the password, so it never lands in shell history).
4. *Sparkle keys.* Download the Sparkle 2.9.6 release tarball, run
   `./Sparkle-2.9.6/bin/generate_keys`, and keep the printed **public key**. The private key
   lives in your login keychain: **back it up** (`generate_keys -x file`). Lose it and no
   installed copy can ever accept another update.

**3. Cut a release build**

Reconfigure with the updater's key, and raise the build number every release — Sparkle
compares `CFBundleVersion` (`MV_MAC_BUILD_NUMBER`, default = the project version), so a
build that does not increase is never offered:

```sh
cmake -S . -B build-darwin -DMV_SPARKLE_PUBLIC_ED_KEY=<public key> -DMV_MAC_BUILD_NUMBER=<n>
cmake --build build-darwin --target mediaviewer_app     # fetches Sparkle into the bundle

# safe local rehearsal: signs and builds the .dmg/.zip, sends nothing to Apple
python3 tools/mac/macpack.py release --app build-darwin/MediaViewer.app \
    --identity "Developer ID Application: … (TEAMID)" --skip-notarize --allow-no-updater \
    --out-dir /tmp/mv-rehearsal

# the real thing: notarizes and staples the app and the image, and signs the appcast
cmake --build build-darwin --target mediaviewer_app     # release re-signs the app in place: rebuild first
python3 tools/mac/macpack.py release --app build-darwin/MediaViewer.app \
    --identity "Developer ID Application: … (TEAMID)" --notary-profile mediaviewer-notary \
    --sparkle-bin build-darwin/_deps/sparkle-2.9.6/bin \
    --download-url-prefix https://github.com/longtimeno-c/mediaviewer/releases/download/v<version>/
```

Output in `build/release` (or `--out-dir`): `MediaViewer-<version>.dmg` (drag to
Applications, the GPL shown on mount; opening the app straight from the image offers to
move it to Applications, relaunch it and eject the image), `updates/MediaViewer-<version>.zip` and
`updates/appcast.xml`. `--phased-rollout-seconds` spreads an update over time.
Notarization uploads the app and image to Apple and takes a few minutes; without a
`--sparkle-bin` no appcast is written.

On first launch the app asks once whether to become the default for every type in its
`CFBundleDocumentTypes` (photos and videos). It records what it offered
(`MVDefaultViewerTypes`) and whether the user took it (`MVDefaultViewerChosen`). When a
release adds a type and the user had said yes, the next launch sets that type too, off the
main thread; a type the user later moved to another app in Finder is left alone. Installs
from before the record count as a yes if MediaViewer still opens one of their photo types.

**4. Verify the artefacts**

```sh
codesign --verify --deep --strict --verbose=2 build-darwin/MediaViewer.app
spctl --assess --type execute --verbose=2 build-darwin/MediaViewer.app     # "Notarized Developer ID"
xcrun stapler validate build/release/MediaViewer-<version>.dmg
# the image has a licence page, so a headless mount must accept it:
mkdir /tmp/mvdmg && yes | hdiutil attach -nobrowse -readonly -mountpoint /tmp/mvdmg build/release/*.dmg
ls /tmp/mvdmg            # MediaViewer.app  Applications
hdiutil detach /tmp/mvdmg
```

Copy the app out with `ditto --norsrc --noextattr`, not `cp`/`ditto` defaults; Finder
metadata copied along invalidates the signature. Then open a photo and a clip from the
copy, and check Finder's **Open With** and a Quick Look thumbnail.

**5. Publish, and later update**

1. Tag and create a GitHub release `v<version>` and mark it **latest**: the app's feed is
   `…/releases/latest/download/appcast.xml`. Upload the `.dmg`, the `.zip` and
   `appcast.xml`.
2. To ship a new version: bump `project(… VERSION x.y.z)` in `CMakeLists.txt` (the marketing
   version), raise `MV_MAC_BUILD_NUMBER`, rebuild `mediaviewer_app`, rerun step 3 with the
   new `v<version>` in `--download-url-prefix`, and upload the new files to a new release.
   Keep earlier zips in `updates/` so `generate_appcast` keeps them in the feed.
3. Test the update path before announcing: install the *previous* release from its `.dmg`,
   publish the new one, and confirm the old build's command bar, right after About, shows
   *Checking for updates…*, then *Downloading update x.y.z…* with a bar, then **Update ready
   — restart**.
   Installed copies accept only a feed and archive signed with the key baked in at build
   time.
4. First install is the disk image; every update after that is the zip, applied by Sparkle
   on **Update ready — restart** or on quit.

`python3 tools/mac/check_plists.py` and `python3 tools/mac/test_macpack.py` need no Mac and run anywhere.

## Run

```powershell
.\build\bin\Release\mediaviewer_lab.exe
.\build\bin\Release\mediaviewer_lab.exe path\to\photo.jpg
.\build\bin\Release\mediaviewer_lab.exe path\to\folder
```

| Key | |
|---|---|
| `F11` / `F` | fullscreen on the window's monitor; also available from View → Full screen. Hides the command bar and filmstrip; a clip's transport still floats and auto-hides, and the pointer hides with it over the video. `Esc` leaves |
| `F3` | frame-time overlay — off at launch on Windows and macOS unless a soak is running |
| Empty-window runner | `Space` starts/jumps/retries; `3` switches between the default 2D view and a shaded 3D view; `Esc` leaves with a short outro. Switching views keeps your run and score |
| `Space` / `Backspace` | next / previous. On a clip, `Space` is play/pause. It is no longer the lab sweep |
| `Home` / `End` | first / last in the folder |
| `PageUp` / `PageDown` | back / forward ten |
| `Ctrl+PageUp` / `Ctrl+PageDown` | previous / next page of a TIFF, PDF or DOCX (`⌘` on the Mac). Within a page: zoom (wheel, `+` `-`, `1`–`4`) and drag to pan. On a PDF or DOCX with the filmstrip hidden the wheel / trackpad scrolls and turns pages at the edges; `⌘`/`Ctrl`+wheel or a pinch zooms |
| `Enter` on a PDF or DOCX | open it in its default app (the bar's **Open in <app>**, ▾ for the others) |
| `J` / `K` / `L` | clip transport: −10 s / pause / +10 s ([docs/design/16](design/16-commands.md)) |
| `,` / `.` | frame step back / forward while paused |
| `A` / `D` | previous / next beside the arrows, in every mode including on a clip |
| `Q` / `E` | on a clip, two commands on one key: **tap** skips ±2 s, **hold** skims ±2 s per key repeat and settles on an exact seek when released. `Shift+Q` / `Shift+E` step playback speed. Off a clip they do nothing |
| `R` | reset the measurement window |
| `0` | fit to window |
| `1` | 100 % |
| `2` / `3` | 200 % / 400 % |
| `4` | fill: the image covers the canvas |
| `Ctrl+0` | reset pan and zoom (fit) |
| `Up` / `Down`, `Shift+arrows` | pan when zoomed in (`Left` / `Right` alone still walk the folder) |
| `S` | sticky zoom: keep zoom and position when you move to the next item (off by default) |
| hold `Z` | loupe: 100 % (or 2x the current zoom) around the cursor |
| hold `\` | show the previous image, for picking between burst frames |
| `B` | canvas background: dark / gray / white / checkerboard |
| `C` | clipping blinkies (red highlights, blue shadows). The canvas keeps presenting while on |
| `O` | info line: file name, position in the folder, size, zoom |
| `Ctrl+Shift+A` | always on top |
| `Insert` / `Shift+Space` | mark or unmark the current item. `Ctrl+A` marks all, `Ctrl+D` clears |
| `F5` | slideshow (fullscreen). In it: `Space` pause, `+` / `-` interval, `.` blackout, `R` shuffle, `Esc` leave. A clip plays to its end before advancing |
| `F7` / `F8` | copy / move the marked items (or the current one) to the last folder used. `Shift+F7` / `Shift+F8` pick a folder. Never overwrites: a taken name becomes `name (2).ext` |
| `Delete` | move the marked items (or the current one) to the Recycle Bin, after asking. A drive with no Recycle Bin is refused, never deleted permanently |

Drop files or a folder on the window, the gallery, or the filmstrip. Drag a
thumbnail (gallery, filmstrip, or a Local search result) or the current image
(at fit) out to Explorer or another app, such as a video editor: the drag
carries the original file itself (`CF_HDROP` on Windows, `public.file-url` on
the Mac), copy-only and read-only, never a copy of it; on Windows a thumbnail
brings its RAW / Live Photo pair. Dragging a marked thumbnail drags every
marked item, in folder order; an unmarked one drags itself (both platforms).
A drag of ours let go over our own window, gallery or filmstrip is refused
rather than reopening the folder. Pass paths on the command line: the first
one that exists opens (a file opens its
folder with that file selected).
Files added to or removed from the open folder show up without a restart.

A one-pixel grid appears at 400 % and above.
| `+` / `-` | zoom in / out (`=` and the numpad keys too) |
| `Ctrl+O` | open a photo or a clip (JPEG/PNG/BMP/GIF/WebP/TIFF/ICO/HEIC/AVIF/RAW, MP4/MOV/MKV/WebM/AVI/TS) |
| `Ctrl+Shift+O` | open a folder |
| `Ctrl+E` | show the current file in Explorer, selected (`⌘E`: Finder; a Photos library item opens in Photos, selected). Open menu: **Open: filename** |
| Open ▸ **Recent folders** | the folders you opened lately, the jump list's (File ▸ Open Recent on the Mac). From the keyboard: `Tab` to the command bar, `Enter` on Open, arrow to Recent folders, `Right`. A folder that has gone beeps and leaves the list, as a welcome-card row does. The welcome card writes your profile folder as `~` (`~\Pictures`) |
| `Space` / `,` / `.` on an animation | play or pause (a finished one plays again) / previous frame / next frame, like a clip. Delays follow browsers: 10 ms or less plays as 100 ms |
| `?` | the shortcuts for what you are doing right now. Also the `?` button on the right of the command bar |
| `Ctrl+,` | Settings: **General** has grouped preferences with aligned switches and automatic saving; **Keyboard shortcuts** has the searchable remapping list. Both pages scroll independently of the header and Done button. Search the list by command or shortcut. Choose a shortcut and press its replacement; viewer shortcuts are suspended while Settings is open. Escape or Cancel change cancels capture; Escape otherwise closes Settings. Conflicts swap shortcuts, and Reset to default restores the map. `?` lists whatever you bind |
| `Ctrl+G` | go to an item by its number in the folder (Windows; not on the Mac yet) |
| `/` | find an item by name. With the filmstrip or gallery focused, just type |
| `I` | metadata pane: summary card, searchable tag tree, and for clips the per-stream inspector (PR 9). Focuses the pane; `Esc` returns to the photo, a second `Esc` closes it |
| `Ctrl+Shift+E` | folder tree, rooted at the open folder (PR 9). Focuses it: arrows walk, Right / Left open and close a folder, `Enter` opens it, `Esc` returns to the photo. It follows the folder as subfolders come and go |
| `O` / `Shift+O` / `Shift+I` | info overlay with exposure lines / AF points / eyedropper (PR 9) |
| `Ctrl+C` | copy the eyedropper colour if it is on, otherwise the marked (or current) file(s) |

The title bar shows the current file, its position in the folder, its size and
the zoom. Arrow keys, `Space` and the slideshow wrap from the last item to the
first; turn that off under Settings.
| `Ctrl+Shift+O` | open a folder |
| `Left` / `Right` | previous / next in the folder |
| `G` | gallery: thumbnail grid of the folder. A folder of only folders uses big tiles; a mixed folder keeps a short chip row above the photos. Covers and counts show on the tiles; the folder path sits in the command bar just left of `?`. **Up** (↑) and **Root** (house) buttons stay outside the scrolling trail: Up opens the enclosing folder; Root returns to the highest folder reached in this browsing session (the first breadcrumb). The `…` menu opens hidden parent folders directly. Full paths are available on hover. These controls remain available, including while a photo is open. `Ctrl+Up` goes up and selects the folder you left; `Ctrl+Left` / `Ctrl+Right` open the sibling beside it. A search icon at the right end of the path (and beside "Search: …" on a result list) does what `Ctrl+F` / `⌘F` does: with Local search installed (loaded, or still starting at launch) it opens the search panel (where `file:name` searches file names); without it, **file search**, a field over the grid that filters this folder's tiles by name as you type (no index; `Esc` clears, then closes); the command bar stays above the gallery on both hosts, so the path and the icon are there over the grid too. `/` on the folder row finds a tile by name. `W` / `S` or Up / Down move between rows and cross from folders to images; `A` / `D` or Left / Right move between items. `+` / `-` enlarge / shrink thumbnails (`=` also enlarges). `Enter` opens a folder or the selected image. A click does the same; `Esc` leaves |
| `T` | filmstrip show/hide, for the mode you are in (folder open or single image) |
| `Tab` | focus the command bar island |
| `Esc` | walks out one level: gallery, fullscreen, then island focus back to the canvas, and last, in a search result list, **Back to folder**. It never quits |
| `Ctrl+W` / `Alt+F4` | close the window |
| `Ctrl+N` (`⌘N`) | a new window: its own process on the empty window; add-ons stay in the first window's process |

**Editing (PR 10, Windows and macOS).** Edits are kept per file for the session; the
original is only ever rewritten by a lossless JPEG rotate / flip.

| Key | Does |
|---|---|
| `[` / `]` | rotate left / right. On a JPEG with no other edit, the file is rewritten losslessly 0.4 s after the last press |
| `H` / `V` | flip horizontal / vertical (same lossless rule) |
| `Shift+C` | crop / straighten. Arrows move the crop, `Shift`+arrows resize it, `,` / `.` straighten by 0.5°, `Enter` applies, `Esc` cancels |
| `Ctrl+Z` / `Ctrl+R` | undo the last edit / reset to the original |
| `Ctrl+S` | export dialog: JPEG / PNG, quality, long edge, metadata (all / no GPS / none). `↑` `↓` choose, `←` `→` change, `Enter` exports to `<name>-edit.jpg` beside the original; never overwrites |
| `Shift+A` | adjust pane (PR 11): exposure, contrast, saturation, temperature, tint, histogram and clipping. It takes the keyboard: on Windows `Tab` walks the sliders and the arrows step them; on Mac `↑` `↓` pick a slider, `←` `→` step it (`⇧` ×10), `0` zeroes it, `R` resets. `Esc` hands the keyboard back to the photo; `Shift+A` again closes the pane. A slider drag is one `Ctrl+Z`. Colour never rewrites the file: a JPEG with a colour edit keeps `[` `]` in the stack for export |

**Clips (PR 13 / 14, Windows and macOS; not yet built on either).** Nothing here changes the
clip: every result is a new file beside it, and a cancelled job leaves nothing behind.

| Key | Does |
|---|---|
| `Ctrl+T` | trim mode on the clip on screen. Transport keys keep working; `Esc` leaves it (markers are kept for that clip) |
| `[` / `]` | in / out marker at the playhead. The scrub bar shows the markers, the keyframe grid and the range the instant cut keeps |
| `P` | preview the cut: loops exactly what `Enter` will write |
| `Enter` / `Shift+Enter` | save the keyframe cut (instant, lossless, snapped to keyframes) / the frame-accurate re-encode on the hardware encoder (slower) |
| `Ctrl+X` | save a copy without the in–out range |
| `Ctrl+←` / `Ctrl+→` | previous / next keyframe (in trim mode) |
| `Backspace` / `Delete` | clear the markers (in trim mode `Delete` never trashes the clip) |
| `Ctrl+S` | clip tools: rotate losslessly, split, save the frame (PNG / JPEG), extract audio (copy / WAV / FLAC), convert MP4 ↔ MKV, GIF / WebP of in–out (or 5 s from the playhead) |
| `Ctrl+B` | split at the nearest keyframe to the playhead |
| `Ctrl+J` | Jobs pane: progress and time left; `↑` `↓` choose, `Delete` cancels, `R` retries, `Enter` shows the output in Explorer / Finder |

On a Mac these are the `⌘` chords.

Colour edits are worked in linear light (an FP16 working image, D6) and shown through the
same shader on both platforms; `C` blinks what the edit clips. A RAW's sliders wait for
LibRaw's full linear develop (seconds on a large file) rather than editing the embedded
JPEG, so what you adjust is what exports.

Keys go through one router and one table (`src/shell/commands.h`,
[docs/design/16](design/16-commands.md)). Symbol keys (`?`, `+`, `\`) follow your
keyboard layout, not a US key position.

Wheel zooms toward the cursor; drag pans. Zoom-out floors at 50 % (Fit can
still go smaller on a huge image) and rubber-bands a little past that, then
springs back to centre. Drop a file on the window.

Command line: `--soak <seconds>`, `--json <path>`, `--gate` (non-zero exit if the verify
line fails), `--no-overlay`, `--static`, `--no-chrome`, `--open <path>`,
`--browse-soak` (with `--json` and a folder: time each arrow to the next still, and jumps
past the prefetched neighbours; 3 s dwell between arrows; the macOS lab has it too. The
Windows lab adds `quick` steps — Right as soon as the last photo is on screen — and one
`held` run — a Right every 50 ms, then the last photo timed to full resolution, with the
run's decode job counts under `"held"`; every step carries `full_ms`. Those are the cases
the in-flight decode hand-off is for, and `perf-browse.svg` ignores them),
`--av-soak <seconds> --csv <path>` (headless A/V drift soak on a clip — see
[Test](#test)), `--pan-soak` (with `--soak` and `--open`: pan the still at 100 % across
the whole frame on a fixed path, to measure cached-image and tiled-pyramid pan), or a
positional file or folder.

A still's first pixel is a preview (JPEG DCT 1/4, or a RAW's embedded JPEG); the full
decode then *refines* the same item rather than replacing it: the view you have — fit,
or a zoom and pan made while it loaded — is kept as a fraction of the image, and the
full texture cross-fades in over 80 ms (up to 250 ms when the preview and the full
render differ a lot in brightness, as a RAW's embedded JPEG and LibRaw's render do).
Images above ~64 MP, or wider or taller than 16384 px, are shown as a tiled pyramid: a
≤ 2048 px overview is always loaded, and 256 px tiles for the part on screen are
created a few per refresh and drawn over it, so a fast zoom or pan is blurry for a
moment, never blank. `F` / `F3` shows the tile LOD, resident tiles and VRAM, create
times, and refinement counters. The command bar is also on the island: Open (**Media…** or **Folder…** — the picker
takes photos and clips), View (zoom in/out,
fit, 50 / 100 / 200 / 400 %, gallery, filmstrip, overlay), a playback-speed dropdown,
a `?` shortcuts button, Settings (`Ctrl+,`: view defaults and remappable keys), About.
The filmstrip along
the bottom and the gallery grid are both `ItemsRepeater` islands over the same listing; the gallery also shows child-folder tiles (or a chip strip in a mixed folder) and a path bar (`Ctrl+Up` goes up, `Ctrl+Left`/`Ctrl+Right` the sibling). Thumbs are JPEG files from `%LocalAppData%\MediaViewer\thumbs`. Clips get a
thumbnail too — a poster frame from about 10 % into the clip, in that same cache — so a
camera dump does not show blanks where the video is. With no
folder open the canvas shows a welcome card (drop target, open shortcut, formats, key legend) reading *Drop photos, videos or a folder here*, not the
present-lab sweep. **Space** on that empty view starts a small runner game (an intro animation, then
Space to jump, `Esc` to leave with a short outro); it stops presenting once the welcome card is back. The frame-time
soak (`--soak`) keeps the old sweep, which the present-loop gate measures. The gallery and filmstrip accept the same drop, and you can drag a
thumbnail or the fitted image out to Explorer.

The playback transport is a **third island**: a centred bar floating over the bottom of the
video, above the filmstrip, sized to its controls, that appears with a clip and goes away with it. It works the
same way on Windows and macOS (`src/shell/transport_autohide.h` holds the one rule both use):

- While the clip **plays**, the bar hides after 2.5 s with no activity. Moving the pointer,
  clicking, scrolling, a transport key (`Space`, `K`, `J`/`L`, `Q`/`E`, `,`/`.`, speed, mute,
  `↑`/`↓` volume) or `Tab` brings it straight back.
- It **stays up** while the clip is paused or has ended, while the pointer is over it, during
  a scrub, while its More / speed menu is open, while it has keyboard focus, and whenever a
  screen reader (Narrator, VoiceOver) is running. It is never shown over the gallery or
  Settings.
- In **fullscreen** the pointer hides with the bar while it is over the video. Windowed, the
  system pointer is never hidden.
- Hiding is **only visual**: the canvas does not refit, decode and audio carry on, and keys
  keep working. A click where the hidden bar was lands on the video, not on a button you
  cannot see. Idle playback adds one one-shot timer, not a repaint.

The Mac bar fades; on Windows the island is an opaque child window, so it is moved
off-screen instead. The bar covering the video bottom is a reversal of the old reserved
strip ([docs/design/12](design/12-decision-log.md) 2026-09-26). Speed is owned by the core, so the
dropdown and the keyboard cannot disagree.

Opening a single image lists its folder too, so `Left` / `Right` and the gallery work on the
files beside it. Whether the filmstrip comes with it is a preference: **Settings** has
*Filmstrip when opening a folder* (on by default) and *Filmstrip when opening an image* (off),
persisted to `%LocalAppData%\MediaViewer\settings.ini` (also wrap, sticky zoom, canvas
background, and key remaps). `T` toggles the one for the mode you are in. Both take
effect immediately — no restart. The Settings screen groups these defaults under General,
with switches aligned on the right and descriptions on the left. Keyboard shortcuts has
its own searchable tab, so neither page is squeezed into a narrow column. Both platforms
keep the header and Done button visible while the content scrolls.

For the settings and path-bar smoke checks on Windows and macOS, see
[the UI verification checklist](settings-navigation-verify.md).

### Local search (the AI pack) from a source build

Developer builds only; a release build refuses a dev-signed pack. Numbers, state and what is
owed: [docs/design/17](design/17-local-ai-search.md); the Mac checklist is
`src.swift/AIChrome/MAC-VALIDATION.md`.

1. Stage the models (pinned revisions and SHA-256s):
   `python3 tools/package/ai-models.py stage --piece ai --out <staged>/ai` (and `ai-audio`,
   `ai-faces`).
2. Configure with `-DMV_ADDON_DEV_PUBLIC_KEY=<your key's public half>` and build `mv_ai`,
   `mv_ai_chrome` (Mac) or the WinUI chrome, `mv_ai_tests` and `ai-bench`. On the Mac the AI
   targets exist on arm64 only.
3. Pack, sign and install into a scratch folder:
   `python3 tools/package/ai-sideload.py --build <build> --models <staged> --key <key file>
   --addons <folder> --platform win-x64|macos --pieces ai,ai-audio,ai-faces`.
   NVIDIA acceleration (Windows): configure with `-DMV_AI_CUDA_PIECE=ON` to stage ORT's CUDA 13
   build in `build/addons/ai-cuda`, and add `ai-cuda` to `--pieces`. It needs NVIDIA's CUDA 13
   and cuDNN 9 on `PATH` (never shipped; docs/design/12 2026-10-03); without them the panel says
   "CUDA 13 or cuDNN 9 not found — using CPU". Stable releases publish this piece.
4. Run the app or `ai-bench --addons <folder> --index <media folder> --query "a dog"` with
   `MV_DEV_ADDONS_DIR=<folder>` and `MV_DEV_THUMBS_DIR=<another folder>`, so a real install
   is untouched.
5. Quit's cost with a pack loaded: `ai-bench --addons <folder> --query dog --quit-after 1`
   quits the way both hosts do (the pack gets half a second to stop, then the exit skips static
   destructors; `src/addon/host.h`, "Quit") and prints the time it took; `--quit-legacy` is the
   old wait of up to 5 s, `--quit-hash` re-verifies the pack meanwhile as Settings does, and a
   larger `--quit-after` quits once the pack is idle.

`mv_ai_tests "[refine]"` runs the People refinement (docs/design/17 "People refinement") on synthetic
face vectors and a temporary faces.db; it needs no pack. In the app it runs only from
**Refine faces** on a person under Settings → People, and library-wide from **Merge
duplicates** above the grid (docs/design/17 "Merge duplicates": the same check for everyone, then people
who are the same person merged; never two named differently, never a split pair).

The People embedder is AdaFace IR-50 (docs/design/17 "People model"). It has no upstream ONNX: staging
`ai-faces` runs `tools/package/face-export.py` on the pinned safetensors, so the pack builder needs
`pip install -r tools/package/requirements-export.txt` (torch, onnx); `ai-models.py check` does
not. **Re-analyse faces** (Settings → People;
`people_reanalyse`) runs every photo through the People pass again and keeps the user's people;
a pack with a new face model does the same by itself. Tests: `mv_ai_tests "[rerun]"` (no pack);
`"[people]"` with `MV_AI_PACK_DIR` (the ORT folder), `MV_AI_FACES_DIR` (a staged ai-faces) and
`MV_AI_FACE_SET` (a `<person>/<photo>` folder such as LFW, kept out of git); and
`"[.people-bench]"` with `MV_FACE_EVAL` (a vector file: uint32 n, uint32 dim, n int32 labels,
n x dim float32) and optionally `MV_FACE_TUNING="same keep keep_weak margin ambiguous merge"`,
which runs real vectors through faces.db's online clustering and the re-run's settle and prints
BCubed precision and recall.
LFW has only clear faces, so check a clustering or People-speed change on a real library too:
copy its `faces.db` (`sqlite3 "file:<faces.db>?mode=ro" ".backup <copy>"`; opening migrates, so
never the live one), then `MV_FACES_DB=<copy> mv_ai_tests "[.people-settle]"` (the settle, with
people listed before and after) or `"[.people-api]"` (what each People read the chrome makes
costs). Delete the copy after: it is face data.

The People grid (Settings → People, on both platforms) follows the folder
the viewer has open: **People in · This folder | + Subfolders**, + Subfolders by default;
everyone shows only when no folder is open (docs/design/17 "People in the open folder", amended
2026-10-03; `people_in_json` in `mediaviewer_ai.h`).
`mv_ai_tests "[faces]"` covers the scoping with the fake face model.

Tests with the real pack: `MV_AI_PACK_DIR`, `MV_AI_AUDIO_DIR`, `MV_AI_SPEECH_CLIP`,
`MV_AI_EVAL_DIR` (a folder of photos plus a COCO-style `labels.json`) and `MV_AI_GREY_JPEG`
make `mv_ai_tests` run its model cases; `"[.bench]"` prints CPU / Core ML timings and
`"[.calibration]"` the held-out "nothing found" rates and, per Precision level (Settings →
Local search), what captions, nonsense, "helicopter" and near-miss category queries return
(docs/design/17 "Precision scale"). By default it indexes 300 and 1,000 photos per tower; for library
sizes (issue #85) give `MV_AI_CALIBRATION_SIZES=1000,5000,10000,25000` and an eval folder
with that many labelled photos (COCO 2017 val2017 then train2017 by image id, captions as `labels.json`), and
`MV_AI_CALIBRATION_TOWERS=clip-b32` (or `clip-l14`) to run one tower. The image tower runs on
Core ML on a Mac (about 500 img/s B/32 and 25 img/s L/14 on an M-series, CUDA with `MV_AI_CUDA_DIR`, CPU otherwise;
`MV_AI_CALIBRATION_CPU=1` forces CPU), decoding on several threads. Set
`MV_AI_CALIBRATION_CACHE=<folder>` to keep the embeddings (and Core ML's compiled towers)
between runs: the cache is append-only, so a stopped run resumes, and one made for 25,000 photos
serves every smaller size. `MV_AI_CALIBRATION_OUT=<folder>/{tower}.jsonl` writes one line per
tower, size, level and query (rows returned, relevance, and the rule's inputs: `top10_z`, its noise
expectation, `over_margin`, the best margin, `stands_out`, `margin_needed`) and, beside it, every
query's embedding (`queries-<tower>.f32` / `.json`) for replaying a candidate rule offline; the
binary formats are described above the test in `tests/test_ai_infer.cpp`. The rule's label vocabulary
(`src/addons/ai/vocabulary.*`) is on by default; `MV_AI_CALIBRATION_NO_LABELS=1` measures the rule
without it, and `MV_AI_CALIBRATION_VOCAB=<file>` (with `MV_AI_CALIBRATION_OUT`) only embeds that
file's lines as `vocab-<tower>.f32` for trying a list offline. The engine keeps its tower's label
embeddings in `labels.f32` in the AI data folder; delete it to re-embed. On a Mac the first Core ML
open of a tower compiles for 1–5 minutes; the app searches on CPU meanwhile.

Settings → Local search → **Import and export** writes the index of chosen folders to a
`.mvindex` file (SQLite; paths relative to each folder, optional People and cached thumbnails)
and merges one back, each folder pointed at where its files are on this machine; the folders are
then rescanned and anything whose size or date differs is indexed again (docs/design/17 "Sharing an
index"). `mv_ai_tests "[transfer]"` runs two engines as two machines over one library copied
elsewhere: nothing embedded twice, an edited file re-embedded, an empty index adopting the
file's Quality, a used one skipping another model's vectors, People and thumbnails only when
ticked, a non-index file refused. Thumbnails cross the host table as bytes (`thumbnail_jpeg`,
`thumbnail_store_jpeg`, appended to v2; the host decodes what it stores). An export with
thumbnails makes any the viewer's cache does not hold yet, so it can take a while on a library
that was never browsed.
With the real pack, two sideloaded add-on folders are two machines:
`ai-bench --addons <A> --index <library> --make-thumbs <library> --export <file> --export-flags 2`,
then `ai-bench --addons <B> --import <file> --import-to <copy elsewhere> --import-flags 2
--count-thumbs <copy> --query …` (each with its own `MV_DEV_THUMBS_DIR`); `assets_per_s` staying
0 after the import says nothing was embedded again.

The search field's query language (`Tristan beach`, `Tristan "hello"`, `Tristan or Aaryan`,
`@tri`, `-beach`, `beach video`, `in:2024`, `before:2025-06`; docs/design/17 "Query syntax") is parsed
in the pack by `src/addons/ai/query.*`, so both chromes share it. `mv_ai_tests "[query]"` covers
the parser, names, suggestions and the singular / plural pair with no models; it is pure C++20
and also compiles on its own (`clang++ -std=c++20 tests/test_ai_query.cpp
src/addons/ai/query.cpp` with `-Isrc -Itests` and any Catch2).

**The Mac AI chrome's own measurements** (`src.swift/AIChrome/Tests`, not part of the cmake
build): `cd src.swift/AIChrome && swift test -c release -Xswiftc -enable-testing --filter
PeopleGridBench` lays the Settings People grid out at 200 people with an empty table (no pack)
and reports CPU time for the grid at rest, one layout pass, a status-line publish, and a people
update with and without a re-sort. Compare runs of the same build alternated with the base;
the 2026-10-03 numbers are in the PR that added it (a publish 51 → 8 ms, a people update
500 → 46 ms of CPU).

**The Photos library source (Mac, issue #72; docs/design/17 "Photos library source").** Engine
behaviour is tested with a fake PhotoKit: `mv_ai_tests "[photos]"`. On a real library, two
tools each wrap themselves in a throwaway `.app`, because PhotoKit's permission prompt needs
`NSPhotoLibraryUsageDescription`. Each asks once for its own access, and prints counts and
timings only, never an identifier or a name:

- `tools/ai/photos-spike.sh [--stills N] [--videos N] [--opens N] [--edge 448] [--threads 2]`
  measures the source alone, with no model and no index. It reports:
  - enumeration time, and the local-rendition rate at the indexer's size;
  - how many stills and clips are only in iCloud;
  - the cost of opening a result (resolve plus `clonefile`).
- `tools/ai-bench/photos-bench.sh <build> --addons <dev add-ons> [--query "a dog"]...` runs the
  sideloaded pack over the library, as `ai-bench --index` does over a folder. It keeps
  thumbnails in its own scratch folder.

A local app build that should try the library in Settings needs the new `Info.plist` key and the
`packaging/macos/MediaViewer.entitlements` entitlement. `mediaviewer_app` adds both. Configure
with `-DMV_MAC_BUNDLE_ID=io.github.longtimeno-c.mediaviewer.dev`, so that the dev app's Photos
permission and preferences stay apart from an installed MediaViewer.

### Local search from Final Cut Pro and FCPXML export

Status and verify lines: [docs/design/23](design/23-nle-search.md) (issue #71). Phase 1 (the search
agent) is built and measured; Phase 0's hands-on run in Final Cut Pro is owed.

- **Both platforms:** `mv_nle` (`cmake/nle.cmake`, included by `cmake/ai.cmake`) and
  `mv-nle-export`, which runs one search through the installed pack's read-only reader and
  writes FCPXML (Final Cut Pro, DaVinci Resolve and Premiere Pro import it):
  `mv-nle-export "birthday cake" --out cake.fcpxml` (`--json` prints the rows; `--videos`,
  `--photos`, `--scope-dir`, `--max`, `--no-keyword`, `--similar FILE --at MS`). It needs a pack
  new enough to have `mv_ai_reader_get`; with a dev-key build, point it at a sideloaded pack with
  `MV_DEV_ADDONS_DIR`.
- **Tests:** `mv_ai_tests "[search-agent],[nle]"`: the reader's top-K against the app engine's on
  the same index, the index byte-identical after a reader session, catch-up, the wire format's
  bounds, the FCPXML, and the thumbnail cache read without a write.
- **Mac only, in MediaViewer.app:** on an arm64 build, `mediaviewer_app` puts the workflow
  extension in `Contents/PlugIns/MediaViewerSearch.appex` and the agent's launchd job in
  `Contents/Library/LaunchAgents`. The agent is the app's own executable run as
  `MediaViewer --search-agent`.
  Configure with `-DMV_FCP_TEAM_ID=<team>` (the team the app is signed by; it prefixes the Mach
  service and the extension's app group). A dev-key build's agent accepts unsigned clients, so
  an ad hoc `mv-search-client` can reach it. `assemble`
  signs ad hoc, which FCP and the agent's peer check refuse; to try it for real, sign with
  `python3 tools/mac/macpack.py release --app build/MediaViewer.app --identity "Developer ID
  Application: …" --skip-notarize --allow-no-updater`, which keeps the extension's entitlements.
  To try the agent without turning it on, bootstrap a launchd job whose `Program` is the app's
  `Contents/MacOS/MediaViewer`, whose `ProgramArguments` are `MediaViewer --search-agent`, whose
  `MachServices` names `<team>.io.github.longtimeno-c.mediaviewer.fcp.search`, and (dev) whose
  `EnvironmentVariables` set `MV_DEV_ADDONS_DIR`; then, with `mv-search-client` signed by the same
  team: `mv-search-client mountain --thumbs`, `mv-search-client --bench 20 mountain beach dog`
  (agent vs in-process p50/p95, same results) and `mv-search-client --wait-exit 90` (seconds until
  the idle agent has gone). `launchctl bootout gui/$(id -u)/<label>` removes the job.
- **In Final Cut Pro:** with Local search Core installed, open Settings ▸ Local search ▸ Final
  Cut Pro ▸ **Turn on** (it registers the agent with `SMAppService` and elects the extension in
  with `pluginkit`; allow MediaViewer under Login Items if asked), then Extensions ▸ MediaViewer
  Search. **Turn off** reverses both. `pluginkit -m -i io.github.longtimeno-c.mediaviewer.finalcut`
  shows the election (`+` in, `-` out); a fresh install elects it out 10 s after first launch.
  The panel searches the open library's folder by default; the scope picker switches to any
  indexed folder or all of them. Space plays the selected result with sound, N / Shift+N step
  through its matches, and hovering a tile scrubs it. The options menu sets the drag's clip
  handles and the keyword collection, and shows the Phase 0 test drag (`~/Movies/test`).
  The panel logs to the `io.github.longtimeno-c.mediaviewer.fcp` subsystem. Read it with
  `/usr/bin/log show --predicate 'subsystem == "io.github.longtimeno-c.mediaviewer.fcp"'`; in zsh,
  a bare `log` is a shell builtin.

**The Photos library as a folder, and its backup (Mac; docs/design/26, 2026-10-03).** Once the library
was added in Local search's Settings, the folder tree (`⌘⇧E`) shows a *Photos Library* row and
File gains *Open Photos Library*: the host lists the library as **virtual items** (`photos:<id>`,
`shell/folder_model_mac.h` `list_entry::is_virtual`) and resolves each to a file as it is shown
(`shell/photos_items_mac.h`). Search results take the same path. Settings → *Photos Library* →
**Back Up Now** runs `shell/photos_backup.h` over PhotoKit into a chosen folder. Tests, no library
needed: `mv_tests "[folder][photos]"` (virtual entries and tiles over a fake provider),
`"[photos_backup]"` (the engine over a fake library: layout, verified copies, the manifest, a
collision, a cancel), `"[write_guard]"` and `"[poster]"` (a clip's display matrix turns its
poster; the thumbnail spec is `jpg512.4`). A dev build shows the row and the section only when
its own bundle id has Photos access and the flag `mv.photosLibrary.added` is set in its
defaults; the flag is set by the pack's Settings when the library is a root, so a dev build
without the pack loaded can be pointed at it by hand (`defaults write
io.github.longtimeno-c.mediaviewer.dev mv.photosLibrary.added -bool true`) once macOS has
granted that bundle Photos access. The viewer's on-view downloads and previews live in
`~/Library/Caches/MediaViewer/Photos Library/` (write-protected, emptied at launch); a backup's
fetched originals wait in `~/Library/Caches/MediaViewer/Photos Backup/` and are removed when
the run ends.

## Test

```powershell
# the video test corpus — REQUIRED for anything PR 5 claims to prove.
# The clips are gitignored (docs/design/09); only the generator is in the repo.
# Needs an ffmpeg on PATH (or $env:FFMPEG); NVENC is used when present,
# libx264/libx265/SVT-AV1 otherwise. ~1.5 GB, and the 31-minute clip is slow.
bash tools/testmedia/generate.sh          # --list to see what it makes
ctest --test-dir build -C Release -R corpus --output-on-failure   # confirm

# native unit tests and synthetic harness regression tests
ctest --test-dir build -C Release --output-on-failure

# ...and in CI, where an absent corpus must fail rather than skip
$env:MV_REQUIRE_CORPUS = "1"; ctest --test-dir build -C Release --output-on-failure

# the ABI, end to end from C#: SafeHandle, struct layout, completion drain
dotnet build src.managed\MediaViewer.AbiSmokeTest\MediaViewer.AbiSmokeTest.csproj -c Release
dotnet src.managed\MediaViewer.AbiSmokeTest\bin\Release\net8.0-windows\MediaViewer.AbiSmokeTest.dll build\bin\Release

# WinUI chrome (also published beside mediaviewer_lab.exe by the CMake build)
dotnet publish src.managed\MediaViewer.Chrome\MediaViewer.Chrome.csproj -c Release -r win-x64 --no-self-contained

# Milestone G: the Import add-on's suite (built with the core, both platforms)
ctest --test-dir build -C Release -R import_ --output-on-failure
# PR 54 find duplicates: time a first scan and a cached rescan (synthetic tree,
# or MV_DUP_BENCH_DIR=<folder>, read only; the hash cache is a scratch import.db)
build/bin/mv_import_tests "[.perf-bench]"

# docs/design/25, open add-ons: their C++ cases are part of the suite above
# ([open-addon]); this is the author's tool, which packs with Python and
# asks the app's own reader (mv_addon_verify --open) about every package
ctest --test-dir build -C Release -R addon_sdk --output-on-failure
python tools/addon-sdk/test_mvaddon.py        # the tool alone, no build needed

# ...and the same engine headless on Linux or any POSIX machine (the
# portable-core CI job in tools/portable/ci-portable-core.patch; SQLite, libsodium, BLAKE3 and Catch2 from vcpkg via
# tools/portable/vcpkg.json, or the system). With FFmpeg, libspng and libjpeg
# installed (pkg-config) it also builds the PR 13 / 14 clip suites,
# mv_clip_tests and mv_trim_tests:
#   cmake -S cmake/portable -B build-portable \
#     -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
#     -DVCPKG_MANIFEST_DIR=tools/portable
#   cmake --build build-portable && ctest --test-dir build-portable
#   build-portable/bin/mv_clip_tests "[bench]"   # keyframe trim of a ~1 GB MP4 (or MV_CLIP_BENCH_FILE)

# PR 13 / 14 clip editing: the same suites inside the Windows / Mac mv_tests
ctest --test-dir build -C Release -R "clip|trim" --output-on-failure

# policy gates (all run in CI on every push)
.\tools\check-module-graph.ps1     # dependencies point downward only
.\tools\check-hostable-core.ps1    # D9: no windows.h / d3d11.h above gfx/
.\tools\check-winui-controls.ps1   # no ProgressBar / ProgressRing / DropDownButton / InfoBar / TextBox...: they
                                   # fail-fast in the island host (use JobBar, Shared\FlatBar.cs, a Button + Flyout,
                                   # Shared\FakeInput.cs)
.\tools\licence-check.ps1          # no GPL FFmpeg, no software HEVC/AAC encoder
.\tools\telemetry-schema-check.ps1 # telemetry payload stays fixed vocabulary: no path/filename field

# PR 7 broken-file corpus: every seed in tests/data/seeds truncated, stomped,
# bit-flipped and given absurd dimensions, plus tests/data/broken, through every
# decode entry point. Fails on a crash, an escaped exception, a call over 5 s,
# or runaway memory. Part of plain ctest; this runs just that suite.
ctest --test-dir build -C Release -L broken --output-on-failure
$env:MV_BROKEN_FULL = "1"          # exhaustive sweep (nightly CI)
$env:MV_BROKEN_DUMP = "broken-dump" # write each failing input here
# seeds are synthetic and committed; regenerate with (needs Pillow, pillow-heif):
python tools\testmedia\make-seeds.py

# libFuzzer harnesses, one per decoder entry point (clang-cl + ASan)
cmake -S . -B build-fuzz -A x64 -T ClangCL -DMV_FUZZ=ON -DMV_BUILD_TESTS=OFF
cmake --build build-fuzz --config Release --target mv_fuzzers
.\tools\fuzz\run.ps1 -BuildDir build-fuzz -Seconds 60      # -Harness png,gif to pick

# PR 7 clean-VM gate: a HEIC decodes with MV_OS_CODEC=0, and the process has
# loaded libheif + libde265 and NO Media Foundation, WIC codec extension, or
# \WindowsApps\ module. Its own executable, because once mfplat.dll is in a
# process it never leaves and the assertion could not be made again.
ctest --test-dir build -C Release -L cleanvm --output-on-failure

# the frame-time gate — 60 seconds, needs a quiet machine
.\build\bin\Release\frametime.exe --seconds 60

# PR 7 no-pop gate: open a still that has a preview (a RAW, or any JPEG), soak,
# and fail if the preview → full swap popped — a cross-fade cut short, a view
# that jumped, or a frame dropped inside the fade. PR 1's cadence is judged on
# the same run, so a short run is diagnostic only.
.\build\bin\Release\frametime.exe --no-pop tools\testmedia\raw\canon_eos7dmk2.cr2 --seconds 62

# PR 5b's A/V drift soak. Writes a CSV of position, error percentiles, the
# least-squares drift slope and the present counters, one row a second.
# Under 1800 s it exits 4 and is DIAGNOSTIC ONLY — it is not the verify.
.\build\bin\Release\mediaviewer_lab.exe --av-soak 1860 --csv drift.csv `
  tools\testmedia\soak_31min_1080p_hevc_aac.mp4
```

### Performance suite: regenerating the README charts

Every chart in the README's "Speed you can measure" is drawn from a report one of the
app's own harnesses wrote; nothing is typed in. One command re-measures all of them and
redraws the SVGs:

```powershell
python tools/perf/regenerate.py --list           # stages, and what each is missing here
python tools/perf/regenerate.py                  # full run: docs/perf + docs/img (~15 min)
python tools/perf/regenerate.py --out perf-run   # staged: reports + charts under perf-run/
python tools/perf/regenerate.py --quick --out x  # seconds per soak: checks the suite only
python tools/perf/regenerate.py --only first-pixel,browse
python tools/perf/make-charts.py                 # redraw from the committed reports
```

| Stage | Harness | Report | Chart |
|---|---|---|---|
| `pacing` | `frametime --seconds 60` (animated + idle soak) | `frametime-animated.json`, `frametime-idle.json` | `perf-pacing.svg` |
| `pan` | `mediaviewer_lab --soak 60 --pan-soak` on the 42 MP ARW | `pan-soak-42mp-arw.json` | `perf-pacing.svg` |
| `first-pixel` | `mediaviewer_lab --soak 6 --static` per camera file, after one warm-up open | `first-pixel/<file>.json` | `perf-first-pixel.svg` |
| `browse` | `mediaviewer_lab --browse-soak` on `tools/testmedia/raw` | `browse.json` | `perf-browse.svg` |
| `video` | `mediaviewer_lab --soak 60` on the 1080p HEVC + AAC soak clip | `investigation/video-native-60s.json` (+ `.video.json`) | `perf-video.svg` |
| `av-sync` | `mediaviewer_lab --av-soak 120 --csv` on the same clip | `investigation/av-after-120s.csv` | `perf-av-sync.svg` |
| `compare` | `tools/perf/compare-screen.ps1` (MediaViewer vs Windows Photos / Media Player) | `compare/screen.json` | `compare-*.svg` |
| `bench` | `mv_tests "[.perf-bench]"`: headless decode, colour, folder list and sort | `bench.json` | — |
| `search` | `mv_ai_tests "[.calibration]"` at 1,000–25,000 photos, both picture towers; needs `MV_AI_PACK_DIR` and `MV_AI_EVAL_DIR` (the labelled COCO folder, "Local search" above) and takes `MV_AI_CALIBRATION_CACHE` and `MV_AI_EVAL_NAME`. Accuracy, not timing: it runs on either platform and leaves `machine.json` alone | `search.json` | `search-accuracy.svg` |

The published charts are Windows numbers. The macOS lab has `pacing`, `first-pixel`,
`browse` and `video`; `pan`, `av-sync` and `compare` are Windows-only harnesses, so a Mac
run draws the charts it has and names the rest as skipped. Inputs are the RAW/HEIC samples
(`tools/testmedia/fetch-raw.ps1`, `fetch-heif.ps1`) and the video corpus
(`tools/testmedia/generate.sh`); a stage whose inputs are missing is skipped, never faked.
Soaks open windows and need a visible display and a quiet machine. On macOS a measuring
run orders its window front without taking the keyboard, so typing elsewhere cannot reach
it, and `--browse-soak` voids its report (`"error"`) if the window was covered. Each run
also writes `machine.json` (OS, CPU, commit). `tools/perf/test_perf_tools.py` checks the
suite itself (every chart reproduces byte-for-byte from the committed reports) and runs
under `ctest -R perf_tools`.

`mv_tests "[.perf-bench]"` is the quick loop for decode work: `MV_BENCH_JSON=path` writes
its medians, `MV_BENCH_DIR=folder` adds your own files to the decode rows. Its `preview` row is
the canvas's first pixel (`decode_first_pixel`). libheif's example is one small image, so for
camera-sized HEIC run `python tools/testmedia/make-grid-heic.py` (after `fetch-heif.ps1`; needs
an ffmpeg with libx265 on `PATH`, a dev tool only): it writes `tools/testmedia/heif/grid-12mp.heic`,
an iPhone's layout (4032x3024 as 8x6 tiles of 512, a 320x240 thumbnail item, Display P3 ICC).
`MV_OS_CODEC=0` times the bundled libheif path, the one a PC without the HEVC extension takes.

The pacing soak cannot see a paused clip's cost (it presents nothing). `mv_tests "[idle]"`
(macOS, headless, a synthetic clip) does: it prints the process's context switches and CPU
per second with a clip paused on its poster and with one played out, and fails above 200
wakes a second (issue #230: the sleep-polling player measured ~1,750 paused, now single digits).

### Copy throughput: `copybench` (PR 49)

`copybench` runs the copy engine (`io/verified_copy`, what F8 and Import use) over a folder
with no app, and prints one JSON line: files, bytes, seconds, MB/s, files/s. It copies into a
fresh `copybench-<n>` folder under the destination and removes it afterwards (`--keep` leaves it).

```bash
build/bin/copybench --make-fixture ~/scratch/raw   --count 100  --size-kib 16384
build/bin/copybench --make-fixture ~/scratch/small --count 2000 --size-kib 200
# alternate the runs: seq is the pre-PR-49 path, auto is what the app picks
build/bin/copybench ~/scratch/raw /Volumes/nas/scratch --mode seq
build/bin/copybench ~/scratch/raw /Volumes/nas/scratch --mode auto
build/bin/copybench ~/scratch/raw /Volumes/nas/scratch --mode auto --no-verify   # vs ditto / robocopy
```

`--mode deep --depth N --files N --chunk-kib N` sets the shape by hand; `--rtt-us N` makes
every file request wait N µs first, a stand-in for a share's round trip on a local disk (not
a substitute for one: PR 49's verify line wants a real 10 GbE share). The fixtures are
incompressible, so a share that compresses on the wire shows no flattering number. On
Windows the reference is `robocopy <src> <dst> /E /MT:16 /J`; on the Mac,
`ditto <src> <dst>`. Delete the fixtures from the share afterwards.
### Open add-ons self-test (PR 55, macOS)

The Mac app can walk the whole add-on flow by itself and photograph each step: the sheet for a
package, Install, each of its themes, Default again, a file of the installed add-on changed on
disk (the chrome must fall back and say so), Remove. It installs under the folder you name and
keeps the theme choice in memory, so nothing in your profile is touched:

```bash
python3 tools/addon-sdk/mvaddon.py keygen --out /tmp/mv-addon.key
```

```bash
python3 tools/addon-sdk/mvaddon.py pack examples/addons/film-tones --key /tmp/mv-addon.key --out /tmp/mv-addon
```

```bash
MV_ADDON_SELFTEST=/tmp/mv-addon/rig caffeinate -d -i build/bin/MediaViewer /tmp/mv-addon/example.film-tones-1.0.0.mvaddon
```

`rig/state.txt` has one line per step (what is offered, what is installed, which theme is on,
the viewer's background colour, what Settings says); `a1-sheet.png` … `a9-removed.png` are the
window. To look at what the app makes of any package without the app:

```bash
build/bin/mv_addon_verify --open some.mvaddon
```

Add a folder as a second argument and it installs there, as the app would. A `.mvaddon` is
registered with both OSes (the installer's `MediaViewer.Addon` ProgId, the Mac bundle's
`Add-on` document type over the exported UTI `io.github.longtimeno-c.mediaviewer.addon`), so a
double-click opens the install sheet once the installed build carries it; it is not a photo type
and the default-viewer prompt leaves it alone. Windows has no rig
yet; its half is checked by compiling (`dotnet msbuild src.managed/MediaViewer.Chrome/MediaViewer.Chrome.csproj -t:Compile -p:WindowsAppSDKSelfContained=false`
works on any OS with the .NET 8 SDK) and by CI.

### Edit workspace self-test (PR 29, macOS and Windows)

Both apps can drive their own Edit workspace and photograph each step, with no pointer (and, on
the Mac, no screen-recording permission). Point it at a scratch copy (Save copy writes a new file beside it,
and the run edits then reverts its metadata):

```bash
MV_EDIT_SELFTEST=/tmp/mv-edit build-darwin/MediaViewer.app/Contents/MacOS/MediaViewer ~/scratch/IMG_0001.jpg
```

```powershell
$env:MV_EDIT_SELFTEST = "$env:TEMP\mv-edit"
.\build\bin\Release\mediaviewer_lab.exe --open C:\scratch\IMG_0001.jpg
```

It quits when done, leaving `s0-viewer` … `s8-closed-reverted` captures (PNG on the Mac, BMP on
Windows; a clip gives `c0` … `c4`) and `state.txt` in the folder. On the Mac,
`MV_EDIT_SELFTEST_DARK=1` or `0` forces Dark or Light Mode; Windows follows the system theme.

On a clip it opens the Video Editor and, on the Mac, also walks a marked range, a dragged edge,
the `L` / `K` shuttle and the close guard, writing `polish:` lines to `state.txt`. Windows'
`MV_EDIT_SELFTEST_KEYS=1` walk does the same with synthesised keys (End, `J`, `O`, `J`, `I`,
Delete, `L`, `K`, the exports, Esc).

**Editor soak (Mac).** `MV_EDIT_SELFTEST_SOAK=<seconds>` on a clip cuts it to three pieces, plays
the program three times, sits paused, closes the editor and sits again, then writes one `soak:`
line: whole-process and main-thread CPU for each phase, the playback ticks that saw a cut
(`glimpse_ticks`) and the jumps over cuts. Add-ons are not loaded for the run. Compare builds
the usual way (each in its own worktree, runs alternated, the display awake: wrap in
`caffeinate -d -i -u`); a run whose `jumps` is short of 6 stalled and does not count.

```bash
MV_EDIT_SELFTEST=/tmp/mv-soak MV_EDIT_SELFTEST_SOAK=8 caffeinate -d -i -u build/bin/MediaViewer ~/scratch/clip.mp4
```

### Crash reports (PR 7)

Native crashes are captured out-of-process by Crashpad into
`%LocalAppData%\MediaViewer\Crashes`. Nothing is uploaded. On the next launch the app
scrubs each dump: memory outside thread stacks is zeroed, and paths, media filenames and
your username are masked. Managed exceptions go to `Crashes\managed\`.
[docs/design/13](design/13-updates-and-telemetry.md) has the details.

The PR 7 verify is "a deliberately-corrupted RAW produces a minidump containing no path,
filename, or pixel data". The crash hook only fires when **both** the environment variable
and the marker in the file are present.

```powershell
# 1. a marked COPY of a real RAW, in PRIVATE_FOLDER_canary\SECRET_FILENAME_canary_7Q3.dng
.\tools\make-crash-raw.ps1 -Source tools\testmedia\raw\pentax_k50.dng

# 2. crash on it
$env:MV_CRASH_TEST = 'decode'
.\build\bin\Release\mediaviewer_lab.exe --open "$env:LOCALAPPDATA\Temp\mv-crash-canary\PRIVATE_FOLDER_canary\SECRET_FILENAME_canary_7Q3.dng"
Remove-Item Env:MV_CRASH_TEST

# 3. scrub: relaunch the app (it scrubs on start), or run the standalone tool
.\build\bin\Release\mv_minidump_scrub.exe <in.dmp> <out.dmp>

# 4. scan; exit 0 = PASS
.\tools\minidump-scan.ps1 -Dump <out.dmp> -Forbidden '<canary path>','SECRET_FILENAME_canary_7Q3','PRIVATE_FOLDER_canary',$env:USERNAME

# pixel data: run make-crash-raw.ps1 with no -Source. It writes a pattern BMP pair and
# prints the byte patterns. Open SECRET_COMPANION_canary.bmp instead; neighbour prefetch
# crashes on the canary. Pass the printed patterns to the scan as -PixelHex.
```

`--av-soak` exits 0 only on a run of 1800 s or more whose drift slope stays
within 1 ms/min, with no position discontinuities and no host-clock gaps; 1 is a
real failure, 3 means the clip ended early, 4 means the run was too short to
count. Audio must be the master for a clip that has an audio track, or it fails.

**The drift figure only means something with the audio master.** On a clip with no
audio track the clock falls back to the host (`clock host fallback` in the F3
overlay) and the slope is measuring the host clock against itself, so a large
number there — tens of ms/min — is an artefact of the fallback, not a defect. Read
the `audio_master` column in the CSV before reading the slope. That is the whole
reason the corpus now carries an audio-bearing 31-minute clip.

#### macOS (PR 11)

Crashpad's `crashpad_handler` runs out of process (`MediaViewer.app/Contents/Helpers`, or
beside `mediaviewer_lab`). Dumps go to `~/Library/Application Support/MediaViewer/Crashes`;
nothing is uploaded, and forwarding to Apple's crash reporter is off. The next launch
scrubs each dump with the Windows scrub, which also masks POSIX paths under `/Users`,
`/Volumes`, `/private`, … and the short and full user name and computer name. An uncaught
`NSException` from the chrome is written, scrubbed, to `Crashes/chrome/` with the
correlation id of the native call in flight; the dump carries the same id
(`mv_last_call_cid`). The verify, with `tools/mac/crash_canary.py` in place of the two
PowerShell scripts:

```sh
# 1. a marked COPY of a real RAW (or no --source: the pixel-pattern BMP pair)
python3 tools/mac/crash_canary.py make --source tools/testmedia/raw/pentax_k50.dng

# 2. crash on it, then relaunch normally: the relaunch scrubs the dump
MV_CRASH_TEST=decode build/MediaViewer.app/Contents/MacOS/MediaViewer \
    /tmp/mv-crash-canary/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.dng
open build/MediaViewer.app

# 3. scan; exit 0 = PASS (it also fails a dump the app has not scrubbed yet)
python3 tools/mac/crash_canary.py scan ~/Library/Application\ Support/MediaViewer/Crashes/*/*.dmp \
    --forbid SECRET_FILENAME_canary_7Q3 --forbid PRIVATE_FOLDER_canary --forbid "$USER"

# the chrome path: an NSException raised in AppKit event handling, or a Swift trap
MV_CRASH_TEST=nsexception build/MediaViewer.app/Contents/MacOS/MediaViewer
MV_CRASH_TEST=swift_trap  build/MediaViewer.app/Contents/MacOS/MediaViewer
```

`--av-soak` exits 0 only on a run of 1800 s or more whose drift slope stays
within 1 ms/min, with no position discontinuities and no host-clock gaps; 1 is a
real failure, 3 means the clip ended early, 4 means the run was too short to
count. Audio must be the master for a clip that has an audio track, or it fails.

**The drift figure only means something with the audio master.** On a clip with no
audio track the clock falls back to the host (`clock host fallback` in the F3
overlay) and the slope is measuring the host clock against itself, so a large
number there — tens of ms/min — is an artefact of the fallback, not a defect. Read
the `audio_master` column in the CSV before reading the slope. That is the whole
reason the corpus now carries an audio-bearing 31-minute clip.

## Package and install (PR 8)

> Cutting a release, and how updates reach people who already installed:
> **[RELEASING.md](../RELEASING.md)**. The short version — the wizard is a one-time
> download, and every update after it arrives in-app from GitHub Releases.

The v1 release is a **per-user** install under `%LocalAppData%\MediaViewer`, with **no
UAC** at any point. `Program Files` is not offered: a per-machine install needs elevation
for every update, which is how update mechanisms stop working
([docs/design/13](design/13-updates-and-telemetry.md)). There is no Microsoft Store channel — the
app is GPL-3.0-or-later ([docs/design/11](design/11-licensing.md)).

First install is an Inno Setup wizard; every later update is Velopack, in the background,
never re-opening the wizard.

### Building the release artefacts

Needs a built Release tree, plus two tools that are not in the repo:

```powershell
dotnet tool install -g vpk --version 1.2.0
winget install JRSoftware.InnoSetup

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\tools\package\build-release.ps1 -BuildDir build
```

That writes `dist\releases\` (the Velopack release set and the update manifest) and
`dist\MediaViewer-<version>-Setup.exe` (the wizard). It refuses to proceed if the app
breaks docs/design/09's 250 MB cap, fails docs/design/11's licence gate, or contains the Windows App SDK
AI / ONNX / DirectML / WebView2 files docs/design/13 forbids shipping.

The first run downloads the pinned .NET runtime (31.7 MB) once and caches it in the build
directory; its SHA-256 is verified every time. For an offline build, pass the same archive
with `-DotnetRuntimeZip`.

**No prerequisites on the target machine.** The payload carries both runtimes — the
Windows App SDK and .NET — so a clean Windows 10 21H2 install runs it with nothing
installed first. That is not a nicety: the wizard is per-user and takes no UAC, and a
machine-wide runtime prerequisite needs admin. docs/design/09 made the same call for .NET — "a
viewer whose whole pitch is 'point it at a folder and it works' cannot open with a runtime
prerequisite dialog". The app is **208.6 MB**, inside docs/design/09's stated 200–250 MB band; a
first install occupies **292.7 MB** on disk, because Velopack also keeps one full package
so a bad update can be rolled back.

**An artefact from that command is unsigned and not publishable.** It says so on its last
line. Signing needs credentials the repo does not and must not hold:

```powershell
.\tools\package\build-release.ps1 -BuildDir build `
  -SigningMetadata trusted-signing.json `     # Azure Trusted Signing
  -ManifestKey C:\offline\release.key         # Ed25519, signs the update manifest
```

Without Authenticode, Windows SmartScreen may warn. Without a correctly signed update
manifest, clients reject updates. The production public key is already pinned in
`src.managed/MediaViewer.Updater/UpdateKeys.cs`; preserve its matching private key.

### Continuous integration

Pushes to `main` and pull requests build four Windows variants (MSVC Release, MSVC Debug, clang-cl, ASan), run the C# ABI smoke test, and fuzz each decoder for a minute. A newer push to the same branch cancels the run it replaces. Dependencies are installed by one job and reused. The first run after `vcpkg.json` or the pinned vcpkg baseline changes spends about 45 minutes there, mostly building FFmpeg. Later runs restore that cache. The nightly schedule is the long fuzz run, separate from pull requests.

### Releasing from CI

Use **Actions → Release → Run workflow**. Pushes and tags do not start release packaging.
Both platforms use the exact `x.y.z` from `CMakeLists.txt`.

| Mode | Result |
|---|---|
| `artifacts` | Unsigned installers in Actions artifacts; no GitHub Release |
| `preview` | Both unsigned test installers on GitHub Releases as a prerelease |
| `stable` | Windows installer, notarized Mac image, both signed update feeds and checksums on Latest |

Windows and macOS build independently; a final job publishes only after both succeed.
Stable needs the Windows manifest key and six Mac signing/notarization secrets.
Windows Authenticode remains optional. Preview needs no signing secrets; its Mac app
must be replaced manually with a stable build later.

See **[RELEASING.md](../RELEASING.md)** for commands, credentials, assets, version rules and
recovery steps. There is no automatic publishing on push or run-number version stamping.

### Installing and uninstalling

The wizard is six pages and no more: Welcome, Licence (GPL, scroll and accept), Location,
Options (Start Menu **on**, Desktop **off**), Progress, Finish (Launch, GitHub, Licence, and
a checked *Choose MediaViewer as the default for all supported photos and videos*).
It registers MediaViewer for the D5 photo and video types (per-user, removed on uninstall),
so it appears in **Open with** and in Settings > Default apps. Windows does not let an
installer set the default itself: leave the Finish-page box checked to open Default apps on
MediaViewer and confirm there, or untick it to skip. It does **not** ask about telemetry, which is a first-run
screen inside the app.

Uninstall is from Apps & features, and removes the shortcuts, the registry entry, the
whole install directory, the thumbnail cache and the add-ons (`addons\<id>\<version>`, the
Local search models included). It then asks, defaulting to No, whether to also delete each
add-on's `data\` (the Local search index, Import's history); a silent uninstall keeps it.

Running Setup over an existing install keeps `addons\`, `thumbs\`, `metadata-snapshots\`,
`Crashes\`, `clipboard\`, `telemetry\`, `settings.ini` and `import-hint.dismissed`. Velopack's
`--installto` clears the directory, so the wizard moves those into a sibling
`MediaViewer.keepN` first and puts them back afterwards. If one cannot be moved, MediaViewer
is still running and Setup stops before anything is deleted.

The chrome registers its UI font session-wide, so DirectWrite can see it. On an installed
build it registers the copy in `<root>\fonts\`, never the file in `current\`. Windows keeps a
registered font open until sign-out if the process dies before `Detach` (a crash, a kill, or
the `TerminateProcess` exit under a running add-on). Registered from `current\`, that open
file made every in-app update fail on Velopack's rename of `current\`, and made Setup exit
with "failed with code 1". Setup and uninstall both unregister any copy left in `current\` or
`fonts\` before they delete anything. Apps & features shows the payload plus the retained
package (`UninstallDisplaySize`, passed in by `build-release.ps1`); add-ons are not counted.

```powershell
# unattended, e.g. on a test VM
.\dist\MediaViewer-0.1.0-Setup.exe /VERYSILENT /DIR="C:\path\to\install" /TASKS=startmenu
```

### Updates

```powershell
# the local end-to-end check: staging, signature rejection, and rollback
.\tools\package\e2e-update.ps1 -Payload .\build\bin\Release
```

It packs three versions into a temp feed, installs one, proves a **tampered manifest
stages nothing**, proves a signed one downloads in the background and applies on exit
without showing the wizard, and proves a build that fails to start twice **rolls back** to
its predecessor on the third try.

### Telemetry

**Off by default, and it stays off unless you turn it on.** One first-run screen, two
buttons, neither preselected; dismissing it leaves telemetry off. Settings has the same
switch, and turning it off deletes the random install id and anything not yet sent.

Nothing about your files ever leaves the machine — no paths, filenames, folder names,
thumbnails, pixels or EXIF. That is enforced by the shape of the payload rather than by
care: an event is an id from a fixed table, a tag from a fixed vocabulary, and named
integers. There is no free-text field to put a filename in.
`tools/telemetry-schema-check.ps1` fails the build if that ever changes.

There is no upload endpoint yet, exactly as there is none for crash reports.

## Where this actually is

### Local search: cloud folders and folder adds (docs/plans/document-search.md slice 0, 2026-10-05)

A folder moved to OneDrive now indexes. The folder walk steps into OneDrive's reparse points but
never follows a link or junction, and it reaches past `MAX_PATH`. Online-only files are counted
("N only in OneDrive") and never read. *Index online-only OneDrive files* (off by default) brings a
couple down at a time, indexes them, and makes them online-only again; the Mac does the same for
evicted iCloud Drive files. Failed folder adds now say why, Settings offers Pictures and Videos
while they are not indexed, and an unreadable folder says so.

- **Windows:** `mv_ai_tests` (97 cases, 8 skipped for absent model files; `"[cloud]"` is the new
  set, with a fake provider) and `mv_import_tests` (`"[port]"`: links and junctions skipped, a
  file past 260 characters listed) pass. The WinUI chrome compiles. Not yet run against real
  OneDrive placeholders or in the running app.
- **Mac (M-series, 2026-10-05):** compiles clean (`-Werror`). `mv_ai_tests` 101 cases (93 passed, 8
  skipped), `"[cloud]"` 10 of 10 runs, `mv_import_tests` 70 of 70, `mv_tests` 611 passed (8
  skipped). Against real iCloud Drive (a scratch folder, 4 of 8 photos evicted): the walk flags
  exactly the 4 `SF_DATALESS` files without faulting them in. With a dev-sideloaded pack the row
  read "4 of 4 · 4 only in iCloud Drive" and nothing was downloaded. With the switch on, all 4
  came down, were indexed (search finds them), and were evicted again, and `cache/cloud-fetched`
  was gone. A leftover marker plus a hydrated file was given back at the next launch. An ejected
  disk-image root read "Couldn't read this folder…". *Index Pictures* / *Index Movies* showed
  while uncovered. Mac PR 1 gate: 2 × 60 s, 0 dropped, p99 16.9 ms, 0 idle presents.
  **Not run:** the power and Low Data Mode waits on hardware (the fake provider covers them),
  the two-at-a-time cap (4 small files fetched too fast to observe), the "An import is running"
  add error, and another File Provider client's files (none installed).
- **Building on a machine where the Visual Studio installer has lost its Build Tools registration**
  (`vswhere` lists nothing; the VS generator and vcpkg both refuse): configure with
  `-G "NMake Makefiles"` from a `vcvars64.bat` shell, with `-DVCPKG_MANIFEST_INSTALL=OFF`,
  `-DVCPKG_INSTALLED_DIR` pointing at an existing `vcpkg_installed` tree, and
  `-DMSVC_REDIST_DIR=<BuildTools>/VC/Redist/MSVC/<version>`. A Visual Studio Installer *Repair* is
  the real fix.

### Audio and documents (docs/plans/audio-and-documents.md, 2026-10-04)

**Audio (slice 1):** MP3, M4A and M4P are listed and open as audio-only clips on both platforms:
the cover art or a music card is the picture, the transport and keys are the clip's, `,` `.` jump
5 s, and a FairPlay M4P shows a padlock and does not play. Measured on the Mac only (`mv_tests
"[audio]"`, `playprobe` on MP3/M4A with and without art: seek, pause, play-out). The Windows half
(dir scan, installer, `main.cpp`) is compiled by CI only, and has not been run.

**Pages (slice 2) and PDF (slice 3):** `Ctrl+PageUp` / `Ctrl+PageDown` turn the pages of a TIFF or
PDF on both platforms ("Page n of m" in the notice line). PDF renders through the OS — CoreGraphics
on the Mac, Windows.Data.Pdf on Windows — at 3200 px, with a 1024 px first pixel and thumbnail; a
PDF that needs a password shows the locked card. Measured on the Mac (`mv_tests "[pdf],[pages]"`,
the full suite: 597 cases, 8 skipped for the absent RAW corpus). `pdf_win.cpp`,
`mv_folder_select_page` and the Windows page keys are compiled by CI only.

**DOCX (slices 4–5):** pages laid out and drawn by `codec/docx.cpp` over HarfBuzz and FreeType
(new vcpkg ports), with fonts found through CoreText / DirectWrite. Text, styles, lists, tables,
pictures and breaks; not headers, footers, footnotes, text boxes or columns (docs/design/04).
Measured on the Mac (`mv_tests "[docx]"`; a `textutil`-written DOCX rendered by
`MV_DOCX_DUMP=out.jpg MV_DOCX_FILE=x.docx mv_tests "[.docx-dump]"` and looked at). The Windows font
lookup (`fonts_win.cpp`) and the DLLs' packaging are compiled and staged by CI only.

**Settings → File types:** "Show audio files" and "Show documents" are off by default, which leaves
MP3/M4A/M4P and PDF/DOCX out of folder listings; the file a folder was opened on, and the current
stop, stay, so Open With still shows one. Windows filters in the core (`mv_folder_set_hidden_kinds`,
ABI 0.19, `[view] show_audio` / `show_documents` in settings.ini); the Mac host filters its folder
model's listing (`mv.viewFlags` bits 12–13). Test: `mv_tests "[settings]"`. Gallery folder-tile counts still include hidden kinds.

### Performance pass (2026-09-26)

Open and navigation latency, measured on an Apple M5 (60 Hz) with the macOS lab and the
headless bench (`mv_tests "[.perf-bench]"`), before and after, same build configuration:

| | Before | After |
|---|---|---|
| Launch → first pixel of an opened RAW/HEIC (`--soak 6 --static`, 6 samples) | 227–287 ms | 20–50 ms |
| Launch → full-resolution image, same runs | 261–861 ms | 43–618 ms |
| Arrow to the next photo (`--browse-soak`, 12 RAWs, median) | 60–64 ms | 0.3–0.4 ms (full resolution, prefetched) |
| Jump past the prefetched neighbours (median) | 54–61 ms | 52–61 ms |
| 24 MP JPEG decode (codec) | 137 ms | 116 ms |
| 24 MP Adobe RGB JPEG, decode + colour | 296 ms | 174 ms |
| Sub-folder scan, 3,000 files + 40 folders | 5.9 ms | 0.7 ms |

What changed. Shared core: JPEG decodes straight to RGBA through libjpeg-turbo's SIMD path
with no final copy; ICC transforms are cached by profile, applied in place and in bands
across up to four threads (`core/parallel.h`); date-taken sort looks each key up once.
macOS: an opened file starts decoding beside the folder listing instead of after it and a
0.2 s poll; the listing runs in the open job and notifies the UI directly; a relist no longer
re-decodes the photo on screen; ±2 neighbours are prefetched into a budgeted GPU still cache
once the current photo is full (the Windows LRU's twin); view-tied jobs run at
`USER_INITIATED` instead of on efficiency cores; folder scans skip `stat` for plain files;
a lost render-thread wake-up, the first spring step after idle, and video frame choice
before the display-link wait are fixed. The Mac lab gained `--browse-soak` and first-pixel
fields in its soak report, and measuring runs no longer take the keyboard.

Verified: the Mac PR 1 gate on the result (3,600 frames, 0 dropped, p99 16.85 ms; idle 0
presents, 0.28 % CPU) and the Mac test suites. **Not verified on Windows**: the shared-core
changes are built for Windows by CI only; the Windows charts in the README are the earlier
measurements until `tools/perf/regenerate.py` is re-run there. Still open on Windows: a
neighbour decode in flight when the user lands on it is cancelled and restarted
(`abi.cpp` `claim_decode`), and the folder open waits for the .NET chrome to attach.

PR 1's verify line is:

> **Presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle.**

The earlier measurements do not establish this verify line: review found an assumed
60 Hz refresh rate, incomplete statistics coverage, and an idle-input bug. A 2026-09-07
re-run of the corrected instrument on the development box passed one 60 s animated soak
and dropped frames on another the same night; idle zero-presents was not established
while the window could receive mouse input. The harness must pass both soaks on the
intended GPU runner before PR 1 is considered verified. See
[docs/design/12-decision-log.md](design/12-decision-log.md).

`frametime.exe` runs an animated soak and a static idle soak, each with one second of
warm-up followed by at least 60 seconds of measurement. It writes
`frametime-report.json` and `frametime-idle-report.json` beside the executable.
The animated soak opens a generated BMP so the cached-image blit is on the
swapchain, not the sweep bar.

- Animated: zero missed refreshes, continuous DXGI presentation statistics, frame counts
  and mean cadence within 2% of the current display rate, and p50 within 2% plus the
  histogram's 0.05 ms resolution. No interval may exceed twice the refresh interval.
- Idle: zero presents and no input during the measured window, with process CPU time at
  most 1% of **one CPU core**. This makes ~0% independent of the machine's core count.
  A cursor in the lab window counts as input and fails the idle gate; park it off the
  client area. When a run does fail, read `idle_input_events` before calling it a
  regression: observed failures on the development box have had it exactly equal to
  `idle_presents` — one present per real input event and none otherwise, which is the
  loop working correctly while somebody's mouse crossed the window. A genuine idle
  regression presents with `idle_input_events` at zero. This is the "quiet machine"
  caveat in [docs/design/12-decision-log.md](design/12-decision-log.md) showing up in practice.
- Missing refresh information, statistics gaps, interrupted runs, device rebuilds, and
  failing child exit codes cannot pass. Short `--seconds` runs are diagnostic only.
- A saved baseline additionally gates p99 regressions greater than 10%. Use a separate
  baseline path for a different display mode. Old schema-1 baselines are not accepted.

To save the exact animated report after both soaks and the regression check pass:

```powershell
.\build\bin\Release\frametime.exe --seconds 60 --update-baseline
```

CI runs the gate on PRs and main **only when a GPU runner is configured**. Provision an
interactive Windows runner labelled `self-hosted`, `windows`, `gpu`, with a visible
desktop, fixed refresh, pinned power profile, and no competing GPU work. Set repository
variable `MV_GPU_RUNNER_ENABLED` to `true` **only after provisioning it**. Leave it unset
when no runner is available: the job is skipped. That is not a D6 pass and not a hosted
measurement — hosted Windows cannot see dropped frames. Skipping avoids a 24-hour queue
for a nonexistent runner and avoids failing every PR on a machine that does not exist
yet. An enabled runner that later goes offline is still subject to GitHub's queue
timeout. Fork PRs skip this job; they do not execute on the persistent GPU runner.

Once the runner exists, make **Frame-time gate (self-hosted GPU)** a required
branch-protection check. Until then, requiring it does not protect anything: GitHub
treats a skipped job as success for merge. Baseline cache entries use unique keys and
only successful main pushes publish a new baseline; PR runs compare against the restored
main baseline. The repository changes cannot provision a runner or configure branch
protection by themselves.

Every later PR inherits this verify line.

PR 3's verify line is:

> **Zero dropped frames while panning a cached image at the display's refresh rate,
> unchanged from PR 2 now that chrome is on screen — if hosting chrome costs frames,
> that is the bug. Focus and tab traversal cross the island boundary correctly; a
> flyout opens over the canvas without clipping.**

The island is a command bar, not a `SwapChainPanel`. `--no-chrome` is a diagnostic
escape, not the shipped shape. Focus/tab and flyout-over-canvas still need a human
pass; the unit tests cover hostfxr load and struct layout, not those.

PR 4's verify line is:

> **2000 mixed JPEGs — filmstrip scrolls without a hitch, second folder visit has
> near-instant thumbnails, arrow-key browse shows the next image in < 40 ms warm.**

Folder listing + sort (name, mtime, size, type; EXIF date taken arrived with PR 9, see the sort menu).
Portable `io/dir.h`, Windows impl in `io/dir_win.cpp`. The filmstrip is a second
XAML island on the same HWND (bottom strip), not a full-client island and not
thumbs blitted onto the photo swapchain. SQLite + on-disk JPEG-512 cache keyed
by `(path, mtime, size, spec)` with spec `jpg512.1`. Visible-first generation,
directional prefetch of +/-2 into a byte-budgeted GPU LRU (512 MB, 3-12 entries --
five 45 MP stills and five phone JPEGs are the same count and a 10x difference in
VRAM). On-disk BC7 waits; DirectXTex is not a PR 4 dependency. Completions are
drained by C# once the island is attached.

Browsing is view-tied work. Selecting an item bumps the job generation, so the
decodes and prefetches a held arrow key ran past are abandoned rather than
finished for an image nobody is looking at, and the pool serves foreground work
ahead of the background thumbnail sweep. A selection that has to wait on a decode
raises a load indicator under the command bar after 150 ms; a hit in the viewer
LRU publishes in the same drain and never shows one.

The 2000-JPEG scroll and under-40 ms warm-browse clauses are interactive, not CI.
`ctest` covers listing, cache hits, ABI folder/thumb jobs, and — from PR 5 —
container probing by magic bytes, the presentation ring, colour mapping, the
presenter's show/hold/drop rules, transport policy, the drift tracker, the audio
sink, and two real-clip integration tests over seek, frame step, resume and the
video ABI.
`tools/check-hostable-core.ps1` is the D9 gate. PR 1's present-loop verify is
inherited.

PR 6's verify line is:

> **Keyboard-only browse of a real folder — open, next/prev, zoom/fit/100 %, mark,
> copy-to a destination, delete to Recycle Bin, fullscreen, slideshow start/stop —
> without the mouse, with `?` listing those bindings; a file dropped into the folder
> appears without restart; animation timing matches a browser; PR 1's present-loop
> still holds.**

What is demonstrated, on the same developer box:

- Every verify binding is in the one command table, and the table the `?` sheet
  receives lists each of them (`test_key_router.cpp`); no command is
  left pending.
- Copy/move never overwrite (`name (n).ext`), a cross-volume move is copy,
  verify, delete, and a drive with no Recycle Bin refuses rather than deleting
  permanently — `test_file_ops.cpp`, `test_file_jobs.cpp`. The selection survives
  a watcher refresh (`test_folder_reselect.cpp`).
- Animation delays follow browsers (10 ms or less plays as 100 ms), and the frame
  schedule, ring and seek are unit-tested. A 2048² 60-frame GIF and a 2048²
  ICC-tagged animated WebP each soak 20 s at refresh with 0 dropped frames and a
  p99 of 17.0 ms at 60 Hz.
- The 60 s present-loop gate passes, and 30 consecutive launches exit cleanly
  with the chrome attached.
- Display colour (review 43): an 8-bit LittleCMS LUT matches the previous float
  pipeline within one code per channel, sRGB-in-effect profiles copy through, and
  a 2048² tagged frame converts in under 80 ms in Release (`test_gif_webp.cpp`).
- The lab JSON report (schema 2) includes animation frames shown/made, late
  frames, mean delay and last make/ICC times (review 44), so an animation soak
  can fail on a stalled decode, not only on dropped presents.

What is **not** demonstrated, and should not be claimed:

- The mouse-free walk of the whole line by a person, and "animation timing
  matches a browser" side by side. Both are human checks for the PR.
- A file dropped into the folder by Explorer appearing without restart, as
  opposed to the watcher and reselect unit tests.

Slipped from PR 6, recorded in [docs/design/12-decision-log.md](design/12-decision-log.md):
the folder-tree island to PR 9 (landed there: a left-hand island, floating over the canvas), and hiding companion files to PR 7.

PR 7 (in progress) pairs files at scan time: a camera's `DSC_0001.JPG` +
`DSC_0001.NEF` (or HEIC + RAW) and an iPhone Live Photo (`IMG_0001.HEIC` or
`.JPG` + `IMG_0001.MOV`) are **one** filmstrip entry and one arrow-key stop,
badged RAW or LIVE. `;` plays a Live Photo's motion once and returns to the
still. Copy, move and delete act on both files of a pair. "Open RAW of pair" /
"Open JPEG of pair" have no default key; assign one in Settings (`Ctrl+,`).
`.xmp`, `.thm`, `.aae`, `.wav`, hidden and system files are never listed.

PR 7's decoders are in. Measured on five CC0 raw.pixls.us samples (CR2, NEF,
ARW, CR3, DNG): the embedded preview is on screen in 11–69 ms, about a JPEG's
first pixel, and the full LibRaw decode takes 0.8–1.7 s — slower than docs/design/09's
500 ms target, recorded as open in [docs/design/12](design/12-decision-log.md)
(2026-09-14). The original file's hash and mtime are unchanged after both. Test
media is not in git: `tools/testmedia/fetch-raw.ps1` and `fetch-heif.ps1`
download pinned, hash-checked samples, and the tests that need them skip
visibly without them (`MV_REQUIRE_CORPUS=1` makes that a failure). Generated
HEIC/AVIF fixtures live in `tests/data/`; `MV_OS_CODEC=0` forces the bundled HEIC
decoder, as on a clean VM.

The three clauses that used to be "a person must squint at it" are now measured
as far as they can be without a clean VM and a phone:

- **Clean VM.** `mv_clean_vm_tests` (`ctest -L cleanvm`) decodes a HEIC with
  `MV_OS_CODEC=0` and then reads the process module list: `heif.dll` and
  `libde265.dll` must be mapped, and Media Foundation, the WIC codec extensions
  and anything under `\WindowsApps\` must not be. The disabled path is also
  asserted to load *no* module at all, so the `MFTEnumEx` probe cannot creep
  back in. It is a separate executable: inside `mv_tests` an earlier video case
  has already loaded `mfplat.dll` and "absent" could never be shown again.
  What is left for a person: open an iPhone HEIC on a genuinely clean VM.
- **Live Photo.** `tests/test_live_photo.cpp` lists a *real* directory shaped
  like a camera roll — `IMG_0001.HEIC` + `.MOV`, the "Most Compatible" JPG pair,
  the `.AAE` sidecar, the `IMG_E####` edited copy, an iCloud `(1)` duplicate, a
  still whose motion half never came down, a plain clip — with real HEIC bytes,
  and checks the stops, their order and which half is primary. A *hidden* motion
  half must not be attached to a still. With the corpus present, the pair's MOV
  is a real playable clip and `;`'s own ABI calls are asserted to put motion on
  the canvas. What is left for a person: confirm a file straight off a phone
  carries these names, and that an iPhone's HEVC motion track plays.
- **No pop.** The lab counts what a pop is made of — the worst corner shift of
  the picture's on-screen rectangle across the swap, the worst step in its
  on-screen size, whether every cross-fade reached alpha 1, and any frame
  dropped inside a fade — in the `--json` report and on `F3`.
  `frametime --no-pop <image>` gates them together with PR 1's cadence.
  Measuring it found a real pop: a still refines twice (`full_top`, then `full`
  with its mips) and the second publish used to restart the cross-fade, snapping
  the outgoing preview out at alpha 0.6 — on a RAW, most of the
  preview-to-render brightness difference in one frame. A same-size refinement
  arriving mid-fade now swaps the incoming texture under the running fade. A
  62 s soak on the Canon CR2 then passed both gates on the development box: one
  fade, started and completed, 15 frames, no drops, 0.70 px of view shift. The
  PR 1 caveat above still applies — this is a developer box, not the quiet GPU
  runner. What is left for a person: look at a RAW opening, once.

PR 5's verify lines are:

> **5a —** 4K 10-bit HEVC and AV1 play at full rate with GPU video decode > 0 in
> Task Manager, on a clean VM with no Store codec packs; an iPhone HLG clip looks
> correct rather than washed out; the decoder never stalls waiting for a surface
> over a 10-minute play; photo → video → photo leaks no textures.
>
> **5b —** A/V drift flat over 30 minutes, with the overlay to prove it;
> unplugging the audio device mid-playback recovers without stopping video; a
> clip with no audio track plays at correct speed.
>
> **5c —** Scrubbing feels instant; frame step lands on exact frames in both
> directions; media keys and the OS overlay work; resume returns to the right
> position.

What is actually demonstrated, on a developer box with an NVIDIA GPU:

- 4K 10-bit HEVC and 4K 10-bit AV1 open on **D3D11VA**, asserted by `ctest`
  rather than eyeballed in Task Manager — `avcodec_open2` keeping the
  `hw_device_ctx` is the honest test, and the software fallback fails it instead
  of passing slowly. Not yet run on a clean VM.
- Frame step lands on exact frames in **both** directions, and **resume** returns
  to the right position — both against a real clip in `test_video_transport.cpp`.
- A silent clip plays at **correct speed** on the host-clock fallback (5b's third
  clause), measured with `--av-soak`.
- **No texture leak** across four photo → video → photo cycles, by device
  refcount.

What is **not** demonstrated, and should not be claimed:

- **The 30-minute A/V drift soak has not been run.** The corpus now contains a
  31-minute clip with an audio track so that it *can* be — before that, every
  clip but one was silent and the soak ran on the QPC fallback, proving nothing
  about the audio master. Note the caveat: a synthetic sine against synthetic
  video exercises the drift-slope gate, not endpoint-crystal versus
  container-timebase mismatch. A real 30-minute phone clip is worth more.
- The 10-minute no-stall soak. The `ctest` case is a 5-second smoke test of the
  same design and says so.
- HLG "looks correct rather than washed out". The transfer, OOTF, BT.2408 white
  point and tone-map are implemented and the colour *description* is asserted end
  to end, but no golden-image test reads the shader's pixels back, and no human
  has recorded a verdict. All the clips are synthetic; none is an iPhone HLG clip.
- Audio device unplug/recovery on real hardware (covered only through the fake
  sink), media keys and the OS overlay, and "scrubbing feels instant".

The corpus is the load-bearing part of all of that: without
`tools/testmedia/generate.sh` having been run, twelve cases skip and every
hardware-decode, colour, seek, frame-step, texture-leak and video-ABI assertion
above disappears. `ctest` reports those as **Skipped**, not passed, and
`MV_REQUIRE_CORPUS=1` turns them into failures — set it in CI, or a build that
proves nothing about video will look exactly like one that proves all of it.

PR 2's verify line is:

> **A 12 MP JPEG pans at refresh with zero decode on mouse move; dragging the window
> while a 60 MP PNG loads stays smooth; a tagged AdobeRGB JPEG renders correctly and
> an untagged one is treated as sRGB, with no tone-map applied to either (D6).**

Colour is covered by the unit tests (untagged mid-grey stays mid-grey; tagged AdobeRGB
does not decode as sRGB). `frametime.exe` still soaks the **empty** present loop — an
animated bar, then idle. It does not `--open` an image, so the pan-at-refresh clause is
not measured yet. Pan itself does not start a decode (mouse move only updates the
camera); that is architectural, not a 12 MP soak. TIFF is not in until PR 7.

Known holes on this slice, recorded in [docs/design/03-rendering.md](design/03-rendering.md) and
[docs/design/04-image-pipeline.md](design/04-image-pipeline.md): an idle renderer must be woken
when a decode completes; CPU mip sizes must match D3D11's floor chain; LittleCMS needs a
per-job context on the pool.

PR 8's verify line is:

> clean VM → run the wizard (no UAC) → Start Menu shortcut shows the app icon → Launch
> from the finish page → open a real camera dump → browse photos, play video with audio
> and transport, pan/zoom, fullscreen, and slideshow using the PR 1–7 feature set, with no
> SmartScreen block and **no missing-codec dialog anywhere**. […] An update downloads and
> stages without showing the wizard. Uninstall from Apps & features removes the shortcuts
> and install directory. […] Exercise update signature rejection and rollback, and confirm
> telemetry stays off unless explicitly enabled.

**None of the clean-VM half has been run.** What is demonstrated, on this development
machine:

- The wizard compiles, installs per-user with **no UAC**, and produces the layout docs/design/13
  specifies (root stub, `Update.exe`, `current\`, `packages\`). One Start Menu shortcut,
  no desktop shortcut, exactly one Apps & features entry. Uninstall exits 0 and leaves no
  directory, no shortcut and no registry entry.
- The app icon is a single `.ico` with all eight required sizes (16/20/24/32/40/48/64/256),
  and all eight reach the Velopack stub, the payload exe and the wizard. The Start Menu
  shortcut takes its icon from the stub.
- The installed build launches and exits cleanly.
- The updater's signature-rejection suite passes (46 cases: tampered and missing
  signatures, the unconfigured placeholder key, wrong channel, downgrade, blocklist,
  a version already rolled back on this machine, and package hash/size mismatch).
- Telemetry is off, never-asked, and records nothing by default, asserted in tests.
- The payload needs nothing pre-installed: it carries the Windows App SDK runtime and the
  .NET runtime, and an installed copy provably loads `hostfxr`, `coreclr` and
  `Microsoft.UI.Xaml` **from its own directory** rather than from Program Files or a
  framework package.

**Not yet demonstrated, and needed before the verify can be attempted:**

- A clean VM. Everything above ran on a machine that already has the Windows App SDK
  runtime, .NET, and every codec DLL in a build tree. "Provably prefers ours" on a machine
  that has both is weaker evidence than "works on a machine that has neither", and only a
  clean VM settles the "no missing-codec dialog anywhere" clause.
- **No SmartScreen block** cannot be true of an unsigned artefact, and cannot be tested
  without a signing credential. See [Package and install](#package-and-install-pr-8).
- `tools/package/e2e-update.ps1` passes **16 of 16**. A tampered manifest stages nothing;
  a signed one downloads while the viewer runs, keeps the previous package for rollback,
  and applies on exit **without showing the wizard**; a good version clears its trial
  record; and a build that never finishes starting is counted twice, rolled back on the
  third start, recorded as failed, and not offered again. That covers the verify line's
  "an update downloads and stages without showing the wizard" and "exercise update
  signature rejection and rollback" — on this machine, not on a clean VM.
- The browse/play/pan/fullscreen/slideshow pass over a real camera dump, on the installed
  build rather than the build tree, and PR 1's present-loop verify against it.

PR 8's wizard registers the file associations ("Open with", a Default Apps candidate) and
removes every key on uninstall. PR 15's thumbnail handler is registered by the app on first
start (`src/shell/shellext_install.cpp`); the wizard creates its keys so uninstall deletes them,
and removes the versioned copies under `shellext\` (`tools/package/mediaviewer.iss`).

## Layout

```
src/core        job system, result<T>, lock-free rings, ETW
src/io          whole-file reads, directory listing + watcher, copy/move that never
                overwrites, Recycle Bin (Windows impl), the replace port for edited
                pixels (replace_win / replace_mac: never-overwrite export, atomic swap)
src/codec       JPEG / PNG / BMP / GIF / WebP / TIFF / ICO / HEIC / AVIF / RAW,
                APNG walker, frame-at-a-time animation sources, magic-byte probe,
                HEIC-only OS-codec probe (WIC, os_decode_win.cpp)
src/image       LCMS colour (8-bit display LUT; sRGB copy-through), CPU mips,
                immutable GPU upload, JPEG-512 thumbs
src/meta        EXIF / IPTC / XMP (Exiv2) + container / stream (libavformat) read
                model, summary rows, AF geometry (PR 9)
src/edit        EditStack + geometry (the blit's output -> source map), lossless
                JPEG rotate / flip / MCU crop, JPEG / PNG export with a metadata
                policy (PR 10)
src/canvas      pan/zoom springs, fit / fill / 100 %, sticky zoom
src/gfx         D3D11 device, flip-model swapchain, frame pacer, blit
src/addon       the add-on host: MediaViewer's own add-ons (signed manifests, store,
                loader, host function table) and open add-ons from other makers
                (package reader, manifest schema 2, publisher keys, themes; docs/design/25)
src/abi         the flat C ABI — the top of the native graph; animation session
src/shell       Win32 window, render thread, present lab, hostfxr island host,
                key router and live command table, marks, slideshow, file jobs,
                Settings persistence
src.managed/    C# interop and WinUI chrome (hosted as an island, not the app):
                command bar, filmstrip, gallery, `?` sheet, Settings,
                file drag/drop
tests/          Catch2 suites for core, gfx, codec, colour, camera, ABI, folder,
                key router, file ops, slideshow, animation
tools/          frametime harness, module-graph, hostable-core, and licence gates;
                addon-sdk (MIT): the tool people make add-ons with
examples/       an add-on that packs and installs as it is (MIT)
docs/design/    how each subsystem works (design reference)
docs/plans/     forward plans not built yet
```

Dependencies point downward only —
`shell → abi → {canvas, edit, player, image, meta} → {codec, gfx, io} → core`. No back-edges,
and nothing may depend on `shell`. That is what keeps the core testable with no window, and
`tools/check-module-graph.ps1` enforces it on every push.

## Design reference

[docs/design/](design/README.md) describes how each subsystem works today: architecture,
rendering, the image and video pipelines, metadata, editing, the ABI, platforms, commands and the
add-ons. Code comments cite it as `docs/design/NN §section`. It replaced the original `plan/` spec
on 2026-10-03; the spec's full text is in git history. Forward plans that are not built yet are in
[docs/plans/](plans/).

The parts worth knowing before touching anything:

- **[01-decisions.md](design/01-decisions.md)** — the stack and the settled decisions D1–D9.
- **[10-roadmap.md](design/10-roadmap.md)** — what each PR number delivered on both platforms, with
  its verify line.
- **[12-decision-log.md](design/12-decision-log.md)** — why current behaviour is the way it is.
- **[14-abi.md](design/14-abi.md)** — the C ABI between the hosts and the core.
- **[15-platforms.md](design/15-platforms.md)** — Windows and macOS hosts over one core.
- **[18-import.md](design/18-import.md)** — the Import add-on and how add-ons install.
- **[25-open-addons.md](design/25-open-addons.md)** — add-ons from other makers: the package,
  the trust model and the contribution points ([ADDONS.md](ADDONS.md) is the author's guide).
- **[20-edit-workspace.md](design/20-edit-workspace.md)** and
  **[21-video-editor.md](design/21-video-editor.md)** — the Edit pane and the Video Editor window.

Rules that do not bend: nothing blocking touches the UI or render thread; the canvas is a
native swapchain C++ owns (D3D11 on Windows), never XAML; first pixel is never the full
decode; zero dropped frames panning a cached image, measured rather than eyeballed; never
modify an original; nothing about a user's files leaves the machine; never require a Store
codec pack.
