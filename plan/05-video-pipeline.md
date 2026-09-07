# 05 — Video Pipeline

**Decision: FFmpeg + D3D11VA, decoding into your own D3D11 textures, presented on the same
swapchain as photos.** One pipeline, one canvas, no codec packs, no GL↔D3D interop. See **D2** in
[01-decisions.md](01-decisions.md) for why this beats both libmpv and Media Foundation *for this
app*.

Everything lives behind `IVideoSource`, so the two rejected options remain available as drop-ins
if reality disagrees.

## Why not the alternatives (short version)

- **libmpv** is the better movie player and the wrong shape here. Child-HWND embedding is a second
  canvas — flicker on resize, your shaders don't apply — and the render-API escape via
  `WGL_NV_DX_interop2` is fragile on the hybrid GPUs in most laptops. Subtitle layout, bitstream
  passthrough, and motion interpolation are player features this app doesn't sell.
- **Media Foundation** gives you the clock and audio for free, but needs Store codec extensions for
  HEVC and AV1 — the exact formats in an iPhone camera dump — which contradicts **D3**. Once those
  route through FFmpeg anyway, MF is a second pipeline earning its keep on H.264 alone.

## Architecture

```
demux (libavformat)
  → packet queue (bounded, ~2 s)
  → video: avcodec + AV_HWDEVICE_TYPE_D3D11VA  → NV12/P010 decoder-pool surface
  → copy into YOUR presentation-queue texture   (see "Surface ownership" below)
  → audio: avcodec → resample (swresample)     → WASAPI shared-mode render client
  → clock: audio is master; video presents against QPC, drops/duplicates on drift
  → your existing render thread samples the NV12 texture and colour-converts in the shader
```

Once it lands in your presentation queue, the video frame is just another texture in the
compositor: pan/zoom, letterboxing, edit-preview shaders, and UI blending all work on it
unchanged, because there is only ever one swapchain.

### Surface ownership — do not present decoder surfaces directly

**A D3D11VA output surface belongs to the decoder's pool**, sized from the stream's DPB. Holding
one for the duration of presentation removes it from the pool, and once enough are held the
decoder stalls waiting for a free surface — which shows up as a periodic hitch that looks like a
decode performance problem and isn't.

So: **copy each frame out into a small ring of textures you own** (3–4 is plenty) and present
those, releasing the `AVFrame` immediately. A GPU-side `CopySubresourceRegion` of an NV12/P010
surface is cheap — far cheaper than the stall — and you get lifetimes you actually control, which
matters at seek and shutdown where the alternative is a use-after-free in someone else's pool.

If you later want the copy back, the honest version is to enlarge the decoder pool with
`extra_hw_frames` and refcount `AVBufferRef`s into your queue. Measure before bothering.

### Hardware decode

Create the `AVBufferRef` hardware device context from **your existing `ID3D11Device`** — not a
device FFmpeg makes — so the decoded surface is usable directly without a cross-device copy. The
D3D11 device must be created with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, and because FFmpeg's
decoder threads and your render thread both touch it, wrap it with `ID3D10Multithread::SetMultithreadProtected(TRUE)`.

Decoded surfaces arrive as NV12 (8-bit) or P010 (10-bit) array textures, and **they need different
SRV formats — this is two code paths, not one**:

| Surface | Luma SRV | Chroma SRV | Note |
|---|---|---|---|
| **NV12** (8-bit) | `DXGI_FORMAT_R8_UNORM` | `DXGI_FORMAT_R8G8_UNORM` | |
| **P010** (10-bit) | `DXGI_FORMAT_R16_UNORM` | `DXGI_FORMAT_R16G16_UNORM` | 10 bits stored in the **high** bits of a 16-bit word — shift, or accept a 64× scale error |

PR 5's 10-bit HEVC verify exercises the P010 path specifically; if you only implement NV12, that
test fails in a way that looks like a colour bug.

