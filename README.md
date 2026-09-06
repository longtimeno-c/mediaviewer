# MediaViewer

A Windows viewer for a real camera dump — photos and video in one folder. Opens everything
instantly, pans without a dropped frame, shows and edits metadata, does the everyday photo
edits, and trims video without re-encoding.

**Status: PR 1 of 15.** There is a present lab, a native core, and a C ABI. There is not yet
a viewer — it cannot open an image. See [Where this actually is](#where-this-actually-is).

**Licence: GPL-2.0-or-later** ([LICENSE](LICENSE)). Settled in PR 1; the reasoning is in
[plan/11-licensing.md](plan/11-licensing.md).

---

## What is here today

| | |
|---|---|
| **`mediaviewer_lab.exe`** | A Win32 + DirectComposition window with a flip-model D3D11 swapchain, an animated sweep bar, and an F3 frame-time overlay reading real present-to-present intervals. This is the instrument PR 1 exists to build, and under the [D1 amendment](plan/12-decision-log.md) its window and swapchain **are** the app — PR 3 hosts WinUI chrome inside them rather than re-implementing presentation. |
| **`mediaviewer_core.dll`** | The native core behind a flat C ABI: job system with generation-based cancellation, `result<T>`, lock-free rings, ETW tracepoints. |
| **`frametime.exe`** | The frame-time regression harness. Runs a soak, writes a JSON report, compares against a rolling baseline, and fails on a dropped frame. |
| **`MediaViewer.Interop`** | The C# side of the ABI — `SafeHandle`, struct layouts, completion drain. No WinUI yet; PR 1 has none by design. |

## Build

You need **Visual Studio 2022** (or Build Tools) with the C++ workload, the **Windows 10/11
SDK**, **CMake ≥ 3.28**, **vcpkg**, and the **.NET 8 SDK** (for the interop assembly only —
the core builds and tests with no .NET present).

```powershell
# once
git clone https://github.com/microsoft/vcpkg $env:USERPROFILE\vcpkg
& $env:USERPROFILE\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = "$env:USERPROFILE\vcpkg"

# configure and build
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

CMake finds vcpkg from `VCPKG_ROOT`, or from `%USERPROFILE%\vcpkg`, or from an explicit
`-DCMAKE_TOOLCHAIN_FILE`. PR 1 pulls only `imgui` and `catch2`; the rest of the v1 dependency
set arrives with the PR that needs it, listed in [`vcpkg.json`](vcpkg.json).

Other configurations:

```powershell
cmake -S . -B build-asan -A x64 -DMV_ASAN=ON     # AddressSanitizer
cmake -S . -B build-clang -A x64 -T ClangCL      # clang-cl, the CI second opinion
```

## Run

```powershell
.\build\bin\Release\mediaviewer_lab.exe
```

| Key | |
|---|---|
| `F3` | frame-time overlay |
| `Space` | animation on/off — with it off the app stops presenting entirely (~0 % CPU) |
| `R` | reset the measurement window |
| `Esc` | quit |

Command line: `--soak <seconds>`, `--json <path>`, `--gate` (non-zero exit if the verify
line fails), `--no-overlay`, `--static`.

## Test

```powershell
# native unit tests
ctest --test-dir build -C Release --output-on-failure

# the ABI, end to end from C#: SafeHandle, struct layout, completion drain
dotnet build src.managed\MediaViewer.AbiSmokeTest\MediaViewer.AbiSmokeTest.csproj -c Release
dotnet src.managed\MediaViewer.AbiSmokeTest\bin\Release\net8.0-windows\MediaViewer.AbiSmokeTest.dll build\bin\Release

# policy gates (both run in CI on every push)
.\tools\check-module-graph.ps1     # dependencies point downward only
.\tools\licence-check.ps1          # no GPL FFmpeg, no software HEVC/AAC encoder

# the frame-time gate — 60 seconds, needs a quiet machine
.\build\bin\Release\frametime.exe --seconds 60
```

## Where this actually is

PR 1's verify line is:

> **Presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle.**

Measured on the development machine (60.00 Hz panel):

| Clause | Result |
|---|---|
| Presents at exactly display refresh | **Holds** — p50 = 16.700 ms against a 16.667 ms refresh interval, every run |
| ~0 % CPU idle | **Holds** — 0.008 % of the machine with the animation off, and presentation stops entirely |
| 0 dropped frames over 60 s | **Not yet demonstrated** — 4–17 per run here |

The third clause is unproven rather than failed, and the instrument says which: **the app's
own CPU frame time never exceeds 0.51 ms of a 16.67 ms budget**, and the drop count varies
by a factor of four between identical consecutive runs. That is a busy desktop, not a
systematic defect — and it is exactly what
[plan/09-build-and-test.md](plan/09-build-and-test.md) predicted when it required a
self-hosted runner with a real GPU, a pinned power profile, and nothing else scheduled on
it.

**No baseline is committed**, because one captured here would bake that noise in and quietly
lower the bar for every later PR. The CI job is wired up behind a `[self-hosted, windows,
gpu]` label and is skipped, not faked, when no such runner exists.

Every later PR inherits this verify line. That is the mechanism that stops smoothness
eroding one feature at a time.

## Layout

```
src/core        job system, result<T>, lock-free rings, ETW
src/gfx         D3D11 device, flip-model swapchain, frame pacer
src/abi         the flat C ABI — the top of the native graph
src/shell       Win32 window, render thread, present lab
src.managed/    C# interop (SafeHandle, completion pump)
tests/          Catch2 suites for core, gfx and the ABI
tools/          frametime harness, module-graph and licence gates
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

- **[plan/10-roadmap.md](plan/10-roadmap.md)** — 15 PR-sized slices, each with a verify line.
  Work is one slice; PR N+1 does not start until N's verify holds *and* PR 1's still does.
- **[plan/01-decisions.md](plan/01-decisions.md)** — D1–D8, the decisions that do not get
  reopened.
- **[plan/12-decision-log.md](plan/12-decision-log.md)** — why a call was reversed, so it
  does not get quietly re-reversed.
- **[plan/14-abi.md](plan/14-abi.md)** — the C ABI, specified rather than named.

Rules that do not bend: nothing blocking touches the UI or render thread; the canvas is a
D3D11 swapchain, never XAML; first pixel is never the full decode; zero dropped frames
panning a cached image, measured rather than eyeballed; never modify an original; nothing
about a user's files leaves the machine; never require a Store codec pack.
