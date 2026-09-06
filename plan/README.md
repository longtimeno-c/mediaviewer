# MediaViewer — Plan Index

A Windows viewer for a real camera dump — photos and video in one folder — that opens everything
instantly, pans without a dropped frame, shows and edits metadata, does the everyday photo edits,
and trims video without re-encoding.

**Stack:** native Win32 shell owning a Direct3D 11 canvas · WinUI 3 chrome hosted as XAML islands (C#) ·
C++20 core behind a flat C ABI · FFmpeg +
D3D11VA (video) · libjpeg-turbo / libspng / libwebp / libheif / LibRaw (images) · Exiv2 (metadata) ·
SQLite (thumbnails) · CMake + vcpkg.

**v1 is a viewer with light edits, on the camera-dump format set.** Not a develop module, not an
NLE. See D4/D5 in [01-decisions.md](01-decisions.md).

Read in order:

| Doc | What it decides |
|---|---|
| [01-decisions.md](01-decisions.md) | Stack choice, why not Electron/npm, and the pros/cons of all eight contested decisions |
| [02-architecture.md](02-architecture.md) | Module layout, threading model, data flow |
| [03-rendering.md](03-rendering.md) | D3D11 flip model, frame pacing, colour, the "butter" part |
| [04-image-pipeline.md](04-image-pipeline.md) | Decoders per format, tiling, caching, prefetch |
| [05-video-pipeline.md](05-video-pipeline.md) | FFmpeg + D3D11VA, the A/V clock, seeking |
| [06-metadata.md](06-metadata.md) | EXIF/IPTC/XMP/container read, and safe writing |
| [07-photo-editing.md](07-photo-editing.md) | Non-destructive GPU edit stack, v1 vs v1.1 ops |
| [08-video-editing.md](08-video-editing.md) | Two-path trim now, smart cut later |
| [09-build-and-test.md](09-build-and-test.md) | CMake/vcpkg, perf regression harness, fuzzing |
| [10-roadmap.md](10-roadmap.md) | 15 PR-sized slices, each with a verify line |
| [11-licensing.md](11-licensing.md) | FFmpeg LGPL, codec patents, the Exiv2 GPL trap — settle in PR 1 |
| [12-decision-log.md](12-decision-log.md) | What changed, when, and why |
| [13-updates-and-telemetry.md](13-updates-and-telemetry.md) | Auto-update channel, crash reporting, the privacy line |
| [14-abi.md](14-abi.md) | The C ABI between the C# shell and the C++ core — specified, not just named |

## The rules that don't bend

1. **Nothing that can block touches the UI or render thread** — no decode, no I/O, no encode.
2. **The canvas is a native D3D11 swapchain**, never `SwapChainPanel`, never a XAML `Image` or
   `MediaPlayerElement`. C++ owns presentation; chrome is hosted inside it.
3. **First pixel is never the full decode**: memory cache → disk thumb → embedded RAW preview →
   downscaled decode. Full resolution is a refinement.
4. **Zero dropped frames while panning a cached image at display refresh** — measured in CI, every
   PR ([09](09-build-and-test.md)).
5. **Never modify an original.** RAW gets an XMP sidecar; edits and trims write new files.
6. **Nothing about a user's files leaves the machine** — no paths, filenames, pixels, or EXIF, in
   crash reports or telemetry ([13](13-updates-and-telemetry.md)).
7. **Never require a Store codec pack.** An install prompt in front of a folder of holiday photos
   is a failed app.

## Open decisions

- ~~**Do you need the Microsoft Store?**~~ **Settled 2026-09-06: no.** The app is
  **GPL-2.0-or-later**, Exiv2 is kept under the GPL, and distribution is direct download only
  ([11-licensing.md](11-licensing.md), [12](12-decision-log.md)).
- **Whether WinUI 3 XAML islands hold up** — validated in PR 3, before any panes are built on them.
  Fallback: a WinUI app with `SwapChainPanel` and an accepted composed frame.
- **A quiet machine for the D6 gate.** PR 1's "0 dropped frames over 60 s" is measured but not yet
  demonstrated: the development box drops 4-17 frames per run while the app's own frame never
  exceeds 0.51 ms of a 16.67 ms budget. Needs the self-hosted GPU runner
  [09](09-build-and-test.md) already specifies. Blocks starting PR 2.

Everything else is decided; [12-decision-log.md](12-decision-log.md) records why.
