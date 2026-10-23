# 15 — Platforms

How MediaViewer runs on Windows and macOS from one native core: what is shared, what each host owns, the hostable-core rule that keeps it that way, shaders, floors and distribution.

**D9.** The macOS app is a second host of the same decode / colour / edit / metadata core, not a UI port.
Every feature from PR 9 on is one shared core change with a WinUI half and a SwiftUI half; the Mac host
itself was built as the Mac halves of PRs 1–8.

## This is not a UI update

The C ABI lets chrome be native per OS without rewriting decode. It does not make the Mac app "SwiftUI
on the Windows C++": present, hardware decode, audio, I/O and shipping are per-OS too.

| Shared by both hosts | Per OS |
|---|---|
| `codec/`, `image/` colour, `edit/` op graph, `meta/`, `canvas/` springs, `core/` jobs, `addon/`, `infer/`, the C ABI | Present path, GPU backend, hardware decode and encode, audio clock, async I/O, directory watch, atomic replace, windowing, chrome, installer and updater, OS integration |
| libjpeg-turbo / libspng / giflib / libwebp / libtiff / libheif / libavif / LibRaw / Exiv2 / LCMS / SQLite / FFmpeg / Crashpad | D3D11, DXGI, DComp, D3D11VA, WASAPI, `HWND`, `OVERLAPPED`, `ReplaceFileW`, WinUI, Velopack — vs Metal, `CAMetalLayer`, VideoToolbox, Core Audio, FSEvents, POSIX I/O, AppKit/SwiftUI, Sparkle |
| UTF-8 paths across the ABI | `wchar_t` Win32 paths and `ID3D11*` stay in the Windows host and backends |

