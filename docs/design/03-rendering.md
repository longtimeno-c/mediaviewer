# 03 — Rendering & Frame Pacing

How the canvas presents: swapchain setup, DirectComposition hosting, the frame-pacing rules, adapter and display handling, colour, and resampling. The Metal side is in [15-platforms.md](15-platforms.md); the Linux side (OpenGL via EGL on Wayland, Linux PR 1) is [below](#the-linux-present-path).

## Swapchain setup

[`src/gfx/swapchain.cpp`](../../src/gfx/swapchain.cpp):

```
CreateSwapChainForComposition(
  BufferCount   = 3,
  SwapEffect    = DXGI_SWAP_EFFECT_FLIP_DISCARD,
  Format        = DXGI_FORMAT_R8G8B8A8_UNORM            // RTV is R8G8B8A8_UNORM_SRGB, so writes are sRGB-encoded (D6)
                  (R16G16B16A16_FLOAT when swapchain_desc::hdr_output — wired, always false),
  AlphaMode     = DXGI_ALPHA_MODE_PREMULTIPLIED,
  Flags         = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING  (when the factory reports support) )
IDXGISwapChain2::SetMaximumFrameLatency(1)
h = GetFrameLatencyWaitableObject()   // render thread waits on this, not on a timer
MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER)
```

- **Flip model.** No extra copy through the DWM per frame.
- **Waitable object.** The render thread waits on it *before* recording a frame, never after
  `Present`; that keeps input-to-photon at one frame and stays in step with the compositor.
- **Present** is `Present(1, 0)`. `ALLOW_TEARING` is requested at creation when supported, but a
  composition swapchain cannot tear, so the app always presents with vsync.
- **Device loss.** `DXGI_ERROR_DEVICE_REMOVED` / `DEVICE_RESET` rebuild the device and swapchain and
  re-upload from the CPU-side copy the session keeps. Rebuild bumps the job generation so in-flight
  `CreateTexture2D` calls against the old device are never published onto the new one. A test covers
  it ([`tests/swapchain_recovery.cpp`](../../tests/swapchain_recovery.cpp)).
- **Handle ownership.** `GetFrameLatencyWaitableObject` transfers ownership; `swapchain::destroy`
  closes it.

## DirectComposition

The swapchain is the content of an `IDCompositionVisual`, the root of a DComp target on the app's
top-level window (created with `WS_EX_NOREDIRECTIONBITMAP`). The target is created with
`topmost = FALSE`, so the WinUI island child windows draw above the canvas. When the canvas moves
(e.g. into the Video Editor window), the render thread retargets its DComp visual to the new host
window; there is still one swapchain and one present path.

## Frame pacing rules

1. **Never `Sleep`.** The render thread waits only on the swapchain's waitable object, its wake event,
   or the session's image-ready handle.
2. **No fixed-step animation.** Every animation steps by real elapsed QPC time. Pan/zoom settle uses
   critically damped springs (`ω = 18`, `ζ = 1`; [`src/canvas/spring.h`](../../src/canvas/spring.h)),
   which are interruption-safe: grabbing the image mid-settle continues from the current velocity.
3. **Uploads are budgeted.** Texture creation counts against the frame whether it runs on the render
   thread or a worker, because `ID3D10Multithread` serializes worker creates with the immediate context.
   Tiled images create at most 4 tiles per 16 ms tick with at most 24 requests outstanding
   (`src/image/tiles.h`); the F3 overlay shows per-frame upload cost for animations. The target is
   about 2 ms of creates per frame. An untiled still (up to the ~64 MP tile threshold) goes up as
   one `CreateTexture2D` with its whole mip chain, which cannot fit a frame. So a neighbour's
   create over `image::k_frame_budget_create_bytes` (8 MiB) waits on its worker, after the CPU
   mips, for as long as the loop presents a moving camera (pan, zoom, drag:
   `abi::set_canvas_moving`); the image on screen never waits (issue #166,
   [12-decision-log.md](12-decision-log.md)). The create time and the waits are measured
   (`image::texture_upload_stats`, Windows), shown on the F3 overlay's `texture` line, and
   written to the soak report as `texture_max_create_ms` / `texture_max_create_mb` /
   `texture_creates_over_budget` / `texture_creates_deferred` / `texture_max_defer_ms`.
   Windows only: the Mac's `replaceRegion` upload takes no device-wide lock.
4. **Render on demand.** A still image that has been painted stops presenting entirely (0 presents,
   ~0 % GPU). The loop presents every vblank while anything is live: video, an animation, a moving
   spring, a fade, or the **500 ms tail after the last input** (so a flick does not stutter at its end).
   Viewer overlays (info, AF points, pixel grid, loupe) are draws in the same present and idle-stop with
   it, as is the two-up compare (`|`): the previous still is a second draw of its resident texture
   into the left half of the canvas, under the current still's camera ([16](16-commands.md)). **Clipping blinkies are the exception**: they animate, so they present until toggled off; they
   are off by default ([16-commands.md](16-commands.md)).
   **The idle wait has no timeout** (except occlusion probes, soak deadlines and a pending video-gap
   deadline). A worker finishing a texture the canvas should show signals the session's image-ready
   handle, which wakes the render thread; device-loss re-upload uses the same wake.
5. **Occlusion.** On `DXGI_STATUS_OCCLUDED` from `Present` the loop stops rendering and probes with
   `Present(0, DXGI_PRESENT_TEST)` every 200 ms (5 Hz) until visible.

Presenting also requires the window to be visible and (outside soaks, unless
`present_when_inactive` is set) active.

## Adapter selection & hybrid GPUs

Decode and present share one `ID3D11Device`; FFmpeg's D3D11VA context is created from it
([05-video-pipeline.md](05-video-pipeline.md)), so there is no cross-adapter copy per frame.

- The device is created on the adapter that drives **the monitor the window is on**
  (`MonitorFromWindow`, then `EnumAdapters1` → `EnumOutputs`, skipping WARP). If no adapter claims the
  monitor (a hybrid laptop where the iGPU drives the panel through the dGPU, a remote session), it falls
  back to `EnumAdapterByGpuPreference(HIGH_PERFORMANCE)`, not adapter 0
  ([`src/gfx/device.cpp`](../../src/gfx/device.cpp)).
- `WM_DISPLAYCHANGE` and `WM_MOVE` bump a display-change sequence in the input snapshot. The render
  thread then calls `device::adapter_changed_for(window)`; if the adapter changed it rebuilds through
  the same path as device removal, which also tears down and recreates the video blitter.

## DPI & multi-monitor

The app manifest declares `PerMonitorV2`. `WM_DPICHANGED` updates the snapshot's size and DPI scale;
the swapchain is resized on the next frame and the image is resampled on the GPU.

The refresh rate is read per display path, because `IDXGISwapChain::GetContainingOutput` is invalid on
composition swapchains: the host monitor is matched to an active `QueryDisplayConfig` path and that
path's rational `refreshRate` is used. An unknown rate, or a cloned source whose targets disagree, is
**zero** — the frame-time gate cannot pass on a guessed 60 Hz. It is read when the swapchain is created
or moved to another window, and re-read on display change and window move — never on a resize, which
cannot change it and would otherwise query the display topology every frame of a drag-resize.

## The Linux present path

Linux PR 1 ([plan](../plans/linux.md)): the OpenGL present lab on Wayland. One present path, EGL
on a `wl_surface` C++ owns; never a toolkit's frame clock. No GLES fallback, no Vulkan.

- **Device** ([`src/gfx/device_gl.cpp`](../../src/gfx/device_gl.cpp)). `eglGetPlatformDisplay
  (EGL_PLATFORM_WAYLAND_KHR)` on the host's connection, desktop GL 3.3 core (the driver hands back
  its newest core version), an exact ARGB8888 config, `EGL_LOSE_CONTEXT_ON_RESET` robustness when
  the driver offers it. A reset rebuilds device and surface on the same `wl_surface`, as Windows'
  `rebuild_device()` does. The DRM render node (`EGL_EXT_device_query`) is shown in F3.
