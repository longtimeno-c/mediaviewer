# MediaViewer

**Open a camera dump the moment you double-click it. Pan a 42-megapixel RAW without dropping a frame.**

MediaViewer is a fast, keyboard-first viewer for the folder that comes off your camera: photos, RAW and
video side by side in one window. It draws on its own Direct3D 11 surface instead of a stock image or
media-player control, so decoding never stands between you and the screen.

![MediaViewer showing a RAW photo with the filmstrip underneath](docs/img/viewer.png)

## Download

**[MediaViewer 0.1.2](https://github.com/longtimeno-c/mediaviewer/releases/tag/v0.1.2)** is the current public release.

| | |
|---|---|
| **Windows 10/11, x64** | [MediaViewer-0.1.2-Setup.exe](https://github.com/longtimeno-c/mediaviewer/releases/download/v0.1.2/MediaViewer-0.1.2-Setup.exe) |
| **Mac, Apple Silicon, macOS 14+** | [MediaViewer-0.1.2.dmg](https://github.com/longtimeno-c/mediaviewer/releases/download/v0.1.2/MediaViewer-0.1.2.dmg) |

Windows may show a SmartScreen warning the first time. This installer is not Authenticode-signed. Checksums are on the [release page](https://github.com/longtimeno-c/mediaviewer/releases/tag/v0.1.2).

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
| **0 dropped video frames** | 60 s of 1080p HEVC on screen: 3,597 presents, p99 17.10 ms. Audio/video error over that minute stays at p99 7.2 ms |

### Stills

**Panning.** Two 60 s soaks, including a 42 MP Sony RAW. The line at 16.68 ms is one refresh of this display.

![Frame pacing over two 60 second soaks](docs/img/perf-pacing.svg)

**First pixel.** Embedded preview first. The grey bar is the full decode that replaces it.

![Time to first pixel for five camera RAW formats and HEIC](docs/img/perf-first-pixel.svg)

**Next photo.** Arrow after the neighbours have been decoded, and a jump to a photo that has not.

![Time to move from one photo to the next](docs/img/perf-browse.svg)

### Video

**On screen.** 60 s of 1080p HEVC. Frame time sits on the refresh. No present was dropped, and no video frame was skipped.

![Video frame pacing over 60 seconds of playback](docs/img/perf-video.svg)

**Audio against video.** 120 s of the same kind of clip. The error stays under one frame of a 30 fps video (33.3 ms). The selector discarded 2 of 3,597 frames; that is not a dropped screen present.

![Audio/video error over 120 seconds of playback](docs/img/perf-av-sync.svg)

What is still slower than it should be: a full RAW decode takes 1.0–1.9 s behind the preview, and one timed Canon CR3 open dropped a frame while that preview cross-faded to the full image. A 4K HEVC test clip played at about 24 fps on screen, with a worst gap of 367 ms. Playback has been measured for one to two minutes, not yet a half hour. The folder chart is stills only.

### Against Windows Photos and Media Player

Same machine, same sample files, timed from the screen (a grab is about 17 ms). These are cold launches: the clock starts when the app is opened, so they are not the 44–125 ms in-app preview times above.

**Opening.** Two launches each. RAW is a tie. Photos is faster on the HEIC (810 ms vs 964 ms).

![Launch to a settled picture, MediaViewer and Windows Photos](docs/img/compare-open.svg)

**Panning.** With the mouse, MediaViewer updates the picture about every refresh (p50 16.7 ms). The slowest updates were 49–84 ms. The same wheel-and-drag did not move the picture in Photos, so Photos has no pan number here.

![Mouse-drag pan, MediaViewer and Windows Photos](docs/img/compare-pan.svg)

**Video.** On a 30 fps test clip both apps ran at 29 fps. Media Player's slow gaps were shorter (p99 44 ms, MediaViewer 66 ms). On a 4K HEVC clip MediaViewer averaged 24 fps; Media Player stayed on a single frame for the whole sample, so it has no playback number for that file.

![Video playback gaps, MediaViewer and Media Player](docs/img/compare-video.svg)

Charts are drawn by [`tools/perf/make-charts.py`](tools/perf/make-charts.py) from the reports in [`docs/perf/`](docs/perf/).

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
- **Installs per user**, with a public installer and a background updater. The 0.1.2 installer is not Authenticode-signed, so SmartScreen may warn once. It never takes over your file associations.
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

## From source

Building needs Visual Studio 2022, CMake 3.28+, vcpkg and the .NET 8 SDK. The first configure builds FFmpeg and takes a while. Commands, tests and the macOS recipe are in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

```powershell
git clone https://github.com/longtimeno-c/mediaviewer
cd mediaviewer
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\bin\Release\mediaviewer_lab.exe C:\path\to\photos
```

Reproduce the charts with `.\build\bin\Release\frametime.exe --seconds 60`, then `python tools/perf/make-charts.py`. The folder chart is `mediaviewer_lab.exe --browse-soak --json docs\perf\browse.json path\to\folder`.

## Licence

GPL-2.0-or-later, see [LICENSE](LICENSE). Bundled libraries and their licences are listed in
[THIRD-PARTY.md](THIRD-PARTY.md). Screenshots use CC0 sample files from [raw.pixls.us](https://raw.pixls.us) and libheif.
