# MediaViewer

Windows viewer for a real camera dump — photos and video in one folder. Opens everything
instantly, pans without a dropped frame, shows and edits metadata, does everyday photo
edits, trims video without re-encoding.

**v1 is a viewer with light edits, on the camera-dump format set.** Not a develop module.
Not an NLE. Not a movie player.

**Stack:** C# WinUI 3 shell · C++20 core behind a flat C ABI · Direct3D 11 canvas ·
FFmpeg + D3D11VA · libjpeg-turbo / libspng / libwebp / libheif / LibRaw · Exiv2 · SQLite ·
CMake + vcpkg.

The repo is greenfield. `plan/` is the spec. Code follows the plan; the plan is not
inferred from code.

## Source of truth

Read `plan/README.md` first, then the doc for the slice you are touching:

| Doc | When to read |
|---|---|
| `plan/01-decisions.md` | Before any stack, shell, codec, or scope change |
| `plan/02-architecture.md` | Modules, threading, memory budgets |
| `plan/03-rendering.md` | Swapchain, pacing, colour, resampling |
| `plan/04-image-pipeline.md` | Decoders, tiling, prefetch, thumbs |
| `plan/05-video-pipeline.md` | FFmpeg + D3D11VA, A/V clock, seek |
| `plan/06-metadata.md` | Read model, atomic writes, RAW sidecars |
| `plan/07-photo-editing.md` | EditStack; v1 vs v1.1 op split |
| `plan/08-video-editing.md` | Two-path trim; smart cut is v1.1 |
| `plan/09-build-and-test.md` | Toolchain, harnesses, Windows integration |
| `plan/10-roadmap.md` | Current PR, verify line, sequencing |
| `plan/11-licensing.md` | Linkage, FFmpeg configure, Exiv2 |
| `plan/12-decision-log.md` | Why a call was reversed — do not re-reverse quietly |
| `plan/13-updates-and-telemetry.md` | Updater, crash reports, privacy line |

If a change would contradict a **D1–D8** decision, stop and say so. Do not “just this once.”
If you reverse a decision, add a dated row to `plan/12-decision-log.md` with the reason.

## Rules that don't bend

1. **Nothing that can block touches the UI or render thread** — no decode, no I/O, no encode.
2. **The canvas is a D3D11 swapchain**, never a XAML `Image` or `MediaPlayerElement`. If
   `SwapChainPanel` cannot hold the pacing gate, drop to a native HWND/DComp island. Never
   silently degrade the canvas to XAML.
3. **First pixel is never the full decode**: memory cache → disk thumb → embedded RAW
   preview → downscaled decode. Full resolution is a refinement.
4. **Zero dropped frames while panning a cached image at display refresh** — measured, not
   eyeballed. Every PR must still pass PR 1's present-loop verify.
5. **Never modify an original.** RAW gets an XMP sidecar. Edits and trims write new files.
6. **Nothing about a user's files leaves the machine** — no paths, filenames, pixels, or
   EXIF in crash reports or telemetry.
7. **Never require a Store codec pack.** Probe OS codec, prefer it when hardware-backed,
   fall back to the bundled decoder silently.

## Decided — do not reopen

| # | Call |
|---|---|
| **D1** | C# WinUI 3 chrome + C++ core. ImGui is the PR 1 present lab and the F3 overlay, never shipped chrome. |
| **D2** | FFmpeg + D3D11VA on *your* `ID3D11Device`, presented on the same swapchain as photos. Not libmpv. Not MF as the primary path. `IMFMediaEngine` is an escape hatch behind `IVideoSource` if the A/V clock overruns PR 5. |
| **D3** | Bundle decoders. Never require a Store pack. |
| **D4** | v1 = rotate/flip/crop/resize + exposure/contrast/saturation/temperature. No curves, HSL, local, healing, GPU demosaic. |
| **D5** | v1 formats: JPEG, PNG, BMP, GIF, TIFF, WebP, HEIC/HEIF, AVIF, ICO, RAW. Video: MP4/MOV/MKV/WebM/AVI/TS — H.264, HEVC, VP9, AV1, MPEG-2. JPEG XL / EXR / PSD / SVG / DDS wait. |
| **D6** | Linear FP16 *working space* (non-negotiable). 8-bit sRGB *swapchain* in v1. Untagged JPEG → sRGB. Treating a tagged image as sRGB is a bug. |
| **D7** | v1 trim = keyframe stream-copy **or** full re-encode, both labelled. Smart cut is v1.1. |
| **D8** | AppContainer decode process is post-v1. Fuzz from PR 6/7. |

