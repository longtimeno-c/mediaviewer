# Build, Test, Ship

How MediaViewer is built, which test suites and gates exist and what they measure, how the
Windows shell integration works, and how the app is distributed. Commands live in
[DEVELOPMENT.md](../DEVELOPMENT.md); this doc describes the system behind them.

## Toolchain

- **CMake ≥ 3.28** + **vcpkg manifest mode**. [`vcpkg.json`](../../vcpkg.json) pins a
  `builtin-baseline`; the baseline is the pin and `overrides` is empty. Moving it is a
  deliberate commit followed by a frame-time re-run.
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

Linkage: on Windows the vcpkg triplet is `x64-windows`, so dependencies ship as DLLs beside
the exe (Crashpad's client and imgui are static). On macOS permissive libraries link
statically and the LGPL codecs come from a separate manifest
(`tools/mac/dependencies/vcpkg.json`, `arm64-osx-dynamic`) and install root, so they cannot
leak into the static tree. Either way **FFmpeg, libheif, libde265, LibRaw and Exiv2 are always
DLLs / dylibs** ([licensing](11-licensing.md)). Shader bytecode is embedded as resources.

### Dependency set

| Library | Platforms | Used for |
|---|---|---|
| imgui (`dx11-binding` / `metal-binding`) | Win / Mac | Present lab and F3 overlay only. No `win32-binding`: the overlay is fed from the published `input_snapshot`, never from the window procedure |
| catch2 | both | Native tests |
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
   lifetimes, struct layouts, completion drain against the built core.
3. **Format corpus.** Not in git. `tools/testmedia/raw-manifest.json` and
   `heif-manifest.json` list each sample's URL, SHA-256, licence and source;
   `fetch-raw.ps1` / `fetch-heif.ps1` download and verify. The video corpus (~1.5 GB, incl. a
   31-minute HEVC+AAC soak clip) is generated locally by `tools/testmedia/generate.sh`.
   Corpus tests skip when files are absent, and fail instead when `MV_REQUIRE_CORPUS=1`.
   CI's `test-media` job (Linux, apt ffmpeg) generates and fetches the corpus once, cached
   on the scripts and manifests; the Build legs restore it and set `MV_REQUIRE_CORPUS=1`,
   and `test_corpus.cpp` fails any CI run that leaves the variable unset. Hosted runners
   have no GPU, so cases asserting hardware decode or needing a `gfx::device` skip there.
   Small synthetic, licence-clean files under `tests/data/` (AVIF, HEIF, seeds, broken) are
   committed and run everywhere.
4. **Broken-file corpus** (`test_broken_corpus.cpp`, label `broken`). Every seed in
   `tests/data/seeds` is truncated, stomped, bit-flipped and given absurd dimensions, plus the
   hand-made files in `tests/data/broken`, through every decode entry point. Fails on a crash,
   an escaped exception, a call over 5 s, or runaway memory. `MV_BROKEN_FULL=1` is the
   exhaustive nightly sweep; `MV_BROKEN_DUMP` writes failing inputs.
5. **Fuzzing.** One libFuzzer harness per decoder entry point (`tools/fuzz/fuzz_*.cpp`:
   JPEG, PNG, BMP, GIF, WebP, TIFF, ICO, HEIC, AVIF, RAW, RAW preview, animation, the
   generic decode, and the Explorer thumbnail handler), built with clang-cl + ASan
   (`-DMV_FUZZ=ON`).
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
     (A/V drift; under 1800 s it is diagnostic only) and `--json`.
   - The soak proves the render thread presented on time; it does not see first-pixel time,
     full-resolution time or media rate, which have their own reports.
8. **Swapchain recovery** (`mv_swapchain_recovery`): device-loss and handle ownership.
9. **Performance suite** (`tools/perf/regenerate.py`): re-measures pacing, pan, first pixel,
   browse, video, A/V sync, the screen comparison against Windows Photos / Media Player, the
   headless `mv_tests "[.perf-bench]"` decode/colour/folder benches and search accuracy, writes
   reports to `docs/perf/` and redraws the README charts from them.
   `tools/perf/test_perf_tools.py` (`ctest -R perf_tools`) checks that every chart reproduces
   byte-for-byte from the committed reports.
10. **Other harnesses:** `copybench` (copy-engine throughput), `clipjob`, `encprobe`,
    `playprobe`, `ai-bench`, the Edit workspace self-test, and the crash-report canary
    ([13](13-updates-and-telemetry.md)).

### Policy gates

Run on every push by the `policy` CI job:

| Script | Fails when |
|---|---|
| `tools/check-module-graph.ps1` | A dependency points upward or sideways in `shell → abi → {canvas, edit, player, image, meta, addon} → {codec, gfx, io} → core` |
| `tools/check-hostable-core.ps1` | `windows.h` / `d3d11.h` appear above `gfx/` and the hosts ([platforms](15-platforms.md)) |
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
| `windows-chrome`, `mac-chrome` | windows-latest, macos-14 | WinUI and SwiftUI chrome compile |
| `mac-packaging` | ubuntu-latest | Release tooling tests and macOS bundle policy (`tools/mac/*`) |
| `frametime` | self-hosted `[windows, gpu]` | Swapchain recovery, then the 60 s animated + idle gates against the rolling baseline (cached per runner; updated only on `main`). Runs only when `MV_GPU_RUNNER_ENABLED` is set and never for fork PRs; hosted runners have no usable GPU, so a skip is not a pass |
| `fuzz` | windows-latest | 60 s per harness on PRs; 20 min per harness nightly. Uploads crash / timeout / OOM inputs |
| `broken-corpus-full` | windows-latest | Nightly / dispatch: the exhaustive broken-corpus sweep |

Release packaging is a separate manual workflow (`release.yml`; see
[RELEASING.md](../../RELEASING.md)).

## Performance targets

| Metric | Target |
|---|---|
| Cold start to first pixel of an image passed on argv | < 400 ms with the WinUI shell (< 250 ms in the lab) |
| Warm folder navigation (next image visible) | < 40 ms |
| First pixel, 45 MP RAW (embedded preview) | < 60 ms |
| Full-res RAW decode complete | < 500 ms |
| Input → photon (pan/zoom) | ≤ 1 refresh interval |
| **Dropped frames while panning a cached image at display refresh** | **0** |
| Idle CPU / GPU on a static image | ~0 % |
| Editor slider drag → updated preview | ≤ 1 refresh interval |
| Installed app (Windows payload) | **< 250 MB**, enforced by `build-release.ps1` |

Measured results are in `docs/perf/` and the README charts.

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
- **Drag and clipboard out:** files go out as `CF_HDROP` (the original file, not a stream)
  from the canvas (`SHDoDragDrop`), gallery and filmstrip, and to the clipboard; keyboard twins
  are in [commands](16-commands.md). Drop in opens a file or folder.
- **Single instance** (`shell/single_instance_win.cpp`): a second start hands its paths over a
  per-user, per-session named pipe (`\\.\pipe\MediaViewer.Viewer.<session>.<SID>`, local
  clients only, `FIRST_PIPE_INSTANCE`) to the running instance and exits. `--new-instance`
  overrides it.
- **Command line:** `mediaviewer <path>` opens a file or folder. The remaining switches are
  lab / harness switches (`--soak`, `--browse-soak`, `--no-chrome`, `--restore-*`, …).

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
- `--slideshow`, `--fullscreen`, `--compare` command-line switches.
- Explorer property handler; shell "Edit" verb; tabs / multi-window.
- Mica backdrop on the main window (used only in add-on windows).
- An AppContainer decode process.
