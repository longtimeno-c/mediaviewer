# 03 — Rendering & Frame Pacing

This is where "smooth like butter" is won or lost. Everything else is correctness; this is feel.

## Swapchain setup

```
CreateSwapChainForComposition(  // or ForHwnd
  BufferCount   = 3,
  SwapEffect    = DXGI_SWAP_EFFECT_FLIP_DISCARD,
  Format        = sdr ? DXGI_FORMAT_R8G8B8A8_UNORM         // sRGB RTV encodes writes (D6)
                    : DXGI_FORMAT_R16G16B16A16_FLOAT,       // scRGB, when an HDR output is detected
  AlphaMode     = DXGI_ALPHA_MODE_PREMULTIPLIED,
  Flags         = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING )
IDXGISwapChain2::SetMaximumFrameLatency(1)
h = GetFrameLatencyWaitableObject()   // render thread waits on this, not on a timer
```

Non-negotiables:

- **Flip model.** BitBlt model costs you a full extra copy through the DWM every frame.
- **Waitable object.** This is how you get low latency *and* stay in step with the compositor.
  Waiting on `WaitForSingleObject(h)` before you begin the frame — not after Present — is what
  keeps input→photon at one frame.
- **`ALLOW_TEARING`** must be set at creation to be usable later; only pass it to `Present` when
  the display supports VRR and the user has it on. On a G-Sync/FreeSync panel this removes
  judder during panning entirely.
- Handle `DXGI_ERROR_DEVICE_REMOVED` by rebuilding the device and reuploading from the CPU-side
  cache. It *will* happen — driver updates, hybrid-GPU switches, sleep/resume. Rebuild **bumps
  the job generation** (or equivalent) so in-flight `CreateTexture2D` against the old device
  cannot be published onto the new one.
- **`GetFrameLatencyWaitableObject` transfers handle ownership.** Close it on destroy. Releasing
  the swapchain does not close it for you.

## DirectComposition

Host the swapchain in a `IDCompositionVisual` tree. This buys you:

- rounded corners / drop shadow / true transparency without a layered window (which is slow),
- **animations the OS runs at display rate even if your app misses a frame** — use DComp
  animations for chrome fades and panel slides,
- a native canvas island if `SwapChainPanel` cannot pace honestly (**D1**) — the fallback that
  keeps WinUI for chrome without putting XAML on the hot path.

Note that under D1 the shipped app composites through XAML, so `ALLOW_TEARING` and VRR are
unavailable and the flags below apply in full only to the PR 1 present lab. Everything else in this
document — the waitable object, the upload budget, springs, occlusion, resampling — applies
unchanged either way.

## Frame pacing rules

1. **Never `Sleep`.** Wait only on the swapchain object or a GPU fence.
2. **No fixed-step animation.** Drive every animation from real elapsed QPC time. Use critically
   damped springs (`ω=18, ζ=1`) for zoom/pan settle, not eased tweens — springs are
   interruption-safe, so grabbing the image mid-animation feels continuous instead of snapping.
3. **Uploads are budgeted.** At most ~2 ms of texture creates per frame, whether the render
   thread `Map`s a staging resource or a worker calls `CreateTexture2D` on the same
   multithread-protected device. `ID3D10Multithread` serializes with the immediate context; a
   60 MP immutable upload on a worker can still stall `Present`. "The worker did it" is not a
   pass around the hitch. The rest waits. A 100 MP TIFF landing must never cost you a dropped
   frame.
4. **Render on demand, but honestly.** Idle → stop presenting entirely (0 % GPU on a static
   image). Any input, animation, or video frame → present every vblank. Transition into the
   active state on the *first* input event, and keep presenting for ~500 ms after the last one so
   a flick-scroll doesn't stutter at the tail.
   Viewer overlays (info, AF points, pixel grid, loupe) are extra draws in this same present
   and still idle-stop when they are not animating. **Clipping blinkies are the exception**:
   they animate, so they present until toggled off. They are off by default
   ([16-commands.md](16-commands.md)).
   **Idle wait is `INFINITE` (or the soak deadline).** A worker finishing a texture the canvas
   should show must wake the render thread. Draining `MV_COMPLETION_IMAGE_OPENED` on the UI
   thread and only logging it leaves the image sitting in `ready` until the next mouse-move.
   The 500 ms input tail covers a flick, not a 60 MP decode. Device-loss re-upload has the same
   wake requirement.
5. **Occlusion.** On `DXGI_STATUS_OCCLUDED` from Present, stop rendering and poll
   `IDXGISwapChain::Present(0, DXGI_PRESENT_TEST)` at 5 Hz until visible.

## Adapter selection & hybrid GPUs

The fragility of GL↔D3D interop on hybrid GPUs was an argument against libmpv (**D2**) — which
means the hybrid case is now *your* problem, on the path you chose.

- **Decode and present must happen on the same `ID3D11Device`.** Create the FFmpeg hardware device
  context from your existing device ([05](05-video-pipeline.md)), never let it make its own — a
  cross-adapter copy per frame is exactly the hitch you built this pipeline to avoid.
