# 02 — Architecture

## Module layout

```
src/
  core/        arena + pool allocators, job system, result<T>, logging, ETW tracepoints
  io/          async file reads (OVERLAPPED), memory-mapped views, directory watcher
  codec/       decoder registry; one translation unit per format family
  image/       Image struct, color spaces, tiled pyramid, GPU upload, VRAM cache
  gfx/         D3D11 device, swapchain, frame pacer, shader library, compositor
  player/      demux/decode (FFmpeg), A/V clock, audio out, transport, seek, scrub thumbs
  meta/        Exiv2 wrapper, container probe, unified property model, writers
  edit/        non-destructive op stack, GPU op kernels, export/render-out
  canvas/      pan/zoom/springs, gesture + input handling for the canvas HWND
  abi/         the flat C ABI — the top of the native graph (see [14-abi.md](14-abi.md))
  shell/       Win32 top-level window, swapchain host, island bridge, CLI, associations

src.managed/   C# — WinUI 3 chrome hosted as XAML islands
  panes/       filmstrip, folder tree, metadata pane, adjust pane, job queue
  interop/     SafeHandle wrappers, completion pump, P/Invoke surface
tests/
tools/         frame-time harness, golden-image runner, corpus fuzzers
```

Dependencies point **downward only**:
`shell → abi → {canvas, edit, player, image, meta} → {codec, gfx, io} → core`, and the C# side
depends on **`abi` alone**. No back-edges, and **no native module may depend on `shell`** — that
rule is what keeps the core testable headlessly and re-hostable. Enforce with a CI script that
greps includes; it takes 20 lines and saves the project.

Note what changed after **D1**: there is no C++ `ui/` module any more. Chrome is C# XAML; the only
input C++ handles is the canvas itself (pan, zoom, gestures), because that path must not take a
marshalling hop per mouse-move.

## Threading model (this is the whole ballgame)

Five thread roles. **The rule that makes it smooth: the UI thread and the render thread never
block on I/O, decode, or a lock held by a worker.**

| Thread | Owns | Never does |
|---|---|---|
| **UI/input** | top-level `HWND` + window proc; the XAML island dispatcher; canvas input | file I/O, decode, GPU waits, blocking on the core |
| **Render** | D3D11 immediate context, swapchain Present, frame pacing | anything unbounded |
| **Decode pool** (N = cores−2) | image decode, tile generation, thumbnail gen | touch the immediate context |
| **I/O** (1–2) | readahead, directory enumeration, metadata scan | decode |
| **Demux/decode** (FFmpeg) | packet reads, `avcodec` submit/receive into D3D11VA surfaces | present, or own the clock |
| **Audio** (WASAPI) | render client feed; **the master clock** | video, decode |

Communication is exclusively **single-producer/single-consumer ring buffers of POD messages** plus
one MPMC job queue. No mutex is ever held across a call you don't own.

**Free-threaded resource creation.** D3D11 device creation calls are thread-safe. Decode workers
call `CreateTexture2D` with `D3D11_SUBRESOURCE_DATA` directly (immutable, `USAGE_IMMUTABLE`) —
no staging copy on the render thread, no `Map` contention on the immediate context. This one
choice removes the classic hitch when a large image lands.

## The frame loop

```
render thread:
  wait on swapchain frame-latency-waitable-object   // paces us to the display
  t = QPC now
  snapshot = ui_state.acquire()                     // lock-free double-buffered publish
  drain gpu_ready_queue  (bounded: <= 2 ms of uploads per frame)
  animate(t)                                        // springs, not fixed-step tweens
  record + draw
  Present(0, tearing_ok ? ALLOW_TEARING : 0)
```

The UI thread *publishes* a state snapshot; the render thread *consumes* one. They never share a
mutable object. UI logic running long (a folder scan callback, a metadata parse) can never stall a
frame.

## Platform floor

**Windows 10 21H2 and later.** That makes `OVERLAPPED` + a completion-port pool the real async I/O
path — **`IoRing` is Windows 11 only** and is an optional fast path at best, not the design. Say
this out loud in code review whenever someone reaches for a Win11-only API; the alternative is
discovering the floor by shipping.

## Cancellation

Every job carries a generation counter tied to the current "view intent" (which file is on
screen). Navigating away bumps the generation; in-flight decodes check it at tile boundaries and
abandon. Without this, fast arrow-key scrubbing through a folder queues 200 decodes and the app
feels like it's chewing gum.

## Memory budgets

Query `IDXGIAdapter3::QueryVideoMemoryInfo` at startup and on `DXGI_QUERY_VIDEO_MEMORY_INFO`
budget-change events. Split it:

- **60 %** decoded-image VRAM LRU cache — **8-bit sRGB or RGB10A2, not FP16**. A 45 MP FP16
  surface is ~360 MB before mips; caching every viewed JPEG that way starves this split on an
  iGPU laptop. FP16 is the *edit working space*, promoted only when an edit stack is active and
  only at viewport resolution ([07](07-photo-editing.md)).
- **25 %** reserved for the video decoder's surface pool (D3D11VA reference frames + your
  presentation queue; size it from the stream's DPB, not a guess)
- **15 %** slack for the compositor and OS

System RAM: a separate LRU of compressed source bytes and of the tile pyramid for the current
image, capped at min(25 % of physical, 4 GB). Evict by (last-use × size) score, never by count.
