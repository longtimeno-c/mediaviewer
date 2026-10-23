# Plan — Linux, a third host

MediaViewer on Linux as a third host of the same native core: its own present path, pacing gate,
hardware decode, audio clock, I/O, chrome and packaging, not a port of either existing host. The
owner made the call on 2026-10-23 (D9 amended, [12](../design/12-decision-log.md#platforms)).
Written 2026-10-23. It is a forward plan, not a description of the product; when a slice lands,
move what it built into the matching `docs/design/` doc and strike the slice here.

## Status (2026-10-23)

- The shared core already builds headless on Linux: `cmake/portable` runs the Import, add-on,
  AI and clip suites in CI ([09](../design/09-build-and-test.md)), and from 2026-10-23 Driftwire's
  too.
- The first Linux product is Driftwire's command line (`driftwire`, .deb / .rpm / tarball), a
  terminal host of the Driftwire engine ([28](../design/28-driftwire.md)). It needs no window and
  ships ahead of the app.
- Linux PR 1 (below) is the first slice of the app. Nothing of the viewer runs on Linux yet.

## 0. The scope change (done in this slice)

- **D9** now names three hosts. The owner's calls: OpenGL via EGL, Wayland first; X11 runs but is
  not gated; Qt 6 / QML chrome with the canvas a child surface C++ presents into on its own render
  thread; GLSL the third hand-written shader twin; "No Vulkan" stands on every OS; the Linux halves
  of PRs 1–8 first, then the Linux halves of what has landed since, then every PR on all three.
- **D1** gains the Linux chrome (Qt 6 / QML). The present lab and the F3 overlay stay ImGui
  (OpenGL 3 binding), never shipped chrome.
- Until the Linux host has caught up, a PR stays dual-track (Windows and macOS halves, both
  present-loop gates), and its shared code must not take a dependency a Linux host could not
  replace. That is D9's hostable-core rule, applied to a third host now rather than later.

## 1. What is shared, what Linux owns

| | Windows | macOS | Linux |
|---|---|---|---|
| Window and canvas | Win32 window, D3D11 composition swapchain in a DComp visual | AppKit window, `CAMetalLayer` | Qt window; the canvas a child `QWindow` (a desync `wl_subsurface` on Wayland, a child X window on X11) with an EGL window surface C++ owns |
| Pacing | Frame-latency waitable object, `SetMaximumFrameLatency(1)` | `CAMetalDisplayLink` | `eglSwapInterval(0)`; wait for the previous commit's `wl_surface.frame` callback on a private event queue, then record; `wp_presentation` feedback for the gate |
| Chrome | C# WinUI 3 in XAML islands | SwiftUI in `NSHostingView`s | Qt 6 / QML (`src.qt/`), talking to the core over the same C ABI |
| Present lab / F3 | ImGui DX11 | ImGui Metal | ImGui OpenGL 3 |
| Shaders | HLSL, fxc at build time | MSL from source at startup | GLSL from source at startup (the driver compiles it) |
| Video decode | FFmpeg + D3D11VA | FFmpeg + VideoToolbox | FFmpeg + VA-API, frames exported as dma-buf and imported with `EGL_EXT_image_dma_buf_import`; NVIDIA through NVDEC with CUDA–GL interop (open call 1) |
| Audio clock | WASAPI shared | Core Audio output unit | PipeWire native stream (`pw_stream_get_time_n`), libpulse where there is no PipeWire audio graph (open call 2) |
| Hardware encode | NVENC / QSV / AMF / MF | VideoToolbox | VA-API encode, NVENC (LGPL FFmpeg; no x264/x265) |
| Directory watch | `ReadDirectoryChangesW` | FSEvents | inotify (polling on network mounts, where inotify sees nothing) |
| Atomic replace | `ReplaceFileW` | `rename` | `rename`; `renameat2(RENAME_NOREPLACE)` where nothing may be replaced |
| Paths | Known folders | `~/Library/…` | XDG base and user dirs |
| Trash | Recycle Bin | Trash | the freedesktop Trash (`QFile::moveToTrash`) |
| Thumbnails in the OS | Explorer handler in a surrogate | Quick Look extension | a `.thumbnailer` entry under `/usr`, run out of process (GNOME runs it inside bwrap) |
| File associations | ProgId + Default Apps link | UTIs, "Open with" | `.desktop` `MimeType=` (Open With); a default only on an explicit user action |
| Install / update | Inno Setup + Velopack | `.dmg` + Sparkle | signed apt and dnf repositories (open call 4) |
| Crash reports | Crashpad + scrub | Crashpad | Crashpad, same scrub and privacy line (open call 6) |

Shared, unchanged: `codec/`, `image/` colour, `edit/` op graph, `meta/`, `canvas/` springs,
`core/` jobs, `addon/`, `infer/`, the C ABI, the command table and key router.

## 2. The canvas: OpenGL via EGL on a surface C++ owns

- **One present path**: EGL window surface on the canvas's own `wl_surface`, desktop GL 3.3 core
  (4.x where offered), `EGL_GL_COLORSPACE_SRGB_KHR` on an ARGB8888 config with
  `glEnable(GL_FRAMEBUFFER_SRGB)` and an opaque region (D6: linear working space, 8-bit sRGB
  swapchain). No GLES fallback, no Vulkan, no second path.
- **Pacing, the waitable-object analogue**: the render thread owns a `wl_event_queue` with proxy
  wrappers for the surface and its own `wp_presentation`. Before `eglSwapBuffers` it requests the
  next frame callback and a presentation feedback; it never commits while a callback is
  outstanding (one frame in flight, `SetMaximumFrameLatency(1)`); it waits for the callback before
  sampling input and stepping springs, with a timeout. Idle: commit nothing (0 presents, ~0 % CPU).
  `eglSwapInterval(1)` would block inside the swap, after recording, so it is never used.
- **The gate's truth**: `wp_presentation_feedback.presented` (timestamp in its `clock_id` domain,
  refresh, msc, flags). Authoritative only with `VSYNC` and `HW_CLOCK` and a moving msc; a
  `discarded` frame is dropped. Headless Weston reports neither flag, so it can prove plumbing,
  never pacing.
- **Uploads from workers**: each decode worker has a surfaceless EGL context shared with the
  render context; it creates immutable textures (`glTexStorage2D` + `glTexSubImage2D`), fences
  (`glFenceSync` + `glFlush`) and pushes `{texture, fence}` on the SPSC ring; the render thread
  waits on the GPU (`glWaitSync`) or skips a texture whose fence has not signalled. The 2 ms upload
  budget and the 512 MB LRU are unchanged.
- **Device loss**: `EGL_EXT_create_context_robustness`; on a reset, rebuild as Windows'
  `rebuild_device()` does.
- **Qt and the canvas**: the canvas `QWindow` is a `RasterSurface` with no backing store, so Qt
  never attaches a buffer; C++ takes its `wl_surface` through the QPA native interface (Qt ≥ 6.5),
  creates the `wl_egl_window`, and stops and rebuilds on the surface's destroy/create (Qt recreates
  it across hide/show). Qt owns the surface's viewport and fractional-scale objects: the canvas
  renders at logical size × `devicePixelRatio()` and never creates its own. **Airspace**: QML
  cannot overlap the canvas, so anything drawn over it (the F3 overlay is ImGui on the canvas; a
  popover or the slideshow bar) is its own child window, as rule 2's island fallback allows.
