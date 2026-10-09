# 05 — Video Pipeline

How clips are probed, demuxed, hardware-decoded into textures the app owns, clocked against
audio, and transported, on both hosts.

**FFmpeg demuxes and decodes; the app owns the clock, the audio output, the seek model and the
presentation.** On Windows decode is D3D11VA on the app's own `ID3D11Device`; on macOS it is
VideoToolbox, with the decoded surfaces blitted on the app's own `MTLDevice`. Either way the
frame lands in a ring of textures the app owns and is drawn on the same swapchain as photos —
one canvas, no codec packs, no GL↔D3D interop.

Everything sits behind `player::media_source`
([`src/player/media_source.h`](../../src/player/media_source.h)), which is the type the ABI and
the shells talk to. The rejected alternatives (libmpv as a second canvas, Media Foundation
needing Store codec packs for HEVC/AV1) would slot in behind the same interface.

## Formats

Containers are identified by magic bytes (`player::probe`,
[`src/player/container_probe.h`](../../src/player/container_probe.h), first 1024 bytes):
MP4 / QuickTime (ISO-BMFF), Matroska, WebM (EBML DocType), AVI (RIFF), MPEG-TS (0x47 on a
188-byte cadence). Codecs: H.264, HEVC, VP9, AV1, MPEG-2; on the Mac also Apple ProRes
(hardware decode only). `mv_probe_is_video` exposes the probe so the host routes image vs video
without opening anything.

### Audio files

MP3 (an ID3v2 tag, or two consecutive MPEG audio frame headers) and iTunes audio (ISO-BMFF with
major brand `M4A `, `M4B ` or `M4P `) probe as `container::mp3` / `container::m4a`. They go the
clip's way everywhere (`is_video`, `io::is_video_name`, the folder's clip flag and play badge) and
`io::is_audio_name` tells them apart where it matters (no edit workspace).

With no moving video stream the pipeline runs in **audio-only mode** (`video_pipeline::audio_only`):
the audio stream is the seek stream and the time base, the audio is the master clock as usual, and
`run_still_thread` takes the video decode thread's place. The picture is the cover art (the
attached-picture stream decoded once to RGBA, at most 2048 px) or the music card
(`codec/card.h`), republished into the ring at each seek's target so the seek's preview
shows it at once. Position, pause and the resume point follow the clock, not the still's PTS.
`,` `.` move by `kAudioStepNs` (5 s). `media_info::audio_only` tells the host.

FairPlay (`drms`/`drmi`/`drac` or CENC `enca` sample entries, `is_protected_audio`) opens with
`drm_protected`: the padlock card, no audio thread, no duration, so play ends at once. Nothing
decrypts it. Posters follow the same rules (`poster_frame`: the art, or the card at tile size).

## Architecture

```
demux (libavformat)
  → packet queue (bounded, 512 packets ≈ >2 s of 4K)
  → video: avcodec + D3D11VA (Windows) / VideoToolbox (macOS) → NV12/P010 decoder-pool surface
  → copy into OUR presentation-ring texture   (see "Surface ownership")
  → audio: avcodec → swresample to float32 → WASAPI shared (Windows) / Core Audio (macOS)
  → clock: audio is master; video presents against it, drops/holds on drift
  → render thread samples luma + chroma and colour-converts / tone-maps in the shader
```

Once in the ring, a video frame is another texture in the compositor: pan/zoom, letterboxing
and UI blending work unchanged. `player::video_frame`
([`src/player/video_source.h`](../../src/player/video_source.h)) is its own type — two SRVs
(or two Metal textures), an NV12/P010 format, a `colour_desc`, a PTS and a view generation —
not `image::gpu_image`. Time is `int64` **nanoseconds** everywhere in `player/` (suffix `_ns`),
so audio can account in whole samples. The container `start_time` is subtracted (MPEG-TS starts
non-zero).

### Surface ownership

A hardware decoder's output surface belongs to its pool, sized from the stream's DPB; holding
one stalls the decoder. So each frame is copied out (`CopySubresourceRegion` on D3D11, a Metal
blit on macOS) into a ring the app owns and the `AVFrame` is released immediately
([`src/player/video_internal.h`](../../src/player/video_internal.h)):

- **4 slots** on D3D11 (four 4K P010 frames ≈ 100 MB VRAM); **6** on Metal, which holds a
  presented frame for two more presents while the GPU may still sample it.
- Ring textures match the decoder's padded allocation exactly (a null-box
  `CopySubresourceRegion` requires it); the visible size travels in `video_frame`.
- `begin_write` never blocks: with no free slot the decoder holds its `AVFrame` a moment longer,
  bounded by `extra_hw_frames`.
- `acquire` on the render thread returns the oldest frame at or before the deadline whose
  generation matches; stale-generation frames are recycled, never shown.
- On macOS the blit waits for completion on the decode thread (~1 ms at 4K) before the slot is
  published and the `CVPixelBuffer` returns to the pool.

### Hardware decode

**Windows** ([`src/player/hwdecode_win.cpp`](../../src/player/hwdecode_win.cpp)): the FFmpeg
`AV_HWDEVICE_TYPE_D3D11VA` context is created from the app's own device, which is created with
`D3D11_CREATE_DEVICE_VIDEO_SUPPORT` and `ID3D10Multithread::SetMultithreadProtected(TRUE)`. The
device crosses into `player/` as `void*` so headers stay free of `d3d11.h`.

