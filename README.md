# MediaViewer

A Windows viewer for a real camera dump — photos and video in one folder. Opens everything
instantly and pans without a dropped frame. The first release ships the viewer through
PR 7, packaged in PR 8; metadata tools, photo edits/export, video trimming, and additional
Windows integration follow in future updates. **v1 is Windows.** From PR 4 the native core is
kept hostable; macOS is Milestone F (PR 16–20), a later host of the same core, not a UI-only
port — see [plan/15-platforms.md](plan/15-platforms.md).

**Status: PR 7's slices are all merged and pass locally. Its clean-VM HEIC, real
Live Photo and on-screen no-pop checks now have gates around them
([Where this actually is](#where-this-actually-is)); what remains of those three
is a clean VM, a phone and one look at a RAW opening. PR 8 — packaging — is in progress:
the icon, About, the Inno wizard, the Velopack updater, opt-in telemetry and the bundled
runtimes are in the tree and build, the wizard installs and uninstalls cleanly on this
machine, and the signature-rejection suite passes. It has **not** been through the
clean-VM run its verify line asks for, and no artefact is signed — see
[Package and install](#package-and-install-pr-8). PRs 9–15 are future feature updates.
The owner widened the 2026-09-13 sequencing exception on 2026-09-17
([plan/12-decision-log.md](plan/12-decision-log.md)) so Mac work (PR 16–20) no longer waits
on Windows PR 8 shipping; PR 16 (Metal present lab), PR 17 (decode + pan/zoom, folded in the
PR 7 formats/Crashpad scope) and PR 18 (SwiftUI chrome, folded in the PR 4/PR 6
folder/filmstrip-backend/keyboard scope) are all in the tree. The Darwin target configures,
builds and links with a real toolchain (`cmake`+`ninja`+`vcpkg`+`swift build`) and its Catch2
suite passes (210 assertions, 60 cases). It has been run on a real Mac with a display
(2026-09-19): the 60 s present-loop gate passes with the chrome on screen, and the window,
menu bar, filmstrip, gallery and `?` sheet were driven by hand. Still unproven on Mac:
drag-and-drop, copy/move/Trash and slideshow on real folders, animated GIF/APNG/WebP playback
(they show frame 0 as a still), and the tonal step when a RAW's embedded preview is replaced
by the full decode — see [macOS](#macos-pr-1618) below. PR 20 (MediaViewer.app: Finder open,
Quick Look thumbnails, Sparkle updates, the notarized disk image) is written but **not yet
built or run on a Mac** — see [MediaViewer.app](#mediaviewerapp-and-a-shippable-mac-build-pr-20).**
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

macOS is Milestone F ([plan/15-platforms.md](plan/15-platforms.md)), a later
host of the same core — not a UI-only port. PR 16 is the Metal present lab
(AppKit + `CAMetalLayer` + `CAMetalDisplayLink`). PR 17 adds JPEG/PNG/BMP decode,
immutable Metal texture upload, fit / wheel-zoom-toward-cursor / drag-pan, and an MSL
twin of the blit shader, plus (folded in from Windows PR 7) the rest of the D5 still
formats, RAW+JPEG/Live Photo pairing detection, and Crashpad + a Mac minidump scrub.
PR 18 hosts SwiftUI chrome in the same AppKit window (the canvas stays Metal, never
ported): a command bar, a bottom filmstrip, and a full-grid gallery overlay, all driven
by an FSEvents-backed folder model and a JPEG-512 SQLite thumbnail cache sharing
Windows' `jpg512.1` spec, lazy-loading thumbnails so a large folder doesn't stall the
scroll. Real folder navigation (argv, drag-and-drop-in, arrow keys and the rest of
plan/16-commands.md's Browse table), marks, copy/move-to, Trash delete, fullscreen,
a stills-only slideshow, and drag-out round out the folded-in Windows PR 4/PR 6 scope.
It does **not** yet play video (PR 19) or handle rating/metadata/RAW-pairing UI. A
Windows DXGI soak is not that verify.

PR 1's present-loop verify and PR 3's island-on-screen verify are inherited and
not yet demonstrated on a quiet GPU runner, and PR 5's and PR 6's own verify
lines are only partly demonstrated — read
[Where this actually is](#where-this-actually-is) before believing any of it.
The keys below come from the command table specified in
[plan/16-commands.md](plan/16-commands.md); press `?` in the app for the ones
that apply to what you are doing.

**Licence: GPL-2.0-or-later** ([LICENSE](LICENSE)). Settled in PR 1; the reasoning is in
[plan/11-licensing.md](plan/11-licensing.md).

---

## What is here today

| | |
|---|---|
| **`mediaviewer_lab.exe`** | A Win32 + DirectComposition window with a flip-model D3D11 swapchain. Open a folder, drop a JPEG/PNG/BMP/GIF/WebP or a clip, or pass a path on the command line. Animated GIF, APNG and WebP play on the render thread's frame clock, frame 0 first. Wheel-zoom toward the cursor, drag-pan, `0` fits, `1` is 100 %, `+`/`-` zoom, Left/Right browse. Video plays on the same swapchain as photos — never a `MediaPlayerElement`. Decode and ICC convert run on the worker pool; pan never re-decodes. `F` / `F3` toggles the frame-time overlay, which grows codec, decoder, A/V drift and present-counter lines while a clip is up. WinUI command bar (top) and filmstrip (bottom) are `DesktopWindowXamlSource` islands; the canvas is not a `SwapChainPanel`. |
| **`mediaviewer_core.dll`** | The native core behind a flat C ABI: job system, JPEG/PNG/BMP/GIF/WebP decode (giflib, libwebp), TIFF/ICO (libtiff), HEIC/HEIF (libheif + libde265), AVIF (libavif + dav1d) and camera RAW (LibRaw, embedded preview first), scan-time RAW+JPEG / Live Photo pairing, with animated GIF/APNG/WebP fed a frame at a time into a small texture ring, LCMS colour, immutable GPU upload, pan/zoom camera, folder listing, thumbnail cache, ±2 prefetch LRU, and the PR 5 video surface (open, transport, position/state/info/stats, magic-byte video probe). |
| **`MediaViewer.Chrome.dll`** | C# WinUI 3 chrome, loaded by the lab through hostfxr. Open (image or folder), View (zoom in/out, fit, 50 / 100 / 200 / 400 %, overlay), About, `ItemsRepeater` filmstrip, load indicator. Flyouts are supposed to open over the canvas without clipping — that is part of PR 3's verify. |
| **`frametime.exe`** | The frame-time regression harness. Runs a soak, writes a JSON report, compares against a rolling baseline, and fails on a dropped frame. |
| **`mediaviewer_lab` (Darwin)** | PR 16–18 Metal present lab. AppKit window, `CAMetalLayer` (max drawable 2 — Metal's minimum, see plan/12 — 8-bit sRGB), `CAMetalDisplayLink` wait-before-encode, idle → stop presenting, F3 overlay. Decodes a JPEG/PNG/BMP (plus the rest of the D5 stills) onto an immutable Metal texture; wheel-zoom-toward-cursor, drag-pan, `0`–`4` zoom presets. Real folder browsing: argv/drag-drop opens a folder or a file (selecting it), `←`/`→`/`A`/`D`/`Space`/`Home`/`End`/`PageUp`/`PageDown` navigate it (every key goes through the same command table and key router as Windows, with `⌘` standing for `Ctrl` and the Mac Delete key for `Delete`), an FSEvents watch keeps the listing live. SwiftUI chrome hosted in the same window via a C bridge into the render thread's `input_snapshot`: a Windows-style command bar (Open / View / Settings / About, `?` at the right), a Settings screen (`⌘,`: filmstrip/wrap/sticky-zoom/background preferences and remappable keys, persisted in `NSUserDefaults`), a bottom filmstrip (`T` toggles) and a full-grid gallery overlay (`G` toggles), both lazy-loading JPEG-512 thumbnails from a shared SQLite cache. Marks (`Insert`/`Shift+Space`/`Ctrl+A`/`Ctrl+D`), copy/move to a chosen folder (`F7`/`F8`, collision-safe), Trash delete with confirm (`Delete`), fullscreen (`F11`/`F`), a stills-only slideshow (`F5`), and drag-out (`⌘`+drag). **Video (PR 19):** FFmpeg + VideoToolbox decode, copied out of the decoder pool into a presentation ring of our own Metal textures, an MSL twin of the video shader (NV12/P010, the stream's matrix/range/transfer, HLG/PQ tone-mapped to SDR), Core Audio as the master A/V clock (no `AVPlayer`), a SwiftUI transport strip and the plan/16 video keys, and poster thumbnails for clips. No rating/metadata/RAW-pairing UI yet. Built only on Apple Silicon / macOS 14+. |
| **`MediaViewer.Interop`** | The C# side of the ABI — `SafeHandle`, struct layouts, completion drain. The filmstrip island borrows the session and drains folder/thumb completions. |

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
the v1 set arrives with the PR that needs it, listed in [`vcpkg.json`](vcpkg.json).
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

### macOS (PR 16–18)

Apple Silicon, macOS 14+, CMake ≥ 3.28, vcpkg, a full Xcode install (Command Line
Tools alone are not enough — `swift build`'s SwiftUI target and `xcrun metal` both
need it), Swift 6. Intel Macs are out of scope (D9). This path does not build FFmpeg,
WinUI, or the Windows lab.

```sh
export VCPKG_ROOT=/path/to/vcpkg   # bootstrapped
# Format libraries. libheif (+ libde265) and LibRaw are LGPL and must be dynamic
# (CLAUDE.md, plan/11), so they go in the dynamic triplet; the rest are permissive
# and static. libheif's default features stay off (its `hevc` feature is x265).
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx giflib libwebp tiff "libavif[core,dav1d]"
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic "libheif[core]" libraw
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
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

**Video on Mac (PR 19).** FFmpeg is LGPL and dynamic-link only, so it joins libheif/LibRaw in
the dynamic triplet:

```sh
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic \
  "ffmpeg[core,avcodec,avformat,avfilter,swresample,swscale,dav1d]"   # + `ffmpeg` for the CLI
```

Open a folder with clips in it. `Space`/`K` play/pause, `,` `.` frame step, `Q`/`E` ±2 s,
`J`/`L` ±10 s, `Shift+Q`/`Shift+E` speed 0.25–4×, `Shift+M` mute (`?` lists them; the transport
strip appears above the filmstrip while a clip is on screen). `F3` names the decoder that is
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

### MediaViewer.app and a shippable Mac build (PR 20)

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
types at rank *Alternate*: MediaViewer shows up in Finder's **Open With** and never makes
itself the default. After the first photo it opens, it asks once whether to become the
default; the MediaViewer menu has the same command.

A build that leaves your machine needs the updater, a Developer ID, and notarization
([plan/13](plan/13-updates-and-telemetry.md#macos-first-install--a-branded-disk-image-pr-20)):

```sh
# once: Sparkle's key pair, with generate_keys from the Sparkle 2.9.6 release
# tarball (the private half stays in your login keychain), and a notarytool profile
./Sparkle-2.9.6/bin/generate_keys                # prints the public key
xcrun notarytool store-credentials mediaviewer-notary ...
python3 -m pip install -r tools/mac/requirements.txt

cmake -S . -B build-darwin -G Ninja ... -DMV_SPARKLE_PUBLIC_ED_KEY=<public key> \
      -DMV_MAC_BUILD_NUMBER=<raise every release>
cmake --build build-darwin --target mediaviewer_app
python3 tools/mac/macpack.py release --app build-darwin/MediaViewer.app \
    --identity "Developer ID Application: …" --notary-profile mediaviewer-notary \
    --sparkle-bin build-darwin/_deps/sparkle-2.9.6/bin \
    --download-url-prefix https://github.com/longtimeno-c/mediaviewer/releases/download/v<version>/
```

`release` signs inside out with the hardened runtime, notarizes and staples the app, builds
`MediaViewer-<version>.dmg` (drag to Applications, the GPL shown on mount), notarizes and
staples that, and writes `updates/MediaViewer-<version>.zip` plus a signed
`updates/appcast.xml`. The app only accepts a feed and an archive signed with the key it was
built with. First install is the disk image; every later update is the zip, installed by
Sparkle when the user clicks **Update ready — restart** or quits. Upload the `.dmg`, the
zip, and `appcast.xml` to the GitHub release: the app's default feed is the latest
release's `appcast.xml`.

`python3 tools/mac/check_plists.py` and `python3 tools/mac/test_macpack.py` need no Mac and run anywhere.

## Run

```powershell
.\build\bin\Release\mediaviewer_lab.exe
.\build\bin\Release\mediaviewer_lab.exe path\to\photo.jpg
.\build\bin\Release\mediaviewer_lab.exe path\to\folder
```

| Key | |
|---|---|
| `F11` / `F` | fullscreen on the window's monitor; also available from View → Full screen. Hides the command bar, filmstrip and transport. `Esc` leaves |
| `F3` | frame-time overlay (off at launch with chrome) |
| `Space` / `Backspace` | next / previous. On a clip, `Space` is play/pause. It is no longer the lab sweep |
| `Home` / `End` | first / last in the folder |
| `PageUp` / `PageDown` | back / forward ten |
| `J` / `K` / `L` | clip transport: −10 s / pause / +10 s ([plan/16](plan/16-commands.md)) |
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
thumbnail or the current image (at fit) out to Explorer or another app. Pass
paths on the command line: the first one that exists opens (a file opens its
folder with that file selected).
Files added to or removed from the open folder show up without a restart.

A one-pixel grid appears at 400 % and above.
| `+` / `-` | zoom in / out (`=` and the numpad keys too) |
| `Ctrl+O` | open a photo or a clip (JPEG/PNG/BMP/GIF/WebP/TIFF/ICO/HEIC/AVIF/RAW, MP4/MOV/MKV/WebM/AVI/TS) |
| `Ctrl+Shift+O` | open a folder |
| `Ctrl+E` | show the current file in Explorer, selected. Open menu: **Open: filename** |
| `Space` / `,` / `.` on an animation | play or pause (a finished one plays again) / previous frame / next frame, like a clip. Delays follow browsers: 10 ms or less plays as 100 ms |
| `?` | the shortcuts for what you are doing right now. Also the `?` button on the right of the command bar |
| `Ctrl+,` | Settings: view defaults and remappable keys. Search the list by command or shortcut. Choose a shortcut and press its replacement; viewer shortcuts are suspended while Settings is open. Escape or Cancel change cancels capture; Escape otherwise closes Settings. Conflicts swap shortcuts, and Reset to default restores the map. `?` lists whatever you bind |
| `Ctrl+G` | go to an item by its number in the folder |
| `/` | find an item by name. With the filmstrip or gallery focused, just type |
| `Ctrl+Shift+E` | folder tree — arrives in PR 9; for now it beeps |

The title bar shows the current file, its position in the folder, its size and
the zoom. Arrow keys, `Space` and the slideshow wrap from the last item to the
first; turn that off under Settings.
| `Ctrl+Shift+O` | open a folder |
| `Left` / `Right` | previous / next in the folder |
| `G` | gallery: thumbnail grid of the folder. `W` / `S` or Up / Down move between rows; `A` / `D` or Left / Right move between items. `+` / `-` enlarge / shrink thumbnails (`=` also enlarges), keeping the selection visible. The size is remembered until the app closes. `Enter` opens the selected image in the normal viewer, showing the filmstrip if enabled. A click also opens the item; `Esc` leaves |
| `T` | filmstrip show/hide, for the mode you are in (folder open or single image) |
| `Tab` | focus the command bar island |
| `Esc` | walks out one level: gallery, fullscreen, then island focus back to the canvas. It never quits |
| `Ctrl+W` / `Alt+F4` | close the window |

Keys go through one router and one table (`src/shell/commands.h`,
[plan/16](plan/16-commands.md)). Symbol keys (`?`, `+`, `\`) follow your
keyboard layout, not a US key position.

Wheel zooms toward the cursor; drag pans. Zoom-out floors at 50 % (Fit can
still go smaller on a huge image) and rubber-bands a little past that, then
springs back to centre. Drop a file on the window.

Command line: `--soak <seconds>`, `--json <path>`, `--gate` (non-zero exit if the verify
line fails), `--no-overlay`, `--static`, `--no-chrome`, `--open <path>`,
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
the bottom and the gallery grid are both `ItemsRepeater` islands over the same listing; thumbs are JPEG files from `%LocalAppData%\MediaViewer\thumbs`. Clips get a
thumbnail too — a poster frame from about 10 % into the clip, in that same cache — so a
camera dump does not show blanks where the video is. With no
folder open the canvas is a drop target reading *Drop a photo or a clip here*, not the
present-lab sweep. The gallery and filmstrip accept the same drop, and you can drag a
thumbnail or the fitted image out to Explorer.

The playback transport is a **third island**: a bottom-centre strip that appears with a
clip and goes away with it. Its height is reserved out of the canvas rectangle the same
way the filmstrip's is, so it can never cover the video ([plan/16](plan/16-commands.md):
do not grow an island over the canvas). Speed is owned by the core, so the dropdown and
the keyboard cannot disagree.

Opening a single image lists its folder too, so `Left` / `Right` and the gallery work on the
files beside it. Whether the filmstrip comes with it is a preference: **Settings** has
*Filmstrip when opening a folder* (on by default) and *Filmstrip when opening an image* (off),
persisted to `%LocalAppData%\MediaViewer\settings.ini` (also wrap, sticky zoom, canvas
background, and key remaps). `T` toggles the one for the mode you are in. Both take
effect immediately — no restart. The Settings screen is where those defaults live.

## Test

```powershell
# the video test corpus — REQUIRED for anything PR 5 claims to prove.
# The clips are gitignored (plan/09); only the generator is in the repo.
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

# policy gates (all run in CI on every push)
.\tools\check-module-graph.ps1     # dependencies point downward only
.\tools\check-hostable-core.ps1    # D9: no windows.h / d3d11.h above gfx/
.\tools\licence-check.ps1          # no GPL FFmpeg, no software HEVC/AAC encoder

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

### Crash reports (PR 7)

Native crashes are captured out-of-process by Crashpad into
`%LocalAppData%\MediaViewer\Crashes`. Nothing is uploaded. On the next launch the app
scrubs each dump: memory outside thread stacks is zeroed, and paths, media filenames and
your username are masked. Managed exceptions go to `Crashes\managed\`.
[plan/13](plan/13-updates-and-telemetry.md) has the details.

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

## Package and install (PR 8)

The v1 release is a **per-user** install under `%LocalAppData%\MediaViewer`, with **no
UAC** at any point. `Program Files` is not offered: a per-machine install needs elevation
for every update, which is how update mechanisms stop working
([plan/13](plan/13-updates-and-telemetry.md)). There is no Microsoft Store channel — the
app is GPL-2.0-or-later ([plan/11](plan/11-licensing.md)).

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
breaks plan/09's 250 MB cap, fails plan/11's licence gate, or contains the Windows App SDK
AI / ONNX / DirectML / WebView2 files plan/13 forbids shipping.

The first run downloads the pinned .NET runtime (31.7 MB) once and caches it in the build
directory; its SHA-256 is verified every time. For an offline build, pass the same archive
with `-DotnetRuntimeZip`.

**No prerequisites on the target machine.** The payload carries both runtimes — the
Windows App SDK and .NET — so a clean Windows 10 21H2 install runs it with nothing
installed first. That is not a nicety: the wizard is per-user and takes no UAC, and a
machine-wide runtime prerequisite needs admin. plan/09 made the same call for .NET — "a
viewer whose whole pitch is 'point it at a folder and it works' cannot open with a runtime
prerequisite dialog". The app is **208.6 MB**, inside plan/09's stated 200–250 MB band; a
first install occupies **292.7 MB** on disk, because Velopack also keeps one full package
so a bad update can be rolled back.

**An artefact from that command is unsigned and not publishable.** It says so on its last
line. Signing needs credentials the repo does not and must not hold:

```powershell
.\tools\package\build-release.ps1 -BuildDir build `
  -SigningMetadata trusted-signing.json `     # Azure Trusted Signing
  -ManifestKey C:\offline\release.key         # Ed25519, signs the update manifest
```

Without Authenticode every early user gets a SmartScreen block on first run. Without the
manifest key, clients reject every update — the pinned public key in
`src.managed/MediaViewer.Updater/UpdateKeys.cs` ships as an all-zero placeholder so an
unconfigured build **fails closed** rather than trusting an unverified channel.
`tools/package/update-signing.md` has the procedure and the one-off human steps.

### Releasing from CI

`.github/workflows/release.yml` runs the same script on every push to `main` and on every
`v*` tag, and publishes the result as a GitHub Release. The wizard and the in-app updater
both read `/releases/latest/download/`, so the newest push to `main` is what a new install
gets and what an existing install updates to.

| Trigger | Version | Publishes |
|---|---|---|
| push to `main` | `<major>.<minor>.<run_number>` | release, signed if the secrets exist |
| push tag `v<x.y.z>` | exactly that; must match `CMakeLists.txt` | release; **fails** unless signed |
| `workflow_dispatch` | `<major>.<minor>.<run_number>` | nothing — artefacts only, for a dry run |

Versions are strict `x.y.z` because that is all `ReleaseVersion` parses; a `-build.N`
suffix makes the updater go inert rather than fail loudly, so the run number is the patch
component instead. CI stamps it into `CMakeLists.txt` before configuring — nothing is
committed back — so `VERSIONINFO`, `MV_APP_VERSION`, the payload and the manifest agree.
**Bump `major.minor` in `CMakeLists.txt` before cutting a tag**, or `v0.1.0` sorts below
the `0.1.<run>` builds that preceded it.

Repository secrets, none needed for a dry run: `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`,
`AZURE_CLIENT_SECRET`, `TRUSTED_SIGNING_ENDPOINT`, `TRUSTED_SIGNING_ACCOUNT`,
`TRUSTED_SIGNING_PROFILE` for Authenticode, and `MV_MANIFEST_SIGNING_KEY` (Ed25519, hex)
for the update manifest. A `main` build without them warns and publishes anyway so the
pipeline is testable; a tagged release refuses. Until the real public key replaces the
placeholder in `UpdateKeys.cs`, every client rejects every update by design.

There is **one channel and it reaches everyone at once**: plan/13's staged rollout
(5 % → 25 % → 100 %) is not implemented, which is the trade "every push ships" makes. The
kill switch survives — the manifest still carries `min_version` and a blocklist
([plan/12](plan/12-decision-log.md), 2026-09-23).

### Installing and uninstalling

The wizard is six pages and no more: Welcome, Licence (GPL, scroll and accept), Location,
Options (Start Menu **on**, Desktop **off**), Progress, Finish (Launch, GitHub, Licence).
It does **not** ask to become your default photo viewer — that is a later update, prompted
in the app after you have actually opened a photo — and it does **not** ask about
telemetry, which is a first-run screen inside the app.

Uninstall is from Apps & features, and removes the shortcuts, the registry entry and the
whole install directory.

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

PR 1's verify line is:

> **Presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle.**

The earlier measurements do not establish this verify line: review found an assumed
60 Hz refresh rate, incomplete statistics coverage, and an idle-input bug. A 2026-09-07
re-run of the corrected instrument on the development box passed one 60 s animated soak
and dropped frames on another the same night; idle zero-presents was not established
while the window could receive mouse input. The harness must pass both soaks on the
intended GPU runner before PR 1 is considered verified. See
[plan/12-decision-log.md](plan/12-decision-log.md).

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
  caveat in [plan/12-decision-log.md](plan/12-decision-log.md) showing up in practice.
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

Folder listing + sort (name, mtime, size, type — EXIF date-taken waits for PR 9).
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

Slipped from PR 6, recorded in [plan/12-decision-log.md](plan/12-decision-log.md):
the folder-tree island to PR 9 (its key beeps; its command and canvas inset are
in), and hiding companion files to PR 7.

PR 7 (in progress) pairs files at scan time: a camera's `DSC_0001.JPG` +
`DSC_0001.NEF` (or HEIC + RAW) and an iPhone Live Photo (`IMG_0001.HEIC` or
`.JPG` + `IMG_0001.MOV`) are **one** filmstrip entry and one arrow-key stop,
badged RAW or LIVE. `;` plays a Live Photo's motion once and returns to the
still. Copy, move and delete act on both files of a pair. "Open RAW of pair" /
"Open JPEG of pair" have no default key; assign one in Settings (`Ctrl+,`).
`.xmp`, `.thm`, `.aae`, `.wav`, hidden and system files are never listed.

PR 7's decoders are in. Measured on five CC0 raw.pixls.us samples (CR2, NEF,
ARW, CR3, DNG): the embedded preview is on screen in 11–69 ms, about a JPEG's
first pixel, and the full LibRaw decode takes 0.8–1.7 s — slower than plan/09's
500 ms target, recorded as open in [plan/12](plan/12-decision-log.md)
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

Known holes on this slice, recorded in [plan/03-rendering.md](plan/03-rendering.md) and
[plan/04-image-pipeline.md](plan/04-image-pipeline.md): an idle renderer must be woken
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

- The wizard compiles, installs per-user with **no UAC**, and produces the layout plan/13
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

PR 8 creates no PR 15 file associations or handlers. The place they must be removed at
uninstall is marked in `tools/package/mediaviewer.iss`.

## Layout

```
src/core        job system, result<T>, lock-free rings, ETW
src/io          whole-file reads, directory listing + watcher, copy/move that never
                overwrites, Recycle Bin (Windows impl)
src/codec       JPEG / PNG / BMP / GIF / WebP / TIFF / ICO / HEIC / AVIF / RAW,
                APNG walker, frame-at-a-time animation sources, magic-byte probe,
                HEIC-only OS-codec probe (WIC, os_decode_win.cpp)
src/image       LCMS colour (8-bit display LUT; sRGB copy-through), CPU mips,
                immutable GPU upload, JPEG-512 thumbs
src/canvas      pan/zoom springs, fit / fill / 100 %, sticky zoom
src/gfx         D3D11 device, flip-model swapchain, frame pacer, blit
src/abi         the flat C ABI — the top of the native graph; animation session
src/shell       Win32 window, render thread, present lab, hostfxr island host,
                key router and live command table, marks, slideshow, file jobs,
                Settings persistence
src.managed/    C# interop and WinUI chrome (hosted as an island, not the app):
                command bar, filmstrip, gallery, `?` sheet, Settings,
                file drag/drop
tests/          Catch2 suites for core, gfx, codec, colour, camera, ABI, folder,
                key router, file ops, slideshow, animation
tools/          frametime harness, module-graph, hostable-core, and licence gates
plan/           the spec
```

Dependencies point downward only —
`shell → abi → {canvas, edit, player, image, meta} → {codec, gfx, io} → core`. No back-edges,
and nothing may depend on `shell`. That is what keeps the core testable with no window, and
`tools/check-module-graph.ps1` enforces it on every push.

## The plan

`plan/` is the spec, and the code follows it rather than the other way round. Start with
[plan/README.md](plan/README.md).

The parts worth knowing before touching anything:

- **[plan/10-roadmap.md](plan/10-roadmap.md)** — Windows release at PR 8, future updates in PRs 9–15, and Milestone F
  (Mac, PR 16–20), each with a verify line. Work is one slice; PR N+1 does not start until
  N's verify holds *and* PR 1's still does.
- **[plan/01-decisions.md](plan/01-decisions.md)** — D1–D9, the decisions that do not get
  reopened.
- **[plan/12-decision-log.md](plan/12-decision-log.md)** — why a call was reversed, so it
  does not get quietly re-reversed.
- **[plan/14-abi.md](plan/14-abi.md)** — the C ABI, specified rather than named.
- **[plan/15-platforms.md](plan/15-platforms.md)** — v1 is Windows; from PR 4 the core stays
  hostable; macOS is Milestone F (D9), not a SwiftUI-only port.

Rules that do not bend: nothing blocking touches the UI or render thread; the canvas is a
native swapchain C++ owns (D3D11 on Windows), never XAML; first pixel is never the full
decode; zero dropped frames panning a cached image, measured rather than eyeballed; never
modify an original; nothing about a user's files leaves the machine; never require a Store
codec pack.
