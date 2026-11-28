# MediaViewer

A fast, keyboard-first photo and video viewer for camera dumps: stills, camera RAW, HEIC, video,
audio, PDF and Office documents in one window, with on-device search, editing and add-ons.

## Platforms

| Platform | State | Where |
|---|---|---|
| Windows | Shipping | Win32 host + C# WinUI 3 chrome (`src.managed`), Direct3D 11 |
| macOS | Shipping | AppKit host + SwiftUI chrome (`src.swift`), Metal |
| iPhone / iPad | TestFlight | `src.ios`: searches the library on the phone, streams from a home server |
| Linux | Early | The present lab (Wayland, EGL/OpenGL) and the `driftwire` CLI; Qt/QML chrome planned |

## Stack

- C++20 core behind a flat C ABI (`src/abi`); the hosts and chromes call only the ABI.
- The canvas (D3D11 / Metal / EGL) is owned by C++; the chrome draws around it.
- FFmpeg (LGPL build) for video, LibRaw and libheif for stills, Exiv2 for metadata, ONNX Runtime
  for the AI add-ons, SQLite for the thumbnail cache and search index.
- CMake + vcpkg; Catch2 tests; Swift packages and an Xcode project for the Apple chromes.

## Layout

```
src/core src/io src/codec src/gfx   low-level: jobs, I/O, decoders, GPU backends (D3D11/Metal/GL)
src/image src/player src/meta src/edit src/canvas   features
src/addon  add-on host      src/infer  ONNX Runtime wrapper      src/geo  offline place names
src/wire src/catalogue src/mobile src/remote   phone protocol, catalogue, phone core, remote folders
src/nle    Final Cut Pro search        src/shellext  Explorer thumbnails / Quick Look
src/abi    C ABI the hosts call        src/shell  native hosts (Win32, *_mac.mm, *_linux.cpp)
src/addons add-ons: ai driftwire enhance import phone places vpn (core only via mediaviewer_addon.h)
src.managed / src.swift   desktop chrome      src.ios  iPhone/iPad app (MobilePackage, App, UITests)
tests/  tools/  docs/  examples/  packaging/  third_party/
```

Dependencies point downward: `shell → abi → features → codec/gfx/io → core`. The allowed
includes per module are a map in `tools/check-module-graph.ps1`.

## How it works, briefly

- **Threads.** The UI and render threads do no decode or I/O; work runs on the job system and
  results come back as completions. Cancellation is by generation.
- **Stills.** An embedded preview goes on screen first, then the full decode cross-fades in;
  neighbours are decoded ahead into a byte-budgeted GPU cache.
- **Video.** FFmpeg demux, hardware decode (D3D11VA / VideoToolbox) into a frame ring, an audio
  clock, presented on the same surface as stills.
- **Files.** Rotating a JPEG and writing its metadata rewrite the file in place, losslessly and
  through an atomic replace; RAW and other formats get XMP sidecars; edits and exports write new
  files.
- **Add-ons.** Signed packages loaded by the host in `src/addon`; native ones (Local search,
  Import, Driftwire, Enhance, Phone access, Places, VPN) and open ones (themes, keymaps, settings
  pages, sandboxed Lua scripts with no network access).
- **Platform code.** Win32/D3D11 and Cocoa/Metal code lives in `src/shell` and the `gfx/` backends;
  the rest of the core builds on every OS.
- **Privacy.** Crash reports and telemetry carry no paths, filenames, pixels or EXIF.
- **WinUI island.** Some stock controls (`ProgressBar`, `TextBox`, …) fail-fast inside the island
  host; the chrome uses its own replacements, listed in `tools/check-winui-controls.ps1`.

## Build and test

Windows:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
dotnet test src.managed\MediaViewer.Chrome.Tests\MediaViewer.Chrome.Tests.csproj -c Release
```

macOS, Linux, iOS, packaging and the performance harnesses are in `docs/DEVELOPMENT.md`
(2,500 lines; search it for the section). The iPhone app builds from
`src.ios/MediaViewerMobile.xcodeproj`.

## CI

`.github/workflows/ci.yml`: Windows (MSVC Release and Debug, clang-cl, ASan), macOS, a portable
Linux core under ASan/UBSan, the Linux present lab, the iPhone engines (`ios-core`; the app in
`src.ios` is not built or tested in CI), both chromes, fuzzers, and frame-time gates on
self-hosted GPU runners. The `policy` job runs the scripts in `tools/`:

| Script | Checks |
|---|---|
| `check-module-graph.ps1` | Module include directions |
| `check-hostable-core.ps1` | No platform headers outside the platform ports |
| `check-winui-controls.ps1` | No fail-fast WinUI controls in the chrome |
| `check-vcpkg-baseline.ps1` | One vcpkg baseline everywhere |
| `licence-check.ps1` | FFmpeg stays LGPL (no GPL/nonfree, no x264/x265) |
| `telemetry-schema-check.ps1` | No free-text telemetry fields |

## Performance

The README's numbers come from the app's own harnesses; raw reports are in `docs/perf/`, and
`python tools/perf/regenerate.py` re-measures and redraws every chart.

## Docs

- `README.md` — the product page.
- `docs/DEVELOPMENT.md` — build, run and test recipes for every platform.
- `docs/design/NN-*.md` — how each subsystem works today (index: `docs/design/README.md`).
  `12-decision-log.md` is the history of why.
- `docs/plans/` — forward plans; many are partly built.
- `docs/ADDONS.md` — making an add-on.
- `docs/review/2026-11-audit/` — the November 2026 code audit: issues, improvements, work packages.
