# Build, Test, Ship

How MediaViewer is built, which test suites and gates exist and what they measure, how the
Windows shell integration works, and how the app is distributed. Commands live in
[DEVELOPMENT.md](../DEVELOPMENT.md); this doc describes the system behind them.

## Toolchain

- **CMake ≥ 3.28** + **vcpkg manifest mode**. [`vcpkg.json`](../../vcpkg.json) pins a
  `builtin-baseline`; the baseline is the pin and `overrides` is empty. Moving it is a
  deliberate commit followed by a frame-time re-run, and moves all five copies (the three
  manifests and `VCPKG_COMMIT` in `ci.yml` / `release.yml`; `tools/check-vcpkg-baseline.ps1`).
- **Windows:** MSVC 2022 primary, clang-cl as the second compiler in CI, an ASan configuration
  (`-DMV_ASAN=ON`, which copies the ASan runtime DLL beside every executable).
  `/W4 /WX /permissive- /Zc:__cplusplus /GR- /utf-8`.
- **macOS:** full Xcode, Swift 6, macOS 14+, `arm64-osx` / `x64-osx` triplets; the release is
  one universal app made by joining the two builds with `tools/mac/lipo_merge.py`.
- **.NET 8 / C# WinUI 3** chrome (`src.managed/`) loaded by the native host through hostfxr.
  The native core builds and tests standalone with no .NET present; without `dotnet` the
  Windows lab runs `--no-chrome`.
- **Portable core:** `cmake/portable` builds the Import engine and the clip suites headless on
  Linux / any POSIX machine from `tools/portable/vcpkg.json`.
- **Linux host (Linux PR 1):** the root `CMakeLists.txt` includes `cmake/linux.cmake` on Linux and
  builds the OpenGL present lab, `frametime` and the portable core tests. GCC 13 (the one compiler
  measured; clang with libstdc++ trips on `core/json.h`), `-Wall -Wextra -Werror -fno-rtti`, an
  ASan + UBSan configuration (`-DMV_ASAN=ON`). System libraries through pkg-config and
  `FindOpenGL`: `wayland-client` ≥ 1.20, `wayland-egl`, `xkbcommon`, glvnd's `libEGL` and
  `libOpenGL`, and `wayland-scanner`, which generates the protocol code at build time from the XML
  vendored in `tools/linux/protocols/` (wayland-protocols 1.45). With vcpkg the root manifest
  installs only `catch2` and `imgui[opengl3-binding]` on Linux; without it, `MV_IMGUI_SOURCE_DIR`
  names an imgui 1.92.8 tree and Catch2 comes from `CMAKE_PREFIX_PATH`.