Do not introduce Electron, Tauri, Node, D3D12, Vulkan, or a second present path.

## Open — do not silently decide

- **A quiet machine for the D6 gate.** PR 1's 60 s animated and idle soaks are still
  unproven on a dedicated GPU runner. PR 2 inherits that gate; it is not waived.
  `plan/12-decision-log.md`.
- **Whether WinUI 3 XAML islands hold up** is answered by PR 3, not by preference.
  The canvas is already native (D1 amendment). Fallback: a WinUI app with
  `SwapChainPanel` and an accepted composed frame.

## How to implement

Builds, tests, policy checks, and local validation runs are authorized as part of
implementation. Run the checks needed to verify a change without asking for
confirmation each time, including the present lab and frame-time soaks.

Other agents may be working in the same checkout. Preserve their edits and do
not revert unrelated changes. If concurrent work prevents a meaningful build,
validate the current PR in an isolated checkout and report which source was
tested. Keep changes to shared files compatible with the other work.

Work is **one PR slice from `plan/10-roadmap.md`**. Do not start PR N+1 until N's verify
holds **and** PR 1's present-loop verify still holds.

- The verify line is the success criterion. Quote it before you start; do not invent a
  different one.
- Do not implement v1.1 ops, formats, smart cut, HDR swapchain, or AppContainer “while
  you're here.” The stack is designed to take them later; adding them now is scope.
- Do not add a format that is not in the D5 v1 set.
- Do not build a batch metadata engine before the read pane has been used (PR 11 writes
  rating, orientation, and user comment only).
- Crash reporting should land with the format long tail (PR 7), not wait for PR 15.
  The updater must exist before the first build that leaves this machine.
  Telemetry waits for PR 15 and is **default off**.

## Root README

`README.md` at the repo root is the human landing page. **Keep it up to date in the
same change that would make it stale.** Do not finish a PR, scaffold, or behaviour
shift with the README still describing the old repo.

Update it when you:

- Add or change build, run, or test commands
- Scaffold layout, dependencies, or the toolchain
- Land a roadmap PR that changes what the tree actually does (formats, playback,
  edits, trim, install)
- Settle licence, install/update, or “how to open a camera dump”
- Add a verify/harness a human would need to run

Write for someone cloning the repo, not for an agent. What it is, how to build it,
how to run it, current status vs the plan. Do not paste `plan/` into it — link
there. If the README still says the repo is empty after you have added a build,
you are not done.

## Native core (C++20)

Deliberately narrow subset: RAII, `std::unique_ptr` / `ComPtr`, `std::span`,
`std::string_view`, designated initializers. **No exceptions on the hot path**
(`std::expected`-style results). No RTTI. No `std::shared_ptr` in the render loop.
No deep template metaprogramming. Compile `/W4 /WX /permissive- /GR- /utf-8`.

Dependencies point **downward only**:
`app → ui → {edit, player, image, meta} → {codec, gfx, io} → core`.
No back-edges. After D1, chrome is C#; the native top is the C ABI.

Five thread roles. UI and render **never** wait on I/O, decode, or a lock a worker holds.
Decode workers create **immutable** `ID3D11Texture2D`s with `D3D11_SUBRESOURCE_DATA`
(free-threaded device). Communication is SPSC rings of POD plus one MPMC job queue.
Every job carries a **generation counter** tied to view intent; navigation bumps it.

## C ABI

Designed in PR 1, not retrofitted.

- Opaque handles, POD structs, no C++ types, no STL, no exceptions across the line.
- Every call returns a status code. Catch everything at the boundary.
- C# wraps every native resource in `SafeHandle` / `IDisposable`.
- C++ does not call the WinUI dispatcher. Completions are a queue the shell pumps, or a
  documented any-thread callback that C# marshals.
