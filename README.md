# MediaViewer

**Open a camera dump the moment you double-click it. Pan a 42-megapixel RAW without dropping a frame.**

MediaViewer is a fast, keyboard-first viewer for the folder that comes off your camera: photos, RAW and
video side by side in one window. It draws on its own Direct3D 11 surface instead of a stock image or
media-player control, so decoding never stands between you and the screen.

![MediaViewer showing a RAW photo with the filmstrip underneath](docs/img/viewer.png)

---

## Speed you can measure

Every number below comes from the app's own harnesses, and the raw reports are in [`docs/perf/`](docs/perf/).
Measured on a Ryzen 7 5700X3D, RTX 4070 and a 59.95 Hz display. One desktop, not a lab matrix.

| | |
|---|---|
| **0 dropped frames** | 60 s of animated panning on a 42 MP Sony RAW: 3,597 frames, p99 16.95 ms on a 16.68 ms refresh |
| **44-125 ms** | Time for a camera RAW's preview to appear; the full-resolution decode then cross-fades in without moving your view |
| **0.08 % CPU, 0 presents** | Cost of a still image sitting on screen. Idle means idle |
| **0.2 ms, then the next refresh** | Arrow to the next photo once it has been decoded ahead. The frame is ready in 0.2 ms; a 60 Hz monitor shows it within 16.7 ms |
| **51–100 ms** | Jump to a RAW that was not next to the current one: its embedded preview, about the same as opening that file |
| **Under one frame** | Audio/video sync error (p99 16 ms) over a 120 s clip |

![Frame pacing over two 60 second soaks](docs/img/perf-pacing.svg)

![Time to first pixel for five camera RAW formats and HEIC](docs/img/perf-first-pixel.svg)

![Time to move from one photo to the next](docs/img/perf-browse.svg)

![Audio/video error over 120 seconds of playback](docs/img/perf-av-sync.svg)

Honest limits: the full RAW decode takes 1.0-1.9 s (it happens behind the preview); one of six short RAW-open
runs dropped a frame during the preview-to-full swap; and a 120 s clip run logged some dropped video
presents, so smooth 30 fps playback is not yet claimed. The folder chart is stills only (five RAWs and
one HEIC); stepping onto a video was not timed. Charts are drawn by
[`tools/perf/make-charts.py`](tools/perf/make-charts.py) from the reports in [`docs/perf/`](docs/perf/).

---

## Built for browsing a real dump

- **Everything in one folder.** JPEG, PNG, BMP, GIF, TIFF, WebP, HEIC, AVIF, ICO, camera RAW (CR2, CR3, NEF, ARW, DNG)
  and MP4, MOV, MKV, WebM, AVI, TS video open in the same window on the same canvas.
- **RAW+JPEG and Live Photo pairs are one entry**, one arrow-key stop, badged RAW or LIVE. Copy, move and delete act on both files.
- **Filmstrip and gallery.** A virtualised filmstrip on a persistent thumbnail cache; `G` opens a full grid.
- **Nested folders as tiles.** Child folders show with covers and counts, a path bar stays on screen, and
  `Ctrl+Up` goes up and returns to the folder you left. `Ctrl+Left` / `Ctrl+Right` hop to the neighbouring folder.
- **Folder tree** (`Ctrl+Shift+E`) and sort by name, date modified, size, type or EXIF date taken.
- **Fit, fill, 100 %, or any zoom.** Wheel zoom toward the cursor with springy, physical-feeling pan and zoom.
- **Animated GIF, APNG and WebP** play on the same frame clock.

| Gallery | Frame-time overlay (`F3`) |
|---|---|
| ![Gallery of RAW, HEIC and video thumbnails](docs/img/gallery.png) | ![Live frame-time overlay over a Nikon RAW](docs/img/frametime-overlay.png) |

## Keyboard-complete

