# MediaViewer

A Windows viewer for a real camera dump — photos and video in one folder. Opens everything
instantly, pans without a dropped frame, shows and edits metadata, does the everyday photo
edits, and trims video without re-encoding. **v1 is Windows.** From PR 4 the native core is
kept hostable; macOS is Milestone F (PR 16–20), a later host of the same core, not a UI-only
port — see [plan/15-platforms.md](plan/15-platforms.md).

**Status: PR 4 of 15 (in progress).** The present lab still owns the Win32 window
and D3D11 swapchain. WinUI 3 chrome is two XAML islands on that window: command
bar (top) and filmstrip (bottom). Open a folder of JPEG/PNG/BMP; the strip
virtualizes, thumbs come from a SQLite + JPEG-512 disk cache, arrow keys move
the selection. PR 1's present-loop verify and PR 3's island-on-screen verify
are inherited and not yet demonstrated on a quiet GPU runner. See
[Where this actually is](#where-this-actually-is). Keyboard-complete browse
(no mouse, `?` overlay, command palette) is specified for PR 6 in
[plan/16-commands.md](plan/16-commands.md); the keys below are the present lab.

**Licence: GPL-2.0-or-later** ([LICENSE](LICENSE)). Settled in PR 1; the reasoning is in
[plan/11-licensing.md](plan/11-licensing.md).

---

## What is here today

| | |
|---|---|
| **`mediaviewer_lab.exe`** | A Win32 + DirectComposition window with a flip-model D3D11 swapchain. Open a folder, drop a JPEG/PNG/BMP, or pass a path on the command line. Wheel-zoom toward the cursor, drag-pan, `0` fits, `1` is 100 %, `+`/`-` zoom, Left/Right browse. Decode and ICC convert run on the worker pool; pan never re-decodes. `F` / `F3` toggles the frame-time overlay. WinUI command bar (top) and filmstrip (bottom) are `DesktopWindowXamlSource` islands; the canvas is not a `SwapChainPanel`. |
| **`mediaviewer_core.dll`** | The native core behind a flat C ABI: job system, JPEG/PNG/BMP decode, LCMS colour, immutable GPU upload, pan/zoom camera, folder listing, thumbnail cache, ±2 prefetch LRU. |
| **`MediaViewer.Chrome.dll`** | C# WinUI 3 chrome, loaded by the lab through hostfxr. Open (image or folder), View (zoom in/out, fit, 50 / 100 / 200 / 400 %, overlay), About, `ItemsRepeater` filmstrip, load indicator. Flyouts are supposed to open over the canvas without clipping — that is part of PR 3's verify. |
| **`frametime.exe`** | The frame-time regression harness. Runs a soak, writes a JSON report, compares against a rolling baseline, and fails on a dropped frame. |
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
`-DCMAKE_TOOLCHAIN_FILE`. PR 4 pulls `imgui`, `catch2`, `libjpeg-turbo`, `libspng`,
`lcms` and `sqlite3`; the rest of the v1 set arrives with the PR that needs it, listed in
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
.\build\bin\Release\mediaviewer_lab.exe path\to\folder
```

| Key | |
|---|---|
| `F` / `F3` | frame-time overlay (off at launch with chrome) |
| `Space` | lab sweep on/off — overlay **animating**, not "a photo is open". Sweep off and settled → stop presenting (~0 % CPU) |
| `R` | reset the measurement window |
| `0` | fit to window |
| `1` | 100 % |
| `+` / `-` | zoom in / out |
| `Ctrl+O` | open an image (JPEG/PNG/BMP) |
| `Ctrl+Shift+O` | open a folder |
| `Left` / `Right` | previous / next in the folder |
| `G` | gallery: thumbnail grid of the folder. Click or `Enter` opens an item, `Esc` leaves |
| `T` | filmstrip show/hide, for the mode you are in (folder open or single image) |
| `Tab` | focus the command bar island |
| `Esc` | leave the gallery, else quit |

Wheel zooms toward the cursor; drag pans. Zoom-out floors at 50 % (Fit can
still go smaller on a huge image) and rubber-bands a little past that, then
springs back to centre. Drop a file on the window.

Command line: `--soak <seconds>`, `--json <path>`, `--gate` (non-zero exit if the verify
line fails), `--no-overlay`, `--static`, `--no-chrome`, `--open <path>`, or a positional
file or folder. The command bar is also on the island: Open (image or folder), View (zoom in/out,
fit, 50 / 100 / 200 / 400 %, gallery, filmstrip, overlay), Settings, About. The filmstrip along
the bottom and the gallery grid are both `ItemsRepeater` islands over the same listing; thumbs are JPEG files from `%LocalAppData%\MediaViewer\thumbs`. With no
folder open the canvas is a drop target, not the present-lab sweep.

Opening a single image lists its folder too, so `Left` / `Right` and the gallery work on the
files beside it. Whether the filmstrip comes with it is a preference: **Settings** has
*Filmstrip when opening a folder* (on by default) and *Filmstrip when opening an image* (off),
persisted to `%LocalAppData%\MediaViewer\settings.ini`. `T` toggles the one for the
mode you are in. Both take effect immediately — no restart.

## Test

```powershell
# native unit tests and synthetic harness regression tests
ctest --test-dir build -C Release --output-on-failure

# the ABI, end to end from C#: SafeHandle, struct layout, completion drain
dotnet build src.managed\MediaViewer.AbiSmokeTest\MediaViewer.AbiSmokeTest.csproj -c Release
dotnet src.managed\MediaViewer.AbiSmokeTest\bin\Release\net8.0-windows\MediaViewer.AbiSmokeTest.dll build\bin\Release

# WinUI chrome (also published beside mediaviewer_lab.exe by the CMake build)
dotnet publish src.managed\MediaViewer.Chrome\MediaViewer.Chrome.csproj -c Release -r win-x64 --no-self-contained

# policy gates (all run in CI on every push)
.\tools\check-module-graph.ps1     # dependencies point downward only
.\tools\check-hostable-core.ps1    # D9: no windows.h / d3d11.h above gfx/
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

Folder listing + sort (name, mtime, size, type — EXIF date-taken waits for PR 8).
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
`ctest` covers listing, cache hits, and ABI folder/thumb jobs.
`tools/check-hostable-core.ps1` is the D9 gate. PR 1's present-loop verify is
inherited.

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
src/io          whole-file reads, directory listing + watcher (Windows impl)
src/codec       JPEG / PNG / BMP, magic-byte probe
src/image       LCMS colour, CPU mips, immutable GPU upload, JPEG-512 thumbs
src/canvas      pan/zoom springs
src/gfx         D3D11 device, flip-model swapchain, frame pacer, blit
src/abi         the flat C ABI — the top of the native graph
src/shell       Win32 window, render thread, present lab, hostfxr island host
src.managed/    C# interop and WinUI chrome (hosted as an island, not the app)
tests/          Catch2 suites for core, gfx, codec, colour, camera, ABI, folder
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

- **[plan/10-roadmap.md](plan/10-roadmap.md)** — 15 Windows PR-sized slices, then Milestone F
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
