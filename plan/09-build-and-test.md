# 09 — Build, Test, Ship

## Toolchain

- **CMake ≥ 3.28** + **vcpkg manifest mode** (`vcpkg.json` checked in with a pinned baseline —
  reproducible builds matter more than fresh dependencies).
- **MSVC 2022** primary; clang-cl in CI as a second opinion (it catches different bugs).
- **.NET 8+ / C# WinUI 3** for the shell (D1), consuming the C++ core through a flat C ABI. Two
  toolchains, one solution; the core builds and tests standalone with no shell present.
- Static-link everything **except FFmpeg, libheif, libde265, LibRaw, and (if kept) Exiv2**, which are LGPL/GPL and must be dynamically linked — see [11-licensing.md](11-licensing.md). Ship one exe plus those DLLs, with shader bytecode embedded as resources.
  shader bytecode as resources.
- `/W4 /WX /permissive- /Zc:__cplusplus /GR- /utf-8`, ASan in a CI configuration.

Rough `vcpkg.json` dependency set:
```
libjpeg-turbo, libspng, libwebp, libheif[hevc,av1], libavif[dav1d], tiff,
libraw, lcms, exiv2, ffmpeg[core,avcodec,avformat,avutil,swscale,swresample],
imgui[dx11-binding,win32-binding], directxtex, sqlite3, catch2, benchmark

# NOT in the v1 manifest — these arrive with their formats in v1.1 (D5):
#   libjxl, openexr, openjpeg, resvg
```
FFmpeg is the video pipeline (D2), not a fallback — build it LGPL-only and ship it as DLLs.
Verify the configure line carries no `--enable-gpl` and no `--enable-nonfree`. `imgui` is for the
PR 1 present lab and the debug overlay only, never shipped chrome. `libjxl`, `openexr`, `resvg`,
and `openjpeg` move in at v1.1 with their formats (D5).

## Testing

1. **Golden-image tests.** A corpus of ~400 files spanning every format, decoded and rendered
   headlessly to PNG, compared against approved references with a perceptual metric (butteraugli
   or SSIM) and a tight threshold. This is the only way to refactor a decode path without fear.
2. **Format corpus.** Collect real files: every camera RAW you can get, iPhone HEIC + Live Photos,
   Android motion photos, GoPro/drone footage, 10-bit HDR HEVC, ProRes, VFR screen recordings,
   animated WebP/AVIF, 16-bit PNG, CMYK JPEG, gigapixel TIFF. Plus a **broken-file corpus** —
   truncated, wrong-length chunks, absurd dimension headers. Nothing may crash or hang.
3. **Fuzzing.** libFuzzer harness per decoder against the corpus, run in CI nightly. Decoders
   parse untrusted input; this is where your CVEs live.
4. **Frame-time regression harness** (`tools/frametime`). PR 1/PR 2: an animated 60 s soak and a
   static idle soak of the **empty** present loop (sweeping bar, then stop presenting). It does
   not `--open` an image; passing it is not the PR 2 pan-at-refresh clause. Later scripted
   sessions — open a 12 MP JPEG and pan, drag the window while a 60 MP PNG loads, then a 45 MP
   RAW, 4K HEVC — are additive. Captured with **DXGI frame statistics** (and PresentMon/ETW
   when available). CI fails the build if p99 frame time regresses > 10 % or any frame exceeds
   2× the refresh interval. **Treat a dropped frame as a test failure, not a nuisance.** This is
   how "butter smooth" survives contact with feature work.

   Idle is invalidated by any input, including a cursor in the lab client area. Park it off the
   window. A development box that can pass one animated soak and drop frames on the next is not
   the GPU runner; see [12](12-decision-log.md).
5. **Memory-leak and handle-leak** checks across a 500-file browse loop.

## Performance targets (make these explicit and enforced)