- Give every core call a correlation id so a managed error can be tied to the native
  failure (crash reporting, `plan/13-updates-and-telemetry.md`).

## Canvas and colour

- Flip-model swapchain, waitable frame-latency object, `SetMaximumFrameLatency(1)`.
- Wait on the waitable object **before** recording the frame, never `Sleep`.
- Uploads budgeted (~2 ms/frame). Idle → stop presenting (0 % GPU on a still).
- Springs (`ω=18, ζ=1`) for pan/zoom, not fixed-step tweens.
- Viewer LRU should not keep every image as FP16 in VRAM. FP16 is the edit working
  space; the display path blits to 8-bit sRGB in v1.
- Display-referred JPEG/HEIC: ICC → linear → sRGB encode. Do **not** Reinhard/ACES a
  camera JPEG. Tone-map HDR/PQ/HLG sources; iPhone HDR HEVC needs a SDR map in v1
  even if HDR *output* waits.

## Video

- Create the FFmpeg D3D11VA device from **our** `ID3D11Device`
  (`D3D11_CREATE_DEVICE_VIDEO_SUPPORT` + `ID3D10Multithread`).
- Decoded surfaces are decoder-pool memory. Copy or retain into a small presentation
  queue of **our** textures before present — do not bind DPB surfaces as the swapchain
  source.
- NV12: R8 + R8G8 SRVs. P010: R16 + R16G16. Do not assume BT.709 limited range.
- Audio is the master clock (WASAPI shared). No-audio clips fall back to QPC.
- Hardware encode (NVENC / QSV / AMF / MF) for re-encode. **No x264/x265, no software
  HEVC encoder, no `--enable-gpl` FFmpeg.**

## Licensing (enforced in the build)

- FFmpeg, libheif, libde265, LibRaw, and Exiv2 (if kept) are **dynamic-link only**.
- FFmpeg configure: LGPL only. CI fails on `--enable-gpl`, `--enable-nonfree`, or a
  LibRaw GPL demosaic pack.
- `libheif` for HEIC decode: libde265 and/or FFmpeg — **not x265**.
- Pin the vcpkg baseline. Reproducible builds beat fresh deps.

## Privacy

Opt-in telemetry, default off, no pre-ticked box. Crash reports: format, codec,
dimensions, decoder version — never path, filename, bytes, or EXIF. Scrub usernames
from stacks. Minidump filter must exclude decoded image heaps. Ask before first send.

## Planned layout

```
src/core     arena, job system, result<T>, logging, ETW
src/io       async reads, directory watcher
src/codec    decoder registry — one TU per format family
src/image    Image, colour, tiles, GPU upload, VRAM LRU
src/gfx      D3D11, swapchain, pacer, shaders, compositor
src/player   demux/decode, A/V clock, WASAPI, transport
src/meta     Exiv2 / libavformat, property model, writers
src/edit     EditStack, GPU ops, export
src/ui       present-lab / canvas input only (chrome is C#)
src/app      C ABI surface; C# shell is a sibling project
tests/
tools/       frametime harness, golden-image runner, fuzzers
plan/        spec — not code
```

Adding a format is one file plus one registry line. Probe by magic bytes, never extension.

## Build

Not scaffolded until PR 1. Target, from `plan/09-build-and-test.md`:

- CMake ≥ 3.28, vcpkg manifest mode, MSVC 2022, clang-cl in CI, .NET 8+ WinUI 3.
- ASan configuration in CI.
- Frame-time harness (`tools/frametime`) + PresentMon/ETW: p99 regress > 10 % or any
  frame > 2× refresh interval fails the build.
- Golden-image tests and per-decoder libFuzzer harnesses from PR 6/7.
- Do not put the RAW corpus in git.

Until those commands exist, do not invent a different toolchain.

## Explorer / install (when you get there)

- Thumbnail and property handlers are **out-of-process** COM surrogates. Loading
  libheif/LibRaw/FFmpeg into Explorer is how you crash the shell.
- File associations via `ProgId` / `OpenWithProgids` + Default Apps link. Never silently
  hijack.
- Per-user install to `%LocalAppData%\MediaViewer`. Primary channel is a signed
  installer + Velopack, not Store-as-updater.
