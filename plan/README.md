# MediaViewer — Plan Index

A Windows viewer for a real camera dump — photos and video in one folder — that opens everything
instantly and pans without a dropped frame. The first release ships the viewer through
PR 7, packaged in PR 8; metadata tools, photo edits/export, video trimming, and additional
Windows integration follow in future updates. **v1 is Windows.** From PR 4 the native core is kept
hostable; macOS is a second host of the same core (built as Milestone F, now the Mac halves of PRs 1–8), not a UI-only port
([15-platforms.md](15-platforms.md), **D9**). **From PR 9, every PR lands on Windows and
macOS together** (D9 amended 2026-09-24).

**Stack:** native Win32 shell owning a Direct3D 11 canvas · WinUI 3 chrome hosted as XAML islands (C#) ·
C++20 core behind a flat C ABI · FFmpeg +
D3D11VA (video) · libjpeg-turbo / libspng / libwebp / libheif / LibRaw (images) · Exiv2 (metadata) ·
SQLite (thumbnails) · CMake + vcpkg.

**v1 is the PR 1–7 viewer, on the camera-dump format set, packaged in PR 8.**
Light editing and trim follow in future updates. Not a develop module, not an
NLE. See D4/D5 in [01-decisions.md](01-decisions.md).

Read in order:

| Doc | What it decides |
|---|---|
| [01-decisions.md](01-decisions.md) | Stack choice, why not Electron/npm, and the pros/cons of the contested decisions |
| [02-architecture.md](02-architecture.md) | Module layout, threading model, data flow |
| [03-rendering.md](03-rendering.md) | D3D11 flip model, frame pacing, colour, the "butter" part |
| [04-image-pipeline.md](04-image-pipeline.md) | Decoders per format, tiling, caching, prefetch |
| [05-video-pipeline.md](05-video-pipeline.md) | FFmpeg + D3D11VA, the A/V clock, seeking |
| [06-metadata.md](06-metadata.md) | EXIF/IPTC/XMP/container read, and safe writing |
| [07-photo-editing.md](07-photo-editing.md) | Non-destructive GPU edit stack, first editing update vs later ops |
| [08-video-editing.md](08-video-editing.md) | Post-v1 two-path trim, smart cut later |
| [09-build-and-test.md](09-build-and-test.md) | CMake/vcpkg, perf regression harness, fuzzing |
| [10-roadmap.md](10-roadmap.md) | One PR number per feature on both platforms: PRs 1–8 (Windows v1 + their Mac halves), 9–15 dual-track updates, 16–19 Import add-on, 20–24 AI search — each with a verify line |
| [11-licensing.md](11-licensing.md) | FFmpeg LGPL, codec patents, the Exiv2 GPL trap — settle in PR 1 |
| [12-decision-log.md](12-decision-log.md) | What changed, when, and why |
| [13-updates-and-telemetry.md](13-updates-and-telemetry.md) | First-install wizard, Velopack updates, crash reporting, the privacy line |
| [14-abi.md](14-abi.md) | The C ABI between the C# shell and the C++ core — specified, not just named |
| [15-platforms.md](15-platforms.md) | Windows v1, hostable core from PR 4, macOS as Milestone F, dual-track from PR 9 — **D9**. Not a UI-only port. |
| [16-commands.md](16-commands.md) | Keyboard-complete v1, command table, mouse-free verify. Remap UI is v1.1. |
| [17-local-ai-search.md](17-local-ai-search.md) | Post-v1, proposed: local, opt-in AI search over photos and video keyframes, plus faces. A Settings-installed extra, never in the base installer. |
| [18-import.md](18-import.md) | Import add-on (PRs 16–19): card/folder copy with content-hash duplicate skip, verify, date folders, backup, resume. Also how add-ons install. |

## The rules that don't bend

1. **Nothing that can block touches the UI or render thread** — no decode, no I/O, no encode.
2. **The canvas is a native swapchain C++ owns**, never `SwapChainPanel`, never a XAML `Image` or
   `MediaPlayerElement`. On Windows that swapchain is D3D11. One present path per OS; Metal is
   the macOS sequel (Milestone F), not a second Windows path ([15-platforms.md](15-platforms.md)).
   Chrome is hosted inside the native window.
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
- **A quiet machine for the D6 gate.** PR 1's corrected animated and idle instruments
  require a passing 60-second run each. A 2026-09-07 re-run on the development box
  passed one animated soak and dropped frames on another; idle zero-presents was not
  established with the window under a cursor. The gate is inherited, not waived.
  See [12](12-decision-log.md) and the root README.

Everything else is decided; [12-decision-log.md](12-decision-log.md) records why.