Do the YUV→RGB conversion in the pixel shader using the stream's actual colour matrix, primaries,
transfer, and range. **Do not assume BT.709
limited range**; phone video is frequently BT.2020, and getting this wrong is the classic "why is
my video washed out" bug.

**HDR input must be tone-mapped in v1** — see [03-rendering.md](03-rendering.md). Phone video is
frequently HLG or PQ in BT.2020; map it to SDR in this same shader. Skipping it does not fail the
"it plays" test, it just makes every iPhone video look washed out.

Fall back to software decode (with a visible indicator in the debug overlay) when the GPU lacks a
profile. Never silently — a silent software-decode fallback on a 4K clip reads to the user as "this
app is slow."

## The A/V clock — the part that needs care

This is the real cost of owning the pipeline. Budget 2–4 weeks and treat it as a first-class
subsystem, not glue.

- **Audio is the master clock.** Query the WASAPI render client's position; derive presentation
  time from samples actually played, not from a wall clock.
- Each decoded video frame carries a PTS. On the render thread, present the frame whose PTS is
  closest to `audio_clock + one_vblank`; if the next frame is already late by more than a frame
  interval, **drop it**; if the queue is starved, hold the current frame rather than stalling.
- Track drift over a rolling window and log it in the F3 overlay. Steady-state drift must be flat —
  a slow ramp means your audio position query is wrong.
- **No audio** (silent clip, audio-only stream missing) → fall back to a QPC master clock seeded at
  playback start.
- **Variable frame rate** (screen recordings, phone slow-mo) falls out for free, because you present
  on PTS rather than on a nominal frame duration.
- Handle device change (`IMMNotificationClient`) by rebuilding the audio client and re-seeding the
  clock without interrupting video.

**Escape hatch:** if this overruns its PR 5b budget, drop `IMFMediaEngine` in behind `IVideoSource`
for the formats MF handles and keep FFmpeg for the rest. Cheap to hold in reserve, expensive to
adopt as the plan.

## Transport & seeking

- **Two seek modes.** Dragging the scrubber → seek to the nearest keyframe (`AVSEEK_FLAG_BACKWARD`,
  no full decode) — instant. On release, or when stepping → decode forward from that keyframe to
  the exact frame. Fast-then-accurate reads to users as responsiveness.
- **Flush decoders on every seek** (`avcodec_flush_buffers`) and bump the frame generation counter
  so in-flight frames from before the seek are discarded.
- Frame step forward/back when paused (back-step = seek to prior keyframe, decode forward to
  `n-1`). Bind to `,` / `.` and to arrow keys while paused. Space is play/pause on a clip
  (it is *next image* on a still). Full transport map: [16-commands.md](16-commands.md).
- Speed 0.25×–4× with pitch-corrected audio. **`atempo` accepts 0.5–2.0 per instance**, so the
  extremes need a chain (`atempo=0.5,atempo=0.5` for 0.25×); build the chain from the ratio rather
  than assuming one filter.
- A–B loop, per-file resume position, remembered volume and track selection.
- **Scrub preview thumbnails**: a second lightweight decoder instance generating keyframe
  thumbnails into the existing thumbnail cache in the background, shown above the scrubber on
  hover.

## Subtitles

v1: render embedded text subtitles (SRT, and MOV/MP4 timed text) as plain styled text in your own
UI layer — you already have text rendering for chrome. **Not** in v1: ASS/SSA positioning and
animation, or bitmap subs (PGS/VobSub). Those are player features; if they turn out to matter, they
are the strongest argument for revisiting D2.

## What you build, and what you get free

FFmpeg gives you demuxing, decoding, and hardware acceleration. **You** own the clock, the audio
output, the seek model, and the presentation — which is precisely the part that has to share a
swapchain with the photo path. That division is the whole point of D2: the expensive, generic work
is borrowed; the part that determines how the app *feels* is yours.