| | Windows | macOS |
|---|---|---|
| Window and canvas | Win32 top-level window; D3D11 composition swapchain in a DComp visual | AppKit window owning a `CAMetalLayer` (`BGRA8Unorm_sRGB`, sRGB colourspace, `maximumDrawableCount = 2`, Metal's minimum) |
| Pacing | Frame-latency waitable object, `SetMaximumFrameLatency(1)` | `CAMetalDisplayLink` (macOS 14), wait before encode; `gfx/metal_pacer` records target timestamps |
| Chrome | C# WinUI 3 in XAML islands (`src.managed/`) | SwiftUI in `NSHostingView`s (`src.swift/`), bridged into the render thread's `input_snapshot` |
| Present lab / F3 overlay | `src/shell/present_lab.cpp`, ImGui DX11 | `src/shell/present_lab_mac.mm`, ImGui Metal |
| Video decode | FFmpeg + D3D11VA (`player/hwdecode_win.cpp`) | FFmpeg + VideoToolbox (`player/hwdecode_mac.mm`); ProRes decoded here only |
| Presentation ring | Our D3D11 textures (`player/frame_ring.cpp`) | Our Metal textures (`player/frame_ring_mac.mm`) |
| Audio / master clock | WASAPI shared (`player/audio_win.cpp`) | Default-output AudioUnit (`player/audio_mac.cpp`) |
| Hardware encode | NVENC / QSV / AMF / Media Foundation (`edit/hwencode_win.cpp`) | VideoToolbox (`edit/hwencode_mac.cpp`) |
| Directory watch | `ReadDirectoryChangesW` (`io/dir_win.cpp`) | FSEvents (`io/dir_mac.cpp`) |
| Atomic replace | `ReplaceFileW` / `MoveFileExW` (`io/replace_win.cpp`) | POSIX `rename` (`io/replace_mac.cpp`) |
| File reads | Overlapped handles (`io/file_port_win.cpp`) | POSIX (`io/file_port_mac.cpp`) |
| OS codec hook | WIC for qualifying HEIC (`codec/os_decode_win.cpp`) | Declines; bundled libheif (`codec/os_decode_mac.cpp`) |
| Crash reports | Crashpad + minidump scrub (`shell/crash_reporter_win.cpp`) | Crashpad (`shell/crash_reporter_mac.mm`), same privacy line |
| Thumbnails in the OS | Explorer thumbnail handler DLL in a `dllhost` surrogate (`src/shellext/`) | Quick Look extension (`.appex`, own process) and a Spotlight importer |

There is no `AVPlayer` in the viewer and no second display layer: video is drawn on the same Metal layer
as photos. (The Final Cut Pro extension's preview uses `AVPlayerView`; that extension is a separate
Mac-only surface, [23-nle-search.md](23-nle-search.md).) There is no Electron, Tauri, Node, D3D12,
Vulkan, MoltenVK, wgpu or SPIR-V anywhere; each OS has one present path.

The Mac chrome's stores mirror host state by reading the bridge (`mv_chrome_*`), and they read it when
the main run loop has done work, not on timers (`ChromePulse.swift`, issue #178): a run-loop observer
polls every store on the loop's way back to sleep, so an idle app on a still is not woken. One
coalesced 0.1 s timer runs only while something moves off the main thread (a clip playing, a running
job or backup on screen) and for a second after the last key or click. The host polls the render
thread's video status only while a clip or a sound is the current item, and the Video Editor's
cut-follow tick runs only while its clip plays. The Import and AI add-on stores still tick at 2 Hz.

## The call

v1 shipped on Windows first; the core was kept hostable from PR 4 so the Mac app could be a host plus
backends instead of a rewrite. The Mac halves of PRs 1–8 followed, and both platforms move together
from PR 9.

Linux is the third host (owner, 2026-10-23): OpenGL via EGL, Wayland first, Qt 6 chrome. The plan
recommends building it as the Linux halves of PRs 1–8 and then of everything since; the order is
an open call ([plan](../plans/linux.md) §10). Until it has caught up, the tables on this page are
Windows and macOS only, and the hostable-core rule below applies to the Linux host as well.

## Floors

| | Windows | macOS |
|---|---|---|
| OS | Windows 10 21H2+ | macOS 14+ (`CMAKE_OSX_DEPLOYMENT_TARGET 14.0`) |
| CPU | x64 | Apple silicon and Intel — one universal app, each arch built natively and joined by `tools/mac/lipo_merge.py` |
| Not built | Windows ARM64 | AI add-on, NLE search and FCP extension on Intel (ONNX Runtime ships no x86_64 macOS build) |

The frame-pacing gate has been measured on Apple silicon; Intel builds run but have not been measured
for pacing.

## Hostable-core rule

Platform APIs stay out of the shared modules.

- **`HWND`, `ID3D11*`, `IDXGI*`, `wchar_t` paths, `OVERLAPPED`, WASAPI, `ReplaceFileW`** live in
  `shell/` and the Windows backends under `gfx/`, `io/*_win.cpp`, `player/*_win.cpp`, `edit/*_win.cpp`.
  Cocoa, Metal and CoreFoundation live in the Mac host and `*_mac.mm` / `*_mac.cpp` backends.
  Wayland, EGL, GL and xkbcommon live in the Linux host (`shell/*_linux.cpp`) and the OpenGL backend
  (`gfx/*_gl.cpp`, `gfx/*_wayland.cpp`); their headers name no EGL, GL or Wayland type.
- Headers consumed above `gfx/` do not include `d3d11.h`. GPU resources above `gfx/` are opaque
  (`gpu_image.h` / `gpu_image_mac.h`, `native.h` accessors). `image/gpu_image.h` still reaches D3D11
  transitively through `gfx/device.h`.
- Paths across the C ABI are **UTF-8**; the Windows host converts at the boundary.
- Canvas input is the POD `input_snapshot` ([02-architecture.md](02-architecture.md)); both hosts
  publish the same struct.
- **Key bindings live in the host** ([16-commands.md](16-commands.md)). Both hosts share the command
  table and key router; the Mac default map uses `⌘` where Windows uses `Ctrl`. The core never sees
  `VK_*` or Carbon key codes.
- Completions are a queue the host pumps; C++ never calls a WinUI dispatcher or `DispatchQueue.main`.
- Ports are narrow — a header plus a real `*_win` and `*_mac` implementation — for gfx (device,
  texture, blit, present), io (file port, directory watch, atomic replace, volumes, child process),
  hwdecode, audio, encode and the OS-codec hook. There is no general RHI.

Checks: [`tools/check-module-graph.ps1`](../../tools/check-module-graph.ps1) forbids any native module
depending on `shell/`; [`tools/check-hostable-core.ps1`](../../tools/check-hostable-core.ps1) fails a
direct include of `d3d11.h`, `dxgi.h`, `windows.h`, `atlbase.h`, the WASAPI/COM headers, `d3d12.h` or
FFmpeg's D3D11VA/DXVA2 hwcontext headers from `core/`, `codec/`, `canvas/`, `image/`, `meta/`,
`player/`, `edit/`, `addon/`, `addons/`, `infer/` and `io/*.h` outside `*_win.cpp`, and the mirror on the
Mac side: Foundation, AppKit, Metal, QuartzCore, CoreVideo, CoreMedia, VideoToolbox, AVFoundation and
Core Audio headers (`#include` or `#import`) in the same places outside the `*_mac.*`, `*_metal.*` and
`*_posix.*` port files, and the third for Linux: EGL / GL / GLES, the Wayland, xkbcommon and X11
headers, DRM / GBM / VA-API, PipeWire / PulseAudio / ALSA, D-Bus, GLib / GTK, Qt, BlueZ and the
Linux-only `<linux/…>` and `<sys/…>` interfaces outside the `*_linux.*`, `*_posix.*`, `*_gl.*`,
`*_egl.*` and `*_wayland.*` files. `<vulkan/…>` is refused everywhere under `src/`, with no exempt
file (no Vulkan on any OS).

## Shaders

Every GPU kernel has hand-written **HLSL and MSL twins**. The HLSL lives in the D3D11 backend as
`gfx/blit.hlsl` and `gfx/video_blit.hlsl`, compiled at **build time** by the Windows SDK's `fxc`
(SM 5.0, `/O3`) into one `const BYTE` header per entry point, so a device create (launch, device
lost, adapter change) never runs `D3DCompile` (issue #163; CMake `mv_fxc`). The MSL is embedded in
the Metal backend (`gfx/blit_metal.mm`, `gfx/video_blit_metal.mm`) and compiled from source at
startup (`newLibraryWithSource`). Constant-buffer layouts are pinned with `static_assert`s on both sides. The
colour-adjust kernel is written once ([`src/gfx/adjust_kernel.h`](../../src/gfx/adjust_kernel.h)) in
the syntax subset HLSL, MSL and C++ share, and pasted into both shaders and the C++ export bake, so the
twins are the same text by construction. The macro itself is in `gfx/adjust_kernel_tokens.h`, which
holds no C++, so `blit.hlsl` takes it through fxc's preprocessor and the MSL takes its stringified text. There is no cross-compiler, no SPIR-V and no shared bytecode.

## Milestone F — the Mac host

The Mac host was built as the Mac halves of PRs 1–8 (formerly Milestone F, PRs 16–20):

| Mac PR | What the Mac host has |
|---|---|
| 1 — Metal present lab | AppKit window, `CAMetalLayer`, `CAMetalDisplayLink` pacing, idle → stop presenting, F3 overlay with real present-to-present intervals, `frametime` on Darwin |
| 2 + 7 — stills and formats | The shared decoders and LCMS policy; immutable Metal upload from the worker pool; fit / wheel-zoom-toward-cursor / drag-pan; TIFF, WebP, ICO, HEIC/HEIF, AVIF, RAW with embedded-preview first pixel, tiled pyramid, broken-file corpus and fuzz harnesses, Crashpad |
| 3 + 4 + 6 — chrome and browse | SwiftUI command bar, filmstrip and gallery over the shared folder model and JPEG-512 thumbnail cache (`jpg512.1`); FSEvents watch; keyboard-complete browse with the Mac default map, `?` overlay, marks, copy-to / move-to, Trash with confirm, drag in and out, argv, fullscreen, slideshow, animated GIF/APNG/WebP on the display-link clock |
| 5 — video | FFmpeg + VideoToolbox on our `MTLDevice`, copy into our presentation ring, NV12/P010, YUV→RGB and HDR→SDR in MSL, Core Audio master clock, no-audio fallback to host time |
| 8 — ship | UTIs for the still set with one "Open with" registration, Quick Look in a separate process, notarized drag-install disk image, Sparkle updates |

## The Linux host, so far

The plan builds Linux's halves of PRs 1–8 first (its recommended order, open call 8 in
[§10](../plans/linux.md#10-open-owner-calls)); the tables above and below gain a Linux column as
they land.

| Linux PR | What the Linux host has |
|---|---|
| 1 — OpenGL present lab | A raw Wayland xdg toplevel (no toolkit; xkbcommon keys, pointer, fractional scale, `suspended`), EGL desktop GL 3.3 core on an ARGB8888 sRGB surface, `eglSwapInterval(0)` with the frame-callback wait before recording, `wp_presentation` statistics, idle → stop presenting, F3 (ImGui OpenGL 3), `frametime` on Linux ([03](03-rendering.md#the-linux-present-path)); workers named and niced in `core/job_system_posix.cpp`. Built and smoke-tested under headless Weston in CI; its gate is owed on a real session |

## Feature homes per platform

Every feature exists on both platforms; this table records where the per-OS half lives.

| Feature | Shared | Windows half | macOS half |
|---|---|---|---|
| Worker pool | `core/job_system.cpp` (queues, lanes, cancellation) | `core/job_system_win.cpp`: thread name, `THREAD_PRIORITY_BELOW_NORMAL` | `core/job_system_posix.cpp`: thread name, QoS (UTILITY; USER_INITIATED while a view-tied job runs); an autorelease pool around each job and completion (`core/job_system_platform.h`) |
| Present, idle-stop, pacing gate (PR 1) | `gfx/pacer` policy; one `gfx/frame_histogram` scores both pacers | DXGI lab | Metal lab |
| Stills, colour, pan/zoom (PR 2, 7) | `codec/`, `image/`, `canvas/` | D3D11 upload + HLSL blit | Metal upload + MSL blit |
| Chrome (PR 3) | ABI | WinUI islands | SwiftUI in AppKit |
| Folder, filmstrip, gallery, thumbs (PR 4) | folder ABI; the JPEG-512 thumb cache, one `image/thumb*` implementation (pixels, encode, SQLite index) | `io/dir_win.cpp` | `io/dir_mac.cpp`, `shell/folder_model_mac.cpp` |
| Video (PR 5) | demux, clock policy, transport | D3D11VA + WASAPI | VideoToolbox + Core Audio |
| Keyboard browse, slideshow, delete (PR 6) | command table, key router | Recycle Bin | Trash, `⌘` map |
| Metadata read, overlays (PR 9) | `meta/` | WinUI pane | SwiftUI pane |
| Geometry, lossless rotate, adjust (PR 10–11) | `edit/` | HLSL kernels | MSL twins |
| Rating / orientation / comment, XMP sidecar (PR 12) | `meta/` writers | `ReplaceFileW` | POSIX `rename` |
| Trim, extract, remux (PR 13–14) | `edit/clip_*` | NVENC / QSV / AMF / MF | VideoToolbox encode |
| OS integration (PR 15) | thumb request | Explorer associations, OOP thumbnail handler | Quick Look, Spotlight importer, Dock menu, Share |
| Import add-on (PR 16–19) | `addons/import`, BLAKE3, `import.db` | WinUI Import chrome | SwiftUI Import chrome ([18-import.md](18-import.md)) |
| Local AI search (PR 20–24) | `infer/`, `addons/ai` | ONNX Runtime | ONNX Runtime + Core ML ([17-local-ai-search.md](17-local-ai-search.md)) |
| Edit workspace, Video Editor (PR 29–31) | `edit/` | WinUI | SwiftUI ([20](20-edit-workspace.md), [21](21-video-editor.md)) |
| Local search in Final Cut Pro (issue #71) | `nle/` | FCPXML export only | Agent + FCP extension ([23-nle-search.md](23-nle-search.md)) |

## Distribution, per OS

Neither app store (the app is GPL; [11-licensing.md](11-licensing.md)).

| | Windows | macOS |
|---|---|---|
| First install | Inno Setup wizard (`tools/package/mediaviewer.iss`) | Branded `.dmg` with the GPL shown on mount; drag to `/Applications` or `~/Applications` (`tools/mac/macpack.py`) |
| Install location | Per-user `%LocalAppData%\MediaViewer`, never elevated | The dragged `.app`; no `.pkg`, no root, no helper |
| Update | Velopack, signed manifest | Sparkle 2, EdDSA-signed appcast |
| Uninstall | Inno uninstaller | Drag to Trash (removes the Quick Look extension) |
| Signing | Azure Trusted Signing | Developer ID, notarized and stapled |

The privacy line is the same on both: nothing about a user's files leaves the machine
([13-updates-and-telemetry.md](13-updates-and-telemetry.md)).

## Not built

- Windows ARM64.
- The ImageIO HEIC fast path on macOS (the OS-codec hook always declines).
- The Voice query add-on (PRs 27–28, [19-voice.md](19-voice.md)).
- Frame-pacing measurement on Intel Macs.
- Linux past PR 1: decode, chrome, video, packages ([plan](../plans/linux.md)); X11; the Linux PR 1
  gate on real hardware.