Every action has a key, and `?` shows the full list, generated from the same command table the app runs.
Arrow keys or `A`/`D` browse, `Space` advances, `Insert` marks, `F7`/`F8` copy or move marked files to a folder,
`Delete` sends to the Recycle Bin, `F11` goes fullscreen, `F5` starts a slideshow. Every key can be remapped in Settings (`Ctrl+,`).

## Video that lives with your photos

FFmpeg decode with D3D11 hardware acceleration on the same swapchain as photos. H.264, HEVC, VP9, AV1 and MPEG-2,
including 10-bit HDR clips mapped to SDR. Audio is the master clock. Seek, frame step (`,` `.`), speed
0.25x-4x, A-B loop, resume where you left off, and media keys. `;` plays a Live Photo's motion and returns to the still.

## Know your shot

`I` opens a metadata pane: a summary card, a searchable tree of every EXIF, IPTC and XMP tag, and a per-stream inspector
for clips. `O` overlays camera, exposure and date on the image, `Shift+O` draws autofocus points, and `Shift+I` is a
one-pixel colour eyedropper (`Ctrl+C` copies the value).

## Colour done right

Images are converted from their embedded ICC profile through a linear working space to sRGB. A tagged image is
never assumed to be sRGB, and camera JPEGs are never tone-mapped. HDR video is mapped to SDR.

## Trustworthy by design

- **Your originals are never modified.** Browsing is read-only; edits write new files or an XMP sidecar.
- **Nothing about your files leaves the machine.** Telemetry is opt-in and off by default. Crash reports are captured
  locally, scrubbed of paths, filenames and your username, and never sent without asking.
- **No codec packs.** Decoders are bundled; you never need the Store HEVC extension.
- **Installs per user**, with an installer and a background updater (not yet signed), and never silently takes over your file associations.
- **Hardened decoding.** A corpus of broken files and per-decoder fuzzers run in CI.

---

## Coming next

| Soon | |
|---|---|
| **Lossless rotate, crop and export** | Rotate and flip JPEGs without re-encoding, straighten and crop, export with metadata carried over |
| **Colour adjustments** | Exposure, contrast and white balance, non-destructive |
| **Metadata editing** | Rating, orientation and comments written safely, RAW ratings kept in sidecars |
| **Video trim** | Cut clips losslessly at keyframes, or re-encode with hardware encoders; extract a frame or the audio |
| **Windows integration** | Explorer thumbnails and properties for HEIC and RAW, "Open with" and Default Apps |
| **Import** (optional add-on) | Copy cards with duplicate detection, verification, date-based folders, backups and resume |
| **Local AI search** (optional add-on) | Find "dog on a beach" across your dump, entirely on your machine |
| **Voice search** (optional add-on) | Speak the query; speech runs on-device |
| **macOS** | The same core with a native Metal and SwiftUI app; it already builds, browses and plays video, and is being brought to parity |

## Get it

MediaViewer for Windows is in pre-release. There is no signed public build yet. To try it, build from source
(below). Windows 10/11, x64.

```powershell
git clone https://github.com/microsoft/vcpkg $env:USERPROFILE\vcpkg
& $env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\bin\Release\mediaviewer_lab.exe C:\path\to\photos
```

You need Visual Studio 2022 with the C++ workload, CMake 3.28+, vcpkg and the .NET 8 SDK. The first configure builds
FFmpeg and takes a while. Full build, test, packaging and macOS instructions are in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). Reproduce the measurements with
`.\build\bin\Release\frametime.exe --seconds 60` and
`.\build\bin\Release\mediaviewer_lab.exe --browse-soak --json docs\perf\browse.json path\to\folder`,
then `python tools/perf/make-charts.py`.

## Licence

GPL-2.0-or-later, see [LICENSE](LICENSE). Bundled libraries and their licences are listed in
[THIRD-PARTY.md](THIRD-PARTY.md). Screenshots use CC0 sample files from [raw.pixls.us](https://raw.pixls.us) and libheif.