- Pick the adapter that drives the **output the window is on**, via
  `IDXGIFactory::EnumAdapters` → `EnumOutputs`, not "adapter 0" and not "highest VRAM."
- **Handle the window moving to the other GPU.** On `WM_DISPLAYCHANGE` and on window move across
  outputs, re-check the adapter behind the current output; if it changed, rebuild the device and
  reupload from the CPU-side cache — the same path as `DXGI_ERROR_DEVICE_REMOVED`. Mid-playback,
  flush and re-create the decoder too.
- On laptops the app may launch on the iGPU and be dragged to a dGPU display or vice versa. Test
  it deliberately; it is not rare.

## DPI & multi-monitor

Manifest `PerMonitorV2`. Handle `WM_DPICHANGED` by resizing the swapchain and rescaling the UI
scalar — the image itself is resampled on the GPU so it costs nothing. When the window straddles
two monitors with different refresh rates, DXGI reports the one it's mostly on; re-query
the host path's refresh on `WM_DISPLAYCHANGE` and on window move, and re-pace.

`IDXGISwapChain::GetContainingOutput` is **invalid on composition swapchains**. Match the host
monitor (`MonitorFromWindow` + `GetMonitorInfo`) to an active `QueryDisplayConfig` path and read
that path's rational refresh rate. Unknown or cloned-and-conflicting rates are **zero** and
cannot pass the frame-time gate — never guess 60 Hz.

## Color management & HDR

Work in **linear FP16, Rec.709 primaries** throughout the *pipeline* — and blit to an **8-bit sRGB
swapchain** in v1. These are two separable decisions (**D6** in [01-decisions.md](01-decisions.md)):
the linear working space cannot be retrofitted, the swapchain format is one runtime branch. Nearly
every display this app meets is SDR, an FP16 swapchain doubles present bandwidth for nothing, and
at 45 MP an FP16 intermediate is ~360 MB of pressure on the upload budget above.

**v1 colour policy in one line: untagged JPEG is assumed sRGB; treating a *tagged* image as sRGB is
a bug.**

- Decode → convert from the file's color space (ICC profile via LittleCMS, or the tagged
  primaries/transfer for HEIF/AVIF/JXL) → linear scRGB.
- Detect HDR displays via `IDXGIOutput6::GetDesc1().ColorSpace ==
  DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`.
- **SDR display → the output transform depends on what the source *is*, and getting this wrong is
  a visible bug:**
  - **Display-referred sources** (JPEG, HEIC, PNG, most WebP — i.e. nearly every file this app
    opens) → ICC → linear → **sRGB encode. No tone map.** Tone-mapping a camera JPEG crushes its
    highlights and shifts its contrast; do it and you will fight your golden images forever
    while the app looks worse than Explorer's preview.
  - **Scene-referred or HDR sources** (RAW, EXR/Radiance, PQ/HLG video) → tone-map
    (Reinhard-with-shoulder or the ACES fitted curve), then sRGB encode.
  
  Carry a `TransferIntent { DisplayReferred, SceneReferred }` on every decoded image and branch on
  it. Defaulting to "tone-map everything" is the single easiest way to make this app look wrong on
  every photo a user cares about.
- HDR display → `SetColorSpace1(RGB_FULL_G10_NONE_P709)` with the FP16 swapchain and let the OS
  do PQ encode; scale SDR UI elements to the display's SDR white level from
  `GetDesc1().MaxLuminance`.
- **HDR video → SDR tone-mapping is a v1 correctness requirement, not a v1.1 feature.** HDR
  display *output* can wait (D6); mapping HDR *input* cannot. iPhone camera dumps are full of
  HLG-encoded HEVC, and without the map, PR 5's "4K 10-bit HEVC plays" verify passes while the
  picture is visibly washed out. Do PQ/HLG → linear → tone-map in the same shader that does
  YUV→RGB, driven by the stream's real primaries and transfer
  ([05-video-pipeline.md](05-video-pipeline.md)). Dolby Vision is out of scope.

Ship an ICC-correct path from day one. Retrofitting color management is a rewrite.

## Resampling quality

Zoomed **out** is where naive viewers look bad. Use:

- a **mip pyramid generated by a compute shader with a Kaiser/Mitchell kernel**, not
  `GenerateMips` (box filter, produces aliasing shimmer while panning). Until that compute
  pass exists, CPU Mitchell is acceptable. **Level sizes must match D3D11:
  `max(1, floor(prev/2))`.** Ceil (`(w+1)/2`) produces a chain `CreateTexture2D` cannot
  consume — odd camera JPEGs are common. Filter in **linear**, not in 8-bit sRGB.
- trilinear + 16× anisotropic sampling between levels,
- **Lanczos-3 in a compute pass** for the "fit to window" static view once panning settles
  (render the settled frame at high quality, cache it, keep the cheap path for motion).

Zoomed **in** past 1:1: nearest-neighbour above 400 % (pixel-peeping should show pixels), Catmull-Rom
between 100 % and 400 %.
