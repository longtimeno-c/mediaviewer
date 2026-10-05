# 03 — Rendering & Frame Pacing

How the canvas presents: swapchain setup, DirectComposition hosting, the frame-pacing rules, adapter and display handling, colour, and resampling. The Metal side is in [15-platforms.md](15-platforms.md).

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
   about 2 ms of creates per frame.
4. **Render on demand.** A still image that has been painted stops presenting entirely (0 presents,
   ~0 % GPU). The loop presents every vblank while anything is live: video, an animation, a moving
   spring, a fade, or the **500 ms tail after the last input** (so a flick does not stutter at its end).
   Viewer overlays (info, AF points, pixel grid, loupe) are draws in the same present and idle-stop with
   it. **Clipping blinkies are the exception**: they animate, so they present until toggled off; they
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
**zero** — the frame-time gate cannot pass on a guessed 60 Hz. It is re-read on display change and
window move.

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
  ([`src/gfx/video_blit.cpp`](../../src/gfx/video_blit.cpp), MSL twin `video_blit_metal.mm`), driven by
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
overview ([04-image-pipeline.md](04-image-pipeline.md)).

**Zoomed in** ([`src/gfx/blit.cpp`](../../src/gfx/blit.cpp)): Catmull-Rom between 100 % and 400 %,
nearest-neighbour above 400 % (with an optional one-pixel grid). The Catmull-Rom result is saturated:
unclamped overshoot encodes per channel on the sRGB target as coloured fringes along edges.

The same filters are in the Metal blit (`src/gfx/blit_metal.mm`).

## Not built

- A compute-shader (Kaiser/Mitchell) mip pass; mips are built on the CPU.
- A Lanczos-3 "settled" high-quality pass for the fit view.
- DirectComposition animations for chrome; chrome animation is WinUI's.
- HDR display output (scRGB swapchain, `SetColorSpace1`, SDR white level).
- Variable refresh / tearing presents in the shipped app.
