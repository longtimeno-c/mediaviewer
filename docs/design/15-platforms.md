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

## The call

v1 shipped on Windows first; the core was kept hostable from PR 4 so the Mac app could be a host plus
backends instead of a rewrite. The Mac halves of PRs 1–8 followed, and both platforms move together
from PR 9.

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
`player/`, `edit/`, `addon/`, `addons/` and `io/*.h`.

## Shaders

Every GPU kernel has hand-written **HLSL and MSL twins**: the HLSL is embedded in the D3D11 backend
(`gfx/blit.cpp`, `gfx/video_blit.cpp`, the edit kernels) and the MSL in the Metal backend
(`gfx/blit_metal.mm`, `gfx/video_blit_metal.mm`), compiled from source at startup
(`newLibraryWithSource`). Constant-buffer layouts are pinned with `static_assert`s on both sides. The
colour-adjust kernel is written once ([`src/gfx/adjust_kernel.h`](../../src/gfx/adjust_kernel.h)) in
the syntax subset HLSL, MSL and C++ share, and pasted into both shaders and the C++ export bake, so the
twins are the same text by construction. There is no cross-compiler, no SPIR-V and no shared bytecode.

## Milestone F — the Mac host

The Mac host was built as the Mac halves of PRs 1–8 (formerly Milestone F, PRs 16–20):

| Mac PR | What the Mac host has |
|---|---|
| 1 — Metal present lab | AppKit window, `CAMetalLayer`, `CAMetalDisplayLink` pacing, idle → stop presenting, F3 overlay with real present-to-present intervals, `frametime` on Darwin |
| 2 + 7 — stills and formats | The shared decoders and LCMS policy; immutable Metal upload from the worker pool; fit / wheel-zoom-toward-cursor / drag-pan; TIFF, WebP, ICO, HEIC/HEIF, AVIF, RAW with embedded-preview first pixel, tiled pyramid, broken-file corpus and fuzz harnesses, Crashpad |
| 3 + 4 + 6 — chrome and browse | SwiftUI command bar, filmstrip and gallery over the shared folder model and JPEG-512 thumbnail cache (`jpg512.1`); FSEvents watch; keyboard-complete browse with the Mac default map, `?` overlay, marks, copy-to / move-to, Trash with confirm, drag in and out, argv, fullscreen, slideshow, animated GIF/APNG/WebP on the display-link clock |
| 5 — video | FFmpeg + VideoToolbox on our `MTLDevice`, copy into our presentation ring, NV12/P010, YUV→RGB and HDR→SDR in MSL, Core Audio master clock, no-audio fallback to host time |
| 8 — ship | UTIs for the still set with one "Open with" registration, Quick Look in a separate process, notarized drag-install disk image, Sparkle updates |

## Feature homes per platform

Every feature exists on both platforms; this table records where the per-OS half lives.

| Feature | Shared | Windows half | macOS half |
|---|---|---|---|
| Present, idle-stop, pacing gate (PR 1) | `gfx/pacer` policy | DXGI lab | Metal lab |
| Stills, colour, pan/zoom (PR 2, 7) | `codec/`, `image/`, `canvas/` | D3D11 upload + HLSL blit | Metal upload + MSL blit |
| Chrome (PR 3) | ABI | WinUI islands | SwiftUI in AppKit |
| Folder, filmstrip, gallery, thumbs (PR 4) | folder ABI, SQLite thumb cache `jpg512.1` | `io/dir_win.cpp` | `io/dir_mac.cpp`, `shell/folder_model_mac.cpp` |
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