- **X11**: the child `QWindow` is a child X window; EGL on its XID through
  `EGL_PLATFORM_X11_KHR`, with the config whose native visual matches Qt's. X11 has no wait before
  recording, so it runs but is not gated; F3 says so.

## 3. Chrome: Qt 6 / QML

- `src.qt/` beside `src.swift/` and `src.managed/`; C++ and QML against the C ABI only (the chrome
  never sees `src/` internals), stores that read the bridge when the event loop has done work (the
  Mac's `ChromePulse` pattern, no timers on a still).
- Qt is LGPL-3 and dynamic, bundled at a pinned version (≥ 6.8 for `WindowContainer`): Ubuntu
  22.04 ships 6.2 and 24.04 ships 6.4. Accessibility over AT-SPI, IME, Wayland and X11.
- The key map uses Ctrl where Windows does (the Mac uses ⌘); key repeat is synthesised on
  Wayland (it sends none). Same command ids ([16](../design/16-commands.md)).

## 4. Video and audio

- FFmpeg (LGPL, dynamic) with VA-API: decode on the decoder's own device, `av_hwframe_map` to
  DRM PRIME on a decode worker (it blocks in `vaSyncSurface`, so never on the render thread), import
  NV12 as R8 + RG88 and P010 as R16 + RG1616, copy on the GPU into our presentation ring (never
  bind the decoder's pool as the source), YUV→RGB and HDR→SDR in the GLSL video twin (never
  assume BT.709 limited range).
- NVIDIA: NVDEC through FFmpeg's CUDA hwaccel and `cuGraphicsGLRegisterImage` copies into our
  textures. `nvidia-vaapi-driver` is not a product path (its own README says so).
- The audio master clock: a PipeWire stream with `pw_stream_get_time_n` (PipeWire ≥ 0.3.50), or
  libpulse with interpolated timing. Both are shared-mode streams into the mixing server, the
  WASAPI-shared analogue. No-audio clips fall back to `CLOCK_MONOTONIC`.

## 5. Files and the desktop

- `io/dir_linux.cpp` (`getdents64` + `statx`, inotify; network mounts polled), `io/paths_linux.cpp`
  (XDG), Linux volumes (`/proc/self/mountinfo`; eject through UDisks2), `core/job_system_posix.cpp`
  thread names and priorities.
- `.desktop` with `MimeType=` for the D5 still and video set; audio and documents as Open With only
  (D5). Never `xdg-mime default` without the person asking.
- The thumbnailer runs out of process from `/usr` (GNOME's bwrap sandbox binds only `/usr`): that,
  and the Open With entries, are why the app ships as native packages.

## 6. The GLSL twin

Every kernel gets a GLSL twin of its HLSL and MSL, by hand. `gfx/adjust_kernel.h` is written in the
syntax subset the three languages and C++ share; `gfx/adjust_kernel_tokens.h` gains the GLSL
spellings (`float3` → `vec3`, …) so the text stays one text. Compiled from source at startup by the
driver, as MSL is. No SPIR-V, no cross-compiler.

## 7. Distribution

Recommended: **signed `.deb` and `.rpm` from our own apt and dnf repositories**, the payload under
`/usr/lib/mediaviewer/` with Qt and FFmpeg bundled (dynamic). The package manager is the updater,
which meets "the updater exists before the first build that leaves this machine" only with a
signed repository; a loose `.deb` never updates. It is the one channel where the thumbnailer runs
and Open With works, and it is the channel Driftwire's command line uses too. AppImage with
Velopack is the runner-up (same updater family as Windows; no thumbnailer, no associations).
Flatpak is not: its sandbox fights a viewer that opens any folder and a LAN and Bluetooth tool, and
it exports no thumbnailer.

## 8. The rules, on Linux

| Rule | On Linux |
|---|---|
| 1: nothing blocks UI or render | `av_hwframe_map` and dma-buf export on workers; the render thread waits only on its frame callback and its wake eventfd |
| 2: a native swapchain C++ owns | the canvas's own EGL surface on a desync subsurface; never a Qt `Image`/`VideoOutput`, never GTK's frame clock |
| 3: first pixel is never the full decode | unchanged |
| 4: zero dropped frames panning at refresh | the Linux PR 1 gate, from `wp_presentation` feedback |
| 5: never modify an original | unchanged; `renameat2(RENAME_NOREPLACE)` for new files |
| 6: nothing leaves the machine | unchanged; the crash scrub covers `.so` stacks and `/home/<user>` paths |
| 7: never require a codec pack | FFmpeg and the decoders are bundled; VA-API/NVDEC come with the GPU driver; no distro codec package is required |

## 9. Slices

Each Linux slice is one PR; each also re-runs the Windows PR 1 and Mac PR 1 gates.

| Slice | Delivers | Verify |
|---|---|---|
| **0 — Scope** | D1 and D9 amended, decision-log rows, this plan | Docs only |
| **Linux PR 1 — OpenGL present lab** | A Wayland toplevel (xdg-shell), EGL, the frame-callback pacer and `wp_presentation` statistics, idle → stop presenting, F3 overlay (ImGui OpenGL 3), `frametime` on Linux, a headless-Weston smoke job in CI, a gated job on a self-hosted Linux GPU runner | Presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle, on a real Wayland session (Mutter or KWin) with Mesa on Intel or AMD, measured from `wp_presentation` feedback (VSYNC and HW_CLOCK). Under headless Weston it runs, reports, and labels its source weaker |
| **Linux PR 2 + 7 — stills and formats** | Worker uploads on shared contexts, the GLSL blit twin, fit / wheel-zoom / drag-pan, the D5 still set, RAW preview first pixel, pairing | As Mac PR 2 + 7: a 12 MP JPEG pans at refresh with zero decode on mouse move; tagged AdobeRGB correct, untagged sRGB, no tone-map; iPhone HEIC opens with no extra install; a CR2/NEF/ARW previews in JPEG-comparable time and refines without a pop; originals unchanged; the broken corpus neither crashes nor hangs |
| **Linux PR 3 + 4 + 6 — Qt chrome, folder, browse** | Qt 6 chrome in the window, the canvas a child surface, folder / filmstrip / gallery over the shared model and the `jpg512.1` cache, inotify, keyboard-complete browse, Trash | As Mac PR 3 + 4 + 6: zero dropped frames panning with chrome on screen; focus crosses the Qt / canvas boundary; a popup over the canvas does not clip; 2000 JPEGs scroll without a hitch; keyboard-only browse; a dropped file appears; animation timing matches a browser |
| **Linux PR 5 — video** | FFmpeg + VA-API (and NVDEC) into our ring, NV12/P010, the GLSL video twin with HDR→SDR, the PipeWire clock, transport | 4K 10-bit HEVC at full rate with hardware decode on Intel/AMD and NVIDIA, no extra package; an iPhone HLG clip looks right; A/V drift flat over 30 minutes; photo → video → photo leaks no textures |
| **Linux PR 8 — packages and the desktop** | Signed apt/dnf repositories, `.desktop` + MIME, the out-of-process thumbnailer, Crashpad | On clean Ubuntu LTS and Fedora: install from the repository, open a real camera dump; N → N+1 through the package manager and an unsigned repository refused; Open With lists MediaViewer without making it the default; a corrupt HEIC in a browsed folder leaves Files running; removing the package leaves nothing |
| **Linux halves of PRs 9–60** | In roadmap order: metadata, edits, trim, OS integration, Import, Local search, folder tiles, Edit workspace and Video Editor, copies, duplicates, open add-ons (screens drawn by Qt), Driftwire's window and a BlueZ bearer, Places | Each PR's own verify line, on Linux |
| **Tri-track** | From the PR after the catch-up, every PR has a Linux half | Every PR's verify on all three, plus all three PR 1 gates |

## 10. Open owner calls

1. **NVIDIA video**: NVDEC + CUDA–GL interop as a second import path beside VA-API (recommended),
   or VA-API only (NVIDIA decodes in software until then).
2. **Audio**: PipeWire native with a libpulse fallback (recommended), or libpulse only (runs
   everywhere through pipewire-pulse, one backend to keep).
3. **Floor and architectures**: Ubuntu 22.04+, Debian 12+, Fedora 40+ (glibc ≥ 2.35), x86_64 and
   arm64 (recommended); whether NVIDIA's driver gates Linux PR 1 or is measured and recorded.
4. **Channel and signing**: signed apt + dnf repositories (recommended) or AppImage + Velopack;
   who holds the repository signing key.
5. **Qt**: bundle a pinned Qt ≥ 6.8 (recommended; LGPL, dynamic) or require the distro's
   (rules out 22.04 and 24.04).
6. **Crash reports**: Crashpad on Linux with the same scrub (recommended) or none until PR 8.
7. **Bluetooth for Driftwire**: a BlueZ GATT bearer in its Linux half, or Wi-Fi and the internet
   only.

## Not in this plan

HDR output, an HDR swapchain, a GLES or Vulkan path, Flatpak, Snap, a Linux App Store, Wayland
colour-management protocols (they arrive with HDR, not before).