| Metric | Target |
|---|---|
| Cold start to first pixel of an image passed on argv | < 400 ms (C#/WinUI shell; < 250 ms in the PR 1 lab) |
| Warm folder navigation (next image visible) | < 40 ms |
| Time to first pixel, 45 MP RAW (embedded preview) | < 60 ms |
| Full-res RAW decode complete | < 500 ms |
| Input → photon (pan/zoom) | ≤ 1 refresh interval |
| **Dropped frames while panning a cached image at display refresh** | **0 — the v1 gate (D6)** |
| Idle CPU / GPU on a static image | ~0 % |
| Editor slider drag → updated preview | ≤ 1 refresh interval |
| Installed size | **< 250 MB** — see the note below; the old < 120 MB target was not reachable |

## Installed size — pick the .NET deployment model deliberately

The old "< 120 MB" target was not reachable and would have been discovered as a failure late. The
payload is roughly: self-contained .NET ~70 MB + FFmpeg ~30 MB + libheif/libde265/dav1d ~10 MB +
LibRaw ~8 MB + your code and shaders.

| | **Self-contained .NET** ✅ | **Framework-dependent** |
|---|---|---|
| Installed size | ~200–250 MB | ~130 MB |
| First-run experience | **Just works** | May prompt to install the .NET runtime |
| Update payload | Larger, but deltas make it moot ([13](13-updates-and-telemetry.md)) | Smaller |
| Support burden | None | "It won't start" tickets from missing runtimes |

**Take self-contained and publish the honest number.** A viewer whose whole pitch is "point it at a
folder and it works" cannot open with a runtime prerequisite dialog — that's the same mistake as a
codec-pack prompt (D3). Enable .NET trimming to claw back part of it, and let deltas carry the
update cost.

## CI needs a real GPU — the frame-time gate is otherwise theatre

**GitHub-hosted Windows runners have no usable GPU and noisy neighbours.** PresentMon on a cloud VM
cannot measure the D6 gate, and a green check that proves nothing is worse than no check, because
people trust it.

- **Self-hosted runner with a real GPU**, pinned power profile, fixed refresh rate, nothing else
  scheduled on it. One mid-range desktop or a dedicated cloud GPU instance.
- Run the frame-time suite **only** there; run correctness, golden-image, and fuzz suites on
  hosted runners where they belong.
- Report p50/p99 with a **rolling baseline** rather than an absolute threshold — silicon and
  drivers drift, and an absolute number becomes a nuisance everyone learns to ignore.

## The test corpus does not live in git

A meaningful corpus is tens of GB of RAW, HEIC, and 4K video. It does not belong in the repository
and it does not belong in Git LFS at that size either.

- **Corpus store** (object storage or a NAS) with a manifest of `(url, sha256, licence)` checked
  into the repo; a script fetches and verifies.
- **A tiny CI subset** — a few MB, one small file per decoder, licence-clean and redistributable —
  runs on every PR. The full corpus runs nightly on the self-hosted box.
- Record the **provenance and licence of every sample.** Camera-manufacturer sample RAWs are
  usually *not* redistributable, which is precisely why they cannot be committed.

## Windows integration (do this properly — it's most of "feels like a real app")

- Per-monitor-v2 DPI manifest, dark-mode title bar (`DWMWA_USE_IMMERSIVE_DARK_MODE`), Mica
  backdrop, snap layouts.
- File associations via the standard `ProgId`/`OpenWithProgids` registry keys, plus a
  **Default Apps** deep link; never silently hijack associations. Windows 10+ will not
  let an app write `UserChoice` itself — "set as default" means sending the user to
  Settings.

  **Ask once, after the first successful still open**, not at install and not on an
  empty first launch: "Make MediaViewer your default photo viewer?" Yes opens Default
  Apps focused on this app. No / dismiss is remembered; never ask again. Settings
  keeps the same action so a later change is one click. Skip the prompt if we are
  already the default. The prompt covers the **D5 still set** (JPEG, PNG, BMP, GIF,
  TIFF, WebP, HEIC/HEIF, AVIF, ICO, RAW) — not video. Video stays on "Open with" and
  a separate Settings row so we do not steal Movies & TV / VLC by surprise.

  Do not stack this with the telemetry first-run screen ([13](13-updates-and-telemetry.md)).
  If both would fire, finish the telemetry choice first; the default-app ask waits
  until the next successful still open.
- Shell verbs ("Open with MediaViewer", "Edit"), thumbnail provider (`IThumbnailProvider`) and
  property handler so *Explorer itself* gets your format support for HEIC/AVIF/RAW.

  **These must run out-of-process, and this is a landmine, not polish.** Your handler loads
  libheif, LibRaw, and FFmpeg — decoders that parse untrusted files — and if it is registered
  in-process, a malformed HEIC in a folder someone browses **takes down Explorer**. That is a
  crash the user cannot attribute to you and will never forgive.

  - Register with an `AppID` declaring `DllSurrogate` so the handler is hosted in `dllhost.exe`.
  - **Tight timeouts and hard memory caps**; return "no thumbnail" rather than working hard.
  - **No state, cache, or decoder instance shared with the app process.** Separate everything.
  - Fuzz the handler entry points specifically, not just the decoders behind them.
- Jump list of recents, taskbar thumbnail transport buttons, SMTC (`SystemMediaTransportControls`)
  so media keys and the OS overlay work.
- Drag-and-drop in and out (`IDataObject` with `CFSTR_FILEDESCRIPTOR` so you can drag an edited
  copy directly into another app). Keyboard twins in [16-commands.md](16-commands.md):
  `Ctrl+C` / `Ctrl+Shift+C` / `Ctrl+Alt+C` / `Ctrl+Shift+S`.
- Command line: `mediaviewer <path...>`, `--slideshow`, `--fullscreen`, `--compare`.
- Single-instance-with-tabs by default (named pipe hands the path to the running instance),
  overridable.

## Security

Decoders are the attack surface. Phase 3: move decoding into a **separate AppContainer process**
with no network and no filesystem access beyond a duplicated handle to the one file, passing
decoded pixels back over shared memory. This is what browsers do and it converts "RCE" into
"a tab crashed".

## Distribution

MSIX for the Store path and a plain signed installer (Inno Setup or WiX) for direct download.
Code-sign both — SmartScreen will otherwise block every user's first run. Auto-update, crash
reporting, and telemetry are designed in [13-updates-and-telemetry.md](13-updates-and-telemetry.md);
the short version is a **per-user** install (so updates need no elevation), versioned folders (so
locked codec DLLs are never overwritten in place), a signature-verified update manifest, and
staged rollout gated on the crash-free rate.
