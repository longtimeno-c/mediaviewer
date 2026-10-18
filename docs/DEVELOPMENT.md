Build, test and status notes for contributors. The product overview is in the [README](../README.md); links below are relative to the repo root.

# MediaViewer: development notes

A Windows viewer for a real camera dump — photos and video in one folder. Opens everything
instantly and pans without a dropped frame. The first release ships the viewer through
PR 7, packaged in PR 8; metadata tools, photo edits/export, video trimming, and additional
Windows integration follow in future updates. **v1 is Windows.** From PR 4 the native core is
kept hostable; macOS is Milestone F (the Mac halves of PRs 1–8), a host of the same core, not a UI-only
port — see [docs/design/15-platforms.md](design/15-platforms.md).

**One PR number per feature, on both platforms (2026-09-24).** The Mac host, first built as
PRs 16–20, is now filed as the Mac halves of PRs 1–8, and from PR 9 every PR lands on both, with one shared core change, a WinUI half and a SwiftUI half,
and a verify line on each platform (D9, amended). The order is:

| PR | What | State |
|---|---|---|
| 9 | Metadata read: pane, overlays, AF points, eyedropper, folder tree, date-taken sort | In main, both platforms ([docs/design/10](design/10-roadmap.md#pr-9--metadata-read)) |
| 10 | Geometry edits + export, lossless JPEG rotate | In main, both platforms |
| 11 | Colour adjusts, plus Mac crash reporting (Crashpad + the same scrub as Windows) | In main, both platforms |
| 12–15 | Metadata write · two-path trim · extract & remux · OS integration | In main, both platforms; built and tested in CI (Windows) and on the Mac. Their hands-on verify lines are not recorded as run on either platform |
| 16–19 | **Import add-on**: copy cards with content-hash duplicate skip, verify, date folders, backup, resume ([docs/design/18-import.md](design/18-import.md)) | In main, optional download, packed and signed by the release workflow; hardware verify owed |
| 54 | **Find duplicates** in the Import add-on: a folder tree compared by content, one copy at a time to the Recycle Bin / Trash, never the last ([docs/design/18](design/18-import.md#find-duplicates-pr-54)) | Engine tested on the Mac (Release, TSan); Swift and C# compile; Windows native build, both live windows and both gates while scanning owed |
| 20–24 | Local AI search add-on, both platforms (Core ML on Mac) ([docs/design/17-local-ai-search.md](design/17-local-ai-search.md)) | Built and tested on both with the real pack (PR #59); quiet-machine gates and the Mac in-app walk-through owed. Mac Photos library as a source (issue #72): in main (PR #86). [docs/design/26](design/26-photos-library.md) (2026-10-03): the library as a folder (virtual list items), its backup to a folder or NAS, and the display-matrix fix for clips on both platforms; engine tests pass, the in-app Photos walk-through needs a build with Photos access |
| 27–28 | **Voice query add-on**: speak a Local search query, on-device, as its own download ([docs/design/19-voice.md](design/19-voice.md)) | Proposed |
| 29 | **Edit workspace**: an Edit image / Edit video button, a docked Edit pane, crop presets, every metadata tag editable ([docs/design/20-edit-workspace.md](design/20-edit-workspace.md)) | Both halves written and run on their platform (PR 54); Mac build of the merged tree, the quiet-machine present-loop gates, Narrator / VoiceOver owed |
| 30 | **Video Editor**: its own window with the viewer's canvas as the preview, a timeline (thumbnails, waveform), Split / Delete, marked ranges (`I` `O`, Delete, `X`), Trim start / end (`[` `]`), draggable piece edges, `J K L` shuttle, frame timecode, Undo, Export as keyframe cuts or exact on the hardware encoder; ABI 0.13 `keep_ranges` ([docs/design/21-video-editor.md](design/21-video-editor.md)); the Editor add-on is proposed ([docs/design/22-editor-addon.md](design/22-editor-addon.md)) | Both halves written and run on their platform (PR 55, `MV_EDIT_SELFTEST`, a key walk on Windows); present-loop gates with the editor open, an interactive-desktop pass, Narrator / VoiceOver and encoder spike S1 on Windows owed |
| 49 | **Fast network copies**: F8 and Import to or from a share keep several requests and files in flight, still verified; `copybench` measures it ([docs/design/24-transfer.md](design/24-transfer.md)) | Engine and Mac F8 built; `mv_import_tests "[io]"` passes, TSan-clean; Windows half compiled by CI only; the 10 GbE share run and the present-loop gates owed |
| 50 | **Transfer**: a general copier (copy/move any files and folders, verified, resumable) and an SMB link check | Planned |
| 55 | **Open add-ons**: add-ons anyone can make, one `.mvaddon` file signed by its publisher, installed from a file or a link with a sheet that says what it is; themes as the first thing one can contribute; the author's tool and guide ([docs/design/25-open-addons.md](design/25-open-addons.md), [ADDONS.md](ADDONS.md)); ABI 0.17 | Shared core and SDK tested on the Mac (ASan / UBSan) and cross-checked against each other; both halves run in the app (Mac: `MV_ADDON_SELFTEST`; Windows: by hand, 2026-10-03), launch and pacing measured against the base on both; install from a link, `.mvaddon` double-click on a rebuilt installer, VoiceOver / Narrator and the idle clause of both gates (which fails on the base too) owed. PRs 56–60 are planned; the owner's calls for 57–60 were made 2026-10-03 (docs/design/25 §17) |
| 56 | **Import described by its manifest** (the first half of [docs/design/25](design/25-open-addons.md)'s declarative contributions): its commands, keys, Settings line and card hint come from `manifest.json`; both chromes drive it through one generic run-command call; an older installed Import keeps working through the built-in rows; ABI 0.18 | Mac: shell and manifest suites, the packer cross-check, the full suite and the rig pass; a signed Import with the new manifest has not been loaded in the app. Windows: both C# projects compile; the native side is CI's |
| 56 | **Declarative contributions** (the second half; contribution API 2): an add-on's settings page drawn from its manifest on both hosts, its values kept per add-on (and on remove unless asked), key layouts (keymap packs) applied once with Reset back to the defaults, theme shape (radius, density, type scale), and Import / Duplicates / Local search on the host's theme tokens ([docs/design/25](design/25-open-addons.md#pr-56-second-half-declarative-contributions-2026-10-11), [ADDONS.md](ADDONS.md)); ABI 0.24 | Mac: Catch2 (schema, values, keymaps over every built-in command, shape), the SDK cross-check, 2.5 M fuzz inputs, the full suite and the rig's PR 56 leg pass in the app; launch and pacing measured against the base on a loaded machine (no difference; the idle CPU clause fails on base too). Windows: every C# project compiles clean; the native half is CI's; nothing run in the app yet |
| 57 | **Add-on scripts**: other makers' add-ons run Lua 5.4 (vendored, static) in a sandbox per add-on on its own worker, with commands on keys, permissions in the consent sheet (`folder.read`, `thumbnails`, `selection`; no network) and limits (64 MB, 100 M instructions and 1 s per task, a watchdog) that stop the add-on and say why in Settings ([docs/design/25-open-addons.md](design/25-open-addons.md#implementation-notes-pr-57-2026-10-11), [ADDONS.md](ADDONS.md#scripts)); ABI 0.26 | Mac: built, `mv_script_tests` and the SDK cross-check pass, the key walk runs in the app (`MV_SCRIPT_SELFTEST`), the fuzz harness ran ~15 min clean, Mac PR 1 gate and launch timings measured with a script spinning / installed. Windows: compiled in CI only (shell, ABI, chrome); the Windows key walk, the Windows gate with a script spinning and Narrator / VoiceOver on the new Settings lines owed |
| 58–59 | **Screens and slots**: MediaViewer's own native add-ons add to pages that exist (an info-pane section at the very bottom of the Metadata pane, file search and Local search providers, command-bar / path-bar / status items, gallery tile badges, context-menu entries) and open windows, docked panes and sheets from a view tree both chromes render ([docs/design/25](design/25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)); host table v3; ABI 0.25 | Mac: the shared suite passes (with a real test add-on through a signed store; also under ASan / UBSan and TSan); the app built and, with the test add-on sideloaded, drew its section at the bottom of the pane and its items. Windows: both C# projects compile; the native side is CI's. Measurements and the keyboard / VoiceOver / Narrator passes are owed ([Where this actually is](#screens-and-slots-prs-58-59-2026-10-11)). Script add-ons wait for PR 57; Places is #344 |
| #410, #413 | **Sharepoint**: send photos, video and audio to a MediaViewer nearby over Wi-Fi or Bluetooth; the receiver shows a code the sender types (#442) and nothing moves until the other person accepts; its window is a view tree both hosts draw ([docs/design/28](design/28-sharepoint.md)). The gallery's Shift-click range, drag-out from any gallery cell and one share icon that opens Sharepoint; ABI 0.29 | Mac: built; `mv_import_tests "[sharepoint]"` (two and three engines over loopback TCP), the full suites and the packer test pass; the window opened offline in a dev copy and the range drove copy path there; the PR 1 gate passed with the add-on installed. Windows: the C# compiles; native (sockets, WinRT Bluetooth, the range) is CI's. Two machines, Bluetooth and the hand checks are owed ([Where this actually is](#sharepoint-issues-410-413-2026-10-15)) |
| #444 | **Share everywhere**: one share command (`Ctrl+Shift+S` / `⌘⇧S`, the gallery's share icon, a share button beside Home, right-click Share ▸ on a gallery cell, a filmstrip cell or the canvas) opens one menu: Sharepoint first (or its install), then Windows Share / the macOS picker (Mail, Messages, AirDrop…). A right-click on a selected file shares the selection, on any other file that file; the system sheet gets media only ([docs/design/16 "Share"](design/16-commands.md#share-444)) | Mac: built; `mv_tests "[share]"` passes (file set, wire argument, command row; the `[.perf-bench]` case, 500 of 20 000 stops: 0.12 ms); the macOS picker for 500 files opens in 21 ms warm (was 65) by showing one file per kind past 50 files. Windows: the C# compiles; native is CI's. Owed: Mail with 3 files on both, the right-click menus and bar button by hand, a 500-item open on Windows, both PR 1 gates |
| #452 | **Sharepoint in the background**: opt-in **Receive when MediaViewer is closed** keeps the same process with a menu-bar item (Mac) / notification-area icon (Windows) after every window closes; an offer is a system notification (sender, count, size; never a file name or the code) that opens the window with the code, with Decline; Accept stays in the window ([docs/design/28](design/28-sharepoint.md#in-the-background-452)); ABI 0.33 | Mac: built; `"[sharepoint]"` and `"[slots]"` pass with new cases, full suites pass; the Mac PR 1 gate's animated soak passes (idle phase failed on base and new alike on a loaded machine). Windows: the C# compiles; native (tray, hide-on-close) is CI's. Owed: the windowless idle measurement in the app, the notification seen and clicked on both, two machines ([Where this actually is](#sharepoint-in-the-background-issue-452-2026-10-16)) |

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
On top of that, the **PR 10 geometry edits** (Windows and macOS, same core): `[` `]` rotate and `H` `V` flip a still — on a JPEG the file itself is rewritten *losslessly* (DCT coefficients rearranged, never re-encoded; atomic swap) — `Shift+C` crops and straightens, `Ctrl+Z` / `Ctrl+R` (`⌘` on Mac) undo / reset, and `Ctrl+S` opens an export dialog (format, quality, size, metadata) that writes `<name>-edit.jpg` beside the original with its metadata carried over (orientation and dimensions corrected). JPEGs are now displayed through their EXIF orientation, so thumbnails regenerate once.
Then the **PR 11 colour adjusts** (Windows and macOS, same core): `Shift+A` (`⇧A` on Mac) opens an adjust pane with exposure, contrast, saturation, temperature and tint, a histogram and a clipped-highlights / crushed-shadows readout. Slider drags only change shader uniforms — nothing is re-decoded — and the colour is worked in linear light from an FP16 working image; for a RAW the sliders stay disabled ("Preparing…") until LibRaw's full linear develop is ready, never the embedded preview. Export (`Ctrl+S`) bakes the same maths at full resolution. The Mac half also brings **crash reporting**: Crashpad out of process, the Windows privacy scrub (now aware of `/Users/…`-style paths), and uncaught `NSException`s recorded with the id of the native call they happened in.
Then the **PR 12 metadata writes** (Windows and macOS, same core): keypad `0`–`5` (or `⌘⇧0`–`5`, `Ctrl+Shift+0`–`5` on Windows) rate the photo on screen, and `⌘I` puts the keyboard in the metadata pane (on the Mac, the *All tags* search; on Windows, the comment field). A plain JPEG is rewritten in place, checked against the original before it replaces anything; every other format (RAW, HEIC, PNG, video, …) gets an `IMG_1234.xmp` sidecar beside it and the original is never opened for writing. The pane has clickable stars, the comment and a "Revert metadata" button. **On a Mac, `⌘⇧3`/`4`/`5` are the system's screenshot shortcuts and never reach the app; use the keypad or turn those shortcuts off.** See [docs/design/12](design/12-decision-log.md) 2026-09-25.

And **PR 13 / 14 clip editing** (Windows and macOS, same core): on a clip, `Ctrl+T` (`⌘T`) arms trim — `[` `]` set in and out, the scrub bar shows the keyframe grid and what will be kept, `P` previews the cut as a loop, `Enter` saves an instant keyframe cut (stream copy, no quality loss) and `Shift+Enter` a frame-accurate re-encode on the GPU's hardware encoder (NVENC / Quick Sync / AMF / Media Foundation, VideoToolbox on Mac; labelled slower). `Ctrl+S` on a clip opens the clip tools: lossless rotate, split, remove in–out, MP4 ↔ MKV remux, save the frame as PNG / JPEG, extract the audio (copy, WAV or FLAC), and GIF / WebP. Every result is a new file beside the clip (`<name>_trimmed.mp4`, …); the original is never touched, and jobs run in a Jobs pane (`Ctrl+J`) where they can be cancelled without leaving a partial file. Anything that decodes or encodes runs in a separate helper process (`MediaViewerClipJob`), so a crash in a GPU driver fails that one job and never the viewer. The shared core is also tested headless on Linux ([tools/portable](../tools/portable/README.md)).

**PR 15 OS integration** (Windows and macOS, same command rows): `Ctrl+Shift+C` (`⌘⇧C`) copies the marked or current file's path as text; `Ctrl+Alt+C` (`⌘⌥C`) copies the photo as you see it, edits applied, as a PNG (both a file and an image, so it pastes into Explorer / Finder and into Word, Keynote or a chat; no EXIF rides along); `Ctrl+Shift+S` (`⌘⇧S`) opens the share menu: Sharepoint, then the system Share sheet (#444). The folders you open show up as **Recent folders** in the taskbar jump list and in the Dock icon's menu, in File ▸ Open Recent on the Mac, and as clickable rows on the empty window's welcome card (up to six, as many as the window has room for; a folder that has gone is dropped when clicked, and the × on a hovered row removes a folder from every recent list without touching it on disk). The card and the host share one layout (`src/shell/welcome_layout.h`), so the row drawn and the row hit are the same. The taskbar thumbnail gains previous / play-pause / next buttons, and on the Mac, Control Centre, the media keys and AirPods drive a clip through Now Playing. Explorer gets MediaViewer's thumbnails (HEIC, AVIF, RAW and the rest) for the file types you make MediaViewer the default for; the handler runs outside Explorer, so a damaged file can't take Explorer down. Explorer's Details-pane properties need a machine-wide install and are deferred. On the Mac, Spotlight learns the length, size and codecs of MKV, WebM, AVI and TS clips (macOS already indexes photos and MP4/MOV itself). `Ctrl+Alt`-drag (`⌘⌥`-drag) drags out the edited copy, and opening a file while MediaViewer is running opens it in the running window instead of starting a second one (`--new-instance` overrides). `Ctrl+N` (`⌘N`, File ▸ New Window) opens another window as its own process. Several windows grouped as tabs come in a later update ([docs/design/10](design/10-roadmap.md) PR 15). The macOS half is built, its tests pass and the Metal present-loop gate holds; the Windows half builds in CI. Neither platform's hands-on verify (Explorer / Finder, installed builds) has been run yet.
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
apply while the app is open. There is no separate canvas background: the empty
window, the welcome card and the area around a photo or a video all take the
theme in use (Default follows the system), and a built-in theme's ground effect
moves behind the welcome card. Build and interactive checks are documented in
[System appearance verification](system-theme-verify.md).

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
| **`mediaviewer_lab` (Darwin)** | Mac PRs 1–6 Metal present lab. AppKit window, `CAMetalLayer` (max drawable 2 — Metal's minimum, see docs/design/12 — 8-bit sRGB), `CAMetalDisplayLink` wait-before-encode, idle → stop presenting, F3 overlay. Decodes a JPEG/PNG/BMP (plus the rest of the D5 stills) onto an immutable Metal texture; wheel-zoom-toward-cursor, drag-pan, `0`–`4` zoom presets, `+`/`-`, keyboard pan when zoomed, hold `Z` loupe, hold `\` previous, `C` clipping blinkies. Real folder browsing: argv/drag-drop opens a folder or a file (selecting it), `←`/`→`/`A`/`D`/`Space`/`Home`/`End`/`PageUp`/`PageDown` navigate it (every key goes through the same command table and key router as Windows, with `⌘` standing for `Ctrl` and the Mac Delete key for `Delete`), an FSEvents watch keeps the listing live. SwiftUI chrome hosted in the same window via a C bridge into the render thread's `input_snapshot`: a Windows-style command bar (Open / View / Settings / About, `?` at the right), a Settings screen (`⌘,`: filmstrip/wrap/sticky-zoom preferences and remappable keys, persisted in `NSUserDefaults`), a bottom filmstrip (`T` toggles) and a full-grid gallery overlay (`G` toggles), both lazy-loading JPEG-512 thumbnails from a shared SQLite cache. **Nested folders (PR 26):** child folders show as tiles. A folder of only folders uses big tiles; one that also holds photos keeps a short folder row above them. A tile says when photos were found further down, when it is only more folders, and when that look stopped early. The path stays on screen while a photo is open. `⌘↑` goes up and returns to the folder you left; `⌘←` / `⌘→` open the folder beside it; `/` on the folder row finds a tile by name. The Windows host matches that chrome. Marks (`Insert`/`Shift+Space`/`Ctrl+A`/`Ctrl+D`), copy/move to a chosen folder (`F7`/`F8`, collision-safe), Trash delete with confirm (`Delete`), fullscreen (`F11`/`F`), a stills-only slideshow (`F5`; `.` blackout, `R` shuffle), and drag-out (`⌘`+drag on the canvas; a plain drag from a gallery, filmstrip or search-result cell, the marks when that cell is marked, as the original files). **Video (Mac PR 5):** FFmpeg + VideoToolbox decode, copied out of the decoder pool into a presentation ring of our own Metal textures, an MSL twin of the video shader (NV12/P010, the stream's matrix/range/transfer, HLG/PQ tone-mapped to SDR), Core Audio as the master A/V clock (no `AVPlayer`), a SwiftUI transport strip and the docs/design/16 video keys, and poster thumbnails for clips. Metadata read (PR 9, see above); no rating/metadata writes or RAW-pairing UI yet. Not on the Mac yet: `⌘G` go to, `;` Live Photo motion, Open RAW / Open JPEG of a pair ([docs/design/16](design/16-commands.md) "Not built"). Builds on Apple Silicon and Intel, macOS 14+ (Intel: unverified for frame pacing). |
| **`MediaViewer.Interop`** | The C# side of the ABI — `SafeHandle`, struct layouts, completion drain. The filmstrip island borrows the session and drains folder/thumb completions. |
| **Import add-on** (Milestone G) | An optional add-on installed from Settings → Add-ons ([docs/design/18](design/18-import.md)): copy a card or folder into a library, skip what is already there by content (size, then BLAKE3), verify every copy by reading it back, sort into dated folders with RAW+JPEG / Live Photo pairs and camera sidecars kept together, resume after an unplug, back up to a second drive from one read, verify an old folder for silent corruption, and **find duplicates** in a folder tree (every file, grouped by identical bytes; PR 54), where one copy at a time can be moved to the Recycle Bin / Trash, never the last. Apart from that, never deletes from, formats or overwrites anything. `mv_import.dll` / `libmv_import.dylib` plus its chrome (`MediaViewer.Import.Chrome.dll` / `Import.bundle`) are built beside the app in `build/addons/import` and shipped as a separate signed download that a stable run of the release workflow packs beside the app (RELEASING.md); the base install does not contain them. Settings → Add-ons offers Install only when that download exists and verifies for the running app. With it absent, `Ctrl+Shift+I` / `Ctrl+Shift+F7` do not exist. The shared engine is tested; the Windows and Mac hosts are written but their first platform builds and every hardware verify line are owed (docs/design/10). Also from this work, in the base app: `F8` across drives now deletes the source only after a verified copy. |

## Build

You need **Visual Studio 2022** (or Build Tools) with the C++ workload, the **Windows 10/11
SDK**, **CMake ≥ 3.28**, **vcpkg**, the **.NET 8 SDK**, and the **Windows App SDK 2.4
runtime** (the command-bar island is unpackaged). The native core still builds and tests
with no .NET present; without `dotnet` on `PATH` the lab runs as it did in PR 2
(`--no-chrome`). The build compiles the blit shaders (`src/gfx/*.hlsl`) with the SDK's `fxc.exe`,
found in the targeted SDK's `bin` without a developer shell; pass `-DMV_FXC=<path>` to override.

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
# LibRaw's OpenMP runtime, built for macOS 14 (Homebrew's targets the build Mac's OS).
python3 tools/mac/build_libomp.py --prefix "$PWD/build/libomp/arm64" --arch arm64
export MV_LIBOMP_ROOT="$PWD/build/libomp/arm64"
# Dynamic LGPL codecs use a separate manifest/tree; CMake installs the static
# permissive dependencies from the root manifest. See RELEASING.md.
# tools/mac/dependencies/vcpkg-configuration.json adds the repo's overlay ports
# (LibRaw's OpenMP colour patch); re-running this on an older tree rebuilds LibRaw.
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
| OpenMP runtime | `tools/mac/build_libomp.py` builds LLVM's libomp for the app's macOS 14.0 (hash-pinned sources); export `MV_LIBOMP_ROOT` to its prefix before the vcpkg install and configure. The dynamic codec triplets use it for bounded parallel RAW decoding, and packaging bundles it. Homebrew's `libomp` is only a fallback when `MV_LIBOMP_ROOT` is unset: its dylib declares the build Mac's macOS as its minimum, so `tools/mac/check_minos.py build-darwin/MediaViewer.app` (run by the release) fails such a bundle. |
| vcpkg, bootstrapped | `export VCPKG_ROOT=…`. The baseline is pinned in `vcpkg.json`; do not float it |
| Python 3.10+ | for `dmgbuild==1.6.7` (`pip install -r tools/mac/requirements.txt`). On 3.9, `dmgbuild==1.6.5` works for a local dry run only |
| Apple Developer Program | needed for Developer ID signing and notarization; without it you can only build the ad-hoc bundle |

**1. Build the app**

```sh
export VCPKG_ROOT=/path/to/vcpkg
python3 tools/mac/build_libomp.py --prefix "$PWD/build-darwin/libomp/arm64" --arch arm64
export MV_LIBOMP_ROOT="$PWD/build-darwin/libomp/arm64"
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
bundle the same Team ID, satisfies that check. `python3 tools/mac/check_plists.py`,
`python3 tools/mac/test_macpack.py` and `python3 tools/mac/test_check_minos.py` need no Mac and
run anywhere. `python3 tools/mac/check_minos.py build-darwin/MediaViewer.app` fails when any
bundled binary declares a minimum macOS above the app's `LSMinimumSystemVersion`.

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
| `Ctrl+PageUp` / `Ctrl+PageDown` | previous / next page of a TIFF, PDF, DOCX or PPTX, size of an ICO, or image of a multi-image HEIC (`⌘` on the Mac). Within a page: zoom (wheel, `+` `-`, `1`–`4`) and drag to pan. On a PDF or DOCX with the filmstrip hidden the wheel / trackpad scrolls (a notch glides, a trackpad follows the fingers) and turns pages as soon as it pushes past an edge, momentum running on into the next page; `⌘`/`Ctrl`+wheel or a pinch zooms. With the filmstrip shown the notice line says "Press T to hide the filmstrip and scroll" until `T` has been used on a document |
| `Enter` on a PDF, DOCX or PPTX | open it in its default app (the bar's **Open in <app>**, ▾ for the others) |
| `Shift+T` | page sidebar on / off: page thumbnails down the left of a multi-page PDF, DOCX or PPTX while the filmstrip is hidden; a click turns to that page (remembered; View ▸ Page Sidebar on the Mac) |
| `J` / `K` / `L` | clip transport: −10 s / pause / +10 s ([docs/design/16](design/16-commands.md)) |
| `,` / `.` | frame step back / forward while paused |
| `A` / `D` | previous / next beside the arrows, in every mode including on a clip |
| `Q` / `E` | on a clip, two commands on one key: **tap** skips ±2 s, **hold** skims ±2 s per key repeat and settles on an exact seek when released. `Shift+Q` / `Shift+E` step playback speed. Off a clip they do nothing |
| `Left` / `Right` on a clip | walk the folder while the filmstrip is showing, paused or playing; with the filmstrip hidden (`T`) they are `Q` / `E` (tap ±2 s, hold skims). Stills always walk |
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
| `\|` | two-up compare: the previous image beside the current one, panned and zoomed together (stills; `--compare` starts with it on) |
| `C` | clipping blinkies (red highlights, blue shadows). The canvas keeps presenting while on |
| `O` | info line: file name, position in the folder, size, zoom |
| `Ctrl+Shift+A` | always on top |
| `Insert` / `Shift+Space` | mark or unmark the current item. `Ctrl+A` marks all, `Ctrl+D` clears |
| Keypad `0`–`5` / `Ctrl+Shift+0`–`5` | rate the item on screen (0 clears) |
| `X` | reject the item on screen (rating −1, "Rejected" in Lightroom and Bridge); `X` again un-rejects |
| Keypad `6`–`9` / `Ctrl+Shift+6`–`9` | colour label Red / Yellow / Green / Blue (Lightroom's keys; `⌘⇧` on the Mac); the same key again clears it. `U` clears the label. Written as XMP `xmp:Label`: in a plain JPEG, else in its `.xmp` sidecar. The metadata pane's Label row has a swatch per colour |
| View ▸ **Show** | All / Photos / Videos / RAW / Marked: only that kind in the filmstrip, gallery and arrow keys, for this run (not saved). RAW is a RAW or a RAW+JPEG pair; Marked is the marks when you chose it. The stop on screen moves to the nearest one shown. Also in the command bar's View menu on the Mac. Below them, **Picks** (1★ or more) / **Not rejected** / **Red** … **Purple** filter on what `X` and the rating and label keys wrote (read on a worker; the stop on screen stays when you rate it; choose the filter again to re-apply). **Show next kind** cycles them all; it has no key until you give it one in Settings |
| `F5` | slideshow (fullscreen). In it: `Space` pause, `+` / `-` interval, `.` blackout, `R` shuffle, `Esc` leave. A clip plays to its end before advancing |
| `F7` / `F8` | copy / move the marked items (or the current one) to the last folder used. `Shift+F7` / `Shift+F8` pick a folder. Never overwrites: a taken name becomes `name (2).ext` |
| `Delete` | move the marked items (or the current one) to the Recycle Bin, after asking. A drive with no Recycle Bin is refused, never deleted permanently |
| `F2` | rename the current file (File ▸ Rename… on the Mac). Type the name without its extension; `Enter` renames. Its RAW+JPEG or Live Photo partner and its `.xmp` sidecars are renamed with it. A taken name becomes `name (2)`; nothing is overwritten |

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
| `Ctrl+E` | show the current file in Explorer, selected (`⌘E`: Finder; a Photos library item opens in Photos, selected). Open menu: **Show in Explorer** (Mac: **Show in Finder**, or **Open in Photos** while a Photos library item is current; `?` and Settings use the same words) |
| Open ▸ **Recent folders** | the folders you opened lately, the jump list's (File ▸ Open Recent on the Mac). From the keyboard: `Tab` to the command bar, `Enter` on Open, arrow to Recent folders, `Right`. A folder that has gone beeps and leaves the list, as a welcome-card row does. The welcome card writes your profile folder as `~` (`~\Pictures`). With OneDrive set up, a **OneDrive** row leads the card, above Recent folders, as iCloud Photos does on the Mac: it opens OneDrive's Pictures folder (OneDrive itself if there is none) |
| `Space` / `,` / `.` on an animation | play or pause (a finished one plays again) / previous frame / next frame, like a clip. Delays follow browsers: 10 ms or less plays as 100 ms |
| `?` | the shortcuts for what you are doing right now. Also the `?` button on the right of the command bar |
| `Ctrl+,` | Settings: **General** has grouped preferences with aligned switches and automatic saving; **Keyboard shortcuts** has the searchable remapping list. Both pages scroll independently of the header and Done button. Search the list by command or shortcut. Choose a shortcut and press its replacement; viewer shortcuts are suspended while Settings is open. Escape or Cancel change cancels capture; Escape otherwise closes Settings. Conflicts swap shortcuts, and Reset to default restores the map. `?` lists whatever you bind |
| `Ctrl+G` | go to an item by its number in the folder (Windows; not on the Mac yet) |
| `/` | find an item by name. With the filmstrip or gallery focused, just type |
| `I` | metadata pane: summary card, searchable tag tree, and for clips the per-stream inspector (PR 9). Focuses the pane; `Esc` returns to the photo, a second `Esc` closes it |
| `Ctrl+Shift+E` | folder tree, rooted at the open folder (PR 9). Focuses it: arrows walk, Right / Left open and close a folder, `Enter` opens it, `Esc` returns to the photo. It follows the folder as subfolders come and go |
| `O` / `Shift+O` / `Shift+I` | info overlay with exposure lines / AF points / eyedropper (PR 9) |
| `Shift+Z` | zoom to the AF point: 100 % centred on the in-focus point, for a focus check (issue #209) |
| `Ctrl+C` | copy the eyedropper colour if it is on, otherwise the marked (or current) file(s) |

The title bar shows the current file, its position in the folder, its size and
the zoom. Arrow keys, `Space` and the slideshow wrap from the last item to the
first; turn that off under Settings.
| `Ctrl+Shift+O` | open a folder |
| `Left` / `Right` | previous / next in the folder |
| `G` | gallery: thumbnail grid of the folder. A folder of only folders uses big tiles; a mixed folder keeps a short chip row above the photos. Covers and counts show on the tiles; the folder path sits in the command bar just left of `?`. The Open, View, Settings and `?` buttons keep their width; the trail is what shrinks and scrolls. **Up** (↑) and **Root** (house) buttons stay outside the scrolling trail: Up opens the enclosing folder; Root returns to the highest folder reached in this browsing session (the first breadcrumb). The `…` menu opens hidden parent folders directly. Full paths are available on hover. These controls remain available, including while a photo is open. `Ctrl+Up` goes up and selects the folder you left; `Ctrl+Left` / `Ctrl+Right` open the sibling beside it. A search icon at the right end of the path (and beside "Search: …" on a result list) does what `Ctrl+F` / `⌘F` does: with Local search installed it opens the search panel (the first search of a session starts the pack, which loads nothing at launch) (where `file:name` searches file names); without it, **file search**, a field over the grid that filters this folder's tiles by name as you type (no index; `Esc` clears, then closes); the command bar stays above the gallery on both hosts, so the path and the icon are there over the grid too. `/` on the folder row finds a tile by name. `W` / `S` or Up / Down move between rows and cross from folders to images; `A` / `D` or Left / Right move between items. `+` / `-` enlarge / shrink thumbnails (`=` also enlarges). `Enter` opens a folder or the selected image. A click does the same; `Esc` leaves |
| `T` | filmstrip show/hide, for the mode you are in (folder open or single image) |
| `Tab` | focus the command bar island |
| `Esc` | walks out one level: gallery, fullscreen, then island focus back to the canvas, and last, in a search result list, **Back to folder**. It never quits |
| `Ctrl+W` / `Alt+F4` | close the window |
| `Ctrl+N` (`⌘N`) | a new window: its own process on the empty window; add-ons stay in the first window's process, except those that only fill screens and slots (Places), which every window loads (#402) |

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

**Clips (PR 13 / 14, Windows and macOS).** Nothing here changes the
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
springs back to centre. Drop a file on the window. A pinch (touchscreen,
touchpad, trackpad) zooms like the wheel; a sideways swipe or mouse tilt steps
next / previous; a finger drag pans once zoomed
([docs/design/16](design/16-commands.md) "Touch and trackpad"). The gesture
maths has unit tests (`mv_tests "[gesture]"`); real touch hardware is not in
any automated run.

Command line: `--soak <seconds>`, `--json <path>`, `--gate` (non-zero exit if the verify
line fails), `--no-overlay`, `--static`, `--no-chrome`, `--open <path>`, `--background` (#452:
the sign-in start: the window is never shown, only the tray / menu-bar icon once Sharepoint keeps
the process; it quits after 60 s if nothing does),
`--browse-soak` (with `--json` and a folder: time each arrow to the next still, and jumps
past the prefetched neighbours; 3 s dwell between arrows; the macOS lab has it too. The
Windows lab adds `quick` steps — Right as soon as the last photo is on screen — and one
`held` run — a Right every 50 ms, then the last photo timed to full resolution, with the
run's decode job counts under `"held"`; every step carries `full_ms`. Those are the cases
the in-flight decode hand-off is for, and `perf-browse.svg` ignores them),
`--page-soak <path>` (macOS lab, issue #429: with a PDF, DOCX or PPTX, scroll the page sidebar
from the first page to the last in 1.5 s, as a hard flick does, then go to pages 250, 10, 150, the
last, 1, 250 and 100 the way a thumbnail click does, each timed to its first image, the present
showing it and the full-size page; add `--soak N` to have the canvas pacing measured meanwhile),
`--av-soak <seconds> --csv <path>` (headless A/V drift soak on a clip — see
[Test](#test)), `--pan-soak` (with `--soak` and `--open`: pan the still at 100 % across
the whole frame on a fixed path, to measure cached-image and tiled-pyramid pan),
`--compare` (start in the two-up compare; add it to `--pan-soak` to pan both halves, the
left one drawing the same still again since one file has no previous, or to `--browse-soak`
to walk a folder with the split up; `compare_frames` in the soak JSON counts the two-half
presents), or a positional file or folder.

A still's first pixel is a preview (JPEG DCT 1/4, or a RAW's embedded JPEG); the full
decode then *refines* the same item rather than replacing it: the view you have — fit,
or a zoom and pan made while it loaded — is kept as a fraction of the image, and the
full texture cross-fades in over 80 ms (up to 250 ms when the preview and the full
render differ a lot in brightness, as a RAW's embedded JPEG and LibRaw's render do).
Images above ~64 MP, or wider or taller than 16384 px, are shown as a tiled pyramid: a
≤ 2048 px overview is always loaded, and 256 px tiles for the part on screen are
created a few per refresh and drawn over it, so a fast zoom or pan is blurry for a
moment, never blank. `F` / `F3` shows the tile LOD, resident tiles and VRAM, create
times, and refinement counters. Its `texture` line times the whole-texture creates of
untiled stills and counts the neighbour creates held back while the camera moved (issue
#166). The soak JSON carries the same numbers: `texture_*` keys, including
`texture_creates_deferred` and `texture_max_defer_ms`. The command bar is also on the island: Open (**Media…** or **Folder…** — the picker
takes photos and clips), View (zoom in/out,
fit, 50 / 100 / 200 / 400 %, gallery, filmstrip, overlay),
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
video, above the filmstrip, that appears with a clip and goes away with it. As on the Mac it is
44 DIPs tall and up to 720 wide: back / play / forward, elapsed time, a scrubber that takes the
spare width, duration, the speed menu (`1x`), mute and More. It works the
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
   is untouched. `--documents` turns on File types "Show documents" for the run, so the
   folder's PDFs and DOCX files are read for their words (#354); the status lines then carry
   `"docs":[done,total,locked,failed]`. The pack stages PDFium beside `mv_ai` from its pinned
   download (`cmake/ai.cmake`); set `MV_ORT_ARCHIVE_DIR` to a folder holding
   `pdfium-<platform>-8066.tgz` (and ORT's archive) to configure offline.
5. Quit's cost with a pack loaded: `ai-bench --addons <folder> --query dog --quit-after 1`
   quits the way both hosts do (the pack gets half a second to stop, then the exit skips static
   destructors; `src/addon/host.h`, "Quit") and prints the time it took; `--quit-legacy` is the
   old wait of up to 5 s, `--quit-hash` re-verifies the pack meanwhile as Settings does, and a
   larger `--quit-after` quits once the pack is idle.

**An unfinished index waits for Resume** (issue #299; docs/design/17 "Unfinished roots wait for
Resume"). A root left mid-index is held at the next start, and the window asks "Resume indexing
…?" in a bottom-right notice without loading the pack. Tests, no pack needed:
`mv_ai_tests "an index left unfinished waits for Resume after a restart"` (the engine: held,
`resume.json`, `resume_root`) and `mv_import_tests "[ai-resume]"` (the host's read of
`resume.json` and the question's wording). To see the notice in the app, use a sideloaded pack
(above): index a large folder, quit part-way, start again. The notice should appear once the pack
has verified, with nothing of it loaded (no ONNX Runtime in the process) until **Resume**. Or,
with the pack installed, drop a `resume.json` by hand into its data folder (the one that holds
`index.db`, `data` beside the pack's version folder):
`{"version":1,"roots":[{"id":1,"path":"/some/folder","kind":"folder","done":1,"total":2,"held":true}]}`.

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
BCubed precision and recall. Then a corrections phase: after 60 % of the faces, it merges each
person's fragments and names everyone with 3+ photos, and files the remaining 40 % online
(`corrected`, `new 40%`).
LFW has only clear faces, so check a clustering or People-speed change on a real library too:
copy its `faces.db` (`sqlite3 "file:<faces.db>?mode=ro" ".backup <copy>"`; opening migrates, so
never the live one), then `MV_FACES_DB=<copy> mv_ai_tests "[.people-settle]"` (the settle, with
people listed before and after) or `"[.people-api]"` (what each People read the chrome makes
costs, then the copy's unfiled faces offered again through `add`: ms per face and where they
went; it writes, so use a fresh copy per run). Delete the copy after: it is face data.

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
`--filter SearchModelTests` checks the search panel's model against a fake table (issue #191):
a search re-asks when another folder opens under "Folder & subfolders", and Tab only takes a
name looked up for the words as typed. `--filter StatusLineTests` checks the footer's status
line from a status struct (issue #417): with iCloud clips queued the pictures count and ETA are
named as this Mac's files and the clips as the rest of the run; with the queue empty it is the
plain pictures / sound / speech line.

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
`"[photos_backup]"` (the engine over a fake library: each folder layout, verified copies, the
manifest across a layout change and a remount, a collision, a cancel), `"[write_guard]"` and `"[poster]"` (a clip's display matrix turns its
poster; the thumbnail spec is `jpg512.4`). A dev build shows the row and the section only when
its own bundle id has Photos access and the flag `mv.photosLibrary.added` is set in its
defaults; the flag is set by the pack's Settings when the library is a root, so a dev build
without the pack loaded can be pointed at it by hand (`defaults write
io.github.longtimeno-c.mediaviewer.dev mv.photosLibrary.added -bool true`) once macOS has
granted that bundle Photos access. The listing's time is logged as `photos: listed N assets in
M ms`: `log stream --predicate 'subsystem == "org.mediaviewer" AND category == "photos"'` (in zsh,
`/usr/bin/log`: `log` is a builtin). `tools/photos/name_probe.m` times the walk and its names
outside the app and checks them against the resources (counts only; its header says how to give
it Photos access). The viewer's on-view downloads and previews live in
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

# ...and as CI runs it, where an absent corpus must fail rather than skip. CI's
# test-media job makes the clips and fetches the RAW/HEIC samples once (cached);
# every Build leg restores them and sets this. With CI set, test_corpus.cpp fails
# if MV_REQUIRE_CORPUS is unset ('0' is an explicit opt-out, as release.yml uses).
$env:MV_REQUIRE_CORPUS = "1"; ctest --test-dir build -C Release --output-on-failure

# the ABI, end to end from C#: SafeHandle, struct layout, completion drain. The
# second argument is mv_abi_layout's dump: the AI structs' C# mirrors are then
# compared field for field with the native layout (skipped without it; CI passes it)
cmake --build build --config Release --target mv_abi_layout
build\bin\Release\mv_abi_layout.exe > build\abi-layout.txt
dotnet build src.managed\MediaViewer.AbiSmokeTest\MediaViewer.AbiSmokeTest.csproj -c Release
dotnet src.managed\MediaViewer.AbiSmokeTest\bin\Release\net8.0-windows\MediaViewer.AbiSmokeTest.dll build\bin\Release build\abi-layout.txt

# WinUI chrome (also published beside mediaviewer_lab.exe by the CMake build)
dotnet publish src.managed\MediaViewer.Chrome\MediaViewer.Chrome.csproj -c Release -r win-x64 --no-self-contained

# Issue #153, the chromes' headless tests (no core build needed; CI's "WinUI chrome"
# and "SwiftUI chrome" jobs run them). They read the chromes' sources as text and fail
# when a copy of a command id, a captured key code, the rebind packing or a menu's
# printed shortcut drifts from src/shell or from tests/data/command_table.snapshot.
dotnet test src.managed\MediaViewer.Chrome.Tests\MediaViewer.Chrome.Tests.csproj -c Release
# macOS: the same checks for the SwiftUI chrome, Import's model, AI's search model
swift test --package-path src.swift/MediaViewerChrome
swift test --package-path src.swift/ImportChrome
swift test --package-path src.swift/AIChrome --filter SearchModelTests
# The native half (part of plain ctest): key remaps survive persist and reload, and
# describe_commands() by mode matches tests/data/command_table.snapshot. A change to
# the default key map fails it on purpose; regenerate, then read the diff:
$env:MV_UPDATE_SNAPSHOTS = "1"; .\build\bin\Release\mv_tests.exe "[snapshot]"; Remove-Item Env:MV_UPDATE_SNAPSHOTS

# Milestone G: the Import add-on's suite (built with the core, both platforms)
# Its verified copies flush durably and read back uncached in scratch folders
# under %TEMP%, so they run at the speed of that disk; CI points TMP/TEMP at
# the runner's local disk (runner.temp), not C:.
ctest --test-dir build -C Release -R import_ --output-on-failure
# PR 54 find duplicates: time a first scan and a cached rescan (synthetic tree,
# or MV_DUP_BENCH_DIR=<folder>, read only; the hash cache is a scratch import.db)
build/bin/mv_import_tests "[.perf-bench]"

# docs/design/25, open add-ons: their C++ cases are part of the suite above
# ([open-addon]); this is the author's tool, which packs with Python and
# asks the app's own reader (mv_addon_verify --open) about every package
ctest --test-dir build -C Release -R addon_sdk --output-on-failure
python tools/addon-sdk/test_mvaddon.py        # the tool alone, no build needed
# PR 56: settings pages, keymap packs and theme shape, and keymaps on the live table
build/bin/mv_import_tests "[addon]"
build/bin/mv_tests "[keymap]"

# docs/design/25 PR 57, the script sandbox: escapes, limits, permissions absent
# from the environment, script manifests and packages, the host's set of
# script add-ons (also in cmake/portable)
ctest --test-dir build -C Release -R script_ --output-on-failure

# ...and the same engine headless on Linux or any POSIX machine (CI's
# portable-core job, Ubuntu under ASan + UBSan; SQLite, libsodium, BLAKE3 and Catch2 from vcpkg via
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
.\tools\check-hostable-core.ps1    # D9: no windows.h / d3d11.h, nor Cocoa / Metal, above gfx/ outside the ports
.\tools\check-vcpkg-baseline.ps1   # the five vcpkg baseline pins agree
.\tools\check-winui-controls.ps1   # no ProgressBar / ProgressRing / DropDownButton / InfoBar / TextBox...: they
                                   # fail-fast in the island host (use JobBar, Shared\FlatBar.cs, a Button + Flyout,
                                   # Shared\FakeInput.cs)
.\tools\licence-check.ps1          # no GPL FFmpeg, no software HEVC/AAC encoder
.\tools\telemetry-schema-check.ps1 # telemetry payload stays fixed vocabulary: no path/filename field

# PR 7 broken-file corpus: every seed in tests/data/seeds truncated, stomped,
# bit-flipped and given absurd dimensions, plus tests/data/broken, through every
# decode entry point. Fails on a crash, an escaped exception, a call over 5 s
# (60 s for each family's first-use warm-up), or runaway memory. Part of plain
# ctest; this runs just that suite.
ctest --test-dir build -C Release -L broken --output-on-failure
$env:MV_BROKEN_FULL = "1"          # exhaustive sweep (nightly CI)
$env:MV_BROKEN_DUMP = "broken-dump" # write each failing input here
$env:MV_BROKEN_TIMEOUT_MS = "5000"  # per-call budget (60000 in Debug/ASan)
$env:MV_BROKEN_FIRST_USE_MS = "60000" # each family's warm-up call
# seeds are synthetic and committed; regenerate with (needs Pillow, pillow-heif):
python tools\testmedia\make-seeds.py

# libFuzzer harnesses, one per decoder entry point (clang-cl + ASan), and
# fuzz_script: arbitrary text as an add-on script under tight limits (PR 57;
# non-gating in CI until its first runs there, seeds in tests/data/seeds/script)
cmake -S . -B build-fuzz -A x64 -T ClangCL -DMV_FUZZ=ON -DMV_BUILD_TESTS=OFF
cmake --build build-fuzz --config Release --target mv_fuzzers
.\tools\fuzz\run.ps1 -BuildDir build-fuzz -Seconds 60      # -Harness png,gif to pick
# CI (#384) gates on the eleven single-format image harnesses and runs the rest
# non-gating, 30 s each: decode animation thumbnail script pdf docx pptx in one step;
# meta_c2pa meta_jpeg meta_png meta_webp meta_bmff meta_xmp meta_icc meta_location
# meta_probe meta_search geo places_map addon_contributions ai_query in another.
# meta_search (#291, links mv_meta) and ai_query (#291, the Local search query
# language, seeds tests/data/seeds/query) are in mv_fuzzers like the rest.
# #290: the metadata readers (meta/parse.h) have harnesses too: meta_c2pa, meta_jpeg,
# meta_png, meta_webp, meta_bmff, meta_xmp, meta_icc, and (#345) meta_probe, the AI badge's
# provenance probe (add src/meta/probe.cpp src/codec/probe.cpp to the line below, seeds
# tests/data/seeds/probe). They link mv_meta_parse alone, so on
# a Mac they build with Homebrew LLVM (Apple clang ships no libFuzzer), e.g.:
#   /opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -fsanitize=fuzzer,address,undefined -fno-rtti -Isrc \
#     -Itools/fuzz tools/fuzz/fuzz_meta_png.cpp src/meta/{parse_common,c2pa,containers,xmp_raw,generators}.cpp \
#     src/codec/xml.cpp -lz -o fuzz_meta_png
#   ASAN_OPTIONS=detect_leaks=0 ./fuzz_meta_png -max_total_time=600 corpus tests/data/seeds/png tests/data/seeds/c2pa
# Their seeds (tests/data/seeds/{c2pa,xmp,icc}) come from the test builders:
#   MV_SEED_DIR=tests/data/seeds mv_tests "[.write-seeds]"
# #352: fuzz_meta_location (meta::parse_location, the Location editor) is pure and builds
# from one file:
#   /opt/homebrew/opt/llvm/bin/clang++ -std=c++20 -fsanitize=fuzzer,address,undefined -fno-rtti -Isrc \
#     -Itools/fuzz tools/fuzz/fuzz_meta_location.cpp src/meta/location_edit.cpp -o fuzz_meta_location
#   ASAN_OPTIONS=detect_leaks=0 ./fuzz_meta_location -max_total_time=600 -max_len=512 corpus tests/data/seeds/location
# On a Mac, Homebrew's LLVM has libFuzzer (Apple clang does not): build
# tools/fuzz/fuzz_script.cpp, third_party/lua/src/*.c (as C++),
# src/addon/script_runtime.cpp, script_thread_posix.cpp, open_manifest.cpp and
# manifest.cpp with -fsanitize=fuzzer-no-link,address,undefined, then link with
# -fsanitize=fuzzer,address,undefined, libsodium.a and build/libmv_io.a.

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

### Place database (Places, docs/design/27)

`src/geo` reads a place database built from pinned Natural Earth and GeoNames downloads by
`tools/geo/build_places.py`. The script uses the Python standard library only and must be run
with `-I`. The full blob is not in git (17 MB). A 111 KB test blob is:
`tests/data/geo/places-test.mvgeo`.

```sh
# the full blob, through CMake: first run downloads ~66 MB into build/geo-downloads
# (MV_GEO_DOWNLOADS to move it) and takes about a minute; later runs reuse the cache
cmake --build build --target mv_geo_data          # -> build/geo/places.mvgeo
# ...or by hand
python3 -I tools/geo/build_places.py --downloads build/geo-downloads --out build/geo/places.mvgeo
# GeoNames republishes daily under the same URL: a changed file fails the pin.
# --accept-upstream takes it and prints the hash to pin.

# the committed test blob, after changing the script or the format (src/geo/blob.h)
python3 -I tools/geo/build_places.py --downloads build/geo-downloads \
  --out tests/data/geo/places-test.mvgeo --subset test

# the fold table (shared with file search and Local search, core/fold.h), after
# changing the fold rule or for a newer Unicode
python3 -I tools/fold/gen_fold_table.py > src/core/fold_table.inc

# tests: fold, the test blob, a damaged-blob sweep; the 300-row table when the
# full blob exists (MV_GEO_BLOB=path for one built elsewhere; MV_REQUIRE_GEO=1
# fails instead of skipping, as CI does)
build/bin/mv_tests "[geo]"
# load < 30 ms, one lookup < 50 us
build/bin/mv_tests "geo: load and lookup timings"     # also part of "[.perf-bench]"
# the loader fuzzer is fuzz_geo in mv_fuzzers (run.ps1 -Harness geo)

# The Places add-on (docs/design/27): build/addons/places/(lib)mv_places.(dylib|dll)
cmake --build build --target mv_places
build/bin/mv_tests "[places]"                  # map pixels, cache, gestures, section, search
build/bin/mv_import_tests "[places]"           # its manifest, then the add-on through a signed store
build/bin/mv_tests "places: map render time at pane width x 160 pt"   # in "[.perf-bench]"
MV_PLACES_MAP_DUMP=<dir> build/bin/mv_tests "[.places-map-dump]"      # BMPs of the map to look at
# the map's fuzzer is fuzz_places_map in mv_fuzzers (run.ps1 -Harness places_map)
# Package it (release: the workflow does this after signing the code)
python3 tools/package/addon-pack.py stage-places --src build/addons/places --blob build/geo/places.mvgeo
python3 tools/package/addon-pack.py pack --addon places --platform macos --src build/addons/places \
  --version 1.0.0 --key <key file> --out dist/

# Sharepoint (docs/design/28): build/addons/sharepoint/(lib)mv_sharepoint.(dylib|dll)
cmake --build build --target mv_sharepoint
build/bin/mv_import_tests "[sharepoint]"       # wire, channel, 2-3 engines over loopback TCP, the add-on through the loader
build/bin/mv_import_tests "[.sharepoint-bench]"   # MB/s against a plain copy over loopback (MV_BENCH_DIR, _BIG_MB, _SMALL, _SMALL_KB, _CASES)
build/bin/mv_import_tests "[.sharepoint-idle]"    # CPU while Keep receiving is on, window closed (MV_IDLE_SECONDS)
python3 -m unittest tools/package/test_addon_pack.py   # SharepointPackTest among them
build/bin/mv_import_tests "[.sharepoint-send]"    # #452: a sender on this machine (MV_SEND_FILE, MV_SEND_CODE_FILE or MV_SEND_HOLD)
python3 tools/package/addon-pack.py pack --addon sharepoint --platform macos --src build/addons/sharepoint \
  --version 1.0.0 --key <key file> --out dist/
```

To see it in the app on a developer build (`-DMV_ADDON_DEV_PUBLIC_KEY`): pack with the dev key as
above, unzip it with `ditto -x -k` into `<scratch>/addons/Places/1.0.0/` beside the signed
`manifest.json(.sig)`, and start a copy of the app with its own bundle id, `HOME`,
`MV_DEV_ADDONS_DIR` and `MV_DEV_THUMBS_DIR` (as the slots test add-on, under "Where this actually
is"). PR 56 is in the same build, so the manifest's `settings` page installs as it is; a change
on Places' page in Settings redraws the Place section at once (`mv_import_tests "[settings]"`
covers the re-ask, `"[places]"` Places' own settings).

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

Timing asserts are not in the unit suite (it runs under `ctest -j` and on shared runners):
they are hidden `[.perf-bench]` tests beside the bench, run on a quiet machine in an optimised
build. `mv_tests "[.perf-bench]"` holds the colour-stage starvation bound, pairing a 4000-file
dump and, on Windows, the WARP tile frame and create budgets; `mv_ai_tests "[.perf-bench]"`
holds the 100 k-row search scan.

`mv_tests "[.perf-bench]"` is the quick loop for decode work: `MV_BENCH_JSON=path` writes
its medians, `MV_BENCH_DIR=folder` adds your own files to the decode rows. Its `preview` row is
the canvas's first pixel (`decode_first_pixel`). libheif's example is one small image, so for
camera-sized HEIC run `python tools/testmedia/make-grid-heic.py` (after `fetch-heif.ps1`; needs
an ffmpeg with libx265 on `PATH`, a dev tool only): it writes `tools/testmedia/heif/grid-12mp.heic`,
an iPhone's layout (4032x3024 as 8x6 tiles of 512, a 320x240 thumbnail item, Display P3 ICC).
`MV_OS_CODEC=0` times the bundled libheif path, the one a PC without the HEVC extension takes.
`mv_tests "thumb store lookup throughput"` (also tagged `[.perf-bench]`, printed, not in the
JSON) times 2,000 thumbnail-cache hits from four threads, the filmstrip sweep's lookups.
`mv_tests "job system per-job overhead"` (the same tag, printed) times the worker pool alone:
100,000 empty jobs on four workers, foreground only and alternating lanes (on macOS, a QoS switch
per job).

`mv_tests "foreground start latency under a background sweep"` (also `[.perf-bench]`, printed)
times how long a view-tied job waits for a worker while 60 ms background jobs fill a two-worker
pool: the job system's reserved foreground worker (issue #160).

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

### Open add-ons self-test, PR 56 leg (macOS)

The same rig walks settings pages and key layouts across a restart: install
`example.settings-demo`, photograph its page (`p2-page.png`, `p4-page.png`), change four values;
then a second launch over the same folder installs `example.vim-keys`, reads the values back,
applies the Vim layout, Reset, removes settings-demo keeping its settings, installs it again and
removes it without. Keys are not written to your preferences while it runs:

```bash
python3 tools/addon-sdk/mvaddon.py pack examples/addons/settings-demo --key /tmp/mv-addon.key --out /tmp/mv-addon
python3 tools/addon-sdk/mvaddon.py pack examples/addons/vim-keys --key /tmp/mv-addon.key --out /tmp/mv-addon
```

```bash
MV_ADDON_SELFTEST=/tmp/mv-addon/rig56 MV_ADDON_SELFTEST_PR56=a caffeinate -d -i build/bin/MediaViewer /tmp/mv-addon/example.settings-demo-1.0.0.mvaddon
MV_ADDON_SELFTEST=/tmp/mv-addon/rig56 MV_ADDON_SELFTEST_PR56=b MV_ADDON_SELFTEST_REINSTALL=/tmp/mv-addon/example.settings-demo-1.0.0.mvaddon caffeinate -d -i build/bin/MediaViewer /tmp/mv-addon/example.vim-keys-1.0.0.mvaddon
```

`rig56/state.txt` has a line per step: the page's values, the keys of Previous / Next / Toggle
mark / Flip horizontal / Forward 10 s before and after the layout and after Reset, and whether
the settings file was kept.

The settings / keymap / theme parsers have a libFuzzer harness, `tools/fuzz/fuzz_addon_contributions.cpp`,
in the Windows clang-cl fuzz build (`mv_fuzzers`, seeds in `tests/data/seeds/addon`); its header
has the one-line Homebrew LLVM build for a quick run on a Mac.

### Script add-ons self-test (PR 57, macOS)

`MV_SCRIPT_SELFTEST=<folder>` presses an add-on's keys as a person would (key events through the
app's own queue: router, command table, runtime) and photographs the result. Put two add-ons in
`<folder>/store` first: the example Folder Count (Count files, `⌘⇧K`, with `folder.read`) and a
spinner whose Spin command (`⌘⇧J`) never returns:

```bash
python3 tools/addon-sdk/mvaddon.py pack examples/addons/folder-count --key /tmp/mv-addon.key --out /tmp/mv-script
```

```bash
build/bin/mv_addon_verify --open /tmp/mv-script/example.folder-count-1.0.0.mvaddon /tmp/mv-script/rig/store
```

Pack and install the spinner the same way (`"scripts": ["main.lua"]`, `function spin() while
true do end end`, a command `spin` on `Ctrl+Shift+J`), then:

```bash
MV_SCRIPT_SELFTEST=/tmp/mv-script/rig caffeinate -d -i build/bin/MediaViewer tools/testmedia/raw
```

`rig/state.txt` gets the notice Count files showed ("Folder Count: 4 files in this folder"), the
scripts' state after Spin (stopped, "ran for more than 100 million steps"), and `s0` … `s3` PNGs,
the last of Settings → From others. Two more variables serve the measurements and are inert
unless set (both hosts): `MV_SCRIPT_STORE=<folder>` reads open add-ons from that folder instead
of the profile's, and `MV_SCRIPT_SPIN=<addon>/<command>` keeps that command running back to back
(for the present-loop gate with a script busy; a command that stays inside the limits, such as a
half-second loop, keeps the add-on's worker at one core without stopping it):

```bash
MV_SCRIPT_STORE=/tmp/mv-script/rig/store MV_SCRIPT_SPIN=rig.spinner/burn caffeinate -d -i build/bin/frametime --seconds 60 --lab build/bin/mediaviewer_lab
```

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

# video threads (#233): MV_CRASH_TEST=video crashes the video decode thread on the
# first frame of any clip. Copy a corpus clip into the canary folder and open that.
$env:MV_CRASH_TEST = 'video'
.\build\bin\Release\mediaviewer_lab.exe --open "<canary folder>\SECRET_FILENAME_canary_7Q3.mp4"
Remove-Item Env:MV_CRASH_TEST
# scrub and scan as above; the dump's mv_decode_N slots read
# "role=video fmt=mov,mp4,... dec=<codec>/Lavc... <w>x<h> <bits>bit" and name no path.
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

# the video threads (#233): the video decode thread crashes on the first frame of any clip;
# the dump's mv_decode_N slots read "role=video fmt=mov,mp4,... dec=<codec>/Lavc... WxH Nbit"
mkdir -p /tmp/mv-crash-canary/PRIVATE_FOLDER_canary
cp tools/testmedia/av_transport.mp4 /tmp/mv-crash-canary/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.mp4
MV_CRASH_TEST=video build/MediaViewer.app/Contents/MacOS/MediaViewer \
    /tmp/mv-crash-canary/PRIVATE_FOLDER_canary/SECRET_FILENAME_canary_7Q3.mp4
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

Pushes to `main` and pull requests build four Windows variants (MSVC Release, MSVC Debug, clang-cl, ASan), run the C# ABI smoke test, and fuzz each decoder for a minute. On macOS they build the native core on macos-15 (arm64) and run the full `ctest` suite (`mv_tests`, `mv_import_tests`, `mv_ai_tests` and the Mac-only tests), build the three SwiftPM chrome packages, and run `swift test` on `src.swift/AIChrome`. The Mac job shares its vcpkg cache key with the release workflow's arm64 leg, so a cold cache costs the whole Mac manifest once. A newer push to the same branch cancels the run it replaces. Dependencies are installed by one job and reused. The first run after `vcpkg.json` or the pinned vcpkg baseline changes spends about 45 minutes there, mostly building FFmpeg. Later runs restore that cache. The nightly schedule is the long fuzz run, separate from pull requests.

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

### Windows search: the typed space, the resume notice, a result list from an empty window (#443, 2026-10-15)

Three Windows-only fixes; the Mac is unchanged.

- **Space in a search box.** Every type-in field (`Shared/FakeInput.cs`: Local search, file
  search, rename, Settings, add-on pages) draws its text in TextBlocks either side of the caret,
  and a TextBlock measures no trailing space, so the caret sat against the last letter until the
  next one came. The runs now draw spaces as no-break spaces (same advance); the text keeps plain
  spaces. The key router already left every key but Esc to a focused field
  (`test_key_router.cpp`, "a focused text control owns every key but Esc").
- **Resume indexing notice.** It is now an island of its own over the canvas corner, as the AI
  badge is, instead of a windowed `Popup` of the command bar that never showed. A launch read
  that lands before the bar is up waits for it.
- **A result list from an empty window** (a later window's first search, a search from the
  welcome) left native on "nothing open": no gallery, no strip, no arrows, only the chosen
  result. FolderReady for a list now moves it to a listing (`main.cpp`), and the chrome sends
  the panel's Enter / Ctrl+Enter view after FolderReady, not before it.
- **Checked:** the three C# chromes (Chrome, Ai.Chrome, Import.Chrome) compile on the Mac
  (`dotnet msbuild -t:Compile`, warnings as errors); no banned WinUI control. Not run on Windows:
  the native change (`main.cpp`) compiles only in CI, and the verify line, the placement of the
  notice island (100 % / 200 %, resize, under Settings) and the PR 1 gate are owed to a Windows
  machine.

### Windows: the "Press T" filmstrip hint shows (#446, 2026-10-15)

The hint card never appeared on Windows. It was the last windowed `Popup` in the chrome, built like
the resume notice above, and is now an island of its own in the same corner
(`IslandHost.HintNotice.cs`), made on the first show and parked, not disposed, when it fades. Native
also asks again when the filmstrip comes up (`main.cpp`, the layout sync): on Windows the strip
state is what is shown, so a clip or document that opened before its folder had a strip was never
due. The Mac was already correct; a Mac that has learned the hint (`mv.hint.filmstripT`) shows
nothing, by design.

- **Checked:** the C# compiles on the Mac (warnings as errors). Not run on Windows: the native line
  compiles only in CI, and the card's placement, stacking over the resume notice, "Got it" keeping
  the keyboard on the canvas and the PR 1 gate are owed to a Windows machine.

### The AI badge on the canvas (#345, docs/plans/metadata-search-and-place.md slice 5, 2026-10-12)

A file that says it was AI-made gets a small "AI" badge in the bottom-right of the canvas, with
"Made with …" and the evidence on hover or focus; *Settings → Viewer* switches it off
([06-metadata.md](design/06-metadata.md#the-ai-badge-345)). The answer comes from a bounded
provenance probe (`meta/probe.h`) on the pool, after the metadata pause, so the pane need not be
open. The probe covers JPEG, PNG, WebP, TIFF, TIFF-based RAW (DNG, CR2, NEF, ARW, PEF, ORF, RW2),
RAF, CR3, HEIF / AVIF items (Exif, XMP) and clips (moov udta / meta: (C)swr, (C)too, the
QuickTime software key, XMP), by seeking, under one byte budget. Tests: `mv_tests "[ai-badge]"`
(the setting's default, the evidence lines, the badge text, the store's probe cache) and
`"[probe]"` (the probe gives the full read's answer, fixture by fixture — HEIF items in mdat and
idat, MOV (C)swr, MP4 (C)too, QuickTime keys, clip XMP, DNG / ORF / RW2, CR3 CMT1, RAF — plus
WebP, truncated and mutated files). `mv_tests "[.perf-meta]"` prints the probe's cost and answer
beside each full read (`MV_BENCH_DIR=folder` adds every file in it).

- **Mac (M-series, 2026-10-12):** compiles clean, app and SwiftUI chrome included. `mv_tests` 681
  cases (674 passed, 8 skipped). Probe vs full read, warm, median of 40 (one fuzzer running on
  another core): camera JPEG 6 MP 38 µs vs 15.0 ms, PNG 2.2 MP 685 µs vs 3.4 ms, HEIC seed 23 vs
  92 µs, libheif example HEIC 54 vs 691 µs, WebP 23 vs 97 µs, CR3 (Canon EOS R6) 307 µs vs 18.2 ms,
  NEF 93 µs vs 38.6 ms, DNG 32 µs vs 21.7 ms, ARW 48 µs vs 19.8 ms, 60 s 1080p MP4 / MOV (moov at
  the end) 42–49 µs vs 12.7–13.1 ms; the MOV with `com.apple.quicktime.software=Sora` reads "Sora,
  AI" from both. `fuzz_meta_probe` (Homebrew LLVM, ASan + UBSan): 661 s, 8.2 M executions, coverage 4 856, no finding. In a copy of the app
  under its own bundle id: the badge shows on a ChatGPT-style C2PA JPEG and not on a camera JPEG,
  is named "AI-generated" in the accessibility tree, and switching the setting off and on hides
  and restores it with no relaunch. The hover / focus popover and VoiceOver reading it were not
  checked by hand.
  `--browse-soak` on a 14-file folder (12 camera JPEGs from the RAW samples, 2 with a C2PA store),
  base and new alternated, three runs each: cold jumps median 60.1 / 53.6 / 59.6 ms base vs
  39.6 / 63.6 / 64.7 ms new (the last new run voided: window covered 0.7 s); cached arrows
  0.38 / 0.41 / 0.48 vs 0.44 / 0.47 / 0.46 ms. First pixel (`--soak 6 --static`, three
  alternated runs): AI JPEG 61 / 79 / 63 ms base vs 48 / 124 / 57 new, camera JPEG 31 / 39 / 38
  vs 34 / 40 / 26; full resolution overlaps the same way. Held arrows were not measured: the Mac
  `--browse-soak` has no held phase (the probe is never submitted while keys repeat; the 90 ms
  pause restarts on each arrow). Idle with the badge up: on this in-use machine every 20 s
  static soak presented, base included (badge on 58 / 19 / 14, badge off 27 / 27 / 27, base 53 /
  13 / 28): the badge's own zero is not established by measurement. PR 1 gate: the animated soak
  passed once (3 600 frames, p99 16.95 ms, max 22.9 ms) and the idle soak had 0 presents but
  failed the CPU line (1.9 %), as the base did (1.75 %); a second new run stalled 22 s with the
  window covered. Owed to a quiet run.
- **Windows:** the C# chrome compiles on the Mac and passes the WinUI control check by script.
  The native half (`main.cpp`, `chrome_host.*`, `chrome_ai_badge_args`, the geometry test in
  `test_chrome_host.cpp`) and the running island are compiled and run only by CI and a Windows
  machine: not yet. The island's corners are filled with the chrome's canvas colour, not see-
  through: an island cannot blur the swapchain under it.

### Every tag searchable (#291, docs/plans/metadata-search-and-place.md slice 4, 2026-10-12)

File search (Ctrl/⌘+F, and `/`) matches names, then tags: camera, lens, made-with, software,
title, caption, keywords, creator ([06](design/06-metadata.md#search-text-291),
[16](design/16-commands.md)). A folder's tags are read once, in the background, after the first
image, into `meta_cache.db` next to the thumbnail cache. Date-taken sort rides the same pass. Local
search gains `camera:`, `lens:`, `ai:` / `made:`, `place:` / `in:` and the AI index schema 4
([17](design/17-local-ai-search.md)). Place names are the Places add-on's (owner, 2026-10-11).
The app's search text has none.

- **Tests:** `mv_tests "[search]"` (fold, builder, match / why, cache keying and invalidation,
  the folder pass, a fake Places provider), `mv_ai_tests "[schema4],[tags]"` (3 → 4 migration,
  a real kill mid-step via `fork` + `SIGKILL`, the backfill and its resume, the terms) and
  `"[query]"`. `mv_tests "[.perf-search]"` times one search read against `meta::read` and
  `read_date_taken`.
- **Verify fixture:** `MV_SEARCH_FIXTURE_DIR=<dir> [MV_SEARCH_FIXTURE_COPIES=n] mv_tests
  "[.make-search-fixture]"` writes `camera_r5.jpg`, `chatgpt.jpg`, `plain.jpg` and `nikon.jpg`.
  For the 20 k folder, hard-link them 20,000 times (no disk cost).
- **Dependency:** `sqlite3[fts5]` in `vcpkg.json` (both platforms). The first configure
  rebuilds sqlite3.
- **Mac (M-series, 2026-10-12):** compiles clean, app and both SwiftUI chromes included. `mv_tests` 688 cases (680
  passed, 8 skipped); `mv_ai_tests` 114 (106 passed, 8 skipped). Two of four full AI runs had one
  different engine test time out (`an empty index adopts the file's Quality`, another), and each
  passed alone. The `[tags]` and `[schema4]` cases passed 5 of 5 runs.
  - Bridge check: the dev app was driven through lldb on the four-file fixture, with no AI pack.
    `mv_chrome_tag_search` gave `R5` → camera_r5.jpg ("Canon EOS R5"), `ChatGPT` → chatgpt.jpg,
    `nikon` → nikon.jpg, `holiday` / `pena` → camera_r5.jpg, and `portugal` → nothing (no
    place names in the app).
  - Perf, base (`feature/issue-290-all-tags` eab9f08b, own worktree) vs new, alternated, Release,
    20,000 hard-linked JPEGs, meta_cache.db deleted before each new run so the pass read all
    20,000 during it:
    - First pixel (`--soak 6 --static`, 3 runs each): base 9 / 14 / 10 ms, new 18 / 12 / 0 ms.
      Full resolution: base 20–26 ms, new 12–21 ms.
    - `--browse-soak --json` (2 runs each): arrow → next, warm, ready median 0.02 ms in all four
      runs. The max was 0.03 / 0.10 ms base and 0.06 / 0.02 ms new; warm present median was
      0.23 / 0.18 ms base and 0.22 / 0.20 ms new. Cold jumps: median 0.02 ms everywhere; max
      86 / 23 ms base and 24 / 14 ms new.
    - Limits of this run: the hard-linked fixture is four images, so decode work is light, and
      first pixel was measured 3 times only. Not measured: a 20 k folder of distinct camera
      files, the Windows `quick` / `held` steps.
  - PR 1 gate: the animated soak passes (3,600 frames, p99 16.75 ms, max 16.86 ms). The idle
    soak's CPU line fails on this machine for new (76.8 %) and base (84.6 %) alike, both with
    0 presents. That is environmental, as in #290.
  - Not clicked through the SwiftUI search bar by hand.
- **Windows:** the C# chromes (Chrome, Ai.Chrome, Interop) compile on the Mac with
  `dotnet msbuild -t:Compile`. The native half is compiled and run only by CI and a Windows
  machine, so none of it has run yet: `abi.cpp` (the session's tag pass, `mv_folder_tag_search`,
  `mv_folder_forget_tags`, date sort from the pass), `addon_abi.cpp` (`tag_facts`),
  `main.cpp`, and the running WinUI search and `/` find box. Also owed there: the verify line
  and the PR 1 gate.

### Local search over audio files, slice A1 (#355, docs/plans/audio-search.md, 2026-10-15)

MP3, M4A and M4P are indexed by their tags and cover art, only while Settings → File types
**Show audio files** is on ([17](design/17-local-ai-search.md) "Audio"). The index goes to schema
8 (no table change). `meta_cache.db` rows are read again once (`kSearchFactsVersion` 3: album,
album artist, composer, genre). Sound and speech on audio files are slice A2.

- **Tests:** `mv_ai_tests "[audiofiles],[query]"` (the gate: off adds no row and decodes nothing,
  on indexes, off drops results at once and rows after, a restart keeps it off; the kind bit,
  `is:audio`, an older chrome's 3); `mv_tests "[audio]"` (cover art from an M4A the test writes,
  none is `not_found`; title, artist, album and genre as search fields);
  `swift test --filter SearchModelTests` (the Audio segment).
- **Mac (M-series, 2026-10-15):** `mv_ai_tests` 133 cases, 125 passed, 8 skipped (model-backed);
  `mv_tests` 830 cases, 819 passed, 11 skipped; `mv_import_tests` 112 passed; `MediaViewer`,
  `mediaviewer_lab`, `mv_ai` and the AI SwiftUI chrome build.
- **Windows:** both C# chromes compile on the Mac. The native change is portable (pack,
  `addon_media.cpp`, `edit/clip_sample.cpp`, `meta/search.cpp`) and is compiled on Windows only
  by CI.
- **Owed:** the plan's A1 verify on a real folder (300 photos and 200 audio files) in the app on
  both hosts; both PR 1 gates with the pack indexing audio files; timing the walk with the
  setting off.

### Local search over documents, slice D4: scanned pages (#354, 2026-10-17)

A PDF page with no text layer is drawn by the host and read by the OS text reader of #403
(Vision on the Mac, `Windows.Media.Ocr` on Windows), so a quoted phrase finds a scan's page
([17](design/17-local-ai-search.md) "Documents", "Scanned pages (D4)"). No model, no setting of
its own (part of Words), no schema change. Its own pass, last of all, 4 pages a claim.

- **Tests:** `mv_ai_tests "[documents],[ocr]"`: only pages without a text layer are drawn (a
  stamped "- 2 -" counts as none; a text PDF and a DOCX never are), found at their page with the
  text layer's passages untouched, read once across a restart, no count without an OS reader,
  read while the tower is still its CPU stand-in for Core ML, and a 100-page scan stopped at a
  page resumes at the next (each page read once, PDFium asked for 227 page visits rather than
  about 1,300) while a photo added mid-scan is pictured before most of the scan is read. The
  SwiftUI chrome's `StatusLineTests` cover the new line. With D3's piece, a scan's read text is
  embedded by meaning (the meaning pass waits for the scan pass). Mac (M5, 2026-10-17, rebased
  on main with D3): `mv_ai_tests "[ai]"` 157 cases, 145 passed, 12 skipped (hidden benches);
  `mv_tests` 900 cases, 889 passed, 11 skipped (one run under load had one failure that two
  reruns did not repeat); `swift test` on `src.swift/AIChrome` 24 passed; the WinUI AI chrome's
  C# compiles.
- **Verify set:** `swift tools/ai-bench/doc-scans/make_doc_scans.swift <out> --photos <dir> 300`
  types 50 one-page letters, invoices and reports (5 fonts, 9-12 pt body, a 7.5 pt footer),
  rasterises them at 200 dpi in grey, turns them up to a degree, adds paper tone and noise,
  JPEG-compresses them and writes image-only PDFs, plus a 100-page scanned book (142 MB),
  `truth.tsv` and `queries.txt` (every scored word, quoted, and a 4-word phrase a book page).
  Then `ai-bench --documents --brief --top 200 --query-file queries.txt` and
  `score_doc_scans.py <output> truth.tsv`. The reader's own cost and recall, without the engine:
  `MV_OCR_SCAN_DIR=<out>/scans MV_OCR_SCAN_TRUTH=<out>/truth.tsv mv_ai_tests "[.ocr-scans]"`.
- **Verify line, Mac (M5, 2026-10-17), real Vision, PDFium and host renderer through `ai-bench`
  on a dev-signed pack:** the 50 scanned pages, every word quoted (1,261 searches, stop words and
  single letters not scored): **mean 99.4 % of a page's words found, median 100 %, 49 of 50
  pages at ≥ 90 %**. The worst page, 83.5 %, lost four lines of its first paragraph: Vision
  skips them at 1,583 × 2,048 and reads them at 1,236 × 1,600 or when the top half is read alone,
  every time; the other misses are single misread numbers and words. The reader bench agrees
  (recall 0.995, min 0.839, 49 of 50). Slowest query 0.7 ms.
- **Cost a page** (`[.ocr-scans]`, default QoS, the 50 pages): the host's draw 60 ms median, the
  read 640 ms median (614 mean) for a full page of text: about 15 times a photo's (44 ms, #403),
  so a 100-page scan is about 70 s of one worker. Inside the engine, at background QoS on this
  machine while other sessions held its load at 35-46, both were far slower: while ViT-B/32 was
  compiling for Core ML, Vision waited behind it (0 % CPU, in Vision's compute-device query) and
  the 50 pages took 47 s on the two workers.
- **Seen, not fixed (D1's words pass):** inside `ai-bench` the 142 MB book's text layer took 51-76 s
  to read on five of six runs (0.7 s on the other), against 50-190 ms through the same reader in a test
  process at the same QoS and load; samples put the time in the file reader's `read()` calls. A
  256 KB read-ahead window in `pdf_pdfium.cpp` measured no better (alternated, 62-76 s against
  0.7-56 s), so it is not in this change. The scan pass waits on it only for a document whose words
  are still being read.
- **A 100-page scan and the photo pass:** shown by the engine test (a photo added while a
  100-page scan is read is pictured before 60 of its pages are, each page read once), not yet timed
  on hardware. Timed runs (`ai-bench --documents --add <30 photos> --add-after 15` on a folder
  holding the scan, alternated with one without it) were started three times, and this Mac stayed at load
  35-90 from other sessions' work throughout. Background-QoS work crawled: 4 photos and one
  scanned page in 10 minutes. The photos kept moving on the other worker while the scan's read
  waited in Vision, but those are not numbers to publish. The timed A/B is owed on a quiet
  machine.
- **Not measured:** Windows (the native and WinUI code compiles in CI only; `Windows.Media.Ocr`
  on scans, the Windows timing and the Windows PR 1 gate are owed). The Mac PR 1 gate with scans
  being read is owed: the screen was locked through this session, and a soak there gets no
  frames. The book's 4-word page phrases on hardware (`score_doc_scans.py` prints them; the
  engine test covers a phrase at its page). Real scans (phone-camera pages, skew past a degree, two columns, handwriting) beyond
  the synthetic set. The app on either host with a scanned folder.
### Local search over documents, slice D3: the Documents piece (#354, 2026-10-17)

An optional piece, **Documents** (`ai-docs`, ~91 MB), adds one sentence-embedding model,
all-MiniLM-L6-v2 (fp32, CPU, 2 threads), so a document's words also match by meaning
([17](design/17-local-ai-search.md) "Documents": the spike table, the margins, the ranking). Schema
10 adds `passage_vecs`; `.mvindex` goes to v2, with documents behind the new **Documents** tick
on export and import (`MV_AI_TRANSFER_DOCUMENTS`, off by default). Both chromes list the piece,
say "Understanding documents N of M", and add the tick and the "matches by meaning" hint.

- **Spike** (`tools/ai-bench/doc-meaning`): `fetch_corpus.py <out>` fetches the 478 public
  documents (run it with `python -I`: they are untrusted), `spike.py passages|embed|speed|score|
  calibrate` scores the candidates (`MV_SPIKE_CAP`, default 64 passages a document), and
  `queries.json` holds the 50 queries, their relevance and 20 "nothing" queries. Targets were
  written before the first score (P@5 >= 0.70, R@10 >= 0.80, above BM25).
- **Model:** all-MiniLM-L6-v2 met them with the best P@5 in the capped spike (0.810 / 0.827 fp32;
  the other three and their int8 numbers are in 17) and ran 112-119 passages/s at 2 threads
  against 53-69 for the others (alternated, three rounds, load 10-36).
- **ORT-Python reference:** `mv_ai_tests "[docs]"` with `MV_AI_PACK_DIR` (the ORT folder) and
  `MV_AI_DOCS_DIR` (a staged ai-docs: `tools/package/ai-models.py stage --piece ai-docs`).
  `tests/data/ai/docs_reference.json` comes from `tools/ai-reference/make_docs_reference.py`
  (tokens and vectors, ORT-Python 1.31 on CPU). Mac (M5, ORT 1.30): every token id equal, worst
  cosine **0.999999** over 12 passages (accents, Japanese, a 400-word one) and 4 queries, one at a
  time and batched. The WordPiece rules always run (`"[docs]"`, no model).
- **Engine, end to end** (M5, 2026-10-17): `ai-bench --addons <sideloaded ai + ai-docs> --index
  <the 478> --documents --quality 1 --top 10 --query ...` then `doc-meaning/score_bench.py`:
  - All 478 read for words in 61 s; every passage (93,726) embedded in 838 s from the start; the
    meaning pass runs while the picture tower compiles for Core ML.
  - The 50 queries: **P@5 0.731, R@10 0.767** (English 0.748 / 0.784). Words alone (no piece):
    0.304 / 0.272, 19 queries answer nothing. Queries took up to 94 ms.
  - **Short of the R@10 target (0.80).** The spike met it over 64 passages a document; uncapped,
    the model alone scores 0.757 / 0.782, the long IRS publications' passages outranking the
    forms. Ranking by margin instead of cosine lost a further 0.03 (now cosine).
- **Ceiling:** `addon-pack.py ceiling` over the installed 0.1.32 AI, AI Audio and AI Faces
  manifests and a dev-packed AI Documents: 2.419 GB of 5.0 GB (Mac). `ai-models.py check`:
  Core + People + Sound + Documents + a vendor piece ~3.02 GB (the Windows case, estimated).
- **Tests:** `mv_ai_tests` 151 cases, 142 passed, 9 skipped (model-backed) on the M5, including
  the meaning pass (embedded after the words, found at its page, "nothing found", kept through a
  restart, the reader, gone with Show documents) and `.mvindex` v2 (documents only when ticked,
  passages and vectors imported with nothing embedded again); `mv_tests` 869 cases, 858 passed,
  11 skipped; the packaging tests (`test_addon_pack`, `test_github_release`) pass. The SwiftUI AI
  chrome builds and the WinUI AI chrome's C# compiles (MediaViewer.Chrome stops on
  `IslandHost.Slots.cs`'s `DownloadSize`, unchanged here and fixed on main by #459).
- **Not measured:** both present-loop gates (the Mac's screen was locked for every attempt; the
  Windows gate is CI's), Windows altogether (the ORT-Python cosine, the timing, the ceiling on
  real manifests), the app on either host with the piece installed, and another language than
  English beyond the spike's 3 queries.

### Local search over documents, slice D2: pages as pictures (#354, 2026-10-16)

Settings → Local search → Documents gains **Words and pages** (`doc_index` 2, per folder
`MV_AI_DOCS_PAGES`) on both hosts. A document's first 3 pages, and any page mostly covered by
images, go through the picture tower already installed, so "a receipt" or "a floor plan" finds the
page ([17](design/17-local-ai-search.md) "Documents", "Page pictures (D2)"). No new model, no
schema change.

- **Tests:** `mv_ai_tests "[documents]"`: which pages are drawn (first 3, picture pages, the end of
  a DOCX, nothing for a locked one), found at their page as a picture match, kept through a
  restart, dropped by a folder's or Settings' Words and drawn again at Words and pages. Also PDFium's
  picture-page measure on hand-built PDFs (`[pdfium]`). Mac (M5, 2026-10-17, on D1 with main
  merged): `mv_ai_tests` 148 cases, 140 passed, 8 skipped (model-backed); `mv_tests` 896 cases,
  885 passed, 11 skipped; `mv_import_tests` 158 passed; the SwiftUI AI chrome builds; the WinUI AI
  chrome's C# compiles.
- **Verify set:** `tools/ai-bench/doc-pages/make_doc_pages.swift <out> <photos>` draws 120 PDFs,
  6 each of 20 kinds of page (receipt, floor plan, bar / pie chart, line graph, slide, sheet music,
  calendar, crossword, chessboard, flowchart, map, QR code, barcode, spreadsheet, certificate,
  sudoku, circuit diagram, a full-page photo, business card). The kind's page sits on page 0-2, or
  for 4 of the photos on page 7 or 8 of a long document. Names are neutral and the labels go to
  `labels.tsv`. Run `ai-bench --documents --doc-index 2 --top 10 --query ...` over them beside D1's
  200 text documents, then `score_doc_pages.py`.
- **Verify line, Mac (M5, ViT-L/14 on the Neural Engine, 2026-10-16), 320 documents:**
  - Mean P@5 over the 20 queries: **0.80** as typed (words and pictures) and **0.83** with Match
    by Picture only (`--kinds 31`).
  - Every relevant hit was on the labelled page (80 / 80 and 83 / 83). Queries took 10-25 ms, and
    100 ms for "a receipt" as typed.
  - 15 queries scored 1.0. "a line graph" and "a calendar" scored 0.8.
  - "a map" scored 0.4: slides and sudoku pages ranked above two of the maps.
  - "a spreadsheet" scored 0.0: D1's plain DOCX text pages rank above the drawn spreadsheets.
  - "a receipt" scored 0.0 as typed: D1's random-word documents contain the word "receipt", and
    a full word match outranks a picture. With Picture only it scored 0.6.
  - "a photo of a person" found nothing. So did "a person", "a man", "a portrait" and "a face",
    tried afterwards, although the 6 photo pages are indexed: the "nothing found" gate turns them
    away.
  - All 4 deep photo pages were found by the picture-page rule (pages 7 and 8).
  - No new model: the pack's files are D1's, and the process opened `clip-l14/image.onnx` alone.
  - Timing: with the tower in, 316 documents pictured (851 pages) in 49 s. The first start of a
    freshly sideloaded pack spent about 11 minutes on ViT-L/14's Core ML compile first; words
    were read meanwhile. The second start loaded in 4.6 s.
- **Mac PR 1 gate** (`frametime --seconds 60`):
  - Alone: 3,600 frames, p99 16.80 ms, max 22.4 ms, 0 idle presents, 0.035 % idle CPU, PASS.
  - With `ai-bench` picturing 1,280 documents' pages in a separate process through both soaks:
    3,600 frames, p99 17.05 ms, max 23.3 ms, 0 idle presents, 1.0 % idle CPU, PASS.
- **Not measured:** Windows (the native and WinUI code is compiled by CI only; the Windows timing
  and the Windows PR 1 gate are owed), the app on either host with a real document folder, and
  DOCX picture pages beyond page 2.

### Local search over documents, slice D1 (#354, docs/plans/document-search.md, 2026-10-16)

PDFs and DOCX files are indexed for their words while Settings → File types **Show documents**
is on ([17](design/17-local-ai-search.md) "Documents"): a quoted phrase finds the page, Enter
opens the document there, plain words rank by the speech rule. PDFium (pinned in
`cmake/ai.cmake`) ships in the Core pack; the index goes to schema 9.

- **Tests:** `mv_ai_tests "[documents],[query]"` (the gate on, off and through a restart; first
  and last page phrases, page rows, `is:pdf`, `text:` / `said:`, locked and broken files, a
  folder's Off and Settings' Off dropping the text, a 600-page PDF stopped mid-way and resumed
  under the 500-page cap, no PDFium setting PDFs aside; the passage rule; PDFium on hand-built
  PDFs; the host's DOCX text and page tiles; schema 8 → 9); `mv_tests "[docx]"` (`docx_text`
  reading order and pages); `swift test --filter SearchModelTests`; `tools/package/test_addon_pack.py`
  (PDFium and its notices in the pack). Mac (M5, 2026-10-16): `mv_ai_tests` 145 cases, 137
  passed, 8 skipped (model-backed); `mv_tests` 856 cases, 845 passed, 11 skipped;
  `mv_import_tests` 151 passed; the SwiftUI chrome's tests 9 passed; everything builds.
- **Verify line, Mac (M5, 2026-10-16), through `ai-bench --documents` on a dev-signed pack:** a
  synthetic folder of 200 documents (120 PDFs incl. one of 500 pages, two password-protected and
  one cut short; 80 DOCX incl. one cut short), made by CoreGraphics / CoreText and a zip writer.
  All 196 text documents found by a quoted phrase from their first page (page 0) and from their
  last page (the PDF's last page, checked against CoreGraphics' page count): 392 of 392, slowest
  query 4.1 ms. The 2 locked and 2 broken are counted, nothing crashed. Indexing the 200: within
  3 s of the pack loading on three runs (all 200 done at the first 5 s status line), so ≤ 60 s
  holds. Plain words ("tenant invoice") 70 ms over 190 matches.
- **Mac PR 1 gate with documents indexing:** `frametime --seconds 60`, alone and then with
  `ai-bench` reading 4,000 documents (the 200 hard-linked 20 times, about 90 a second, through
  the first 45 s of the soak, in a separate process, so the viewer's yield never reached it):
  animated 3,599 frames and 0 dropped both times, p99 16.95 → 16.90 ms, 0 idle presents. The
  idle-CPU clause failed both times (5.55 % and 5.68 % of a core) on this used machine, as it
  does on base.
- **Not measured:** the verify line's 300 photos beside the documents, and Enter in the app (both
  hosts open a document result on its page in code; only the core tests ran it). Windows: both
  C# chromes compile on the Mac; `main.cpp`, `addon_abi.cpp`, `abi.cpp` and PDFium's Windows
  build are compiled by CI only. The Windows timing on the dev box and the Windows PR 1 gate are
  owed.

### Local search reads more of each file (#403–#407, 2026-10-15)

Five changes to the AI pack's index and query language
([17](design/17-local-ai-search.md), [12](design/12-decision-log.md)). The index goes from schema
4 to 7, one transaction a step; nothing already embedded is embedded again.

| Issue | What changed | Schema |
|---|---|---|
| #404 | `in:` / `before:` / `after:` / `since:` / `until:` use the date taken, the file's time only when there is none | none |
| #405 | `rating:`, `rejected`, `label:`, `iso:`, `f:`, `mm:`, `shutter:`; meta_cache.db and the host's `tag_facts` carry them | 5 |
| #406 | chapter titles and text subtitle lines (host `read_cues`, `meta/cues.h`) match quoted and plain words | 6 |
| #407 | the People pass decodes the moments the picture pass kept; a Live Photo's movie is sampled onto its still | 7 |
| #403 | the OS reads the text in each photo (Vision / Windows.Media.Ocr) for quoted and plain words | (uses 6) |
| #403 follow-up | the text on the moments a clip's picture pass kept, one row per moment; Settings → Local search → **Read text in photos** on both hosts; `mv_ai_status` counts the cue and text passes | (uses 6) |

- **Tests:** `mv_ai_tests "[dates],[exposure],[cues],[moments],[live],[ocr],[schema4],[tags],[query]"`,
  and `mv_tests "[search],[cues]"` (the cue reader over Matroska / SubRip and MP4 / mov_text
  files the test writes, `tests/cue_fixture.h`).
- **OCR recall and cost:** `python3 tools/ai/ocr-fixture.py <dir>` writes 40 labelled photos;
  `MV_OCR_FIXTURE_DIR=<dir> mv_ai_tests "[.ocr-fixture]"` reads them with the real OS reader and
  prints recall and per-photo times as JSON (needs Pillow; on Windows, an OCR language).
  `MV_OCR_CLIP_DIR=<folder of clips> mv_ai_tests "[.ocr-moments]"` prints what a clip moment
  costs the text pass (the 2,048 px seek and decode, and the read) as JSON.
- **Mac (M-series, 2026-10-15):** `mv_ai_tests` 127 cases, 119 passed, 8 skipped (the
  model-backed ones, as before); `mv_tests` 824 cases, 813 passed, 11 skipped; `mv_import_tests`
  (the add-on host table) 112 passed. The app (`mediaviewer_lab`) and the AI SwiftUI chrome build. Vision over the fixture: 38 of
  40 phrases, read 44 ms median a photo.
- **Windows:** the C# chromes compile on the Mac (`dotnet msbuild -t:Compile`). The native
  half is compiled only by CI: `ocr_win.cpp` (C++/WinRT), the host's `read_cues` and `tag_facts`
  in `addon_abi.cpp`. The OCR fixture on Windows, the Windows PR 1 gate and the Mac PR 1 gate
  with the pack indexing are owed.
- **#403 follow-up, Mac (M-series, 2026-10-15):** `mv_ai_tests` 131 cases, 123 passed, 8 skipped;
  `mediaviewer_lab`, `mv_ai` and the AI SwiftUI chrome build. Tests: `mv_ai_tests "[ocr]"` (clip
  moments, resume after a quit, the status counts, `read_text` / `text_available`);
  `"[.perf-bench]"` times the status counts (14 ms at 25,000 assets, cached between refreshes).
  A clip moment costs ~0.2 s (`"[.ocr-moments]"`, mostly the seek and decode; machine loaded).
  **Windows:** the C# chromes compile on the Mac; the engine change is portable and compiled only
  by CI there. The cue and text counts are appended on `MvAiStatus` (1,672 bytes). `AiApi` does not
  pin that size (#388); the ABI smoke test compares the C# mirror with the header. Windows OCR on
  clips, both PR 1 gates with the pack indexing are owed.

### Place terms need Places, and intersect (#441, 2026-10-16)

Without the Places add-on, `place:portugal` in Local search now shows no results and "Place search
needs the Places add-on". The help line names `place:` only while Places is loaded. With Places,
the AI pack's engine asks it for each place term through the host table's new `place_matches`
and intersects: `beach place:portugal` is Portugal's beaches only, and `-place:spain` takes
Spain's out. The "+ N in Portugal" button no longer appears for place terms
([17](design/17-local-ai-search.md) "Places over the index", ABI 0.31).

- **Tests:** `mv_import_tests "[slots]"` (`has_place_terms`; Local search asks no provider for a
  place term; `provider_paths` with no provider, every batch, a failing provider, a caller that
  stops, a provider removed mid-request; the host table's `place_matches`);
  `mv_import_tests "[places]"` (the real `mv_places` answering the pack's requests over a fake
  `locations_of_index`); `mv_ai_tests "[places]"` (no Places: `place:` / `in:` / `-place:` and
  `blue place:portugal` find nothing; with a fake Places: only its assets, `-place:`, two places,
  `blue place:portugal` ranks only Portugal's, kinds and scope still apply);
  `swift test --filter SearchModelTests` (`testPlaceTermsNeedPlaces`).
- **Mac (M-series, 2026-10-16):** `mv_tests` 860 cases (849 passed, 11 skipped);
  `mv_import_tests` 155 passed; `mv_ai_tests` 136 cases (128 passed, 8 skipped, model-backed as
  before); AIChrome `SearchModelTests` 9 passed. `MediaViewer` and the AI chrome bundle build.
  Not run: the live check with the pack and Places installed, and the PR 1 gate. The change adds
  no render-thread or UI-thread work: a place-term search now waits on its own search worker for
  Places' walk of the index's coordinates. That walk used to run after Return instead. Its
  latency was not measured on a large index.
- **Windows:** the core change (`slots.cpp`, `host.cpp`, `slots_abi.cpp`) is shared and compiled on
  the Mac; MSVC builds it only in CI. The C# (`SearchWindow.cs`, `Slots.cs`) compiles on the Mac
  (dotnet@8, compile-only). The live check is owed on Windows too.

### `place:` over the whole Local search index (#398, 2026-10-15)

`place:portugal` in Local search with no folder open now finds the indexed Portuguese assets.
Places walks the host table's new `locations_of_index`, which serves the AI pack's index.db
coordinates (mv.ai.1 `each_location`) and reads no file
([17](design/17-local-ai-search.md) "Places over the index").

- **Tests:** `mv_import_tests "[slots]"` (the thunk over a fake index: every row once, a visit
  that stops, a stale generation, a request going stale mid-walk, a removed add-on, publish and
  retract of the process-wide source with a walk in flight); `mv_import_tests "[places]"` (the
  real `mv_places` answering `place:portugal` / `in:spain` with no folder, and no index attached);
  `mv_ai_tests "[places]"` (`index_db::located_assets` batches and `engine::each_location`).
- **Mac (M-series, 2026-10-15):** `mv_import_tests` 114 passed; `mv_ai_tests` 133 cases, 125
  passed, 8 skipped (model-backed, as before); `mv_tests "[places]"` 12 passed, 1 skipped.
  `addons_mac.mm` compiles. Not run: the live verify (the AI pack and Places installed in the
  app, `place:portugal` with no folder open) and the PR 1 gate, which this change does not touch
  (no render-thread or UI-thread work).
- **Windows:** the wiring is in `addon_abi.cpp` (publish on the `ai` load, retract and wait on
  unload, retract at quit), compiled only by CI; no C# change. The live verify is owed there too.

### Every tag and AI provenance in the Metadata pane (#290, docs/plans/metadata-search-and-place.md slice 1, 2026-10-11)

All tags now lists C2PA / Content Credentials (JPEG APP11, PNG `caBX`, ISO-BMFF `uuid`), every
PNG text chunk, raw XMP Exiv2 did not list, the ICC profile and JFIF / WebP / HEIF container
items, all read-only ([06-metadata.md](design/06-metadata.md#beyond-exiv2-290)). Summary gains
"Made with" from one generator table; `meta::summary` keeps GPS as numbers (`where`, from EXIF,
XMP or ISO 6709) and `made_with` / `ai_generated` for the map and badge slices.

- **Mac (M-series, 2026-10-11):** compiles clean (`-Werror`), app and SwiftUI chrome included.
  `mv_tests` 675 cases (667 passed, 8 skipped); `"[provenance]"`, `"[gps]"`, `"[icc]"` cover the
  ChatGPT (multi-segment APP11), Firefly, Microsoft Designer `caBX`, Stable Diffusion, ComfyUI,
  Midjourney XMP, DJI XMP GPS, ISO-BMFF, ICC and 2 000-tag fixtures plus truncation / mutation of
  every reader. The seven `fuzz_meta_*` harnesses ran 600 s each under ASan + UBSan (Homebrew
  LLVM 23): 0.66–2.3 M executions each, no finding. `mv_tests "[.perf-meta]"`, five alternated
  base / new runs, 60 reads each: camera JPEG 17.7–20.0 ms base vs 16.9–22.8 ms new, 10 s clip
  10.3–13.3 vs 11.0–13.8 ms, HEIC 61–71 vs 56–77 µs, PNG and WebP overlapping (the readers alone
  cost ~2 µs on a JPEG; meta reads run only when the pane or overlay wants a record). PR 1 gate:
  the animated soak passes (3 599 frames, max 17.2 ms); the idle soak's CPU line failed on this
  machine for base (95.7 %) and new (74.4 %) alike, 0 presents in both — owed to a quiet run.
  Not checked in the running app by hand.
- **Windows:** the C# chrome compiles on the Mac (`dotnet msbuild -t:Compile`). The native side
  (`chrome_host.*`, `main.cpp`, the 80-byte `chrome_meta_args`), the virtualised All tags list
  (`ItemsRepeater`, no 400-row cap) and the clang-cl fuzz build are compiled and run only by CI
  and a Windows machine: not yet.

### Location editing in the Metadata pane (#352, owner 2026-10-12)

Summary's Location row has **Edit** instead of Remove: type decimal or degrees-minutes-seconds
coordinates, see the parsed point (or the error) under the field, Return saves, Esc cancels. A
blank save removes the location after a confirmation. Plain JPEGs get EXIF GPS in place;
everything else an XMP sidecar; RAW originals are never touched
([06-metadata.md](design/06-metadata.md#the-location-row-352-owner-2026-10-12)). Tests:
`mv_tests "[location]"` (link or fetch `tools/testmedia/raw` for the RAW case).

- **Mac (M-series, 2026-10-12):** core, bridge, SwiftUI half and `mv_tests` built and run;
  `fuzz_meta_location` ran 600 s under ASan + UBSan with no finding. The Mac alert's Esc and
  focus were not driven in the app.
- **Windows:** the C# chrome compiles on the Mac. `main.cpp`, `chrome_host.*`,
  `src/abi/meta_abi.cpp` (ABI 0.27) and `test_abi_roundtrip` are compiled only by CI and a
  Windows machine: not yet run, and neither is the in-pane confirmation's Esc (closes only the
  confirmation; the field keeps the keyboard).
### Places: the offline reverse geocoder (issue #344, plan slice 2, 2026-10-11)

The core is built:
- `src/geo`, `tools/geo/build_places.py` and the place database format;
- `mv_tests "[geo]"`, including 300 hand-checked coordinates when the full blob exists;
- the `fuzz_geo` loader fuzzer, which ran 10 minutes clean after its first find was fixed;
- the perf bench.

Measured on this Mac (Release, warm): open in 11–12 ms; lookup p99 8.4 µs on land and 4 µs on
a random sweep. The blob is 17.2 MB, or 5.4 MB xz-compressed.

### Places add-on and its map (issue #344, plan slice 3 + place search, 2026-10-12)

Built on `feature/issue-344-places-addon` (merges #348's slots): `src/addons/places`, its
manifest (the info section, the search provider, the settings page; nothing else), the map
rasteriser with cursor zoom down to town scale, packaging (`addon-pack.py` entry, `stage-places`,
the data licences) and both release workflows. A stable release publishes
`mediaviewer-addon-places-{win-x64,macos}.{zip,json,json.sig}` when that pack is complete. Places
is preferred, not required: `publish` still publishes when it was not packed, warns, and refuses a
partial or invalid pack. A staging or pack failure does not fail the release. Preview and
artifacts runs do not pack it. Settings → Add-ons offers it on both hosts even when it is not
installed (optional; coordinates still show without it).

- **Mac (M-series):** `mv_tests "[places]"` 9 cases / 261 assertions and `"[geo]"` pass;
  `mv_import_tests` 94 cases pass, including `"[places]"` (2 cases, the add-on through a signed
  store); `test_addon_pack.py` 22 tests pass with the C++ verifier. The map renders in 4.5–4.7 ms
  CPU (median) at 640 × 320 px, max ~20 ms ([27](design/27-places.md#performance)). A dev copy of
  the app with Places sideloaded showed, at the bottom of the pane opened with `I`, the label, the
  coordinates and the map with the accent pin and town names; −, double-click and drag re-drew it
  with the pin held on the photo's point. A 5-minute `fuzz_geo` run with `towns_in` was clean.
- **Review pass (2026-10-12):** fixed Antarctica drawn as water (a ring round the globe), rings
  a world away from the view, `wrap180` looping on a huge longitude, a 1600 pt pane at 3× getting
  no map, search never saying done past the host's 100 000-row cap, `mv_geo` symbols exported from
  the module, and `mv_addon_verify` checking against host table 1. Now `mv_tests "[places]"`
  12 cases / 296 assertions (ASan + UBSan clean), the full `mv_tests` 682 cases (8 skipped),
  `mv_import_tests` 94 cases, `test_addon_pack.py` 22 tests with the verifier; a 5-minute
  `fuzz_places_map` run (71.8 k executions, ASan + UBSan) was clean. Map CPU time not worse
  (240 renders, base/new alternated ×3, medians: 1× 0.58/0.58, 0.69/0.69, 0.66/0.64 ms; 2×
  1.70/1.63, 1.80/1.76, 1.88/1.74 ms).
- **Static soak, dev copy, alternated base (no add-on) / new (Places):** both present about once a
  second with the pane closed and about twice a second with it open (30 s: base 54, new 61
  presents with the pane open, one run each), on a machine at load average 50–85. Places did not
  add a present pattern of its own, but the base was not at 0 idle presents in this setup either,
  so **0 presents idle with the section shown is not established**; it is owed to a quiet-machine
  run with the PR 1 gate.
- **Not measured:** `--browse-soak` with the pane open (the harness cannot open the pane, and the
  machine was loaded), launch → first pixel with the add-on installed (alternated), pinch and
  mouse-wheel zoom by hand (computer use cannot send them; the gesture maths is unit-tested and
  #348's chrome sends the events), Windows anything: the add-on compiles in CI only.

### Screens and slots (PRs 58-59, 2026-10-11)

What is built is in [docs/design/25](design/25-open-addons.md#implementation-notes-prs-58-59-2026-10-11).

- **Mac (M-series, 2026-10-12):** `mv_import_tests` 92 cases pass, including 19 new ones
  (`"[screens]"`, `"[slots]"`; the last loads `mv_slots_test_addon` through a signed store and the
  v3 table). The same suite passes under ASan / UBSan and `"[slots]"` under TSan (the portable
  build, with `-DCMAKE_EXE_LINKER_FLAGS="-framework CoreFoundation -framework DiskArbitration
  -framework IOKit"` on a Mac). `mv_tests` 665 cases (657 passed, 8 skipped), the 2 new `"[location]"` among them. The app builds; with the test
  add-on sideloaded (below) into a dev copy, the Metadata pane showed its section last, under
  the footer, with the coordinates and a surface carrying the accent pin, and the command bar and
  path bar showed its items.
- **Windows:** both C# projects compile (warnings are errors); no `check-winui-controls` word in
  the new files. `slots_abi.cpp` and `addon_abi.cpp` were syntax-checked with clang on the Mac;
  MSVC builds them in CI.
- **Owed, both hosts:** launch → first pixel with a slot add-on installed (alternated runs), 0
  presents idle with a section shown, both PR 1 gates, the 2,000-tile grid scrolling, file search
  finding the fixtures in the running app, a screen driven by keyboard alone, VoiceOver and
  Narrator naming every control, removal from Settings in the running app, Windows' docked pane
  placement.
- **Every window, and entries on the filmstrip and canvas (#402, 2026-10-15):** slot add-ons
  load in every window's process, found by manifest without hashing; context-menu entries show
  on filmstrip tiles and the canvas as on gallery tiles. Mac: `mv_import_tests` 114 cases pass
  (`"[slots]"` 14, two new); the app and its Swift chrome build. Windows: the chrome compiles;
  the native side (`addon_abi.cpp`, `chrome_host.cpp`, `main.cpp`) is CI's. Owed: a second
  window and the menus tried in the running app on both hosts, the first-pixel cost of a later
  window loading Places (alternated runs), both PR 1 gates.

**Running the test add-on (developer builds only).** Configure with
`-DMV_ADDON_DEV_PUBLIC_KEY=<your key's public half>`, build `mediaviewer_app` (Mac) or the core
and chrome (Windows) and `mv_slots_test_addon`, then:

```sh
python3 tools/package/slots-test-sideload.py --module build/addons/slots-test/libmv_slots_test_addon.dylib \
  --key <dev key file> --addons <scratch>/addons --fixtures <scratch>/fixtures    # --platform win-x64 on Windows
# Mac: a copy with its own bundle id, HOME, add-ons and thumbnails; never the real profile
open -n --env HOME=<scratch>/home --env MV_DEV_ADDONS_DIR=<scratch>/addons \
  --env MV_DEV_THUMBS_DIR=<scratch>/thumbs <copy of MediaViewer.app> --args <scratch>/fixtures/inside_a.jpg
```

Every name in the test add-on is "Slots test"; nothing in it looks like Places. Quit the dev
copy when done.
### Sharepoint (issues #410, #413, 2026-10-15)

What is built is in [docs/design/28](design/28-sharepoint.md).

- **Mac (M-series):** the app, the Swift chrome and `libmv_sharepoint.dylib` build clean.
  `mv_import_tests` 138 cases pass, 26 of them `"[sharepoint]"`: the path rule, the offer (a
  wrapping total and an oversized file refused), the beacon, the code, the channel (same code
  both ends, a machine in the middle shows two codes, a key that does not match its commitment
  and an altered byte end it, another version is refused, pings never stretch a deadline), a
  peer that connects and stalls does not hold up shutdown, made-up beacons neither flood the list
  nor take a peer's address, and two or three engines over loopback TCP
  found through an in-memory bus (nothing about the files leaves before the sender's person
  confirms the code; Accept delivers with matching bytes and keeps both on a name clash; a set
  over 32 MiB to a peer heard only on Bluetooth says so before Send, waits, and sends by itself
  once the peer's beacon arrives, and Cancel ends the wait; pair half and sidecar ride along; a folder keeps its shape without housekeeping; Decline,
  closing the window and a third client receive nothing; busy; Cancel from either side keeps what
  finished and leaves no `.mvtmp`; no room; an offer opens a closed window when receiving is on;
  no presence with the window closed), then the add-on through a signed store and the v3 table,
  offline. `mv_tests` 819 cases (808 passed, 11 skipped) and `mv_script_tests` pass;
  `SharepointPackTest` passes.
- **ThreadSanitizer (Mac, 2026-10-15):** `"[sharepoint]"` is TSan-clean: 16 runs, 8 one after
  another and 8 as four at once (load average ~12), every one 23 cases passed, 0 reports. The
  engine, the channel and its pinger, the transfer's reader, the busy refusals and the accept loop
  raised nothing. The first run's 4 reports were all the flood test's own fake discovery
  publishing its callback to the test thread unsynchronised; it now publishes through an atomic.
  Sharepoint is not in `cmake/portable`, so this is the [macOS](#macos-mac-prs-16) configure in
  its own build directory with `-fsanitize=thread -fno-omit-frame-pointer -g` in
  `CMAKE_{C,CXX,OBJC,OBJCXX}_FLAGS` and `-fsanitize=thread` in
  `CMAKE_{EXE,SHARED,MODULE}_LINKER_FLAGS`, then `ninja mv_import_tests` and
  `TZ=UTC build-tsan/bin/mv_import_tests "[sharepoint]"`. Bluetooth (`bt_mac.mm`, `bt_win.cpp`)
  and the UDP beacon thread (`presence.cpp`) are not exercised by these tests, so TSan says nothing
  about them.
- **In a dev copy (offline, `MV_SHAREPOINT_OFFLINE=1`, sideloaded with the dev key):** its command
  opened the window with the two files it was given, on the host's theme; the gallery shows the
  share icon; a range set through the bridge made copy path copy the three files, and Next ended
  it. The Shift-click itself, the range's tint, the drag out of the grid, the Choose… dialog and
  the share icon's install offer need a mouse: owed to a hand check.
- **Measured, Mac, alternated, warm (load average 5–22):** launch → first pixel of a RAW
  (`mediaviewer_lab --soak 6 --static`) without / with Sharepoint installed, 15 runs each:
  22–76 ms (median 39) / 23–88 ms (median 33). No difference within noise. The PR 1 gate
  (`frametime --seconds 60`) passed with Sharepoint installed (3600 frames, max 17.0 ms, idle
  0 presents, 0.06 % CPU); the alternated base run failed on a 1.3 s stall on a machine that had
  just woken, so the base's pass is not shown by this run.
- **Windows:** the C# compiles (warnings are errors). `net_win.cpp`, `bt_win.cpp` (C++/WinRT,
  the first use in the tree, under `/W4 /WX /GR-`), `main.cpp`'s range and `slots_abi.cpp` meet a
  compiler in CI only.
- **Owed:** the issue's verify on two machines on one Wi-Fi (see each other, same code, Accept
  delivers the marked files with matching bytes, Cancel mid-way leaves nothing partial, a third
  machine receives nothing), the same small send with Wi-Fi off and Bluetooth on (Mac–Mac,
  Mac–Windows, Windows–Windows), the crash-report check after a transfer, Windows anything in
  the app, both hosts' first-run prompts (local network and Bluetooth on the Mac, Defender
  Firewall on Windows), VoiceOver and Narrator over the window, the Windows PR 1 gate.

### Sharepoint in the background (issue #452, 2026-10-16)

What changed is in [docs/design/28 "In the background"](design/28-sharepoint.md#in-the-background-452):
**Receive when MediaViewer is closed** (off by default, implies Keep receiving) keeps the process
with a menu-bar item / notification-area icon; offers are system notifications that open the
window; the host table gains `notice_post`, `notice_authorize` and `keep_alive` (ABI 0.33).

- **Mac (M-series):** the app, the Swift chrome and `libmv_sharepoint.dylib` build. `mv_tests`
  (849 passed, 11 skipped) and `mv_import_tests` (154 cases) pass; new cases: the notice names the
  sender and then the count and size, never the file name or the code, is taken back when the
  offer ends, and its Decline declines; the toggle implies Keep receiving, keeps the process, is
  kept across launches and asks for permission only when turned on; the registry's notice and
  keep-alive tables (bounds, replace, the button's event, detach).
- **In a dev copy (dev-signed, own bundle id and HOME):** with the toggle kept on, the add-on came
  up receiving at load (listener and beacon, the lock held) and a quit event was refused as the
  background path should. The run stopped there: macOS asked the dev copy for local-network
  access and a first-run alert was open over the viewer, both needing the owner's click, so the
  hide, the menu-bar item, the windowless idle cost and the notification were not seen.
- **Measured (Mac, base and new built in their own trees, alternated, load average ~5 with
  another session's lab at 99 % CPU and a Simulator running):** the PR 1 gate's animated soak
  passed on both (3600 frames, p99 16.75 ms, max 16.82 ms new / 16.85 ms base); its idle phase
  failed on both on CPU (8.2 % new, 5.7 % base, 0 presents each): the machine, not the change, so
  the quiet-machine idle verdict is owed. Launch → first pixel (`--soak 6 --static`, a HEIC
  fixture, 5 each): base 6–17 ms, new 3–17 ms, no difference within noise.
- **Windows:** the C# compiles (warnings are errors). `main.cpp` (hide on close, the tray icon and
  its menu, the `----AppNotificationActivated:` argument) and `chrome_host.cpp` meet a compiler in
  CI only. Not run: anything in the app.
- **Owed:** the issue's verify on two machines per platform (notification within ~2 s, click opens
  the code, Accept saves); the windowless idle measurement in the app against `"[.sharepoint-idle]"`
  (Keep receiving 0.020 %); note that a hidden viewer window is occluded, and the Mac render loop
  polls occlusion every 200 ms (`k_occlusion_poll_ms`), which is in that number; with it off, no
  process after quitting and no beacon seen by the other machine.
- **Start at login (2026-10-17):** built on both hosts ([28](design/28-sharepoint.md#in-the-background-452)):
  `SMAppService.mainApp` on the Mac, the Run value `MediaViewer` = `"<install>\MediaViewer.exe"
  --background` on Windows (`src/shell/login_item_win.*`), both following the keep-alive list.
  Try the hidden start without signing out: `mediaviewer_lab.exe --background` (Windows; it exits
  at once if a viewer is already running) or `MediaViewer.app/Contents/MacOS/MediaViewer
  --background` (Mac). Installed builds and the bundled app only write the login item.
  Windows, dev build (2026-10-17): with a viewer running, `--background` exits in 195 ms without
  bringing it forward; with no add-ons it stays hidden and exits 0 at 60 s; with Sharepoint's
  toggle kept on it stays hidden and kept (`keep=1`); `mv_tests "[login-item]"` and the hidden
  `"[.login-item]"` (writes and removes the real Run value) pass. Not seen: the tray icon (a
  tool-launched lab is on another desktop), an installed build writing the value, a real sign-in.
  The PR 1 gate failed 3 of 3 runs of the new build on a loaded machine (one with 15 dropped
  frames, one on idle CPU only); the alternating base runs are owed.
  Mac, MediaViewer.app (M-series, 2026-10-18; a copy with its own bundle id, `HOME` and a
  dev-signed Sharepoint): `--background` with no add-ons has no window and no Dock icon and quits
  at 60 s; with Receive when closed kept on, Sharepoint listens, the menu-bar item appears and
  nothing else; reopening from Finder or Open MediaViewer shows the window, a Quit event goes
  back to the menu bar, the menu's Quit ends the process. A kept start registers
  `SMAppService.mainApp` (status enabled); the toggle off unregisters it, and closing the
  Sharepoint window then quits; a login start with the toggle off quits at 60 s and removes a
  registered item. Fixed on the way: a Quit event (Dock, AppleScript) while kept closed the
  menu-bar item's window and then AppKit's last-window check terminated the app. Mac PR 1 gate
  2 × 60 s PASS (p99 16.80 / 16.75 ms, 0 idle presents). The kept windowless app: 0.12 % of one
  core over 60 s (`ps`, 10 ms resolution) against 0.047 % for `"[.sharepoint-idle]"`'s engine
  alone. Not seen on the Mac: a real logout/login (`keyAELaunchedAsLogInItem` on a mainApp
  item), the notification from a second machine.

### Sharepoint, AirDrop-smooth (issue #442, 2026-10-15)

What changed is in [docs/design/28](design/28-sharepoint.md): the AirDrop-like window on both
hosts and the screen vocabulary it added (`tiles`, card, ring, code field, display text); the
sender types the code the receiver shows (three wrong codes end the link); received files go to
Downloads and Save to is kept; Keep receiving is kept across launches with **Sharepoint:
receiving** in the command bar; the receiver checks file N while N+1 arrives; the share icon no
longer waits for an earlier selection; Windows screen windows have the app icon, a size floor and
an owner, and Settings shows each add-on as one card.

- **Mac (M-series, Mac17,4):** `MediaViewer.app`, the Swift chrome and `libmv_sharepoint.dylib`
  build. `mv_import_tests` 150 cases and `mv_tests` 850 (839 passed, 11 skipped) pass;
  `"[sharepoint]"` 33 cases and `"[screens]"` 13. ThreadSanitizer: `"[sharepoint]"` four runs at
  once, 0 reports; `"[screens]"` 0. `test_addon_pack.py` passes.
- **Measured (Mac):** `"[.sharepoint-bench]"` (loopback, alternated with the base three times):
  500 × 2 MiB 124–141 → 158–180 MB/s, 1 GiB unchanged (crypto-bound); `"[.sharepoint-idle]"`
  Keep receiving on, window closed: 0.020 % CPU, 6.4 switches a second, no present (off 0.0003 %).
  The PR 1 gate (`frametime --seconds 60`) passed alone (3599 frames, p99 16.8 ms, max 16.9 ms,
  idle 0 presents, 0.042 %) and with Sharepoint sending over loopback in another process the whole
  time (p99 16.8 ms, max 16.8 ms, idle 0 presents, 0.037 %).
- **Seen on the Mac:** every state of the window drawn by the chrome's own `ScreenNodeView` (light
  and dark) in a scratch render harness, and the window opened in a dev copy of the app, offline
  (`MV_SHAREPOINT_OFFLINE=1`, dev-signed, files handed over through `mv_chrome_slots_invoke`).
- **Windows:** the C# compiles (warnings are errors). `net_win.cpp`'s Downloads lookup and lock
  meet a compiler in CI only. Not run: the window's look, its icon, Settings' cards, the stall fix
  in the app.
- **Owed:** two machines (Wi-Fi and Bluetooth MB/s against a plain copy, typing the code, Keep
  receiving after a relaunch seen from the other machine), Bluetooth's idle cost with Keep
  receiving on, Windows screenshots and an ETW trace of the share icon on 500 items, the Windows
  PR 1 gate, VoiceOver and Narrator over the new tiles and code field.

### Local search: ask before resuming an interrupted index (issue #299, 2026-10-11)

A quit or a kill part-way through indexing no longer carries on, or loads the pack, at the next
launch. The window asks in a bottom-right notice (Resume / Dismiss), and Settings' folder rows
offer Resume. Built on both hosts; the shipped pack holds unfinished roots.

- **Mac (M-series):** `MediaViewer`, the Swift chromes and `mv_ai` build clean. `mv_ai_tests`
  (`[engine]`, `[nle]`, `[search-agent]`, `[infer]`, `[photos]`, `[cloud]`, `[query]`: 77
  passed, 7 skipped for no pack), `mv_import_tests` (75 cases, `"[ai-resume]"` new) and the
  AIChrome Swift tests pass. Not run in the app: the dev build loads no AI pack, so the notice,
  its Resume and the rows' Resume are unexercised with a real pack.
- **Windows:** both C# chromes compile (warnings as errors). The notice is a windowed `Popup` of
  the command-bar island; its placement (window corner, above the filmstrip, after a resize or a
  window move, at 100 % and 200 % scaling) is unchecked on Windows.
- **Owed on both:** launch → first pixel and idle CPU with a root left mid-index, against no pack
  work, with an installed pack (the same measurement #292 owes).

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
pictures and breaks; since issue #429 also headers and footers with page fields, footnotes and
endnotes, columns, floating pictures and shapes with text wrap, text boxes and right-to-left text
(the shared layout is `codec/doclayout`); not comments (docs/design/04). Measured on the Mac
(`mv_tests "[docx]"`; a `textutil`-written DOCX rendered by
`MV_DOCX_DUMP=out.jpg MV_DOCX_FILE=x.docx mv_tests "[.docx-dump]"` and looked at; the #429 verify
report by `MV_DOCX_REPORT=1 MV_DOCX_PAGE=0 MV_DOCX_DUMP=out.jpg`, its bytes kept with
`MV_DOCX_SAVE=report.docx`; `mv_tests "[.docx-mutate]"` damages a package's bytes and its XML).
The Windows font lookup (`fonts_win.cpp`) and the DLLs' packaging are compiled and staged by CI
only.

**PPTX (issue #429, D5 amended 2026-10-15):** a slide a page, drawn by `codec/pptx.cpp` over the
DOCX zip / XML / `codec/doclayout` path; what is and is not drawn is in docs/design/04 and the
file's header. Only the slide being drawn is parsed (with its layout, master and theme). Tests:
`mv_tests "[pptx]"`, `"[.pptx-mutate]"` (package bytes and slide XML, also clean under
`-DMV_ASAN=ON`), the seed `tests/data/seeds/pptx/small.pptx` (`make-seeds.py pptx_small()`) for
`tools/fuzz/fuzz_pptx.cpp` and the Windows broken-corpus sweep. A slide is looked at with the DOCX
dump: `MV_DOCX_DUMP=out.jpg MV_DOCX_FILE=deck.pptx MV_DOCX_PAGE=n mv_tests "[.docx-dump]"`.

**Page sidebar (issue #429):** both hosts, `Shift+T`, docs/design/04 "Page sidebar" and
docs/design/16. Core: `codec::decode_pages` (PDF opened once, DOCX laid out once, PPTX slides
parsed one by one), `image::make_page_thumbs` / `page_thumb_book`; Windows ABI 0.32
`mv_folder_page_thumbs`. Tests: `mv_tests "[page-sidebar]"`. Numbers:
`MV_BENCH_DOC=file mv_tests "[.perf-bench][page-sidebar]"` prints a JSON line (8 thumbnails cold
at the top and at the far end, and a jump to page 250: the cached thumbnail's first pixel, then the
full page). Measured 2026-10-16 on the Mac (M-series, release build), medians of 5:

| Document | 8 thumbnails cold | jump: first pixel | jump: full page |
|---|---|---|---|
| PDF, 300 pages (`cupsfilter` text) | 5.3 ms | 0.22 ms | 3.4 ms |
| DOCX, 78 pages (`textutil`) | 56 ms | 0.11 ms | 40 ms |
| PPTX, 100 slides (pictures, table) | 5.3 ms | 0.22 ms | 7.3 ms |

The machine was shared with other builds (load average 18–33) for later runs, which spread the
DOCX numbers to 190–300 ms; the table is the first, quietest run. A DOCX page past the first lays
out the whole document, as before #429. **Not done yet:** the Windows half (native `main.cpp`,
`chrome_host`, ABI) is compiled by CI only and has not been run; the PR 1 present-loop soak while
flinging the sidebar has not been run on either platform; first pixel of a document open is
unchanged in code (the open path does not touch the sidebar) but was not re-measured.

**Culling (issue #207):** `X` reject, colour labels on keypad / `Ctrl+Shift` `6`–`9`, `U`
to clear, and the pane's Label swatches, on both hosts through PR 12's writer and sidecar rule
(`shell::reject_toggled`, `label_toggled`, `label_fields`; `meta::summary::label`). View ▸ Show
filters on them (Picks, Not rejected, a colour), stacked on #206's kind filter: each file's rating
and label is read on a worker (`meta::read_cull_marks`) and cached by path + size + mtime, in the
core on Windows (ABI 0.20 cull bits, `mv_folder_note_metadata_written` after a write) and in
`shell::meta_store` on the Mac. Tests: `mv_tests "[cull]"` (both platforms; the ABI case in
`test_folder.cpp` is Windows-only). Not driven interactively in the app on either platform yet.

**Settings → File types:** "Show audio files" and "Show documents" are off by default, which leaves
MP3/M4A/M4P and PDF/DOCX out of folder listings; the file a folder was opened on, and the current
stop, stay, so Open With still shows one. Windows filters in the core (`mv_folder_set_hidden_kinds`,
ABI 0.19, `[view] show_audio` / `show_documents` in settings.ini); the Mac host filters its folder
model's listing (`mv.viewFlags` bits 12–13). Test: `mv_tests "[settings]"`. Gallery folder-tile counts still include hidden kinds.

**View ▸ Show (issue #206):** the kind filter, Windows and macOS. Windows filters in the core
(`mv_folder_set_kind_filter`, ABI 0.20, the marks passed in for Marked); the Mac host filters its
folder model's listing with the same rule (`io::keeps_stop`). The Mac does not pair RAW+JPEG, so
there the RAW half shows under RAW and the JPEG half under Photos. Tests: `mv_tests "[filter]"`
(both platforms; the ABI case is Windows-only).

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

The Mac PR 1 gate is the **Frame-time gate (self-hosted Mac)** job: an interactive
Apple Silicon runner labelled `self-hosted`, `macOS`, `ARM64`, `gpu`, logged in with the
display on and nothing else drawing (Homebrew at `/opt/homebrew`; the job installs the
vcpkg trees itself). Set `MV_MAC_GPU_RUNNER_ENABLED` to `true` only after provisioning it.

Requiring either gate job directly does not protect anything: GitHub treats a skipped
job as success for merge. Require **Present-loop gates (Windows PR 1 + Mac PR 1)**
instead. It always runs, fails when a gate failed or was cancelled, and puts a warning on
the run when a gate was skipped. Once both runners exist, set repository variable
`MV_REQUIRE_PRESENT_GATES` to `true` so a skip on this repository's own branches fails
too (fork PRs still only warn; they never run on the persistent runners). Baseline cache entries use unique keys and
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
  The README's playback numbers come from runs of one to two minutes.
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
`MV_REQUIRE_CORPUS=1` turns them into failures. CI's Build job sets it, over media
the test-media job generates; without that, a build that proves nothing about video
looks exactly like one that proves all of it. Hosted runners have no GPU (only the
Basic Render Driver), so the cases that assert hardware decode — D3D11VA on the 4K
HEVC/AV1 clips, the no-stall ring, and the ones that need an `mv::gfx::device` — SKIP
there and are proved only on a machine with a GPU.

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
src/geo         offline reverse geocoder over the place database (the Places
                add-on links it; docs/design/27)
src/edit        EditStack + geometry (the blit's output -> source map), lossless
                JPEG rotate / flip / MCU crop, JPEG / PNG export with a metadata
                policy (PR 10)
src/canvas      pan/zoom springs, fit / fill / 100 %, sticky zoom
src/gfx         D3D11 device, flip-model swapchain, frame pacer, blit
src/addon       the add-on host: MediaViewer's own add-ons (signed manifests, store,
                loader, host function table) and open add-ons from other makers
                (package reader, manifest schema 2, publisher keys, themes; the Lua
                script sandbox, its limits and commands, script_*; docs/design/25)
third_party/lua Lua 5.4.8, vendored, static (MIT); without io/os/package/debug
                (third_party/lua/VENDOR.txt)
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
                addon-sdk (MIT): the tool people make add-ons with; geo/build_places.py
                (the place database from pinned Natural Earth + GeoNames data)
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
