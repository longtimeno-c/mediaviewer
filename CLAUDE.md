# MediaViewer

A fast photo + video viewer for camera dumps. Windows and macOS (Linux in progress), one C++ core.

**Stack:** C++20 core behind a flat C ABI (`src/abi`) · chrome: C# WinUI 3 (`src.managed`),
SwiftUI (`src.swift`), Qt/QML planned for Linux · canvas: D3D11 / Metal / EGL, owned by C++ ·
FFmpeg for video · CMake + vcpkg.

The design changes often. Docs describe the current state, not rules — if a better approach
fits the task, take it and update the doc it touches.

## Layout

```
src/core src/io src/codec src/gfx   low-level: jobs, I/O, decoders, GPU backends
src/image src/player src/meta src/edit src/canvas   features
src/abi    C ABI the hosts call       src/shell  native hosts (Win32, *_mac.mm, *_linux.cpp)
src/addons add-ons (talk to the core only via mediaviewer_addon.h)
src.managed / src.swift / src.ios    chrome      tests/  tools/  docs/
```

Dependencies point downward (`shell → abi → features → codec/gfx/io → core`);
`tools/check-module-graph.ps1` enforces it.

## Build and test

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
dotnet test src.managed\MediaViewer.Chrome.Tests\MediaViewer.Chrome.Tests.csproj -c Release
```

macOS, Linux, iOS, packaging and the perf harnesses: search `docs/DEVELOPMENT.md` for the
section you need — it is large, don't read it whole. Use your own build dir if another
session may share the checkout.

## Standards

- **Tests:** every change comes with tests that would fail without it. Test behaviour, not
  implementation; no sleeps or flaky timing; keep them fast. Fix or delete a flaky test,
  don't retry it.
- **CI stays green** (`.github/workflows/ci.yml`). Run the relevant local checks before
  pushing; if a policy script in `tools/` fails, fix the cause.
- **Clean UI:** consistent spacing, type and colour with the rest of the chrome; every
  command reachable from the keyboard; check light and dark. Match both chromes when a
  feature exists on both.
- **Keep it simple:** smallest change that does the job, no speculative abstractions, no
  drive-by refactors.

## Hard technical constraints (these break things if ignored)

- Never block the UI or render thread (no decode, I/O or waiting on a worker's lock).
- Never modify a user's original file — write new files or sidecars.
- No user paths, filenames, pixels or EXIF in crash reports or telemetry.
- FFmpeg stays LGPL (no `--enable-gpl`/`nonfree`, no x264/x265); `tools/licence-check.ps1`.
- Keep Win32/D3D11 and Cocoa/Metal code in `src/shell` and the `gfx/` backends so the core
  builds on every OS (`tools/check-hostable-core.ps1`).
- In the WinUI island host, controls like `ProgressBar`/`TextBox` fail-fast; see
  `tools/check-winui-controls.ps1` for the list and the replacements.
- For speed-sensitive changes (open, navigate, pan, playback), measure before/after.

## Docs

- `docs/design/NN-*.md` — how each subsystem works; read only the one you are touching
  (index: `docs/design/README.md`). Update it if your change makes it wrong.
- `docs/plans/` — ideas not built yet.
- `README.md` is the product page: no status logs or dev notes there.
- `docs/design/12-decision-log.md` is history. Don't read it unless you need the reason for
  something; don't add to it for routine changes.

## PRs

Open a draft PR if the work is incomplete. Describe what changed and how it was tested.
