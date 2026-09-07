# MediaViewer

A Windows viewer for a real camera dump — photos and video in one folder. Opens everything
instantly, pans without a dropped frame, shows and edits metadata, does the everyday photo
edits, and trims video without re-encoding.

**Status: PR 2 of 15.** The present lab opens JPEG, PNG and BMP, colour-manages them,
and pans/zooms on the same swapchain. PR 1's present-loop verify is inherited and not
yet demonstrated on a quiet GPU runner. See [Where this actually is](#where-this-actually-is).

**Licence: GPL-2.0-or-later** ([LICENSE](LICENSE)). Settled in PR 1; the reasoning is in
[plan/11-licensing.md](plan/11-licensing.md).

---

## What is here today

| | |
|---|---|
| **`mediaviewer_lab.exe`** | A Win32 + DirectComposition window with a flip-model D3D11 swapchain. Drop a JPEG/PNG/BMP, or pass a path on the command line. Wheel-zoom toward the cursor, drag-pan, `0` fits, `1` is 100 %. Decode and ICC convert run on the worker pool; pan never re-decodes. The F3 overlay still reads real present-to-present intervals. Under the [D1 amendment](plan/12-decision-log.md) this window and swapchain **are** the app — PR 3 hosts WinUI chrome inside them. |
| **`mediaviewer_core.dll`** | The native core behind a flat C ABI: job system, JPEG/PNG/BMP decode, LCMS colour, immutable GPU upload, pan/zoom camera. |
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
`-DCMAKE_TOOLCHAIN_FILE`. PR 2 pulls `imgui`, `catch2`, `libjpeg-turbo`, `libspng` and
`lcms`; the rest of the v1 set arrives with the PR that needs it, listed in
[`vcpkg.json`](vcpkg.json).

Other configurations:

```powershell
cmake -S . -B build-asan -A x64 -DMV_ASAN=ON     # AddressSanitizer
cmake -S . -B build-clang -A x64 -T ClangCL      # clang-cl, the CI second opinion
```

## Run

```powershell
.\build\bin\Release\mediaviewer_lab.exe
.\build\bin\Release\mediaviewer_lab.exe path\to\photo.jpg
```

| Key | |
|---|---|
| `F3` | frame-time overlay |
| `Space` | animation on/off — with it off (and the image settled) the app stops presenting entirely (~0 % CPU) |
| `R` | reset the measurement window |
| `0` | fit to window |
| `1` | 100 % |
| `Ctrl+O` | open JPEG/PNG/BMP |
| `Esc` | quit |

Wheel zooms toward the cursor; drag pans. Zoom-out stops at the opening fit
view and springs back to centre — it will not shrink the image into the
letterbox. Drop a file on the window.

Command line: `--soak <seconds>`, `--json <path>`, `--gate` (non-zero exit if the verify
line fails), `--no-overlay`, `--static`, `--open <path>`, or a positional path.

## Test

```powershell
# native unit tests and synthetic harness regression tests
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
  client area.
- Missing refresh information, statistics gaps, interrupted runs, device rebuilds, and
  failing child exit codes cannot pass. Short `--seconds` runs are diagnostic only.
- A saved baseline additionally gates p99 regressions greater than 10%. Use a separate
  baseline path for a different display mode. Old schema-1 baselines are not accepted.

To save the exact animated report after both soaks and the regression check pass:

```powershell
.\build\bin\Release\frametime.exe --seconds 60 --update-baseline
```

CI runs the gate on PRs and main. Provision an interactive Windows runner labelled
`self-hosted`, `windows`, `gpu`, with a visible desktop, fixed refresh, pinned power
profile, and no competing GPU work. Set repository variable `MV_GPU_RUNNER_ENABLED`
to `true` **only after provisioning it**. Leave it unset when no runner is available:
the gate then fails promptly on hosted Windows with a configuration message, instead
of queueing for a nonexistent runner or claiming a pass. An enabled runner that later
goes offline is still subject to GitHub's queue timeout. Fork PRs fail this check with
instructions to test a reviewed in-repository branch; they do not execute on the
persistent GPU runner.

Make **Frame-time gate (self-hosted GPU)** a required branch-protection check. Baseline
cache entries use unique keys and only successful main pushes publish a new baseline;
PR runs compare against the restored main baseline. The repository changes cannot
provision a runner or configure branch protection by themselves.

Every later PR inherits this verify line.

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

## Layout

```
src/core        job system, result<T>, lock-free rings, ETW
src/io          whole-file reads (worker threads only)
src/codec       JPEG / PNG / BMP, magic-byte probe
src/image       LCMS colour, CPU mips, immutable GPU upload
src/canvas      pan/zoom springs
src/gfx         D3D11 device, flip-model swapchain, frame pacer, blit
src/abi         the flat C ABI — the top of the native graph
src/shell       Win32 window, render thread, present lab
src.managed/    C# interop (SafeHandle, completion pump)
tests/          Catch2 suites for core, gfx, codec, colour, camera, ABI
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