- **Surface** ([`src/gfx/gl_surface_wayland.cpp`](../../src/gfx/gl_surface_wayland.cpp)). A
  `wl_egl_window` on a *borrowed* surface (Linux PR 3 passes Qt's child surface).
  `EGL_GL_COLORSPACE_SRGB` and `GL_FRAMEBUFFER_SRGB`, so writes are sRGB-encoded (D6); alpha is
  written as 1 and the opaque region covers the surface. Both the EGL colour space and the
  framebuffer's encoding are checked; a surface that is not sRGB on both is refused (exit 2).
  `eglSwapInterval(0)`.
- **The wait before recording** ([`src/gfx/wayland_frame_clock.cpp`](../../src/gfx/wayland_frame_clock.cpp)).
  On a private event queue: before `eglSwapBuffers` the render thread requests the next
  `wl_surface.frame` callback and a `wp_presentation` (v1) feedback; before it records the next
  frame it waits for that callback. One frame in flight, never a second commit over an outstanding
  callback. The wait also wakes on the lab's eventfd (input, stop). The buffer count is the
  driver's; the callback rule bounds what is in flight.
- **Idle.** Not live and painted: nothing is committed, so the compositor sends nothing back
  (0 presents, ~0 % CPU); the idle wait has no timeout outside soaks and occlusion.
- **Occlusion.** A callback that has not come in 200 ms means the surface is hidden: the loop stops,
  and the still-outstanding callback wakes it when the surface is shown. `xdg_toplevel`'s
  `suspended` state makes the window not visible in the snapshot.
- **The gate's truth** ([`src/gfx/presentation_pacer.cpp`](../../src/gfx/presentation_pacer.cpp)).
  Each feedback answer is joined to the frame it is for. Authoritative only with `VSYNC` and
  `HW_CLOCK` and a way to count refreshes: a step of n refreshes is n − 1 missed, counted by the
  vblank counter (msc) or, where the compositor sends seq 0 with both flags (KWin always does), by
  the hardware timestamps over the constant refresh; `discarded` is a dropped frame. Timestamps stay in the compositor's `clock_id` domain and are only differenced;
  intervals and springs are on `CLOCK_MONOTONIC`. The report is labelled "Wayland presentation
  feedback"; anything weaker is "clock intervals (weaker)" and cannot pass. Headless Weston gives
  flags 0 and seq 0, so it proves the plumbing, never pacing.
- **Refresh.** The compositor's own prediction in each presented answer, the outputs' current mode
  until one comes (0 when the outputs the surface is on disagree). Re-read on an output change,
  never on a resize. A refresh of 0 (a variable-rate output) or a change of more than 0.1 %
  invalidates a soak.
- **Size and scale.** The window acks each configure on the render thread's commit of its size;
  fractional scale is `wp_fractional_scale_v1` with a viewport onto the logical size, buffer
  scale 1, the buffer `round(logical × scale)`.
- **Threads.** `mv.render` at the default priority (Linux has no unprivileged MMCSS); the workers
  run five nice levels below the thread that started the pool.

X11 is not built yet; when it is, it will run through EGL on the child X window and not be gated
(it has no wait before recording).

## Color management & HDR

Working space and swapchain are separate (**D6**, [01-decisions.md](01-decisions.md)): the edit working
space is linear FP16 with Rec.709 primaries; the viewer path is 8-bit sRGB textures blitted to an 8-bit
sRGB swapchain. The blit samples the sRGB texture as linear light and the sRGB render target view
encodes on write.

**Colour policy: an untagged image is sRGB; a tagged image is never treated as sRGB.**

- Decode converts from the file's colour space — the embedded ICC profile (`APP2` / `iCCP` / `colr`)
  via Little CMS, or CICP/nclx primaries and transfer for HEIF/AVIF — to sRGB
  ([`src/image/colour.cpp`](../../src/image/colour.cpp)).
- Every decoded raster carries a `transfer_intent` ([`src/codec/raster.h`](../../src/codec/raster.h)).
  **Display-referred** sources (JPEG, HEIC, PNG, BMP, GIF, WebP, TIFF — nearly every file) go ICC →
  linear → sRGB with **no tone map**; tone-mapping a camera JPEG crushes its highlights. Every still
  decoder produces `display_referred`; the colour stage refuses `scene_referred`.
- CICP descriptions ([`src/codec/cicp.h`](../../src/codec/cicp.h), shared by HEIF and AVIF): SDR with
  BT.709 primaries and an sRGB-like curve is copied through; any other SDR primaries or curve (Display
  P3 above all) get a synthesised ICC v4 matrix/shaper profile for Little CMS; **PQ/HLG stills are
  tone-mapped to SDR at decode** with the same curves and constants as the video shader.
- **HDR video → SDR is tone-mapped** in the same shader that does YUV → RGB
  ([`src/gfx/video_blit.hlsl`](../../src/gfx/video_blit.hlsl), MSL twin `video_blit_metal.mm`), driven by
  the stream's real matrix, primaries and transfer: PQ through the ST 2084 EOTF normalised to 203-nit
  reference white; HLG through the inverse OETF plus OOTF; then Reinhard-extended on luminance with the
  ratio applied to RGB to keep hue. iPhone HLG HEVC therefore does not look washed out on an SDR
  display. Dolby Vision metadata is not used.
- **HDR display output is not built.** There is no HDR-output detection; the FP16 scRGB swapchain
  branch exists but is always off.

## Resampling quality

**Zoomed out.** Each viewer texture carries a full mip chain built on the CPU with a separable Mitchell
filter (B = C = 1/3) in linear light, sRGB-encoded per level
([`src/image/upload.cpp`](../../src/image/upload.cpp)) — not `GenerateMips`, whose box filter shimmers
while panning. Level sizes follow D3D11: `max(1, floor(prev / 2))`; ceil halving produces a chain
`CreateTexture2D` rejects for odd sizes. Sampling between levels is trilinear with 16× anisotropy.
Tiled images use a floor-half Mitchell CPU pyramid with 256×256 tiles (2-texel border) over a resident
overview ([04-image-pipeline.md](04-image-pipeline.md)). Tile records go up in one per-instance
buffer write per frame, not a constant-buffer rename per tile, and once the LOD's tiles cover the
view neither the coarser levels nor the overview are drawn under them (issue #164).

**Zoomed in** ([`src/gfx/blit.hlsl`](../../src/gfx/blit.hlsl)): Catmull-Rom between 100 % and 400 %,
nearest-neighbour above 400 % (with an optional one-pixel grid). The Catmull-Rom result is saturated:
unclamped overshoot encodes per channel on the sRGB target as coloured fringes along edges.

The same filters are in the Metal blit (`src/gfx/blit_metal.mm`).

## Not built

- A compute-shader (Kaiser/Mitchell) mip pass; mips are built on the CPU.
- A Lanczos-3 "settled" high-quality pass for the fit view.
- DirectComposition animations for chrome; chrome animation is WinUI's.
- HDR display output (scRGB swapchain, `SetColorSpace1`, SDR white level).
- Variable refresh / tearing presents in the shipped app.