**macOS** ([`src/player/hwdecode_mac.mm`](../../src/player/hwdecode_mac.mm)): VideoToolbox
owns its sessions; the `CVMetalTextureCache` and the copy-out blit are on the app's `MTLDevice`.
ProRes decodes to 4:2:2/4:4:4, so VideoToolbox is asked for P010 (4:2:0) and ProRes stays on
the GPU path.

Surfaces and their SRV formats — two code paths:

| Surface | Luma SRV | Chroma SRV | Note |
|---|---|---|---|
| **NV12** (8-bit) | `R8_UNORM` | `R8G8_UNORM` | |
| **P010** (10-bit) | `R16_UNORM` | `R16G16_UNORM` | 10 bits in the **high** bits of each 16-bit word |

Other surface formats (P016, 4:2:2/4:4:4 on D3D11) fail hardware setup and route to software.

YUV→RGB is done in the pixel shader (`gfx/video_blit`, HLSL and MSL twins) from the stream's
actual matrix, primaries, transfer and range (`gfx::colour_desc`; unspecified is never assumed
to be BT.709 limited). **PQ and HLG are tone-mapped to SDR** in the same shader into the 8-bit
sRGB swapchain ([03-rendering.md](03-rendering.md)).

**Software fallback** (CPU frames, dav1d for AV1) is used when the GPU lacks a profile or the
surface format is unsupported. It is never silent: `mv_video_info.decoder` reports
`MV_DECODER_SOFTWARE` and the F3 overlay names it.

## The A/V clock

[`src/player/av_clock.h`](../../src/player/av_clock.h),
[`src/player/presenter.h`](../../src/player/presenter.h),
[`src/player/audio_sink.h`](../../src/player/audio_sink.h).

- **Audio is the master.** The sink reports position from samples actually played (the render
  client's position minus queued frames), not a wall clock.
- `presenter::choose()` is pure and lock-free: present the frame whose PTS is closest to
  `master + one vblank`; **drop** a frame superseded by another due frame; **hold** the current
  frame when the queue is starved. Adjacent PTS, not nominal frame rate, define a frame's
  lifetime, so variable frame rate works.
- Counters are separate: `frames_dropped_late` and `holds_starved` are faults; `holds_cadence`
  is the correct 3:2 result of 24p on 60 Hz.
- Error is reported as p50/p99 and the gate is the **least-squares drift slope** over a long
  series (`drift_slope_ms_per_min` in `mv_video_stats`). The instantaneous error is flat by
  construction and proves nothing alone. Endpoint crystal ppm is reported as a diagnostic.
- `position_discontinuities` counts position jumps (device change, stream reset); a soak with
  any is re-run.
- **No audio** (silent clip, or the track played out) → a monotonic host clock
  (`std::chrono::steady_clock`) seeded at playback start; `mv_video_stats.audio_master` is 0 and
  `fallback_reason` says why.
- **No device** (the endpoint will not open, or a rebuild found nothing): the clock stays on the
  host master and retries the open after 1 s, doubling to 8 s between tries, never while paused.
  A failed retry is not a rebuild: `device_rebuilds` counts devices lost, `fallback_reason`
  stays `device_open_failed` until an open succeeds, and the failure is logged once.
- **Device change** (`IMMNotificationClient` on Windows) rebuilds the audio client and re-seeds
  the clock without interrupting video.
- Stats are published wait-free (`core/spsc_ring.h`); `player/` makes no ImGui calls.

## Transport & seeking

[`src/player/transport.h`](../../src/player/transport.h) — pure, no FFmpeg, D3D or threads, so
the rules are unit-tested.

- **Two seek modes** (`mv_video_seek(..., exact)`): scrubber drag → nearest keyframe
  (`AVSEEK_FLAG_BACKWARD`, no decode); release, frame step or a typed position → decode forward
  from the keyframe to the exact frame.
- Every seek flushes the decoders (`avcodec_flush_buffers`) and bumps the generation so
  pre-seek frames are discarded.
- **Frame step** (paused): forward decodes the next frame; back is a seek to the prior keyframe
  and a decode to `n−1`. A half-frame bias in the direction of travel avoids landing on the same
  frame twice. Bound to `,` / `.`; Space is play/pause on a clip
  ([16-commands.md](16-commands.md)).
- **Speed** 0.25×–4×, clamped, pitch-corrected by an `atempo` chain built from the ratio (each
  instance covers 0.5–2.0; 0.25× is `atempo=0.500,atempo=0.500`). Rate 1.0 has no filter.
- **A–B loop** (`mv_video_set_loop`; B < 0 clears). A and B are normalised, so marking B first
  still loops.
- **Per-file resume position**, stored in a `resume` folder beside the thumbnail cache. Only
  stored past 15 s and more than 10 s before the end.
- Volume, mute and audio-track selection are ABI calls (`mv_video_set_volume`,
  `mv_video_set_muted`, `mv_video_select_audio_track`).
- **Playback hold** (`mv_video_set_hold`, [`src/player/playback_hold.h`](../../src/player/playback_hold.h)):
  while the gallery covers the canvas a playing clip pauses and a newly opened clip stays on its
  first frame. Release resumes only the clip that was playing at entry, if still on screen. Both
  hosts run the same rule.
- **Video posters** for the filmstrip and gallery are a separate one-shot software decode
  (`player::poster`), never the playback decoder ([04-image-pipeline.md](04-image-pipeline.md)).

## Not built

- Subtitle rendering (subtitle streams are listed in the metadata stream inspector only).
- Scrub-preview thumbnails above the scrubber.
- An `IMFMediaEngine` implementation behind `media_source`.
