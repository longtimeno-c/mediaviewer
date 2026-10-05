# 02 — Architecture

The native module layout and its dependency rules, the thread roles and how they communicate, the frame loop, cancellation, and memory budgets.

## Module layout

```
src/
  core/        job system (MPMC queue, generations), result<T>/status, SPSC rings, triple buffer,
               parallel-for, JSON, tracing (ETW on Windows), crash context
  io/          file port (async/overlapped reads), directory listing + watch, atomic replace,
               verified copy, content hash, volumes, child processes — portable headers, *_win/*_mac impls
  codec/       decoder registry; one translation unit per format family; magic-byte probe; OS-codec hook
  image/       decode→upload pipeline, colour (LCMS), linear FP16 working image, tiled pyramid,
               GPU upload, thumbnails
  gfx/         D3D11 backend (device, swapchain, pacer, blit, video blit) and the Metal twins (*_metal.mm)
  player/      FFmpeg demux/decode, hwdecode (D3D11VA / VideoToolbox), audio sinks, A/V clock, transport
  meta/        Exiv2 + libavformat readers, property model, writers, XMP sidecars, AF points
  edit/        EditStack, geometry and adjust ops, export bake, lossless JPEG, clip trim/encode
  canvas/      camera (pan/zoom), springs, refinement
  addon/       the add-on host: manifest verification, loader, host function table
  infer/       ONNX Runtime wrapper and preprocessing (AI search)
  abi/         the flat C ABI — the top of the native graph ([14-abi.md](14-abi.md))
  nle/         Local search from an editing app, FCPXML ([23-nle-search.md](23-nle-search.md))
  shell/       Windows host (Win32 window, island bridge, key router, CLI, OS integration) and the
               Mac host (AppKit + Metal, *_mac.mm)
  shellext/    Explorer thumbnail handler (out-of-process DLL)
  addons/      add-ons (import, ai) — reach the core only through the host function table

src.managed/   C# WinUI 3 chrome (Windows)
  MediaViewer.Chrome/   the island host: command bar, filmstrip, gallery, panes, settings, video, edit
  MediaViewer.Interop/  SafeHandle wrappers, P/Invoke surface, completion pump
  MediaViewer.Updater/  Velopack update flow
  MediaViewer.Import.Chrome/, MediaViewer.Ai.Chrome/  add-on chrome
src.swift/     SwiftUI chrome (macOS): MediaViewerChrome, ImportChrome, AIChrome
tests/         Catch2 suites, fixtures, frame-time fixture
tools/         frame-time harness, fuzzers, perf suite, packaging, policy checks
```

Dependencies point **downward only**. The enforced map
([`tools/check-module-graph.ps1`](../../tools/check-module-graph.ps1)):

| Module | May include |
|---|---|
| `core` | — |
| `io`, `gfx` | `core` |
| `codec` | `gfx`, `io`, `core` |
| `image`, `meta`, `player` | `codec`, `gfx`, `io`, `core` |
| `edit`, `canvas` | `image`, `codec`, `gfx`, `io`, `core` |
| `addon` | `io`, `core` |
| `addons` | `core` header-only pieces (`result.h`, `status.h`, `json.h`) |
| `abi` | everything below it |
| `nle` | `addon`, `image`, `io`, `core` |
| `shellext` | `image`, `codec`, `gfx`, `io`, `core` |
| `shell` | everything, including `abi` and `nle` |

No native module depends on `shell`, which keeps the core testable headlessly and hostable on both
OSes. The C# and Swift chrome depend on the C ABI alone. An add-on links nothing of the core and
reaches it through the host function table ([18-import.md](18-import.md)).

There is no C++ UI module. The only input C++ handles itself is the canvas (pan, zoom, gestures), so
mouse-move on the photo never takes a marshalling hop.

**Island layout (Windows).** The shell hosts one `DesktopWindowXamlSource` per chrome strip on the
native HWND: the command bar (top), the filmstrip (bottom), a folder tree (left, hidden by default) and
a docked right pane (the Edit workspace). The canvas is the rect left over; the input snapshot carries
`chrome_height_px`, `chrome_bottom_px`, `chrome_left_px` and `chrome_right_px`, and the usable canvas
subtracts all four. No strip is drawn over the canvas. On macOS the AppKit window lays SwiftUI views
around the Metal layer the same way.

**Keys are not per-island accelerators.** One key router on the UI thread maps key → command id,
except while a text field has focus. Bindings live in the host; the core sees ABI calls. Table and
default maps: [16-commands.md](16-commands.md).

The chrome borrows the `mv_session` (`retain` + `SafeHandle`) and drains the completion queue. When
chrome is off (`--no-chrome`) the native host drains it.

## Threading model (this is the whole ballgame)

The UI thread and the render thread never block on I/O, decode, or a lock held by a worker.

| Thread | Owns | Never does |
|---|---|---|
| **UI/input** | top-level window + window proc; the island dispatcher; canvas input | file I/O, decode, GPU waits, blocking on the core |
| **Render** | D3D11 immediate context / Metal command queue, Present, frame pacing | anything unbounded |
| **Decode pool** (N = max(1, cores − 2)) | image decode, tile generation, thumbnails, metadata reads | touch the immediate context |
| **Service threads** | directory watch, tile service, writer queues | decode on the render path |
| **Demux/decode** (FFmpeg) | packet reads, decode into hardware surfaces, copy into the presentation ring | present, or own the clock |
| **Audio** (WASAPI / Core Audio) | render client feed; **the master clock** | video, decode |