Linkage: on Windows the vcpkg triplet is `x64-windows`, so dependencies ship as DLLs beside
the exe (Crashpad's client and imgui are static). On macOS permissive libraries link
statically and the LGPL codecs come from a separate manifest
(`tools/mac/dependencies/vcpkg.json`, `arm64-osx-dynamic`) and install root, so they cannot
leak into the static tree. That manifest has its own `vcpkg-configuration.json` naming the
repo's overlay ports (`tools/vcpkg/ports`), so the Mac's LibRaw is the same patched port as
Windows' (#287). Either way **FFmpeg, libheif, libde265, LibRaw and Exiv2 are always
DLLs / dylibs** ([licensing](11-licensing.md)). Shader bytecode is embedded at build time: on Windows `fxc` compiles `src/gfx/*.hlsl` into
`const BYTE` headers (CMake `mv_fxc`), so no `D3DCompile` runs at device create; the Mac's MSL is
still compiled from source at startup.

### Dependency set

| Library | Platforms | Used for |
|---|---|---|
| imgui (`dx11-binding` / `metal-binding` / `opengl3-binding`) | Win / Mac / Linux | Present lab and F3 overlay only. No `win32-binding` (or any platform backend): the overlay is fed from the published `input_snapshot`, never from the window procedure |
| catch2 | all three | Native tests |
| libjpeg-turbo, libspng, lcms, sqlite3 | both | JPEG, PNG, colour, thumbnail cache |
| giflib, libwebp, tiff | both | GIF, WebP, TIFF/ICO |
| libavif[dav1d] | both | AVIF |
| libheif (default features off) + libde265 | Win (Mac: dynamic manifest) | HEIC/HEIF decode. The port's `hevc` feature is the x265 encoder and is not enabled |
| libraw[openmp] | Win (Mac: dynamic manifest) | Camera RAW |
| ffmpeg (`avcodec avformat avfilter swresample swscale dav1d nvcodec qsv amf webp`) | Win (Mac: dynamic manifest) | Video decode, hardware encode for re-encode trims, clip → WebP |
| exiv2[bmff,png,xmp] | Win (Mac: dynamic manifest) | Metadata read/write |
| crashpad | both | Crash reports ([13](13-updates-and-telemetry.md)) |
| blake3, libsodium | both | Content hashes (verified copy, Import); Ed25519 + SHA-256 for add-on and update manifests |

`directxtex` is not in the manifest: thumbnails are JPEG-512 in SQLite, not GPU-resident BC7.

## Testing

Suites are registered with CTest and run by `ctest` on both platforms; see
[DEVELOPMENT.md → Test](../DEVELOPMENT.md#test).

1. **Native unit and regression tests** (`mv_tests`, `tests/test_*.cpp`). Decoders, colour,
   tiles, upload, pacer and present policy, A/V clock, transport, key router, navigation,
   metadata read/write, edit, clip/trim, telemetry schema, update guard, minidump scrub, ABI
   round-trip. `mv_import_tests`, `mv_ai_tests`, `mv_clip_tests`, `mv_trim_tests` are the
   add-on and clip suites. `tests/test_video_eof_mac.mm` and `test_metal_pacer.cpp` are
   Mac-only.
2. **ABI smoke test from C#** (`src.managed/MediaViewer.AbiSmokeTest`): `SafeHandle`
   lifetimes, struct layouts, completion drain against the built core. Structs C# mirrors by
   hand (`MvAiStatus`, `MvAiResult`) are compared field for field with `tools/abi-layout`'s
   dump of the native layout, so a mirror that misses an appended field fails CI.
   **Chrome contract tests** (issue #153). Neither chrome is linked into a test: the WinUI
   chrome is a class library the host loads through hostfxr, and the SwiftUI package calls
   bridge functions that resolve only in the app's final link. So
   `src.managed/MediaViewer.Chrome.Tests` (xUnit, plain net8.0) and the
   `MediaViewerChromeTests` SwiftPM target read the chromes' sources as text against
   `src/shell`: every command-id constant must name a `command_id` (or, C#, a
   `chrome_command`) by the same value, the C# checksum list must follow
   `chrome_command_checksum()`, Settings key capture must encode `commands.h`'s key codes
   and the `chrome_cmd_rebind` packing native unpacks, and every shortcut a Windows menu
   prints or a Mac menu looks up by name must be in the table. `mv_tests`
   (`test_command_contract.cpp`) holds the native half: a remap survives persist and
   reload exactly (`live_key_remaps` / `apply_key_remaps`, which both hosts persist with),
   and `describe_commands()` by mode matches the committed
   `tests/data/command_table.snapshot`, so a default-map change is a reviewed diff
   (`MV_UPDATE_SNAPSHOTS=1` rewrites it). `ImportChromeTests` covers the Import model's
   table-free pieces; AIChrome's tests run in CI, where its `PeopleGridBench` reports and
   does not gate. Not built yet: a UI smoke test per platform (UIA, XCUITest) opening
   `tests/data` and walking G → Enter → Esc.
3. **Format corpus.** Not in git. `tools/testmedia/raw-manifest.json` and
   `heif-manifest.json` list each sample's URL, SHA-256, licence and source;
   `fetch-raw.ps1` / `fetch-heif.ps1` download and verify. The video corpus (~1.5 GB, incl. a
   31-minute HEVC+AAC soak clip) is generated locally by `tools/testmedia/generate.sh`.
   Corpus tests skip when files are absent, and fail instead when `MV_REQUIRE_CORPUS=1`.
   CI's `test-media` job (Linux, apt ffmpeg) generates and fetches the corpus once, cached
   on the scripts and manifests; the Build legs restore it and set `MV_REQUIRE_CORPUS=1`,
   and `test_corpus.cpp` fails any CI run that leaves the variable unset. Hosted runners
   have no GPU, so cases asserting hardware decode or needing a `gfx::device` skip there.
   Small synthetic, licence-clean files under `tests/data/` (AVIF, HEIF, seeds, broken, the
   111 KB place test blob) are committed and run everywhere. The same job builds the full place
   database once per `tools/geo/build_places.py` revision and caches it; the Build legs restore it
   and set `MV_REQUIRE_GEO=1` so `mv_tests "[geo]"` runs its 300-row table ([27](27-places.md)).
4. **Broken-file corpus** (`test_broken_corpus.cpp`, label `broken`). Every seed in
   `tests/data/seeds` is truncated, stomped, bit-flipped and given absurd dimensions, plus the
   hand-made files in `tests/data/broken`, through every decode entry point. Fails on a crash,
   an escaped exception, a call over 5 s, or runaway memory. Each family's first call in the
   process is a warm-up on its real seed with 60 s (`MV_BROKEN_FIRST_USE_MS`): loading an OS
   decoder (Windows.Data.Pdf) on a cold runner is one-time setup, not a hang. `MV_BROKEN_FULL=1` is the
   exhaustive nightly sweep; `MV_BROKEN_DUMP` writes failing inputs.
5. **Fuzzing.** One libFuzzer harness per decoder entry point (`tools/fuzz/fuzz_*.cpp`:
   JPEG, PNG, BMP, GIF, WebP, TIFF, ICO, HEIC, AVIF, RAW, RAW preview, animation, the
   generic decode, the Explorer thumbnail handler, the PDF and DOCX readers, the place
   database loader `fuzz_geo` and the Places map `fuzz_places_map`, [27](27-places.md)), built
   with clang-cl + ASan (`-DMV_FUZZ=ON`). CI gates on the eleven single-format image harnesses;
   `decode`, `animation`, `thumbnail`, `script`, `pdf`, `docx` and `pptx` run in a quarantined,
   non-gating step (30 s each) until they are seen starting on the runner
   (`docs/design/12-decision-log.md`). The metadata, place, add-on and query harnesses below
   (#384) run in a second non-gating step, 30 s each, until CI has seen them start and run
   clean there; then they join the gating list.
   #290 adds one per metadata reader beyond Exiv2 (`fuzz_meta_c2pa`, `_jpeg`, `_png`, `_webp`,
   `_bmff`, `_xmp`, `_icc`; [06-metadata.md](06-metadata.md#beyond-exiv2-290)), which link only
   `mv_meta_parse` and abort on a row past the cap or a string that is not valid UTF-8. Their
   seeds are `tests/data/seeds/{c2pa,xmp,icc}` plus the image families.
   #352 adds `fuzz_meta_location` for the Location editor's parser (`meta::parse_location`,
   built from `src/meta/location_edit.cpp` alone): an accepted point is finite, in range and
   never `-0`, a refusal is one UTF-8 line that fits the ABI's 128 bytes, and the editor's
   starting text reads back to the point. Seeds: `tests/data/seeds/location`.
   #345 adds `fuzz_meta_probe` (the AI badge's provenance probe). #291 adds `fuzz_meta_search`
   (the folder tag pass's bounded read and the search text it builds; it links `mv_meta`, so
   Exiv2 and FFmpeg come in as uninstrumented DLLs) and `fuzz_ai_query` (the Local search query
   language; seeds `tests/data/seeds/query`). PR 56 adds `fuzz_addon_contributions`.
6. **Clean-VM gate** (`test_clean_vm_heic.cpp`, label `cleanvm`). A HEIC decodes with
   `MV_OS_CODEC=0`, and the process has loaded libheif + libde265 and no Media Foundation,
   WIC codec extension or `\WindowsApps\` module. It is its own executable because
   `mfplat.dll` never unloads from a process.
7. **Frame-time harness** (`tools/frametime`, `frametime.exe`; `main_mac.cpp` on macOS). It
   drives `mediaviewer_lab` through a 60 s animated soak and a 60 s idle soak and reads the JSON
   reports. The animated gate (`gfx/pacer.h`, `meets_pr1_gate`) requires, over ≥ 60 s:
   frames and displayed presents within ±2 % of the refresh count, mean and p50 within 2 % of
   the refresh interval, max frame ≤ 2× the interval, zero dropped frames, zero missed
   refreshes, and timing sourced from DXGI frame statistics (Metal display-link on macOS) with
   no discontinuities. The idle gate requires zero presents, CPU ≤ 1 % and no input events
   during the soak (park the cursor off the window). Against a rolling baseline
   (`frametime-baseline.json`, same refresh interval only) a p99 regression > 10 % fails.
   Exit codes: 0 pass, 1 gate or regression, 2 could not measure.
   - `--no-pop <still>`: opens a still with a preview and fails if the preview → full swap
     popped (fade cut short, view jumped, frame dropped inside the fade), with the animated gate
     judged on the same run.
   - The lab itself has `--soak`, `--static`, `--pan-soak`, `--browse-soak`, `--av-soak N --csv`
     (A/V drift; under 1800 s it is diagnostic only) and `--json`. `--compare` starts in the
     two-up compare; with `--pan-soak` (one file, so no previous still) the left half draws the
     same texture again, so the soak measures both draws, and the report's `compare_frames`
     counts the presents that drew two halves.
   - The soak proves the render thread presented on time; it does not see first-pixel time,
     full-resolution time or media rate, which have their own reports.
8. **Swapchain recovery** (`mv_swapchain_recovery`): device-loss and handle ownership.
9. **Performance suite** (`tools/perf/regenerate.py`): re-measures pacing, pan, first pixel,
   browse, video, A/V sync, the screen comparison against Windows Photos / Media Player, the
   headless `mv_tests "[.perf-bench]"` decode/colour/folder benches and search accuracy, writes
   reports to `docs/perf/` and redraws the README charts from them.
   `tools/perf/test_perf_tools.py` (`ctest -R perf_tools`) checks that every chart reproduces
   byte-for-byte from the committed reports.
   Wall-clock bounds live only in hidden `[.perf-bench]` tests (issue #154): the unit suite
   runs on shared runners and under `ctest -j`, so it asserts counters and states and waits
   on conditions, never on a sleep or a time budget.
10. **Other harnesses:** `copybench` (copy-engine throughput), `clipjob`, `encprobe`,
    `playprobe`, `ai-bench`, the Edit workspace self-test, and the crash-report canary
    ([13](13-updates-and-telemetry.md)).

### Policy gates

Run on every push by the `policy` CI job:

| Script | Fails when |
|---|---|
| `tools/check-module-graph.ps1` | A dependency points upward or sideways in `shell → abi → {canvas, edit, player, image, meta, addon} → {codec, gfx, io} → core`, or `addons → infer → core` ([architecture](02-architecture.md)); `.mm` / `#import` included |
| `tools/check-hostable-core.ps1` | `windows.h` / `d3d11.h` appear above `gfx/` and the hosts outside `*_win.cpp`, Foundation / AppKit / Metal / CoreVideo / AVFoundation outside the `*_mac.*` ports, or Wayland / EGL / GL / X11 / Qt / PipeWire and Linux's own headers outside the `*_linux.*`, `*_posix.*`, `*_gl.*` and `*_wayland.*` ports; a Vulkan header anywhere under `src/` ([platforms](15-platforms.md)) |
| `tools/check-vcpkg-baseline.ps1` | The vcpkg baseline differs between `vcpkg.json`, `tools/mac/dependencies/vcpkg.json`, `tools/portable/vcpkg.json` and `VCPKG_COMMIT` in `ci.yml` / `release.yml` |
| `tools/check-winui-controls.ps1` | The chrome uses a WinUI control that fail-fasts in the island host (`ProgressBar`, `ProgressRing`, `DropDownButton`, `InfoBar`, `TextBox`, …) |
| `tools/licence-check.ps1` | FFmpeg was configured with `--enable-gpl` / `--enable-nonfree`, or a software HEVC/H.264/AAC encoder is present ([licensing](11-licensing.md)) |
| `tools/telemetry-schema-check.ps1` | A telemetry event gains a free-text field ([13](13-updates-and-telemetry.md)) |

## Continuous integration

[`.github/workflows/ci.yml`](../../.github/workflows/ci.yml), on pushes to `main`, pull
requests, manual dispatch and a nightly schedule. A newer push to the same ref cancels the run
it replaces.

| Job | Runner | What it does |
|---|---|---|
| `policy` | windows-latest | The policy gates above |
| `windows-deps` | windows-latest | Installs the vcpkg manifest once and caches it (≈45 min after a manifest/baseline change, mostly FFmpeg) |
| `build` | windows-latest | MSVC Release, MSVC Debug, clang-cl, ASan; full `ctest`; uploads broken-corpus reproducers |
| `abi-smoke` | windows-latest | The C# ABI smoke test |
| `windows-chrome`, `mac-chrome` | windows-latest, macos-14 | WinUI and SwiftUI chrome compile; the chrome contract tests (`dotnet test`, `swift test`) |
| `mac-build` | macos-15 | The native core on macOS (arm64): vcpkg trees cached under release.yml's key, the licence gate over both Mac trees, the full build and `ctest --no-tests=error` (`mv_tests`, `mv_import_tests`, `mv_ai_tests`, the Mac-only Metal pacer / AVFoundation / PDFKit / Photos-folder / write-guard tests) |
| `mac-packaging` | ubuntu-latest | Release tooling tests and macOS bundle policy (`tools/mac/*`) |
| `portable-core` | ubuntu-latest | `cmake/portable` under ASan + UBSan: the Import engine, add-on host and verified copy (`mv_import_tests`), the add-on packer and SDK cross-checks against the C++ verifier, and the clip and trim suites against the distro FFmpeg |
| `linux-lab-smoke` | ubuntu-24.04 | Linux PR 1: the root CMake build on Linux (vcpkg for imgui and Catch2), `ctest` (the portable `mv_tests` and `linux_lab_smoke`), and the smoke again with Weston's pixman renderer. The smoke runs the lab under a headless Weston with Mesa llvmpipe and asserts that it presents, writes reports `frametime` parses, labels its source "clock intervals (weaker)", fails the gate, and presents nothing at ~0 % CPU in the static soak: plumbing, never pacing |
| `frametime` | self-hosted `[windows, gpu]` | Swapchain recovery, then the 60 s animated + idle gates against the rolling baseline (cached per runner; updated only on `main`). Runs only when `MV_GPU_RUNNER_ENABLED` is set and never for fork PRs; hosted runners have no usable GPU, so a skip is not a pass |
| `frametime-mac` | self-hosted `[macOS, ARM64, gpu]` | Mac PR 1: the 60 s animated + idle gates on Metal (`frametime --seconds 60`, under `caffeinate`). Runs only when `MV_MAC_GPU_RUNNER_ENABLED` is set and never for fork PRs |
| `frametime-linux` | self-hosted `[linux, gpu]` | Linux PR 1: the 60 s animated + idle gates on OpenGL/EGL, judged from `wp_presentation` feedback (`frametime --seconds 60`, under `systemd-inhibit`), against a rolling baseline as on Windows. The runner must run inside a real Wayland session (Mutter or KWin, Mesa on Intel or AMD). Runs only when `MV_LINUX_GPU_RUNNER_ENABLED` is set and never for fork PRs; no such runner exists yet |
| `present-gates` | ubuntu-latest | Always runs. Fails if any frame-time gate (Windows PR 1, Mac PR 1, Linux PR 1) failed or was cancelled; a skipped gate is a warning annotation and a line in the run summary, or a failure on this repository's own branches when `MV_REQUIRE_PRESENT_GATES` is set (for Linux, once `MV_LINUX_GPU_RUNNER_ENABLED` is too). This is the check to require in branch protection, since GitHub treats a skipped job as passing |
| `fuzz` | windows-latest | 60 s per harness on PRs; 20 min per harness nightly. Uploads crash / timeout / OOM inputs |
| `broken-corpus-full` | windows-latest | Nightly / dispatch: the exhaustive broken-corpus sweep |

There is no macOS native job: CI on macOS compiles the SwiftUI chrome packages only. The Mac
core, its `ctest` suites and the Metal present-loop gate run on a developer Mac
([DEVELOPMENT](../DEVELOPMENT.md)).

Release packaging is a separate manual workflow (`release.yml`; see
[RELEASING.md](../../RELEASING.md)).

## Performance targets

| Metric | Target |
|---|---|
| Cold start to first pixel of an image passed on argv | < 400 ms with the WinUI shell (< 250 ms in the lab) |
| Launch to interactive (chrome attached, input pumping) | tracked, no target yet: ~570 ms warm with no file on the Windows desktop, ~185 ms on the M5 MacBook (issue #486) |
| Warm folder navigation (next image visible) | < 40 ms |
| First pixel, 45 MP RAW (embedded preview) | < 60 ms |
| Full-res RAW decode complete | < 500 ms |
| Input → photon (pan/zoom) | ≤ 1 refresh interval |
| **Dropped frames while panning a cached image at display refresh** | **0** |
| Idle CPU / GPU on a static image | ~0 % |
| Editor slider drag → updated preview | ≤ 1 refresh interval |
| Installed app (Windows payload) | **< 250 MB**, enforced by `build-release.ps1` |

Measured results are in `docs/perf/` and the README charts.

Launch is measured by the **boot timeline** (issue #486, `core/boot_timeline.h`): the host marks
each stage (host entry, crash reporter, settings, session, window, render thread, show, the
chrome's runtime / entry points / command bar / other islands, interactive, first frame, first
pixel, full resolution) as milliseconds from process creation, logs the line once the launch is
ready, and with `--boot-report <path>` writes it as JSON and closes. `tools/perf/boot.ps1` (Windows)
and `tools/perf/boot.py` (either OS) repeat such launches, alternating builds, and prints per-stage medians; reports are in `docs/perf/boot/`.
A stage a host does not have stays -1, so both hosts write one shape.

## Installed size

The Windows payload is **self-contained**: it carries the .NET runtime and the Windows App SDK
runtime, so a clean Windows 10 21H2 machine runs it with nothing installed first and the
per-user, no-UAC installer never needs a machine-wide prerequisite. The cost is size: the app
is ~209 MB (inside the 200–250 MB band), and a first install occupies ~293 MB because
Velopack keeps one full package for rollback. `build-release.ps1` fails above 250 MB.

## Windows integration

- **Window:** per-monitor-v2 DPI manifest (`src/shell/app.manifest`), immersive dark title
  bar (`DWMWA_USE_IMMERSIVE_DARK_MODE`).
- **File associations:** per-user `ProgId` / `OpenWithProgids` registration for the D5 photo
  and video types, written by the installer and removed on uninstall. Windows does not let an
  app set itself as default, so the installer's Finish page has a checked *Choose MediaViewer
  as the default…* box that opens Settings → Default apps on MediaViewer. Each still `ProgId`
  has a `DefaultIcon` pointing at the app `.ico`, the same icon as window, taskbar, Start Menu
  and wizard. On macOS the equivalent is `MvSetDefaultViewer` (`NSWorkspace
  setDefaultApplicationAtURL`).
- **Explorer thumbnails** (`src/shellext`, `MediaViewerThumbs.dll`): an `IThumbnailProvider`
  over `IInitializeWithStream`, registered on the `MediaViewer.Image` ProgId only, so it never
  replaces another vendor's handler. It runs **out of process**: stream-initialised handlers run
  in the shell's isolated surrogate, and the class also declares its own `DllSurrogate` AppID
  (`shell/shellext_install.cpp`). A decoder crash on a malformed file takes down `dllhost.exe`,
  not Explorer. Limits (`shellext/thumb_request.h`): ≤ 512 MB read per request, ≤ 1024 px long
  edge, 4 s deadline, 4 in flight; it answers "no thumbnail" rather than working hard. No state
  is shared with the app process and nothing process-wide is changed (the surrogate may host
  other handlers). The entry point has its own fuzz harness. No property handler: it needs an
  HKLM registration a per-user install cannot make (macOS has a Spotlight importer instead).
- **Jump list** of recent folders (`ICustomDestinationList`, honouring user removals).
- **SMTC** (`SystemMediaTransportControls`) for media keys and the OS overlay during video.
  The chrome's 150 ms playback poll runs only while a clip is open, and it pushes SMTC's title,
  status and timeline only when one changes or the position jumps (a seek); the OS runs the
  position on between pushes.
- **Drag and clipboard out:** files go out as `CF_HDROP` (the original file, not a stream)
  from the canvas (`SHDoDragDrop`), gallery and filmstrip, and to the clipboard; keyboard twins
  are in [commands](16-commands.md). Drop in opens a file or folder.
- **Single instance** (`shell/single_instance_win.cpp`): a second start hands its paths over a
  per-user, per-session named pipe (`\\.\pipe\MediaViewer.Viewer.<session>.<SID>`, local
  clients only, `FIRST_PIPE_INSTANCE`, a DACL that admits only this user) to the running
  instance and exits. The name is computable by any local account, so the second start first
  checks that the pipe's server process runs as the same user; if not, it sends nothing and
  runs alone. `--new-instance` overrides it.
- **Command line:** `mediaviewer <path>` opens a file or folder. The remaining switches are
  lab / harness switches (`--soak`, `--browse-soak`, `--no-chrome`, `--restore-*`, …), except
  `--compare`, which starts in the two-up compare (`|`).

## Distribution

Direct download only; the app is GPL-3.0-or-later and has no Store channel
([licensing](11-licensing.md)).

- **Windows:** a signed **Inno Setup** wizard (`tools/package/mediaviewer.iss`), per-user
  under `%LocalAppData%\MediaViewer`, no UAC. **Velopack** delivers every later update silently
  into versioned folders from a signed manifest. `tools/package/build-release.ps1` builds both
  and refuses to proceed past the size cap, the licence gate, or forbidden payload files
  (Windows App SDK AI / ONNX / DirectML / WebView2).
- **macOS:** a notarized `MediaViewer.app` in a disk image, built by `tools/mac/macpack.py`,
  with a signed appcast feed.

Code signing, the update manifest, staged rollout, crash reports and telemetry are in
[13](13-updates-and-telemetry.md); release mechanics in [RELEASING.md](../../RELEASING.md).

## Not built

- Golden-image (perceptual-diff) tests of decoded output.
- A memory/handle-leak check over a 500-file browse loop.
- `--slideshow`, `--fullscreen` command-line switches.
- Explorer property handler; shell "Edit" verb; tabs / multi-window.
- Mica backdrop on the main window (used only in add-on windows).
- An AppContainer decode process.