The job system ([`src/core/job_system.h`](../../src/core/job_system.h)) is one MPMC queue with two
lanes: **foreground** jobs carry the current view generation and are always taken first; **background**
jobs (generation 0 — thumbnails, scans) run only when no foreground work is waiting. Everything else
is single-producer/single-consumer rings of POD messages (`core/spsc_ring.h`) and a wait-free triple
buffer. No mutex is held across a call the holder does not own.

### Free-threaded resource creation

The D3D11 device is free-threaded. Decode workers call `CreateTexture2D` with
`D3D11_SUBRESOURCE_DATA` directly (immutable textures) — no staging copy on the render thread and no
`Map` on the immediate context. On Metal, workers create immutable textures the same way. The device
is created with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` and `ID3D10Multithread` protection because FFmpeg's
D3D11VA decoder shares it.

That protection serializes worker creates with the immediate context, so a large upload can still delay
`Present`; worker uploads count against the per-frame upload budget in
[03-rendering.md](03-rendering.md#frame-pacing-rules) and are shown on the F3 overlay.

The ready GPU image reaches the render thread through an atomic pointer handoff (`ready`), not a mutex
the render thread would have to take. Device rebuild bumps the job generation so in-flight creates
against the old device are discarded.

## The frame loop

```
render thread (src/shell/present_lab.cpp, present_lab_mac.mm):
  snapshot = input.acquire()                        // wait-free triple-buffered publish from the UI thread
  decide: live (video, animation, spring moving, 500 ms input tail, blinkies, fade) or idle
  idle  → wait on {wake event, image-ready handle} with no timeout (200 ms while occluded); continue
  live  → wait on the swapchain frame-latency waitable object     // paces to the display
          poll video (after the wait, immediately before drawing)
          t = QPC now; step springs by real dt
          draw ready textures / tiles (bounded uploads)
          Present(1, 0)
```

On macOS the pacing wait is the `CAMetalDisplayLink` callback instead of a waitable object
([15-platforms.md](15-platforms.md)); the rest of the loop is the same.

The UI thread *publishes* an `input_snapshot` (POD, [`src/shell/input_state.h`](../../src/shell/input_state.h));
the render thread *consumes* one. They never share a mutable object, so a long UI callback (folder
scan, metadata parse) cannot stall a frame. ImGui is fed from the snapshot on the render thread rather
than through its Win32 backend for the same reason.

When the renderer is idle, a completion that changes what is on the canvas wakes it: the session
signals an image-ready handle the render loop waits on, alongside its own wake event. See
[03-rendering.md](03-rendering.md#frame-pacing-rules) rule 4.

## Platform floor

**Windows 10 21H2 and later, x64.** Async file reads use `FILE_FLAG_OVERLAPPED` handles
(`io/file_port_win.cpp`); `IoRing` is Windows 11 only and is not used.

**macOS 14 and later**, Apple silicon and Intel in one universal app (the AI add-on is arm64 only).
Windows ARM64 is not built. Details: [15-platforms.md](15-platforms.md).

## Hostable core (D9)

`HWND`, `ID3D11*`/`IDXGI*`, `wchar_t` paths, `OVERLAPPED`, WASAPI and `ReplaceFileW` stay in `shell/`
and the Windows backends (`gfx/`, `io/*_win.cpp`, `player/*_win.cpp`); Cocoa/Metal stay in the Mac host
and `*_mac.mm` backends. Headers used above `gfx/` do not include `d3d11.h`. Paths across the C ABI are
UTF-8. Completions are a queue the host pumps — C++ never calls a WinUI dispatcher or
`DispatchQueue.main`. [`tools/check-hostable-core.ps1`](../../tools/check-hostable-core.ps1) enforces
the Windows side.

## Cancellation

Every job carries a generation counter tied to the current view intent (which file is on screen).
Navigating bumps the generation; in-flight decodes check it at tile and stage boundaries and abandon.
`mv_image_open` follows a bump. Device rebuild bumps it too, so GPU creates cannot outlive their
device.

Work the new view still needs is not thrown away: when the user lands on a neighbour whose prefetch
decode is already running, the ABI hands that job to the new generation
(`job_context::handed_off_via`) instead of cancelling and restarting it. In-flight decodes are
tracked by path (`inflight_decode` in `src/abi/abi.cpp`), so a neighbour is not decoded twice while
the LRU churns.

## Memory budgets

| Budget | Value | Where |
|---|---|---|
| Viewer texture LRU (Windows) | 512 MB of decoded textures, 3–12 entries, oldest evicted first | `lru_byte_budget` in `src/abi/abi.cpp` |
| Viewer still cache (macOS) | physical memory / 16, clamped 256 MB–1 GB, up to 8 entries (unified memory) | `still_cache_budget()` in `src/shell/present_lab_mac.mm` |
| Prefetch window | ±2 around the current item | `src/abi/abi.cpp` |
| Tile textures for a tiled image | 256 MB VRAM, LRU tiles | `k_tile_vram_budget` in `src/image/tiles.h` |
| Animation frame ring | 256 MB | `src/abi/animation_session.h` |

Viewer textures are **8-bit sRGB, not FP16**: a 45 MP FP16 surface is ~360 MB before mips. FP16 is the
edit working space only, created when an edit is active ([07-photo-editing.md](07-photo-editing.md)).

A tiled image (above ~64 MP or the 16384 texture limit) keeps its decoded full-resolution RGBA plus a
CPU pyramid (one third again — ~533 MB for 100 MP) only while it is the current image
([04-image-pipeline.md](04-image-pipeline.md)). `device::video_memory_budget()` reads
`IDXGIAdapter3::QueryVideoMemoryInfo` but nothing sizes caches from it yet; the budgets above are
fixed.
