# 12 — Decision Log

The reasons behind current behaviour: each entry states what is true in the code now, why, and
the measured numbers behind it, grouped by topic. The date in parentheses is the date the call was
made, so a code comment that cites "12 (YYYY-MM-DD)" lands on the entry here.

Trade-offs for the D1–D9 decisions are in [01-decisions.md](01-decisions.md).

---

## Architecture and threading

**Shell is native; chrome is hosted (2026-09-06).** The Win32 window and the D3D11 swapchain
from the present lab are the app. WinUI 3 chrome is hosted inside as XAML content islands
(`DesktopWindowXamlSource`). One present path, owned by C++. Composition swapchains cannot take
`ALLOW_TEARING` and `SwapChainPanel` resize/DPI is a known cost, so the native window was always
the destination. Cost: the entry point is C++ and C# is chrome content, not app host.

**Video decode is FFmpeg + D3D11VA on our device (2026-09-06).** One pipeline, one canvas,
native D3D11 textures, no interop, no codec packs. Media Foundation needs Store extensions for
HEVC and AV1 (D3), and once those go through FFmpeg, MF would be a second pipeline for H.264
alone. libmpv was rejected because a child-HWND player is a second canvas.

**Wait-free triple buffer for `publish_slot` (2026-09-06).** Producer and consumer each own a
slot and exchange through a third (`core/spsc_ring.h`). Two slots permit overwrite during a read;
a seqlock retries and can starve the render thread. The triple buffer has no retry loop.

**ImGui is fed from the input snapshot, not its Win32 backend (2026-09-06).** ImGui's Win32
backend mutates `ImGuiIO` inside the window procedure, putting the UI thread inside the render
thread's ImGui context. vcpkg takes `imgui[dx11-binding]` only; the platform layer reads the
published input snapshot (~15 lines).

**Constraints from the PR 1 / PR 2 review (2026-09-07).** Each of these is in the code:

- An idle renderer is **woken** when a worker publishes a texture the canvas should show (and on
  a device-loss re-upload); the 500 ms input tail does not cover a 60 MP decode.
- CPU mip sizes follow D3D11: `max(1, floor(prev/2))`.
- LittleCMS on the decode pool uses a per-job `cmsContext`; the global one is not thread-safe.
- The ready GPU image is an atomic hand-off; the render thread never takes a lock a worker holds.
- A device rebuild bumps the job generation, so a `CreateTexture2D` in flight against the old
  device is never published onto the new one. The CPU cache is kept and `attach_device`
  re-uploads (`abi/abi.cpp`).
- `CreateTexture2D` on a multithread-protected device still serialises with the immediate
  context, so the ~2 ms upload budget applies to worker uploads too.
- A broken ICC profile fails the transform. Tagged bytes are never shown as sRGB (D6).
- `mv_image_open` follows a generation bump; opening without one replaces rather than cancels.

**C# borrows the session and drains completions (2026-09-07).** The filmstrip attach payload
carries `mv_session_t`; C# retains it in a `SafeHandle`. While the island is attached, native does
not drain: two drainers race. `--no-chrome` keeps the native drain. Island-only completions a
native path needs (for example `CLIP_INDEX`) are forwarded back as chrome commands.

**Hostable core from PR 4 (2026-09-07).** `io/dir.h` is portable; `ReadDirectoryChangesW` lives
in `io/dir_win.cpp`. `tools/check-hostable-core.ps1` fails `d3d11.h` / `windows.h` / `atlbase.h`
in `core/`, `codec/`, `canvas/`, `image/`, `meta/`, `player/`, `edit/` and `io/*.h`; `*_win.cpp`
under `io/` and `gfx/` is allowed.

**Sort order lives in `io`, owned by the ABI session (2026-09-24).** `io/sort_order` (namespace
`mv::io`) so `abi -> io` stays legal. The Windows listing is owned by `mv_session`, so filmstrip,
gallery and arrow keys read one order (ABI 0.6: `mv_folder_set_sort`, `mv_folder_get_sort`,
`mv_list_subdirectories`). The session keeps the scanned listing so a new order or a batch of
date-taken stamps re-applies without a disk scan. Persisted as `[view] sort`; an unknown value
normalises to name.

**Add-on module boundary (2026-09-24).** `addon` (the base app's add-on host) depends only on
`io` and `core`; `abi` and `shell` may include it. Add-ons in `src/addons/<name>` include only
`core`'s header-only pieces and link nothing of the core. `tools/check-module-graph.ps1` and
`check-hostable-core.ps1` enforce both. `shellext/` is a host module beside `shell/`
(2026-09-25), and `shell` may include `nle` (2026-09-28).

**Document font bytes are read above codec (2026-10-12).** Laying out a page picks font files lazily,
including per-character fallbacks, so the decode caller cannot pass the bytes in up front. `codec/`
still names the file (`fonts::find`, CoreText / DirectWrite). The read used to be `io::read_all`
inside `codec/text.cpp`, the only `codec → io` include and a sideways edge between peers. It is now
`text::set_font_reader`, a function pointer `image/pipeline.cpp` installs before main, and
`tools/check-module-graph.ps1` rejects `codec` including `io`. The docx fuzzer links `codec` without
`image` and installs the same reader over `io::read_all`. With no reader the face slot stays -1 and
layout continues. Issue #286.

**Settings writes are off the UI thread (2026-09-14).** "Settings writes on the UI thread" is
closed. `settings.ini` is read once into an in-memory document (`shell/settings_store`). Saves
mutate it read-copy-update and hand the newest immutable snapshot to a one-thread persist worker,
separate from `file_jobs`, so a long move neither delays nor drops a settings write. Bursts
coalesce; an unchanged document writes nothing. The worker writes temp → `FlushFileBuffers` →
`MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`, so a crash mid-write leaves the old file. Exit
flushes with a bounded 1 s wait after island teardown. A detector counts any settings write on the
UI thread outside the exit scope, and tests assert it. The file is UTF-16LE with a BOM; older ANSI
files still load and unknown keys survive. All access goes through `app_settings()`: direct
`*PrivateProfile*` calls would be lost when the store rewrites the file.

**Background jobs leave a worker free (2026-10-11, issue #160).** The two job lanes used to order
dequeues only: once every worker had started a generation-0 job (thumbnails, posters, scans; never
cancelled mid-run), the decode for the photo just arrowed to waited for one of them to finish.
Background jobs now run on at most N − 1 workers at once (N = 1 keeps its one). Measured on an M-series
Mac, `mv_tests "foreground start latency under a background sweep"` (2 workers, a sweep of 60 ms
jobs, base/new alternated four times): the foreground job's start waited a median 43–56 ms
(p90 83–104 ms) before, 0.02–0.18 ms (p90 0.1–3.1 ms) after. Cost: a sweep with no foreground work
runs on one worker fewer: −50 % thumbnail throughput on a two-worker (four-core) pool, −17 % on six.

---

## Rendering and present loop

**sRGB swapchain: `UNORM` buffer, `_SRGB` view (2026-09-06).** DXGI rejects every `_SRGB`
format on a flip-model swapchain. The buffer is `R8G8B8A8_UNORM` and the render-target view is
`_SRGB`, so the hardware still encodes linear → sRGB on write. D6 is unchanged.

**Present call shape (2026-09-07).** Wait on the frame-latency object before rendering, then
`Present(1, 0)` on the composition path: the waitable bounds queue depth, sync interval 1 sets
display duration.

**MMCSS on the render thread (2026-09-06).** Registered as a `"Games"` task at
`AVRT_PRIORITY_HIGH`. Thread priority alone does not stop the scheduler pre-empting a present loop
on a busy machine.

**Metal `maximumDrawableCount` is 2 (2026-09-19).** `CAMetalLayer` throws for anything outside
[2, 3], so D3D11's "latency 1" cannot be expressed. 2 is Metal's lowest-latency setting. The
intent (one frame of latency, wait on the display link before encoding) holds: 60 s gate, 3600
frames, 0 dropped, p99 16.9 ms.

**Metal storage mode follows the GPU (2026-09-24).** Textures are `Shared` when
`device.hasUnifiedMemory` and `Managed` otherwise (`image/upload_mac.mm`,
`player/frame_ring_mac.mm`), so the same code runs on Intel Macs with discrete GPUs.

**On-canvas labels share the F3 overlay's ImGui draw list (2026-09-13).** The `O` info line, the
loupe frame, the hold-previous label, AF quads and the crop rectangle are canvas overlays: text
and shapes with no focus, input or layout, drawn in the same present as the image. Anything a user
operates (command bar, filmstrip, panes, `?`) is WinUI / SwiftUI. Crop's rectangle is drawn here
because it tracks the camera every frame (2026-09-24).

**A same-size refinement swaps under a running fade (2026-09-20).** A still refined twice
(`full_top`, then `full` with mips) restarted the cross-fade; on a Canon CR2 the outgoing embedded
JPEG snapped out at alpha 0.6 in one frame. Now a same-size refinement arriving mid-fade replaces
the incoming texture without restarting, so the preview fades out once. `frametime --no-pop
<image>` gates it with PR 1's cadence; the lab reports corner displacement, size step, fade
completion and drops inside a fade.

**Panes float; only the Edit workspace docks (2026-09-24, 2026-09-26).** The metadata pane (right,
340 DIP) and folder tree (left, 280 DIP) overlay the canvas; `chrome_left_px` stays 0 and opening
one never refits the photo or touches the present path. The Edit workspace is the exception: it
docks through `input_snapshot.chrome_right_px`, which narrows the rect the picture is framed in.
The swapchain is never resized, only refitted. Panes hide under the gallery, Settings and
chrome-off fullscreen.

**The clip transport floats and auto-hides (2026-09-26).** Issue #38: one layout on both hosts,
over the video rather than a reserved strip (auto-hiding a strip would refit the video on every
wake). One portable rule, `shell/transport_autohide.h`: hide after 2.5 s idle while playing; held
while paused, hovered, scrubbed, focused, with a menu open or under a screen reader. No timer or
repaint while idle, only one one-shot deadline. Windows parks the island below the client area (an
island is an opaque child HWND and cannot fade); the Mac fades it. `chrome_cmd_transport_hold`
(1017) carries a scrub or the More flyout.

**The Windows transport hugs its controls (2026-09-27).** The island reports its row's natural
width on `chrome_cmd_transport_width` (1018); native sizes the bar to that plus
`kTransportPadDip` (14) each side, bounded by the window's side gaps. 880 DIP is only the width
before the first report.

**Native owns the More flyout's audio and loop (2026-10-09).** Issue #214: the Windows flyout
called `mv_video_set_volume` / `set_muted` / `select_audio_track` / `set_loop` itself, so native's
volume and mute (what `↑` `↓` and `Shift+M` step from) drifted, and a flyout loop silently replaced
trim's `P` preview. It now posts `chrome_cmd_video_volume` / `_muted` / `_track` / `_loop`
(1031–1034); native applies them and pushes volume, mute and track back (`ApplyAudio`), the rate
rule. A flyout loop ends the trim preview. The Mac already read both back from native.

**The Video Editor moves the one canvas (2026-09-26, 2026-09-27).** While the editor is open the
viewer's canvas moves into its window and back, so there is no second renderer. Mac: the one
`CAMetalLayer` view moves and refuses first responder. Windows: `gfx::swapchain::retarget` moves
the same DComp visual between frames, requested through the opaque
`input_snapshot::canvas_window`; the editor window is destroyed only after the swapchain has left.

**A neighbour's whole-texture create waits out a pan (2026-10-11, issue #166; Windows).** An
untiled still up to the 64 MP tile threshold goes up as one `CreateTexture2D` with its mips (about
320 MB at 60 MP), and `ID3D10Multithread` holds the device lock for all of it, so a ±2 prefetch
landing mid-pan stalls the render thread's presents. The ~2 ms budget is unchanged. What changed is
that it is now enforced for single textures. A create over `image::k_frame_budget_create_bytes`
(8 MiB, an estimate of 2 ms of driver copy) for a path that is not on screen waits on its worker,
after the CPU mip chain, while the render loop presents a moving camera
(`abi::set_canvas_moving`).
- *Rejected: lowering the tile threshold.* A tiled image is neither prefetched nor kept in the LRU,
  so arrow → next would go cold for every 24–60 MP camera file.
- *Rejected: splitting the texture into bands or top/mips.* The bands need a second draw path with
  seams to manage, and a top-level-only create is still ~240 MB under one lock.
- *What the gate leaves alone:* fades, video and the input tail, so navigation keeps its prefetch;
  and the selected path, which goes at once.
- *The Mac is not gated.* Its upload (`replaceRegion`) takes no device-wide lock.
- *Numbers owed:* a Windows pan soak with 50–60 MP neighbours (`texture_creates_deferred`,
  `texture_max_create_ms` and dropped frames, before and after). Until that run, 8 MiB is a
  guess.

---

## Measurement and performance gates

**What counts as a dropped frame (2026-09-06, 2026-09-07).** `DXGI_FRAME_STATISTICS`, not QPC
intervals: a present the compositor held for an extra vblank looks clean from inside the app. The
QPC path is a labelled fallback and `meets_pr1_gate()` requires the authoritative source.
`PresentCount` advances when the compositor displays a frame, so a same-value sample is normal and
skipped. Track `PresentCount` with `PresentRefreshCount`, keeping the last displayed baseline across
duplicate polls; missing statistics invalidate the window. The refresh rate is queried from the
monitor's active display path; an unknown rate is zero and cannot pass.

**Warm-up is declared (2026-09-06).** The first second is discarded and the report says so: DWM
has not picked the window up yet and the first frame carries the font-atlas upload.

**Idle and input (2026-09-07).** Input snapshots carry cumulative wheel units and an activity
sequence; consumers take differences once, and a parked cursor is not activity. A cursor in the
lab window counts as input and invalidates the idle gate: the "quiet machine" caveat. A run on a
machine in use is reported as such, not as a pass, and a host clock gap (sleep, throttling) is
counted, not smoothed over.

**Frame-time CI skips without a GPU runner (2026-09-07).** The job runs only on
`[self-hosted, windows, gpu]` when `MV_GPU_RUNNER_ENABLED=true`; otherwise it is skipped. A hosted
runner failing without running `frametime.exe` proved nothing. A skip is not a pass.

**Skipped present-loop gates are reported, not silent (2026-10-11, #149).** A `present-gates` job
always runs after the Windows and Mac frame-time jobs: a failed or cancelled gate fails it, a
skipped one is a warning (a failure once `MV_REQUIRE_PRESENT_GATES` is set). Skips stay
non-failing by default for the 2026-09-07 reason: failing every PR on runners that do not exist
proves nothing. The Mac PR 1 gate is its own opt-in self-hosted job (`MV_MAC_GPU_RUNNER_ENABLED`).

**Video copy-out runs on the immediate context (2026-09-07).** The decode thread copies from the
D3D11VA pool into our presentation ring with `CopySubresourceRegion` on the **immediate**
context, holding FFmpeg's `AVHWDeviceContext` lock for the submit only. A D3D11VA `AVFrame` is a
(pool texture, slice) pair, so a COM reference reserves nothing; a deferred context would let the
decoder overwrite the slice before `ExecuteCommandList` (wrong-frame corruption only under DPB
pressure on 4K clips). FFmpeg's decode submits through a video context on the same immediate
context, so submission order guarantees our copy precedes the next write. Constraints: copies only
on that thread (no `Map`, `Flush`, `ClearState` or GPU wait); `extra_hw_frames` covers the ring
depth; the `AVFrame` is released right after submit. If PR 1's gate regresses (p99 > 10 % or any
frame > 2× refresh), the copy moves to the render thread.

**Auto does not run the large tower on CPU after a provider failed it (2026-10-05, owner).** On an
M5 (macOS 26.6) Core ML takes ~8 min and 12.7 GB to open ViT-L/14 from its cache, and when the
background upgrade failed, Auto kept ViT-L/14 on CPU: ~1,570 ms per four images against ~120 ms
for ViT-B/32 on CPU and 7–11 ms on Core ML, so a 24 k library showed "about 3–4 h". The pack now
records each provider's verdict per tower (`provider.txt`) and keeps ORT's message (paths
replaced); Auto opens ViT-B/32 on the provider wherever it failed ViT-L/14, says so, and offers
"Try the larger model again". Starting every machine on ViT-B/32 until ViT-L/14 proved itself was
rejected: each tower change re-indexes the library, so a working ViT-L/14 index would be rebuilt
twice. The 8-minute open itself is not fixed (docs/design/17 "Verified on macOS").

**Model loading waits for a quiet viewer (2026-09-27).** Loading the Local search pack during the
PR 1 soak dropped two frames. The provider self-test's CPU half is kept between runs (~15 s of a
40 s load), and Settings never loads the runtime on the UI thread.

---

## Image pipeline and formats

**Audio and documents join D5 (2026-10-03, owner).** MP3, M4A, M4P, PDF and DOCX open alongside the
camera-dump set (`docs/plans/audio-and-documents.md`). Audio reuses the clip path rather than a second
player: the pipeline gains an audio-only mode (demux and seek on the audio stream, the cover art or a
card republished as a still at each seek), because an attached picture used as the "video" stream
cannot seek — measured: a seek on a cover-art MP3 left the position stuck at the target. M4P is
FairPlay; it is shown with a padlock and never played (nothing decrypts it). The new types are Open With
only on both platforms: not in Windows' Default apps capabilities, and skipped by the Mac's
make-default and adopt-new-types paths — the owner asked that they never be made the default.

**A document scrolls; it is never edited (2026-10-05, owner).** Over a PDF or DOCX with the
filmstrip hidden, the wheel and trackpad scroll the page instead of zooming (this replaces "no
scrolling between pages" from the same day): held against the top or bottom edge they turn the page,
which keeps its zoom and lands at the far edge (`camera::turn_page`). One trackpad gesture or one
wheel burst turns one page, so momentum cannot run through a document. *(Reversed 2026-10-12, below.)* `⌘`/`Ctrl`+wheel and a pinch
still zoom; with the filmstrip shown the wheel zooms as on any still. The Edit button gives way to
**Open in <app>** (the default app, never MediaViewer) and a ▾ of the other apps, and `Enter` opens
the default: there is nothing for the workspace to edit in a document.

**PPTX joins D5 (2026-10-15, owner, issue #429).** PowerPoint files open as documents: one slide per
page, probed by magic bytes (a zip holding `ppt/presentation.xml`), Open With only and never made the
default, like PDF and DOCX. The renderer is ours (`codec/pptx.cpp`), reusing the DOCX zip, XML and
HarfBuzz/FreeType text path, for the same reasons DOCX is: a hosted OS preview would be a second
canvas (rule 2) and LibreOffice is a 300 MB download. Readable slides, not a PowerPoint replica.

**Document scrolling glides and runs through pages (2026-10-12, issue #353).** Reverses "one
gesture turns at most one page" and the 60 px hold at the edge (2026-10-05): reading a long
document was scroll, stop, push, jump. A push past the top or bottom edge now turns the page at
once and trackpad momentum carries into the next page; a wheel notch glides on the canvas spring and
a trackpad pans directly. What stops a fling from skipping to the end is the render thread's land
acknowledgement (one turn per landed page) and, at fit, one page per third of the canvas of travel.
The rule is shared (`canvas/document_scroll.h`) instead of written twice in the hosts. Pages stay
separate stops rather than one continuous strip, which would need two pages on the canvas at once.
With the filmstrip up the wheel still zooms; a "Press T" hint now says so (docs/design/16).

**Thumbnails are JPEG files on disk (2026-09-07).** Spec `jpg512.4` (was `.1`; bumped
2026-09-24 when JPEG orientation was applied, 2026-10-03 when clip posters began following
the display matrix, and 2026-10-10 when transparent images began to be alpha-weighted and
composited onto a dark matte instead of showing the RGB under transparent pixels, issue #169;
opaque thumbnails come out byte-identical, but the spec is one key, so every row regenerates once
on view), keyed in SQLite by `(path, mtime, size, spec)`. The
ABI returns a UTF-8 path; C# `BitmapImage` loads the file, so pixels never cross the ABI. On-disk
BC7 is not used: a BC7 blob cannot be an `Image.Source`; it is the format to take if thumbs ever
need to be GPU-resident. The Mac uses the same spec and schema (`image/thumb_mac.cpp`, 2026-09-17).

**Warm browse is a five-slot GPU LRU (2026-09-07).** Current ± 2 decoded textures, not a second
`mv_image_open`. Prefetch jobs use the view generation; thumb jobs use a folder generation so arrow
keys do not cancel the filmstrip.

**Display-referred sources are not tone-mapped (2026-09-06).** Images carry a `TransferIntent`.
Display-referred sources go ICC → linear → sRGB; only scene-referred / HDR sources are tone-mapped.
Tone-mapping a camera JPEG crushes highlights.

**Animated GIF and WebP (2026-09-13).** giflib (MIT) and libwebp (BSD-3, with libwebpdemux) are
linked dynamically; still WebP arrives with them. APNG frames are walked in-tree and decoded by
libspng. **Delay clamp:** a frame delay of 10 ms or less plays as 100 ms, as Chromium and Firefox
do (`codec/anim.h`); tested at 0 / 10 / 11 / 19 / 20 ms.

**libheif with default features off (2026-09-13).** The vcpkg port's `hevc` feature is `WITH_X265`,
a software HEVC encoder, which is forbidden. HEVC decode is libde265, a hard dependency. AVIF decode
is `libavif[dav1d]`, not libheif's `aom`.

**PR 7 format and pairing calls (2026-09-14).** Numbered as recorded:

| # | What is true | Why |
|---|---|---|
| 1 | **The OS codec probe is HEIC stills only** (`codec/os_decode.h`). WIC is tried only for an 8-bit, no-alpha, non-HDR, non-sequence HEVC HEIC whose colour is ICC or sRGB-in-effect, when WIC has a HEIF decoder and MF has an HEVC decoder. WIC must return the ICC and the size libheif would; any failure but cancel falls back to libheif silently. `MV_OS_CODEC=0` forces the bundled path. | Routing other formats through WIC would drop the LCMS path the D6 tests pin; HEIC is where the OS path can be hardware-backed. With Store packs installed, WIC and libheif agree on size and ICC (mean pixel diff 1.9). |
| 2 | HDR (PQ / HLG) HEIC and AVIF stills are tone-mapped to SDR in the decoder (`cicp::hdr_to_sdr`, the video curves) and handed on as display-referred sRGB. | `image/colour.cpp` refuses `scene_referred`; HDR output is not built. |
| 3 | CICP SDR camera transfers (BT.709/601/2020) display with the sRGB curve; gamma 2.2 / 2.8 / linear get a synthesised ICC; Display P3 nclx gets a synthesised ICC v4. | Browser behaviour; P3-as-sRGB is the D6 bug. |
| 4 | Orientation: JPEG gets EXIF orientation on the display path (`codec/orient.h`, 2026-09-24); HEIC gets libheif's irot/imir; AVIF applies irot/imir/clap; RAW preview and full decode are rotated by LibRaw's flip. **TIFF, PNG and WebP orientation is not applied.** | An orientation pass that reached RAW would rotate twice. |
| 5 | TIFF: 16/32-bit round to 8; float clamps 0–1; CMYK converts naïvely. **TIFF drops grey and CMYK ICC profiles.** The colour stage accepts a grey profile over grey pixels (R = G = B), so grey JPEG / PNG with a grey ICC display (2026-09-27); it still fails one over colour pixels. | RGBA8 raster; a grey profile through an RGBA transform made valid files `corrupt`. |
| 6 | RAW full decode: PPG demosaic, camera WB, sRGB 8-bit, highlight clip, **auto-bright on** (`codec/raw.cpp`). Measured 0.8–1.7 s on 16–42 MP single-threaded, missing the 500 ms target; first pixel is the embedded preview (11–69 ms). LibRaw is now built with OpenMP, its team capped by `raw_foreground_threads()`. | AHD was 2.5–4.7 s; auto-bright off left a ~36-level gap to the embedded JPEG (on: ~20). |
| 7 | Live Photo pairing covers HEIC+MOV and JPG+MOV; RAW+HEIC counts as RAW+JPEG; groups of three or more stay separate. Pairing is by basename only. `IMG_E####` is its own stop; a hidden motion half is not attached. | Exact, cheap, never hides a file. |
| 8 | File operations on a paired stop act on both halves (copy, move, bin, drag-out, `Ctrl+C`); the prompt names both. Collision renaming is per file. | Deleting only the JPEG would make the RAW reappear as its own stop. |
| 9 | Opening a RAW from Explorer selects its pair's stop and shows the JPEG. | The still is first pixel and primary. |
| 10 | **Open RAW / Open JPEG of pair** are command rows with no default key (Unbound in Settings, hidden from `?`). | No key was assigned; a Settings binding makes them routable. |
| 11 | ICO: the largest (then deepest) entry is the still; an unreadable one falls back to the next. AVIF with unknown / infinite repetition loops forever. | Chromium behaviour. |
| 12 | Command id 76 (`palette`) is a keyless retired row. | Keeps the wire enum stable. |

**Clean-VM HEIC is a test (2026-09-20).** `tests/test_clean_vm_heic.cpp` (`mv_clean_vm_tests`, its
own executable) decodes a HEIC with `MV_OS_CODEC=0` and reads the module list: libheif and
libde265 mapped; Media Foundation, the WIC codec extensions and `\WindowsApps\` not. The disabled
path loads no module at all. `codec/os_decode_win.cpp` is the only TU that touches WIC or MF.

**HEIC thumbnail item as first pixel; grid tiles on the thread budget (2026-10-03).**
`codec::decode_heic_thumbnail` decodes the primary's largest thumbnail item (in the primary's
colour when it has none of its own). It is refused when not smaller than the image or not its
shape to 2 %. `image::decode_first_pixel` serves it to the canvas only; thumbnailers and search keep
`decode_preview`. The full decode then goes up once with mips (staging a mip-less copy cost
30–250 ms of launch → full). A grid's tiles use the caller's thread limit, as RAW does
(`raw_foreground_threads()` for the image on screen, 1 for prefetch and thumbnails); pixels do not
depend on it. Bundled path only; WIC is unchanged. Measured, 12 MP grid file, base and new in
separate trees, alternated:

| | Windows (Ryzen 7 5700X3D) | Mac (M5) |
|---|---|---|
| Lab first pixel | 495–530 → 29–40 ms | 548–559 → 15–17 ms |
| Lab full, bundled path | 794–797 → 360–365 ms (`MV_OS_CODEC=0`) | 548–559 → 231–246 ms |
| Lab full, WIC path | 587–634 → 579–610 ms (overlapping) | — |

0 dropped frames in every lab run. With tiles in parallel the bundled path opens this file faster
than WIC (~600 ms) on that machine; routing still prefers WIC per row 1.

---

## Video

**Decoder surfaces are copied into our ring; P010 is R16/R16G16 (2026-09-06).** Holding DPB
surfaces for present starves the decoder. NV12 binds R8/R8G8; P010 binds R16/R16G16 with data in
the high bits. HLG/PQ → SDR mapping is a correctness requirement (iPhone HLG HEVC).

**VideoToolbox through a texture cache and a blit (2026-09-20).** VideoToolbox owns its sessions;
our `MTLDevice` is used for the `CVMetalTextureCache` and the blit that copies out of the pool into
the ring. The blit is waited on the decode thread (~1 ms at 4K), so a slot is complete before it is
published and the pixel buffer returned. A slot is two textures (R + RG): Metal has no planar NV12
/ P010 texture. The Darwin ring is 6 slots: a presented frame is held two more presents before
release. MPEG-2 has no VideoToolbox decoder on Apple silicon and runs in software (`SOFTWARE` on
F3).

**Core Audio position anchoring (2026-09-20).** Played position is anchored to each render
callback's host time with a **signed** offset. `mHostTime` is ahead by the output latency, so an
"only if now > host" extrapolation never engaged; the clock stepped ~11 ms and a 60 Hz presenter
dropped a third of a 30 fps clip as late. Found with `tools/playprobe`.

**Apple ProRes plays on the Mac (2026-09-29).** D5 is amended for the Mac: ProRes 422 and 4444.
When the decoder is ProRes on VideoToolbox, `pick_hw_format` gives FFmpeg a frames context with
`sw_format = P010`, so VideoToolbox converts to 10-bit 4:2:0 in hardware and the existing P010 path
takes it. Windows keeps D5 (ProRes there would be software decode, unmeasured at 4K). Measured (M5,
`playprobe`, 15 s, same 2.7K 59.94 fps clip, HEVC / ProRes alternated): 57.0 / 58.5 / 56.6 / 58.1
frames/s; A/V error p99 9.01 / 6.74 / 8.62 / 6.66 ms.

**A hardware frame the ring cannot take is converted, not dropped (2026-09-29).** Both platforms:
`sw_convert::convert_hw` copies it back (`av_hwframe_transfer_data`) and converts in software, with
one log warning and the overlay naming the decoder "software". Covers 4:2:2 / 4:4:4 hardware output
from any codec. Forced through it, ProRes 422 played at 55 frames/s.

**Tap `Q` / `E` skips (2026-09-13).** Tap skips ±2 s on the down edge (exact seek); hold skims
(non-exact, settle on release). Speed is on the command-bar dropdown and `Shift+Q` / `Shift+E`.
`J` / `L` are ±10 s in the viewer.

**Arrows on a playing or strip-less clip (2026-10-12, issue #361).** `←` `→` are `Q` / `E` while the clip plays or the filmstrip is hidden (mode `skim`, layered over video);
paused with the strip up they still walk, since that is when the strip is being used to pick.
`A` / `D` walk in every case. A hold keeps its command until key-up, so a clip that ends
mid-skim cannot turn the next repeat into a walk. *(The playing half reversed 2026-10-15, below.)*

**Arrows skip only with the filmstrip hidden (2026-10-15, owner; issue #416).** Reverses the
playing half of #361: with the carousel on screen `←` `→` move to the previous / next item on a
clip, paused or playing, and never skip. Only `T` (strip hidden) makes them `Q` / `E` (mode `skim`):
a tap is the exact ±2 s seek, a hold skims, release settles. The strip being visible is the one
rule, so the router no longer carries a play-state bit. `Q` / `E` still skip, `A` / `D` still walk,
a still walks either way, `Shift`+arrows pan and `J` / `L` are ±10 s, in every case. A hold keeps
the command it started with until key-up in both directions: a skim stays a skim if `T` shows the
strip under it, and a walk stays a walk if `T` hides it (the router now pins a plain repeat row's
command for the hold, as it already did a tap/hold's; a pinned walk owes nothing on key-up or on
a focus-loss cancel).

**A transport click does not seek 0 on the way (2026-10-12).** The system slider reports its
minimum before the time under the pointer. `media_source::seek` publishes that target and arms
preview before the frame is decoded, so the minimum presents the opening frame. A non-exact seek
of the origin is dropped while the playhead is past half a second (`scrub_origin_is_spurious`);
the click's real sample and the exact release are kept. An exact seek to the start — replay, a
step, the thumb released there — still goes there. Play and pause are not changed.

---

## Metadata

**Metadata read (2026-09-24).** `src/meta` reads EXIF / IPTC / XMP and maker notes through Exiv2
(dynamic; `bmff`, `png`, `xmp` — without `xmp` a PNG's XMP is silently empty) and container data
through libavformat. `shell/meta_store` reads once per (path, mtime, size). Metadata is read only
while something shows it, after a 90 ms pause, so arrow-key scrubbing queues no reads. Pane data
crosses as three text tables (`meta/tables.h`); the chrome never reads a file.

**AF points (2026-09-24).** `canon_af_points` treats AFInfo2 Y offsets as positive-down; no Canon
file was available to confirm it, and a wrong sign would mirror the quads vertically.

**Eyedropper (2026-09-24).** One texel is copied to a 1×1 staging texture and mapped with
`D3D11_MAP_FLAG_DO_NOT_WAIT` on a later frame, so the render thread never waits on the GPU. 8-bit
RGBA stills only.

**Metadata write (2026-09-25).** In place only for a plain JPEG: Exiv2 rewrites it and the result
is verified before the `io::replace_atomic` swap (decoded maker notes compared value for value,
opaque ones byte for byte). Everything else goes to a `<stem>.xmp` sidecar, merged, never
overwriting a non-XMP file; a sidecar beside an in-place JPEG is kept in step because it wins on
read. A snapshot is taken before the first write per file per session in
`io::metadata_snapshot_dir()`; no snapshot, no write. `shell::meta_writer` coalesces, writes one at
a time and drains at exit (rotation first: it refuses bytes a metadata write changed); rotation and
metadata writes of one JPEG never overlap. Rating is XMP, mirrored into EXIF rating tags only where
present; comments are encoded by hand because Exiv2's comment type needs per-platform iconv.

**Every tag is editable (2026-09-26).** `meta::write` takes any Exif / Iptc / Xmp key, and a date
that moves every capture-time tag together, through the same checked JPEG rewrite (untouched tags
identical, image data unchanged) and sidecar rule: a RAW, HEIC, PNG or clip is never opened for
writing. Layout, maker-note and orientation tags are read-only. The snapshot covers every tag, so
Revert restores the file's bytes exactly. One file, one change set; not a batch engine.

**The GPS map pin is built: read-only, on an offline map (2026-10-11, owner; #344).** 06 listed
"GPS map pin" under Not built. The Places add-on now shows a geotagged file's place label, its
coordinates and a small map with the pin at the foot of the Metadata pane. It is read-only: the
map does not edit coordinates (writing them is #352's own change), and clicking it opens nothing
(rule 6). The map is drawn by the add-on from the Natural Earth polygons and GeoNames towns it
ships, on a worker, into a bitmap the chrome shows: no tiles, no MapKit, no network, because every
tile or geocoder service would receive the photo's coordinates. Places is an add-on carrying its
own code and data (the row "An add-on carries its own code and its own data", under Add-on
mechanism). Without it the pane shows coordinates as before.

**The map is the pane's last section and zooms under the cursor (2026-10-12, owner; #344).**
Places contributes the info-pane section, the search provider and its settings page only: no bar,
path or status item and no window. The section sits at the very bottom of the pane, below the
footer, whenever the file has coordinates. A pinch or mouse wheel over the map zooms about the
cursor, a drag pans, a double-click zooms one step; a trackpad two-finger scroll keeps scrolling the
pane. The chrome moves the last bitmap during the gesture and the add-on re-draws once it settles
(#348's generic surface events); no present. The range runs from country scale (40° tall) to town
scale (0.2°) with cities1000 town dots and names, never streets, and says so under the map. The pin
never leaves the picture: a pan is clamped to keep the photo's point inside it.

**Location is editable; blank removes, with a confirmation (owner, 2026-10-12, #352).** Summary's
Location row has Edit instead of Remove: coordinates typed in, read by one core parser
(`meta::parse_location`) on both hosts, and a blank save removes the location only after a
confirmation (a native alert on the Mac, an in-pane box on Windows; Remove destructive, Cancel
default and Esc). It says Revert all can bring the location back until the app quits, which is
what the session snapshot does. This reverses "writing or editing coordinates:
out of scope" (`docs/plans/metadata-search-and-place.md` §4, doc 06 *Not built*). The write is
`write_fields::location` through PR 12's checked writer and sidecar rule: EXIF GPS in a plain JPEG,
XMP `exif:GPS*` in a sidecar otherwise, the original never touched (rule 5); the sidecar's position
now wins on read, as its rating does. Altitude is kept unless given. Remove is offered only when it
reaches every copy the viewer reads, so a RAW's or HEIC's own fix can be overridden, not removed.

**Every tag searchable (2026-10-12, #291).** File search and `/` match tags after names, from a
per-folder background pass (`meta::tag_index`) over a bounded 512 KB head read, kept in
`meta_cache.db` beside the thumbnail cache. The pass starts after the item's first image, runs as
background-queue chunks behind every view job and waits while the viewer presents. Date-taken sort
keys moved onto the same pass, one read per file instead of two. On a folder never seen, each key
costs the search read (0.19 ms on a 6 MP JPEG) instead of the date read (0.09 ms); a folder seen
before sorts at once. The app's search text has no place names: owner, 2026-10-11, places come
from the Places add-on's search provider. The AI pack's query fold moved onto the hosts' rule
(`core/fold.h`): ß, æ and ø are no longer expanded to ss, ae and o, so a word means the same in
file search and Local search. AI index schema 4 adds the tag columns and an FTS5 `asset_text`
(`sqlite3[fts5]` in vcpkg). Its place / country / region columns stay NULL, and `place:` is
answered at query time by the provider.

**A place term needs Places, and intersects (2026-10-16, owner, #441).** Without the Places
add-on, `place:Portugal` used to list every geotagged file, newest first, including a photo taken
in England. With Places, the provider's rows were only offered beside the list ("+ N in
Portugal"), never intersected, though doc 17 said they were. Now the AI pack's engine asks the
`place:` provider for each place term through the host table's `place_matches` and keeps only
those assets (`-place:` removes them). With no provider a place term matches nothing, and the
search panel says Places is needed instead of guessing; the owner asked that `place:` leave the
help line until Places is installed. The core still links no geocoder: the borders stay in the
add-on.

---

## Photo editing

**Geometry is a uv map, never a re-decode (2026-09-24).** The `EditStack` folds to one canonical
geometry; `place()` turns it into an affine output → source map the blit samples through, so a
turn, flip, crop or straighten costs nothing per frame. The full-resolution chain runs once on the
CPU at export.

**Lossless JPEG on libjpeg's public coefficient API (2026-09-24).** `transupp.c` is jpegtran's
source, not an installed library. Perfect transforms only: an edge that would move a partial MCU
into the frame is refused, never trimmed.

**`[` `]` `H` `V` rewrite a JPEG (2026-09-24).** Only when the stack is rotate / flip alone, only
losslessly, atomically, after a 0.4 s debounce (so `]]` is one write), and only if the file is
still the bytes the turn was made against. `mv_folder_forget` (ABI 0.7) drops a rewritten path from
the navigation LRU. A stack with any colour op is never written back.

**Export (2026-09-24).** Lossless when JPEG → JPEG with only rotate / flip / aligned crop,
re-encoded otherwise; always upright. EXIF is patched in place byte-level so maker notes with
absolute offsets survive. Output `<name>-edit.jpg`, created exclusively. Byte-identical across runs
and platforms (same pinned encoders).

**Colour adjusts (2026-09-24).** The D6 working space is linear Rec.709 FP16, bit-identical on
both hosts; zero sliders bake back to the viewer's pixels exactly. The working texture is built
once (≤ 3072 px) and a slider drag uploads 32 bytes of uniforms. One kernel source
(`gfx/adjust_kernel.h`) in the subset HLSL, MSL and C++ share, so the shader twins cannot disagree.
The histogram is a CPU reduction 120 ms after the sliders settle (a compute shader would be another
kernel copy for a readout that updates at settle). Colour exports are sRGB, untagged. Sliders stay
disabled until the working image exists.

**The Edit workspace (2026-09-26).** Issue #39: **Edit image / Edit video**, `Enter` and a Mac Edit
menu open a workspace of tabs over the existing panes, a base feature with no new engine. It docks
(see "Panes float" above).

---

## Clip editing and the Video Editor

**One clip core, a companion ABI (2026-09-25).** `src/edit/clip*` is its own library (`mv_clip`)
because it links FFmpeg and `mv_edit` does not. `mediaviewer_clip.h` is implemented by the portable
`abi/clip_session`, which the Mac host reuses directly, so both hosts drive one queue.

**Hardware encoders only, through FFmpeg (2026-09-25).** `edit/hwencode.h` lists FFmpeg's
hardware wrappers (`*_nvenc`, `*_qsv`, `*_amf`, `*_mf`; `*_videotoolbox`), so one transcode loop
serves both platforms. The core refuses any encoder FFmpeg does not mark hardware or hybrid, and
`libx26*` by name. Re-encode keeps HEVC as HEVC when a hardware HEVC encoder opens. Audio is
stream-copied (no software AAC encoder), so re-encode audio edges are packet-accurate.

**Encode and decode jobs run in `MediaViewerClipJob` (2026-09-25).** Owner: "the main app
shouldn't be affected". Every job that opens a codec runs in the helper (beside `MediaViewer.exe`;
`Contents/Helpers` on the Mac; it links FFmpeg, so the Mac bundles its dylibs) over a line protocol
(`edit/clip_wire.h`) through `io/child_process` (kill-on-close job object; `posix_spawn`).
Stream-copy jobs stay in process. Cancel asks the helper; after 5 s it is killed and `.mvpart`
files are swept. A crash fails one job; a missing helper fails those jobs and they never run in the
viewer. Decode inside a job is software, never the player's decoder.

**Keyframe grid and snapping (2026-09-25).** The grid reads every video packet, decoding nothing:
`AVDISCARD_NONKEY` gives MOV keyframes the wrong composition offset with B-frames. Probe + keyframe
trim of a 968 MB MP4: 1.0 s. Keyframe trim snaps outward so the output contains the request; split
and remove-middle snap to the nearest keyframe.

**Outputs and keys (2026-09-25).** Written beside the source under a free name (`_trimmed`,
`_part1`, `_frame_…`, ` (2)`), never over it. Frame and GIF/WebP pixels are sRGB. In trim,
`Backspace` **and** `Delete` clear the markers, so `Delete` never bins the clip being trimmed.

**The Video Editor is base, in its own window (2026-09-26).** It adds no payload: the clip core
with a timeline. Export is `clip::op::keep_ranges` (ABI 0.13): keyframe cuts by default, or exact
through the re-encode path in the helper. A 16 s clip with its middle third cut exports to 11.13 s
(keyframe) and 10.69 s (exact) for a 10.71 s program. On Windows it is a Win32 window owned by the
viewer with a XAML timeline island; keys go by window, and a key in the viewer raises the editor so
`A` / `D` cannot walk the folder out from under the edit (2026-09-27).

**I / O mark; J K L shuttle (2026-09-28).** `I` / `O` mark a range and `Delete` removes it as one
edit, as in other editors (they used to cut). `L` plays 1×, 2×, 4×; `K` stops; `J` skims back 1, 2,
4, 8 s within a 600 ms burst, because the player cannot run backwards.

---

## Keyboard, commands and chrome

**One command table, one router (2026-09-07).** Every command is a row in `command_table.cpp`,
routed by one key router on the UI thread; `?` and Settings call `describe_commands()` on the live
table so they cannot drift. Speed rules: no decode on keydown, key-repeat stays inside the
generation counter, copy/move never on the UI thread, blinkies off by default (they animate the
present loop), pairing at scan. New commands are appended so saved Settings indices hold.

**No command palette (2026-09-13).** A WinUI `TextBox` in its flyout fail-fasts (`0xC000027B`),
and a stand-in field never saw keys that are bindings, because the router handles them first. `?`
lists the current mode's bindings; Settings type-to-filter finds a command. The `palette` id (76)
and `chrome_popup::palette` stay as unused holes so later ids do not shift.

**Typeahead (2026-09-13).** With the filmstrip or gallery focused, typing jumps by name (300 ms
reset); from the canvas, `/` opens a find box, since nearly every letter is a canvas command.

**Chrome focus is not a mode (2026-09-13).** Island mode is the filmstrip, the gallery and focused
panes. The command bar, the transport and a cheat-sheet flyout keep the mode underneath; treating
any non-canvas focus as island mode made `A` / `D` / `Q` / `E` dead after clicking Play.

**Settings remaps the live table (2026-09-13).** A clash swaps the two rows so nothing is left
unbound; diffs persist in `settings.ini` `[keys]`; Reset writes `kBindings` back. Settings owns its
input through XAML dispatch, including Escape. The Mac routes `NSEvent` through the same table and
router (2026-09-23); commands its host cannot run are hidden from its remap list.

**Gallery and browse (2026-09-13).** F11 full screen, F5 slideshow; Enter opens the selected item.
Tiles resize with `+` / `−`: 152 DIP, 24 DIP steps, 80–344 DIP, session-local. A `wrap` setting
(default on) applies to arrows, Space, `A` / `D` and the slideshow.

**Folder tree (2026-09-13, 2026-09-24).** Slipped from PR 6; a floating left island since PR 9.
`Ctrl+Shift+E` shows and focuses it; arrows walk and expand, Enter opens, Esc returns, a second Esc
closes. A focused pane owns its keys except Esc; a mouse click on a pane never takes keyboard
focus. The tree follows the directory watcher and diffs rows in place.

**Add-on commands are gated rows (2026-09-24).** Static table rows enabled at run time
(`set_addon_commands_available`); hidden from `?`, Settings and the router while absent.

**Host dispatch is one coverage list (2026-10-12).** `command_coverage.inc` names, for every
`command_id`, whether each host handles it and which area of `run_command` / `runCommand:`
runs it. The Mac Settings list is that Mac set plus the Import and AI gates, so it cannot
drift from the dispatcher. The island callback and the window procedure are split by area
the same way. `tests/test_command_coverage.cpp` fails until a new id has a row.

**File search and the path-bar icon (2026-09-28).** The gallery search bar is gone. A magnifier at
the end of the folder path runs `Ctrl+F` / `⌘F`: with Local search loaded, starting, or installed
and waiting for the first search (2026-10-11) it opens the search panel; without it, **file search**, a name filter over the listed folder with no index (the
Windows chrome entry is still named `GallerySearch`). `/` in the gallery is the folder row's find.

**Touch and trackpad are input, not a camera path (2026-10-12, issue #212).** A pinch becomes
wheel units toward the fingers (`wheel_total`, so `camera::wheel_toward` and the ω=18, ζ=1
springs zoom), a zoomed pan becomes screen pixels (`scroll_px`, `camera::pan_by_screen`), and a
swipe is the folder step `A` / `D` take; `shell/gesture.h` is the shared maths (sub-unit pinch
remainder, a once-per-gesture swipe latch). No snapshot field, ABI call or present path was added.
On Windows the touchscreen uses **`WM_GESTURE`**, not `WM_POINTER` or DirectManipulation: the
system recogniser hands over the pinch distance and centre and a pan while a tap still arrives as
the promoted mouse click every existing path handles; raw pointers would mean re-deriving taps,
and DirectManipulation brings its own viewport and inertia to fight the springs. Pan inertia and
the gutter are configured off (`WM_GESTURENOTIFY`). A precision touchpad's pinch was already
`Ctrl`+wheel; its two-finger sideways swipe (`WM_MOUSEHWHEEL`) and a mouse's tilt now step one item
per swipe or click, latched until 250 ms of quiet so momentum does not run through the folder. A
document keeps its sideways scroll. At fit a one-finger stroke is swipe-next because the opening
view is locked (no drag at fit); zoomed, it pans. The Mac already zoomed on `magnifyWithEvent:`; it
gained the same remainder and the sideways swipe (`scrollWheel:` and `swipeWithEvent:`).

**Copy Text in Image (2026-10-10, issue #289).** Preview's Edit ▸ Copy Text in Image: the still on
screen goes through the OS's on-device text recognition and the lines it read go on the clipboard
as plain text, with a notice ("Copied 42 words", "No text found"). The recogniser is a port
(`shell/text_in_image.h`, D9): Vision `VNRecognizeTextRequest` (accurate, language correction,
automatic language) on the Mac; `Windows.Media.Ocr` (`OcrEngine.TryCreateFromUserProfileLanguages`
on a `SoftwareBitmap`) in the WinUI chrome on Windows, called synchronously from a pool worker. The
decode, the geometry (rotate, flip, crop, straighten — colour adjusts change no letter), a cap of
4096 px on the long edge, alpha flattened over white, the tidy-up and the count are shared C++.
Nothing leaves the machine (rule 6) and no Store download is offered (rule 7): a Windows profile
with no OCR language gets a notice naming the Language settings page. Reached as Preview has it —
Edit ▸ Copy Text in Image on the Mac menu bar — and from a **Tools** flyout beside View on both
command bars; disabled, never hidden, without a still on screen. Keyless like Preview's (Settings
can give it one). No off switch: the owner's ease-of-access call (2026-10-10) was "no extra
concepts or settings", and the command does nothing until it is chosen. Selecting text on the
canvas (a Live Text overlay) is not built.

**Text in images: select on the canvas, not a menu command (2026-10-15, owner, issue #289).**
Replaces the entry above before it shipped. The owner asked for Photos' behaviour, "click down on
the text to select it", instead of Edit ▸ Copy Text in Image and a Tools flyout; the command
(`copy_text_in_image`), its menu item and both flyouts are gone. The recogniser port now returns
words with boxes (`text_word`, 0..1 of the rendered still); `shell/text_select.h` is the shared
selection model (press-drag by whole words, double-click word, triple-click line, a click clears),
driven by each host from the UI thread through `canvas_map`, which the render thread publishes
after each frame with a still. The highlight travels in the input snapshot like the AF quads, and
`input_snapshot::text_press` keeps a press on text from panning. `⌘C`/`Ctrl+C` copies a selection
before the eyedropper and files, `⌘A`/`Ctrl+A` selects all words only when some are selected (Mark
all otherwise), and `Esc` has a `text_selection` level just inside the Live Photo's. Reading a
still is the cost, so it waits for the pointer to rest on it for 500 ms and runs on the pool; a
keyboard-only browse never reads anything. Selection is by whole words because
`Windows.Media.Ocr` boxes words, not characters.

---

## Windows chrome (XAML islands)

**Island layout (2026-09-07).** Top island: command bar (48 DIP). Bottom island: filmstrip with
a virtualising `ItemsRepeater`. `input_snapshot` carries `chrome_bottom_px` / `chrome_left_px` /
`chrome_right_px`, and `usable_canvas` subtracts them. Flyouts use
`ShouldConstrainToRootBounds = false` so they are siblings of the swapchain. Decoded pixels never
go to a XAML `Image`.

**Command bar menus stay visible (2026-10-15).** Open, View, Edit, Settings, About and `?` were
losing the bar whenever a large change added an Auto-sized neighbour (the folder trail, an import
hint, an indexing pill, an add-on icon). Those neighbours sat beside a Star column, the Star
column shrank to nothing, and the new item painted over the menus. The menu cluster is an Auto
column. The trail is the only column that gives up width, and `?` is its own Auto column on the
right. New bar items go in the menu row or the trail, not in a new Star column on the left. The
bar is one 48 DIP row. A star row under it is 0 tall while the island is only the bar, and content
measured into that row blanks the menus — Settings is an overlay added after the island has grown
to the client, not a child of a star row.

**No XAML object in an `IslandHost` static initializer (2026-10-15).** From 2026-10-11 the chrome
did not attach at all — no command bar, gallery or chrome keys. `Attach` touches `IslandHost`
statics (`RegisterUiFont`) before `EnsureApp` starts XAML, and an eager `SymbolFont` `FontFamily`
threw `RPC_E_WRONG_THREAD` from the type initializer. XAML statics are lazy (`IconFont`,
`SymbolFont`), built on first use after XAML is up.

**Tear the chrome down before `DestroyWindow` (2026-09-13).** Chrome-on exits fail-fast about one
in five (`0xC0000602`, `CoreUIComponents.dll`) when islands were disposed during `WM_DESTROY`.
In `WM_CLOSE`, while the parent is whole: detach every island, pump pending messages once
(bounded), then `DestroyWindow`. The bar's `Detach` disposes `WindowsXamlManager` and shuts the
`DispatcherQueueController` down last. Measured: 13 of 60 chrome-on exits crashed before, 0 of 30
after.

**Controls that fail-fast in these islands (2026-09-13 to 2026-09-27).** `TextBox`, `TreeView`,
`FocusManager.TryMoveFocus` and `ProgressBar` all fail-fast (`0xC000027B`, `Microsoft.UI.Xaml.dll`):
there is no `XamlControlsResources` in this island host, and merging it would restyle every control.
Text entry uses `FakeInput` (single line, Backspace, `Ctrl+A`); the tree is StackPanels and
Buttons; directional focus is handled explicitly per control; progress is two borders
(`Shared/FlatBar.cs`). Fluent brushes such as `TextFillColor*` do not resolve either (2026-10-03).
`tools/check-winui-controls.ps1` lists what loads.

**Values that cross as pulls (2026-09-24).** The command callback carries a float, so the island
parks a path or text and native pulls it (`chrome_cmd_tree_open` then `TakeTreePath`;
`take_parked_text`, 16 KiB).

---

## Platforms

**macOS is a host of the shared core (2026-09-07, 2026-09-24).** Decode, colour, EditStack,
metadata, canvas springs and the C ABI are shared. Present (Metal), hardware decode (VideoToolbox),
audio clock (Core Audio), async I/O, directory watch, chrome (SwiftUI in AppKit) and the installer
are per-OS backends. Mac twins are separate `*_mac.*` files, so they never touch a D3D11-owned file
(2026-09-17). Each platform has its own present-loop gate (Windows PR 1, Mac PR 1).

**Intel Macs ship in one universal app (2026-09-24).** macOS 14+. One `MediaViewer.app` (arm64 +
x86_64), one `.dmg`, one Sparkle `.zip`, one appcast. Each arch is built natively with its own
triplets; `tools/mac/lipo_merge.py` joins the trees, then `macpack release` signs, notarizes and
packages once. Pacing on Intel has not been measured. Windows ARM64 is not built.

**A headless Linux build of the shared core (2026-09-24).** `cmake/portable` runs the shared suites
under ASan / UBSan / TSan. It is a test build, not a product platform.

---

## Shell integration

**Single instance (2026-09-25).** Windows: a second start hands its paths over a per-user,
per-session named pipe (`\\.\pipe\MediaViewer.Viewer.<session>.<SID>`, first instance, local
clients only); the running app opens them in its window and comes forward. `--new-instance`
overrides; soaks, `--no-chrome` and an update restart always run alone. macOS: Launch Services
routes a second open to `application:openURLs:`.

**AppUserModelID `MediaViewer.Viewer` (2026-09-25).** Set before the first window, carried by the
wizard's shortcuts, and used for the jump list. Without it the root stub and `current\MediaViewer.exe`
were two taskbar identities. Never renamed: pins are keyed on it.

**Explorer thumbnail handler (2026-09-25).** Registered on the `MediaViewer.Image` ProgId only,
never on an extension, so it serves exactly the types the user made MediaViewer the default for and
never overrides Microsoft's HEIF / Raw extensions. The DLL runs from `<root>\shellext\<version>\`,
copied from `current\` with the DLLs it loads (`MediaViewerThumbs.files`) on a version's first start;
an update never replaces a DLL a surrogate holds. The surrogate is shared with other vendors'
handlers, so the handler changes nothing process-wide and caps its own work: 512 MB read, edges
clamped to 1024, a deadline after which it answers "no thumbnail". It declares its own
`DllSurrogate` AppID; `fuzz_thumbnail` fuzzes its entry point. No property handler: Windows reads
those only from HKLM, which a per-user install cannot write.

**Spotlight importer (2026-09-25).** `MediaViewerSpotlight.mdimporter` claims only Matroska, WebM,
AVI and MPEG-TS, the D5 containers macOS does not index; Apple's Image importer already covers
every still type. It reports duration, size, codecs, audio, bit rate, media types, creation date,
title and camera (`shell/spotlight_fields.h`) through `meta/`, never a decoder.

**Quick Look (2026-09-23).** A sandboxed thumbnail `.appex` running `image::make_thumb_jpeg`, the
filmstrip's pixels, with no cache of its own. Stills only.

**Associations and the default app (2026-09-24).** Windows: the wizard writes per-user `ProgId` /
`OpenWithProgids` / `RegisteredApplications` keys for D5 stills (`MediaViewer.Image`) and video
(`MediaViewer.Video`); the Finish page's "Choose MediaViewer as the default" is ticked and opens
Settings > Default apps, where Windows confirms. Nothing writes `UserChoice` (Windows 10+ does not
allow it). Uninstall removes every key. Mac: Info.plist registers Image and Video document types
(Alternate, Viewer); a first-launch setup sheet has the default checkbox on and applies it only on
Continue; an answered prompt stays answered across updates.

---

## Packaging, install and updates

**First install is a wizard; updates are silent (2026-09-13).** Inno Setup, once: welcome, GPL,
LocalAppData location, Start Menu on / desktop off, progress, finish. Velopack for updates:
versioned folders, signed manifest, rollback. WiX is MSI and fights per-user silent updates. One
icon for wizard, Start, window, taskbar and ProgId.

**The wizard owns uninstall (2026-09-20).** Velopack is packed with `--shortcuts None`; the
duplicate uninstall entry it writes is deleted by the wizard at install and by the host after an
update, and only when its `UninstallString` names this install's `Update.exe`
(`is_velopack_uninstall_string`). Velopack's `--installto` clears its target, so it runs from
`[Code]` at `ssInstall`, before Inno writes anything. `Update.exe --silent uninstall` is not called:
it races Inno's directory removal. `[UninstallDelete]` names the Velopack layout.

**AI and WebView2 payload excluded (2026-09-20).** `dotnet publish` of a Windows App SDK project
copies its whole projection set. `tools/package/build-release.ps1` filters out AI / ONNX / DirectML
/ WebView2 and asserts they are absent. 43.4 MB of a 119.6 MB payload; payload became 72.7 MB.

**Both runtimes ship in the payload (2026-09-23).** A clean Windows 10 21H2 machine runs the
installed build with nothing pre-installed; the wizard is per-user with no UAC.

- **.NET:** `hostfxr_initialize_for_runtime_config` cannot load a self-contained component
  (`0x80008093`), so the shared-framework layout ships privately under `<install>\current\dotnet`;
  `find_hostfxr` prefers it.
- **Windows App SDK:** self-contained WinUI activates registration-free from the **executable's**
  manifest, which is the native host's. The build merges the SDK manifest into it with `mt.exe`;
  without it every XAML activation fails (`0x80040111`) and no command bar appears.
- **Size:** app 208.6 MB; 292.7 MB on disk after first install because Velopack keeps one full
  package for rollback. .NET trimming is not applied (unsafe for a hostfxr component and WinUI
  reflection).

**macOS first install is a disk image (2026-09-23).** A signed, notarized, stapled `.dmg`
(dmgbuild, GPL as its licence agreement), not a `.pkg` (root scripts, no Trash uninstall). Sparkle 2
with an EdDSA-signed appcast; updates are a zip of the stapled app. `mediaviewer_lab` stays the bare
instrument; `MediaViewer` is the same sources with `MV_APP_BUNDLE`. Uninstall is drag to Trash.

**First install tidies up (2026-09-25).** Windows Finish page: "Delete the installer when Setup
closes", on; a hidden `cmd` deletes `{srcexe}` after Setup exits; silent installs skip it. Mac
setup sheet: "Eject the installer disk and move the .dmg to the Trash", on, shown only when the
disk or remembered `.dmg` is still there, never while running from or translocated off the image.

**Versions are strict `x.y.z` (2026-09-23).** `ReleaseVersion` parses three numeric parts; a
suffixed version makes the updater go inert. The version is stamped into `CMakeLists.txt` on the
runner before configure, so `VERSIONINFO`, `MV_APP_VERSION`, the payload and the manifest agree.
The manifest carries `min_version` and a blocklist as a kill switch. Releases go through a manual
workflow: artifacts (unsigned), preview and stable; publication waits for both platforms and goes
through a draft; published assets are never replaced. `RELEASING.md` is the runbook.

**Preview update channel (2026-09-26).** Settings: **Update channel: Stable / Preview**, Stable by
default. Windows: `[update] channel`, mirrored as `kChromeFlagUpdatePreview` (bit 11); Mac:
`mv.updateChannel`. On Preview each app reads the public release listing and uses the feed of the
highest-versioned release that has one (`GithubManifestFetcher` + `ChannelSource`; Sparkle's
`feedURLStringForUpdater:`). The listing only picks which signed feed is read; signature, channel
and "newer than running" checks are unchanged, and asset URLs outside the repo's releases are
ignored. Previews are signed exactly like stable. `github-release.py` refuses a release that does
not exceed every published one, so switching back to Stable never downgrades. Add-ons ship on stable
releases only.

**Add-on updates are offered, not silent (2026-09-27).** Opening Settings probes the channel for
installed add-ons; a newer signed manifest shows **Update to X**. Nothing is fetched without a
click (up to ~1.2 GB). The store writes `prune.pending` naming the new version and removes the rest
at the next start, so a running pack keeps its files. A new piece loads at once; a new Import or
Core takes over at the next start.

---

## Crash reporting and privacy

**A post-crash scrub, not an arena-tagged heap filter (2026-09-14).** Crashpad's handler has no
filter hook. Indirect memory gathering is off, WER forwarding off, no extra ranges, so heap pixels
are excluded structurally. The app rewrites every finished dump on its next launch, before any send
(`shell/minidump_scrub`): zero every captured byte outside a thread stack (PEB, process
parameters, TEBs, the 512 bytes around each register); mask drive / UNC / POSIX paths, bare media
filenames, the username and computer name in UTF-8 and UTF-16LE. The handler never gets an upload
URL: it would upload before the scrub. `tools/minidump-scan.ps1` checks the result. Measured on a
real crash before the scrub: no heap pixels, but the canary path and username 135 times.

**Mac crash reporting (2026-09-24).** Crashpad out of process in `Contents/Helpers`, the same
scrub, plus a Swift / AppKit record tied to the native report by correlation id;
`tools/mac/crash_canary.py` is the verify.

- **Stack fragments:** thread-stack bytes also mask a run of two or more path components (a space
  ends a component; `://` URLs and `./` / `../` runs are left). The same run outside a stack is left
  so `/System/Library/…` stays symbolicable. A single folder name with no separator can survive.
- **NSException:** AppKit routes event-handling exceptions to `-[NSApplication reportException:]`,
  not the uncaught handler, so the chrome record is written there too (one per crash).
- **Report metadata:** the rewrite copies Crashpad's extended attributes onto the replacement.

**The environment block is dropped, including on a stack (2026-10-12).** A Mac Crashpad dump
kept the process environment because the kernel places argv and the environment at the high end
of the main thread's stack, and the scrub zeros only what is *outside* a stack. A username inside
an email local-part (`alice.jones@…`, or a longer token such as `alicejones@…`) is not a bounded
identity, so it survived the text pass. The scrub now zeros a run of `KEY=value` strings on a
stack when the run contains a marker key (`HOME`, `USER`, `PATH`, and the Windows names), and the
argv strings packed against that run. Nothing in the slab is needed to symbolicate.

Windows did not have this gap for the process block itself. The environment, the command line and
the current directory live in the PEB (`RTL_USER_PROCESS_PARAMETERS`), which is not a thread stack
and was already zeroed. A copy that landed on a Windows stack is covered by the same run detector.
There is no separate cwd stream on either host. The Mac process cwd is not a Crashpad stream;
`PWD`, when a shell put it in the environment, goes with the block.

---

## Licensing

**App licence GPL-3.0-or-later (2026-09-06, 2026-09-25).** No Store, so the app is GPL; it moved
from GPL-2.0-or-later to 3.0-or-later for GPLv3's "Appropriate Legal Notices" (§0, §5(d)), which is
what About shows. `LICENSE` (GPLv3), `NOTICE`, copyright + SPDX on every first-party file; holder
`Copyright (C) 2026 longtimeno-c`. Apache-2.0 components (the Crashpad client) are compatible
outright. The name and icon are a trademark request in the README only. `tools/licence-check.ps1`
asserts the GPLv3 `LICENSE`, `NOTICE`, and `vcpkg.json` = GPL-3.0-or-later.

**Rules the GPL does not relax (2026-09-06).** FFmpeg is LGPL-only (no `--enable-gpl` or
`--enable-nonfree`): x264 / x265 are refused for patent exposure, not copyleft. No software HEVC or
AAC encoder anywhere. No LibRaw GPL demosaic pack. FFmpeg, libheif, libde265, LibRaw and Exiv2 are
dynamically linked. The licence check also rejects gpl / nonfree / x264 / x265 / fdk-aac in FFmpeg's
feature list (2026-09-25). Exiv2 stays under the GPL ("GPL-2.0 or later" upstream).

**Other dependencies (2026-09-24).** BLAKE3 under CC0-1.0 (Apache-2.0 alone does not combine
with GPL-2.0 code), libsodium (ISC), SQLite (public domain); all in THIRD-PARTY.md.

**Final Cut Pro framework (2026-09-28).** The extension loads FCP's own `ProExtension.framework`
at run time; no Apple code is distributed. The owner's call as copyright holder.

**PDFium in the AI pack (2026-10-16, #354).** Owner call 2 of docs/plans/document-search.md
(2026-10-05): PDFium reads a PDF's text, shipped in the Core pack, never the base installer. The
prebuilt bblanchon/pdfium-binaries chromium/8066 is pinned by SHA-256 in `cmake/ai.cmake` and
loaded at run time. PDFium is BSD-3-Clause; the binary also carries FreeType (FTL), libjpeg-turbo
(IJG, BSD-3, Zlib), ICU (Unicode-3.0), libpng (libpng-2.0), OpenJPEG (BSD-2), Little CMS and
simdutf (MIT), abseil and LLVM libc (Apache-2.0), zlib and Anti-Grain Geometry 2.3 (a notice-only
permission). All are permissive and combine with GPL-3.0; FTL and IJG were already accepted for
the base app. `addon-pack.py`'s allowed set gains FTL, IJG, Unicode-3.0, libpng-2.0 and
`LicenseRef-AGG-2.3`, the library is labelled with their conjunction, and the pack ships the
upstream notices as `LICENSES/pdfium`. The Windows release signs `pdfium.dll` with the app's
Authenticode certificate, as the Mac pack signs every dylib it carries.

---

## CI and fuzzing

**`fuzz_decode` and `fuzz_animation` are not in the gating fuzz step (2026-09-23 to 2026-09-25).**
Both exit `0xC0000142` (`STATUS_DLL_INIT_FAILED`, not a missing DLL) before libFuzzer runs a unit
on the CI image, while the other eleven harnesses fuzz clean. They are still built and run alone in
a separate `continue-on-error` step (30 s each). Run first and alone they still die, so ordering and
resource exhaustion are ruled out; with clang-cl + static ASan locally both run clean. The cause is
in what the process loads on that image.

---

## Add-on mechanism

**Add-ons are signed, verified, per-user packages (2026-09-24).** Settings → Add-ons, a signed
manifest, verify before load, a per-user versioned folder, and sideloading. A native shared library
behind a host function table, plus a chrome assembly (Windows) or `NSBundle` (Mac). Absent from the
base tree when not installed. One signing key: the update-manifest Ed25519 key, verified in C++ with
libsodium on both platforms; the Mac bundle is also Developer ID-signed. Install refuses an older
signed version than a working installed one. A hidden file or link beside an add-on is refused.

**Host table versions only append (2026-09-26).** The host serves every layout from
`MV_ADDON_HOST_API_OLDEST` up and hands an add-on the newest both know, so an installed Import did
not "need an update" when Local search shipped. Fields appended without bumping the version
(`thumbnail_jpeg`, `thumbnail_store_jpeg`, `recycle_file`) are read only when `struct_size` covers
them and they are non-NULL.

**The Windows chrome does not pin `mv_ai_status`'s size (2026-10-15, issue #388).** `AiApi` refused
any pack whose status struct was not exactly the size it was built with. The number was hand-copied
and went stale (1336 against a 1640-byte struct from 2026-10-05), so the Windows chrome refused every
pack and Local search "could not start". The struct already carries `struct_size`: the pack fills
only what the chrome declared, and the chrome zeroes the rest, so an older or newer pack is safe to
attach. The size check is dropped for `mv_ai_status`. `mv_ai_result` keeps its exact check, because
it comes back as an array with no `struct_size`, so its size is the stride. Drift is now caught
in CI: `tools/abi-layout` prints the offset and size of each field as MSVC lays them out, and the
ABI smoke test compares them, both ways, with the C# mirror.

**Developer key (2026-09-26).** `MV_ADDON_DEV_PUBLIC_KEY` lets a developer build trust a local key
so a pack can be sideloaded; with it, `MV_DEV_ADDONS_DIR` / `MV_DEV_THUMBS_DIR` keep the run out of
the user's install. CMake warns and refuses it under GitHub Actions.

**Files are hashed once per process (2026-09-27).** Re-hashing on list, load and each piece lookup
cost ~6 GB of reads per launch with the whole pack. The first check hashes; later checks stat every
file (size and `io::file_stat::mtime_ns`) and re-walk for extra files.

**Every window searches; one window indexes (2026-10-07, owner).** Reverses "later windows show
no add-on features" ([18](18-import.md) "One host process"). The owner installed Local search on
Windows and could not open it: any window after the first silently turned Ctrl+F into a file-name
search. A window that does not own the host lock now loads the AI pack through its read-only door
(`mv_ai_reader_get`, the Final Cut agent's reader: index.db read-only, text towers on CPU, catches
up every 5 s) with `mv_addon_load_reader` on Windows and the same `loaded_addon::load` entry on
the Mac. Search, Find similar and People's photos work there. Indexing, settings, People edits
and add-on install / update / remove stay in the first window, and the later window says so. Two
writers on one index are still never allowed, which is why this is not "load the pack twice".
It starts at the window's first search or Settings, not at launch: reading what is installed
hashes every file once per process (~1.2 GB with the pack), which must not compete with the new
window's first pixel. Cost once started: that window holds the text tower (CLIP text ~130 MB
fast, ~250 MB high) while open. A reader never sweeps the store's staging (the host may be installing into it). Nothing
indexed yet is `not_found`, not "needs an update"; Ctrl+F retries at most every 10 s.

**An add-on that will not start says why, in the window (2026-10-07, owner).** Load and start
failures went only to Settings' status line, and a DLL Windows refused to load (a missing
dependency) was reported as "did not pass verification", because `shared_library::open` returned
`corrupt`. It now returns `io`, with a reason written for the person and no path (rule 6):
LoadLibrary's error mapped to words on Windows, dlerror's kind on the Mac. The ABI passes it as
`mv_last_error_message`. The window shows "⚠ Local search couldn't start" (or Import), with the
reason and Open Settings, beside the indexing pill.

**An add-on carries its own code and its own data (2026-10-11, owner, #348).** A feature delivered
as an add-on lives in its package: its native library, its chrome (if any) and its data, signed
and verified together, installed and removed together. It is not shipped dormant inside the app
with only the bulk downloaded. This replaces the 2026-09-28 call made for Final Cut Pro search
("Shipped inside MediaViewer.app, off until turned on", below under Final Cut Pro search) as the
rule for add-ons. Still true: there is never a second *app* to install or open; an add-on is a
package the one app loads. The first to follow it is Places (#344): `src/geo` and its GeoNames /
Natural Earth data ship in the Places package, not in the base install. What was built under the
old call (the Final Cut extension and agent) is not moved by this change; moving it is its own
work and its own row.

**Screens and slots are filled through the host table first (2026-10-11, #348).** PRs 58–59 landed
before PR 57's script runtime, so MediaViewer's own native add-ons fill them through host table v3
(`screen_post` … `locations_in`) and the add-on side's `mv.slots.1` (§9 option B), and the
registry reaches an add-on's code through a `slot_client` so a script add-on fills the same points
later. A stranger's (schema 2) add-on that names a screen or slot point is refused
`code_not_allowed` until then. Every request runs on one worker per add-on, coalesced by kind,
generation-tagged; the chromes read tables and never wait (rule 1). Contribution API numbers keep
§7's: screens are 4 and slots 5 although 2 and 3 (PRs 56 and 57) are built separately; a manifest
naming a point by an API it does not reach is malformed, one whose range starts past the host's is
"needs a newer MediaViewer".

**Local search is a search provider (2026-10-11, #348).** While the AI pack is attached the host
lists it in the registry as the local-scope provider; the path bar's search icon stays the base
app's own (file search or Local search, as before), and a Local search query also goes to every
other local-scope provider, whose rows the result list's path row offers to add ("+ 3 in
Portugal"). The pack's own results keep their rich list (moments, faces, scrub markers) in its
panel; it answers the registry with no extra rows. Moving those results into the registry would
mean rebuilding the AI chrome on the screen vocabulary, which is not this change.

**A docked pane is a window held to the viewer's right edge (2026-10-11, #348).** Neither host has
a slot for a third docked island beside the Metadata pane and the folder tree without a layout
change in the native hosts (`main_mac.mm`, `chrome_host.cpp`). An add-on's `pane` screen is a
child panel on the Mac and a window placed at the right edge on Windows, with Done and Esc to
close. A true docked island is a follow-up if an add-on needs one. It overlaps the canvas's right
side (no reflow). Held on the Mac (moves, resizes, full screen, a new main viewer window: 2026-10-12);
on Windows only placed at open, see [25](25-open-addons.md) "Screens".

---

## Import add-on

**What it is (2026-09-24).** The gain is copying less and verifying without a second pass, not a
faster copier: a size-then-BLAKE3-256 duplicate skip, hash on read with an uncached read-back,
parallelism only across physical devices. A name match is never a duplicate. Import never deletes
from or formats a card and never overwrites a destination file. Details: [18](18-import.md).

**Per-destination rules (2026-09-24).** A duplicate on the main destination still goes to a backup
that lacks it; backup copies do not count as library copies. A file a crash left on one destination
is kept there and copied to the other. A member that fails on one destination removes what it wrote
on the other.

**`F8` across volumes moves through the verified copy (2026-09-24).** The base app deletes the
source only after the copy is verified, on both hosts.

**Find duplicates (2026-10-03).** A tool in Import that hashes every file under a folder (not only
media; hidden, system, packages and empty files are skipped) and reports groups.

- A group is never emptied: the engine checks before each move that the file is unchanged since it
  was hashed and re-reads another copy holding the same bytes; otherwise the request is refused.
- Moves go to the Recycle Bin / Trash only, through the host's `recycle_file` (Windows
  `io::recycle_file`, Mac `NSFileManager trashItemAtURL`); no bin, no delete. No confirmation per
  file or batch: every move is checked and recoverable. Several picked copies go in one action
  (platform multi-select); a pick that takes every copy of a file sends nothing.
- `mv.import.1` gains `find_duplicates` / `trash_duplicate` at the end; the Windows Import chrome
  reads them itself (`DuplicatesApi.cs`) so old and new app / add-on combinations still load.
- Hashes are remembered in `import.db` `seen_hashes` (path, size, mtime, hash), trusted while size
  and mtime match. Cached reads; one reader per device; the import's yield to the viewer.
- Measured (M-series, Release, warm, one run): 2,400 files / 846 MB, 400 copies: first scan
  1,042 ms, rescan 84 ms reading no file contents.

---

## Local search (AI add-on)

**Runtime (2026-09-24).** ONNX Runtime; GPU through vendor providers shipped as optional pieces
(CUDA, OpenVINO) with CPU always underneath, and Core ML on the Mac. No DirectML, so no D3D12. A
download from Settings, never in the base installer. Indexes are local to one machine.

**3 GB ceiling for the AI family (2026-09-25).** The base viewer's 250 MB limit does not apply to
the optional pack. Installed pieces may total up to 3 GB; `index.db` and thumbnails are user data,
outside it. CI rejects an oversized combination and the installer refuses it. Raised to 5 GB on
2026-10-13 (next entry).

**AI family ceiling is 5 GB (2026-10-13, owner).** Amends the row above. Installed pieces of the
`ai` family may total 5 GB. The index, the thumbnail cache and the Core ML compile cache stay
outside it, as before. Today's heaviest install (Core, People, Sound and the CUDA piece) is still
about 2.93 GB, so nothing already installed grows and nothing new is downloaded. The room, about
2.0 GB, is what a later piece has to fit in: the document text model in
`docs/plans/document-search.md`, including multilingual-e5-small, which did not fit in 3 GB, or
one more vision or speech piece of that size. A piece that would pass 5 GB is still refused
before download. Enhance stays its own family. Its 5 GB figure is a per-weights-file cap, not
this ceiling. Retired on 2026-10-18 (next entry).

**Models come from Hugging Face; the 5 GB ceiling is retired (2026-10-18, owner, #482).**
Supersedes the 2026-10-13 row's "nothing new is downloaded" and its ceiling. The `ai` pieces
published on GitHub carry code, the signed model list and our own exports only (AdaFace, the
ViT-H/14 tower). Every upstream model file is listed in the piece's Ed25519-signed
manifest (`remote`: URL, size, SHA-256, licence) and fetched by the add-on from Hugging Face at
its pinned revision, a plain GET with no token, identifier, query or cookies. A file that does
not match is deleted and fetched again, never loaded. When Hugging Face cannot be reached the
download waits and resumes by itself from the last verified byte, including after a restart; a
404 on a pinned revision is reported as gone (it needs a new pin in an app update). There is no
mirror of our own and no offline path. Hugging Face sees an IP address and which public files
were fetched, as GitHub does for the add-on; nothing about the user's media. With the models out
of the archives, the ceiling has nothing to bound: `family_ceiling` returns none, `addon-pack.py
ceiling` has no `ai` limit, and `ai-models.py check` no longer sums sizes. The picker still shows
each tier's download and disk size. `ai-models.py check` stays the licence gate: pointing at
research-only weights is no better than shipping them.

**The first-party AI add-on may use the network to fetch its models (2026-10-18, #482).** Through
its own transport (`fetch_mac.mm`: NSURLSession, ephemeral; `fetch_win.cpp`: WinHTTP, cookies
off), to `https://huggingface.co/` URLs that its signed manifest lists, and nothing else. The
"no network, ever" rule of [25](25-open-addons.md) is for third-party code and is unchanged.

**Model tiers (2026-10-18, #482).** Four tiers (Maxxed, Default, Intel, Potato), each a profile
in `ai-models.json` that names a model per role; the add-on names no model in code. Nothing
downloads until the user picks one in the add-on's picker, which recommends a tier from a local
check (cores, memory, accelerator) that is never sent anywhere. An index made before tiers keeps
Default without asking. A change of tier re-indexes while the old index answers. Candidates:
Maxxed's pictures are OpenCLIP ViT-H/14 (CLIP BPE, so no new tokenizer; SigLIP 2 waits for a
SentencePiece tokenizer); its speech is Whisper large-v3-turbo; its documents stay
all-MiniLM-L6-v2, since multilingual-e5-small already missed the R@10 target in the #354 D3
spike; its faces stay AdaFace IR-50, since IR-101's gain on LFW (99.90 % vs 99.81 % TAR at 24 px)
does not pay for twice the cost without a real-library gain. Potato is B/32, Whisper tiny and
SFace: TinyCLIP ViT-8M/16 missed the bar (R@10 0.838) and ViT-39M/16, which passes (0.922), is
about 4x slower than B/32 on CPU, so neither saves anything. Each tower's thresholds come from `tools/ai-eval/calibrate.py`, and a
tower under the PR 20 bar is swapped, not waved through (17 "Model tiers").

**Towers and pieces (2026-09-26).** CLIP ViT-B/32 and ViT-L/14 both ship (L/14: +6 points R@1 but
1.9 img/s on CPU); Auto picks by backend. int8 stored vectors (≤ 0.5 points), not int8 weights.
One add-on id per piece with `part_of`: `ai`, `ai-faces`, `ai-audio`, `ai-cuda`. The Mac pack is
arm64 only: ORT has no x86_64 macOS build.

**`ai-cuda` is published; CUDA and cuDNN are user-supplied (2026-10-03).** The piece is ORT's CUDA
13 build (Microsoft-signed, MIT) and redistributes nothing of NVIDIA's; without CUDA 13 / cuDNN 9
installed the self-test falls back to CPU and says so. Stable Windows releases only; offered only
with an NVIDIA adapter, never in "Install all".

**Core ML runs the picture towers with pinned shapes (2026-09-27).** With dynamic shapes Core ML
took a fraction of the nodes and ran no faster than CPU. Pinned at open
(`infer::session_options::fixed_dims`, `kCoreMLImageBatch`) it takes every node: B/32 499 img/s vs
73 on CPU, L/14 ~30 vs 4.9, cosine ≥ 0.998. The compile takes 82 s / 5.3 min the first time, so the
pack answers on CPU and swaps Core ML in when ready. CLAP and Whisper run on CPU on the Mac (they do
not compile on Core ML).

**Audio search (2026-09-27).** `ai-audio` (~1 GB) searches a video's soundtrack by sound (LAION
CLAP, 87.2 % ESC-50) and speech (Whisper small on a GPU, base on CPU). Video soundtracks only.

**"Nothing found" (2026-09-26 to 2026-09-28).** A calibrated rule over a generic-prompt margin, not
an absolute cosine floor (which cannot separate nonsense from real queries on CLIP). Added since: a
top-ten stand-out test for one-word subjects (2026-09-27); a broad-subject pass when rows clear the
margin on max(5, 1 %) of assets, and singular / plural embedded as one query (2026-09-28); at
library scale (issue #85), margins that grow with log10(assets / 1,000), a stand-out factor of 1.18
of noise, and a 705-label vocabulary that keeps a row only where the query scores at least like its
ninth-best label. At 25 k assets nonsense answered 63 → 23 of 85 (L/14), category precision .60 →
.87, P@5 unchanged, category recall −.06 to −.08. Sentence-like gibberish still floods on L/14.

**Result order (#412, 2026-10-15).** Best match when the search knows what it is answering;
newest by date created to oldest when it does not. A description the "nothing found" gate holds
back used to be an empty page (and, the report said, read as a weak ranking); the owner asked for
what the rest of the query allows, newest first, instead, and kept best match for every answered
query (amended the same day: never replace an answered query with date order). The date is the
one the photo was created — a Photos item's `creationDate`, a file's date taken, else its
modification time — so an edit in Photos does not move it; ties go by path. The gate itself is
not retuned (gibberish that passes it is still #85).

**Precision scale (2026-09-27).** Five steps in Settings; the middle is the calibrated rule. A
stricter level answers a subset of a looser one; read per search, no re-index. On COCO, level 3 stops
L/14 answering "helicopter" with planes. It is the only search-quality setting (2026-09-28).

**Scope always includes subfolders (2026-10-03).** The panel offers Folder & subfolders (default),
Everywhere, and Photos on the Mac; indexing a folder always includes its subfolders. The ABI keeps
`MV_AI_SCOPE_FOLDER` and `recursive` so older folder-only roots work.

**Indexing is explicit and persistent (2026-09-25).** Roots are stored in `index.db`; later
launches scan only what changed and resume partial video work. Results are a listing in the
existing gallery (`mv_folder_open_list`).

**Index export and import (2026-09-28).** Paths relative to each folder with `(mtime, size)`; the
delta scan re-queues what differs. Faces and names go only with "Include People" ticked (off by
default). The imported file is untrusted input; thumbnails must be JPEG ≤ 512 px. With thumbnails
ticked, missing ones are made at export, yielding to the viewer (2026-10-03).

**People (2026-09-28, 2026-10-03).** AdaFace IR-50 (fp16, 87 MB) replaced SFace: LFW TAR@1e-4
99.00 → 99.93 %, 94.33 → 99.81 % at ~24 px; BCubed precision 0.9938 → 0.9994. Refinement runs only
on request ("Refine faces" for one person; "Re-analyse faces" or a new face model ends with one
library-wide settle). Vectors are tagged with their embedder, and re-found faces keep their person,
name, pin and rejections.

**People need a face someone could recognise (2026-10-06, owner: "terrible at categorising
them").** Faces below quality 0.10 belong to no person; weak faces (< 0.35) may join a person but
never start one, and an unnamed person of weak faces only dissolves. People are counted in photos
and clips, not faces, and Settings lists the first 24 with "Show all". The online clustering had
made people of blurred clip frames that resembled each other more than anyone; numbers in
docs/design/17 "People model". Libraries settle once under the new rules, nothing read again.

**New faces meet a confirmed person's exemplars (2026-10-15, #434).** This amends the 2026-10-06
rule that a new face joins by mean pairwise cosine to every member. The best 3 people by that mean
are shortlisted. A shortlisted person with a pinned face (the user named, merged or split it) is
then rescored by the refinement's exemplar `support`, so the faces the user confirmed are what new
photos are matched against. Weak faces and unconfirmed people keep the mean. Exemplars for every
person were tried and dropped: on LFW they cost online BCubed F 0.9975 → 0.9945, because a newcomer
joined a lookalike. Numbers are in docs/design/17 "Online clustering".

**People follow the open folder (2026-10-03).** While a folder is open the People grid offers
"This folder | + Subfolders" only; everyone shows only with no folder open. A folder open is the
user saying what they are looking at, and everyone-from-everywhere inside one read as a leak. A
search is a question rather than a place, so the search panel keeps "Everywhere".

**People lives in Settings on Windows too (2026-10-05, owner).** The Windows People window
(a grid on the left, a person's faces on the right; a click selected, a double-click showed
photos) is gone: People is the Mac's inline grid in Settings → Local search, a click shows that
person's photos, names are edited on the card, and a person's faces open in a small sheet. The
window existed because the Settings island could not hold a text field; FakeInput can, so the two
platforms now behave the same.

**The Photos library as a folder, its backup, upright clips (2026-10-03, owner).** Once the
library is added in Settings it opens as a listing of *virtual items* (`photos:<id>` keys, tiled
from PhotoKit's cache, resolved to a file only as shown), and search results take the same path.
An original is downloaded when viewed (after a 400 ms stay), copied out, or backed up — each is the
user asking for the file; the index downloads only clips, and only with the 2026-10-05 opt-in. The backup copies originals into
`YYYY/YYYY-MM-DD` through `io::verified_copy` with a manifest, so a re-run writes nothing for what is
there; not a mirror, no schedule. Both platforms: the player honours a clip's display matrix
(`video_stream_info::rotation`), because portrait phone clips played sideways. See
[26](26-photos-library.md).

**The Photos backup's folders are a choice (2026-10-16, issue #414).** Was: always Import's
`YYYY/YYYY-MM-DD`, so a library of a few years became thousands of day folders. Settings →
Photos Library → *Folders* offers Year (`YYYY`, which Import does not have), Year and day (the
default, so a backup already on a NAS keeps writing the tree it has), Year, month and day
(`YYYY/MM/DD`), Day (`YYYY-MM-DD`) and Flat; remembered as `mv.photosBackup.layout` beside the
destination. A change moves nothing: a verified file stays where the manifest says, and only a
new or missing file is written in the layout chosen now. The manifest therefore stores each
file's real place under the destination at whatever depth (`2024/IMG_0001.HEIC`), not the last
three components; absolute rows from before 2026-10-05 were all year/day, so their last three
components still rebase. Import's presets, camera and type folders stay Import's.

**The index may download iCloud-only clips, opt-in (2026-10-05, owner).** Reverses "Indexing never
downloads" below for videos only. With Optimize Mac Storage most of an iCloud library's clips are
iCloud-only, so they were found by a poster frame alone: no moments, no sound, no speech, no faces
(the owner's library: 4,466 clips). Settings → Photos Library → *Download iCloud videos to index
them*, off by default: a fetch thread downloads such clips through `PHAssetResourceManager`
(network on, the current edit else the original) two ahead of the workers into
`<data>/cache/icloud/<row id>.<ext>`, re-queues each at the front of every track, and deletes the
file once pictures, sound, speech and People are done with it. It waits on battery, on an
expensive or constrained network (`NWPathMonitor`: a hotspot, Low Data Mode), under 10 GB free, and
while indexing is paused; a clip iCloud does not send is tried again next start. Photos are not
downloaded: PhotoKit keeps a local preview of nearly every photo, and the indexer reads 448 px. The
library is still never written, and nothing about it leaves the Mac (the download is the user's own
iCloud to their own Mac). Mac only: on Windows iCloud for Windows syncs to a folder.

**Cloud files in indexed folders: walked, counted, never read unasked (2026-10-05, owner).** A
Pictures folder moved to OneDrive was not being searched. The folder walk (`io::walk_files`,
written for camera cards) skipped every reparse point, and OneDrive's Files On-Demand files and
folders are reparse points, so such a folder indexed nothing and still said "Up to date". The walk
now skips only name-surrogate reparse points (symlinks, junctions), which are still never
followed. It lists cloud files, flagging the online-only ones (`FILE_ATTRIBUTE_RECALL_ON_*`;
`SF_DATALESS` for an evicted iCloud Drive file on the Mac), and reaches past `MAX_PATH`. Host
table v1's `walk_files` still leaves online-only files out (a copy never starts a download).
`walk_files2` (appended) lists them flagged. The index marks them (`assets.cloud`, schema 3), and
no track reads one, since reading it is a download. Settings says "N only in OneDrive / iCloud
Drive". *Index online-only files* (`cloud_files`, off by default; the owner chose an opt-in
matching the Mac's iCloud videos) brings a couple at a time down in place. It uses the Cloud Files
API on Windows and NSFileManager's ubiquitous-item calls on the Mac, both in the pack, not the base
app. Each file is indexed first and made online-only again once no track wants it. It runs only on
power and an unmetered network, with 10 GB plus the file free, and never while paused. A crash
leaves row ids in `<data>/cache/cloud-fetched`, and those files are given back at the next start.
Folder adds that fail now say why. A folder whose walk fails reads "Couldn't read this folder",
and one with nothing to index reads "No photos or videos found here", rather than "Up to date".

**The Windows welcome card: picked folders count, OneDrive leads (2026-10-07, owner).** A folder
chosen in Open ▸ Folder (`Ctrl+Shift+O`) went straight to `open_folder` and never became a recent
folder, so the Windows card stayed empty for anyone who opens folders that way; the Mac records
every non-browsing open in one place. It is recorded now. With OneDrive set up (the client's
`%OneDrive%`, read from the environment: no disk access before the first pixel), a OneDrive row
leads the card above Recent folders, drawn like the Mac's iCloud Photos row with no × . It opens
OneDrive's Pictures folder, where a phone's camera uploads land, or OneDrive itself, and is not
itself added to Recent folders. Open ▸ Recent folders keeps indexing the folders alone.

**OneDrive support is optional at load time (2026-10-07, owner).** 0.1.27 linked `cldapi.dll`
straight into `mv_ai.dll`, so a Windows without the Cloud Files API (before 10 1709, Server 2016)
could not load Local search at all. It is delay-loaded now, and `make_cloud_files()` probes for it
first. Where it is missing the pack loads and searches, and Settings shows *Index online-only
OneDrive files* greyed with the reason. The option is always shown on Windows. Turning it off
skips online-only files and never makes search unavailable.

**Local search loads nothing at launch (2026-10-11, owner; issue #292).** With the pack installed,
ONNX Runtime, the model sessions (tokenizer vocab parsing alone was ~1.2 s a model before #251)
and the indexing threads all started with the app, costing memory, CPU and launch time in every
session, including those that never searched. Now the host only verifies the pack at launch; the
first search (the icon, `Ctrl+F` / `⌘F`, the pill) or Settings → Local search → **Start now**
loads it, and it then indexes for the rest of the session. Opening Settings alone does not load
it, because both hosts' Settings is one page and a visit to change a key would otherwise start
the whole pack. **Cost:** files added while search was never opened are indexed only after search
is first opened that session, so the first search after launch may say "Indexing new files…" for
a while. Launch → first pixel and idle memory / CPU with the pack installed, before and after, are
owed on both platforms (the measurement needs a release-signed pack).

**An unfinished index waits for Resume (2026-10-11, owner; issue #299, amending the entry
above).** Reopening the app after a quit or a kill part-way through a large root (the Photos
library) used to carry on by itself, which loads ONNX Runtime and the model sessions even for an
app opened for a few seconds. Now such a root is held until Resume (`hold_unfinished`, on in the
shipped pack), and `data/resume.json` names the unfinished roots, which both hosts read at launch
without loading the pack to show a bottom-right notice (Resume / Dismiss); Settings' folder rows
offer Resume too (docs/design/17 "Unfinished roots wait for Resume"). Opening search loads the
pack but does not resume a held root. **Cost:** an index left unfinished stays unfinished across
launches until Resume, and files added to that root meanwhile wait too; search answers from what
was already committed. Launch → first pixel and idle CPU with a root left mid-index, before and
after, are owed on both platforms with an installed pack (the measurement #292 owes too).

**The Mac Photos library as a source (2026-09-28).** Read-only: no PhotoKit write API is called,
and `shell/write_guard.h` refuses every write, move, bin, rotate, export and trim of a Photos file
or listed preview. Indexing never downloads (amended 2026-10-05: iCloud-only clips, opt-in, above);
an iCloud original is fetched only when its preview is viewed (400 ms) and deleted when the next list opens and at quit. `photos:<localIdentifier>` is
treated like a path (never logged). The base app carries `NSPhotoLibraryUsageDescription` and the
photos-library entitlement, because TCC attributes the request to the app; access is asked only on
a click. A full re-enumeration of 23,089 assets takes 0.43 s, so no change tokens.

**Date terms go by the date taken (2026-10-15, #404).** `in:`, `before:`, `after:`, `since:` and
`until:`, and the newest-first order of a filters-only search, use schema 4's `taken_at` (the
camera's wall clock read as UTC, the viewer's date-taken sort key, or a Photos item's
`creationDate`) and fall back to the file's modification time only while an asset has none (no
EXIF date, or the tag backfill has not reached it). A camera dump copied or synced in 2026 matched
`in:2026` and not the year it was shot. No timezone conversion: `in:2024-06` is the stamp's own
calendar month. Nothing is re-embedded and the schema is unchanged; the column was already there.
**Cost:** during the backfill a file's date can move from its mtime to its date taken, so a date
query run then may change its answer once.

**Rating, label and exposure are query terms, not words (2026-10-15, #405).** `rating:`,
`rejected`, `label:`, `iso:`, `f:`, `mm:` and `shutter:` filter on numbers the tag pass already
reads from the header the Metadata pane's summary comes from, stored in AI index schema 5 and in
meta_cache.db (`kSearchFactsVersion` 2). They are kept out of the search fields and `asset_text`
on purpose: a plain "400" matching every ISO 400 frame would bury the picture search. Aperture
matches within 0.05 stops (f/2.8 written as 2.8 or as APEX's 2.83) and shutter within a sixth of
a stop (1/250 is not 1/320). The tag bookmark became a level (1 = schema 4's facts, 2 = these),
so a schema-4 index reads each file's facts once more, embedding nothing. **Cost:** one bounded
head read per indexed file after the update (the cache rows of version 1 are read again), and a
rating written to a RAW's sidecar does not reach the AI index until the file's stamp changes.

**A clip's chapters and text subtitles are indexed as exact text (2026-10-15, #406).** Quoted
words and `said:` match a chapter title or a text subtitle line as they match a Whisper line,
with the cue's start as the moment and a snippet that names the source ("Chapter: …",
"Subtitle: …", `MV_AI_MATCH_TEXT`). The host reads cues as packet text through the FFmpeg it
already links (no subtitle decoder, no new library); they live in their own `cues` table, not
under the Whisper spec, so removing the audio piece keeps them and a shared index never disagrees
about what was said. Bitmap subtitles are not OCR'd. **Cost:** one header open per indexed clip
(after its pictures), and a full demux of the subtitle stream for files that have one (an MKV
reads its clusters to find the cues), capped at two hours and 8,000 lines.

**The People pass looks at the indexed frames; a Live Photo's motion is indexed (2026-10-15,
#407).** Re-analyse (and People turned on after the pictures) used to resample a clip on its own
2 s / 10 s grid from 0, so a face sharp on one kept frame and gone for the next ten seconds was
never seen; on the owner's library, mostly clip frames, that was the common case. It now decodes
exactly the `pts_ms` the picture pass kept (the host's frame fetch, no pixels stored), and keeps
the grid only for a clip with no picture rows. A Live Photo's 1–3 s movie, dropped before, is
sampled onto the still's id (same sampler, same dedupe, seeded with the still) and a moment opens
the movie. Not changed: the sampler's 500 ms / 2 s gaps, keyframe-only decode, the dedupe.
**Cost:** one host frame fetch per stored moment in a People-only pass (a file open and a decode
from the keyframe each, against one sequential sampler pass before), and a handful of frames per
Live Photo in the picture pass. An exported index carries no `motion` columns, so the receiver
samples each Live Photo's movie once more.

**Words in photos are read by the OS and searched as text (2026-10-15, #403, urgent).** "suited
and booted" found the photo in Apple Photos and not here: CLIP does not read. The pack now asks the
operating system's own recogniser (Vision on the Mac, Windows.Media.Ocr on Windows; no model, no
download, nothing sent anywhere) for each photo's text in a background pass after the pictures,
and stores it as a `cues` row (kind 3) that quoted words, `said:` and plain words match like a
transcript line. A separate 2,048 px decode rather than the picture pass's 448 px one, because
sign and shirt text is small: 95 % of 40 labelled phrases on the Mac at 44 ms median a photo.
**Cost:** one extra decode and an OS read per photo, once; on the owner's 23 k-photo library that
is about 20–35 minutes of one background worker on an M-series Mac, after the pictures are done.
Video frames are not read yet.

**The text on clip moments is read too, and the text passes count in the status (2026-10-15,
#403 follow-up).** A sign or a burned-in caption in a clip is found at its moment: the text pass
reads the moments the picture pass stored, after that pass is done with the clip, decoding each
again at 2,048 px. Not the sampler's frames as they go by, because the picture pass decodes at
448 and must never wait on the OS reader; not every keyframe, because the stored moments are
already the clip's distinct views, and a sign held in shot over several of them is stored once.
Its own progress spec (`<reader>/moments`) so the photos already read are not read again, and
commits every 8 moments so a stop resumes; nothing is embedded. **Cost:** the moment's seek and
decode, which is most of it, plus the OS read: about 0.2 s a moment on the Mac (180–218 ms median
over three runs of 72 moments from six of the owner's clips, the read 20–22 ms of it, on a loaded
machine), once per clip, after its pictures. The cue and text passes now have
counts in `mv_ai_status` and keep the state from idle: before, the pill said "Up to date" while
they ran. Those three counts are 14 ms a refresh at 25,000 assets and the refresh runs once a
second even when idle, so they are cached and counted again only when something can have moved
them, or every 30 s (about 0.5 ms a second idle). The **Read text in photos** switch (Settings → Local search, both hosts, on by
default) turns both text passes off. The cue and text counts are appended on `mv_ai_status`
(1,672 bytes with them) and read only when `struct_size` covers them. `AiApi` still does not
pin that size (#388); the ABI smoke test compares the C# mirror with the header.

**Documents are searchable by their words, behind Show documents (2026-10-16, #354 D1).** PDF and
DOCX open in the viewer (#123, #124) but Local search skipped them. Slice D1 of
`docs/plans/document-search.md`, under the owner calls of 2026-10-05 (PDFium in the pack;
`doc_index` defaults to Words) and the issue's gate: indexed only while File types "Show
documents" is on, as audio files are. A PDF's text is read by PDFium in the Core pack, not the
viewer's renderer (the OS's), so both OSes write the same text into the same index; a DOCX's by
the viewer's own reader through the host table (`document_text`). Text is kept as passages of a
few sentences with their page (schema 9), matched through an FTS5 index by the speech rule, not
held in memory like transcript lines: a library of documents holds far more text than its clips'
speech. A password-protected file is a new progress state, `locked`, never retried. **Cost:** none
while the switch is off (the walk drops them by name). On, a document is read once, on one
background worker, after the pictures; the M5 timing is in docs/design/17 "Documents". Index size
grows by the documents' text (about their plain-text size twice over: passages and words).
`MV_AI_KIND_ALL` widens from 11 to 15, so an older chrome never sees a document; `mv_ai_status`
appends four counts (1,704 bytes); `mv_ai_api` appends `root_set_documents`, and the Windows
chrome's `AiApi` now requires the table only up to `people_reanalyse`, checking appended entries
where it uses them, so a pack one entry behind still attaches. The document pass does not wait for
the picture towers: on the M5 the pack spends about 118 s at every start moving ViT-L/14 to the
Neural Engine, during which every pass used to wait, and 200 documents then took 3 s. While the
towers are not ready the workers now claim documents only, straight from the index.

**Documents are also found by what their pages look like, at Words and pages (2026-10-16, #354
D2).** No new model: a document's pages go through the picture tower the Core pack already
loads, drawn by the host's own renderer (the same pixels as the viewer and the tiles), stored as
ordinary `frames` rows with `pts_ms` the page and a page flag (`kFramePage`), so search, ranking,
"nothing found" and the matrix treat a page as a clip's moment; no schema change. Which pages,
by the plan's §3 limit: the first 3, and any page whose image objects (PDFium) cover over half of
it. **Budget added:** at most 64 such picture pages a document beyond the first 3. The plan said
"any page"; a 500-page scan would then be 500 page renders and tower runs (each render reads the
file whole), minutes of a worker for one file. A DOCX is pictured on its first 3 pages only (the
host's layout gives no image measure). The pass runs only after a document's words are read, so
words are searchable first, and only at Words and pages (not the default). Going below it deletes
the pictures at once. `mv_ai_status` appends `pages_total`, `pages_done`.

**Documents are also found by what they are about, with the Documents piece (2026-10-17, #354
D3).** An optional piece, `ai-docs`, as Sound is, holding one sentence-embedding model chosen by
the plan's spike (docs/design/17 "Documents", the table there): **all-MiniLM-L6-v2**, fp32, on CPU
at 2 threads. It had the best P@5 of the four candidates (0.810 against arctic-embed-s 0.810 fp32
/ 0.794 int8, bge-small 0.780, multilingual-e5-small 0.766), twice their passages per second, and
a quarter of e5's size. **Not multilingual:** the owner's 2026-10-13 ceiling raise let
multilingual-e5-small in, and the spike measured it; it led only on the 3 other-language queries
and missed the R@10 target (0.793 int8), so the default is English, and a later multilingual
piece is a measurement away, not a design change. **fp32, not int8** (90 against 23 MB): the int8
export quantises activations per batch, so a passage's vector depended on its neighbours in the
batch (cosine 0.998 to ORT-Python, under the verify line's 0.999) for ~10 % more speed and no
quality. **Margins, as CLAP's** (no z rule): `query_margin` 0.415 and `result_margin` 0.25 from the
spike's 50 queries and 20 queries about nothing. They gate a row; its cosine orders it (ordering
by margin lost 0.03 of P@5), scaled to sit above a partial word match. **Short of a target:**
end to end over every passage the engine reaches P@5 0.731 but R@10 0.767 against the 0.80 set
before the spike (the spike's own capped run met both); long publications crowd the forms out.
Kept because the engine's words alone reach P@5 0.304 / R@10 0.272 on the same set (19 of the 50
queries answer nothing), and it costs nothing without the piece; the target is owed in #354.
**Cost:** nothing without the piece. With it, a document's passages are embedded once after its
words (schema 10 `passage_vecs`, int8 like frames: 0.4 KB a passage plus the row), on the
workers, between the viewer's busy spells, and not behind the picture towers' load. The vectors
are a third in-memory matrix like the sounds'. `mv_ai_status` appends `meaning_total`,
`meaning_done` (1,720 bytes); `MV_AI_STATUS_DOCS_READY` is 512. **Sharing:** `.mvindex` goes to
version 2; documents travel only when the new `MV_AI_TRANSFER_DOCUMENTS` is ticked on both sides
(the file then holds their text, so off by default, as People is), with their passages, vectors
and page pictures. A v1 file still imports. D1's "never in an export" now reads "never unless
ticked".

**Scanned PDF pages are read by the OS, last of all and a few pages a claim (2026-10-17, #354
D4).** A PDF page with no text layer (under 16 characters other than spaces, so a stamped page
number alone still counts as none) is drawn by the host and read by the text reader built for
#403 (Vision, `Windows.Media.Ocr`; no model). What it reads is stored as passages of that page
under the reader's spec plus `/pages`, beside the text layer's, which it never touches. Three
calls, each because the alternative measured or reasoned worse:
- **Last of all, 4 pages a claim.** A dense page is about 0.6 s of Vision (M5, accurate level),
  so a 100-page scan is a minute or more of one worker. Taking a document whole would hold that
  worker for the whole minute while new photos wait. Each claim reads 4 pages, commits each and
  returns, and the queue then looks at every other pass first. The pass also stops asking
  PDFium for text once it has found its pages and one more. Asking again for every remaining
  page each claim cost about 1,300 page visits for 100 pages in the rig, against 227.
- **Not behind Read text in photos, and no setting of its own.** Documents already have
  "Documents" (Off / Words / Words and pages) and Show documents; a scan's words are its words.
  The photo switch stays the photos' and clips'.
- **While the towers load.** Like the words pass, it needs no model, so it runs during the
  minutes a first Core ML compile takes. It still yields to the viewer, the pause, battery and a
  clear like every pass.
- **Meaning (D3) waits for it.** D3 embeds a document's passages once, whole; scanned text
  added after that would never be embedded. So where the OS reads, the meaning pass waits for
  this pass to be done, not just the words (`engine::meaning_after`). A text document's scan
  progress is done with its words, so only a scanned one waits. A scan whose pages never
  draw or read is done with nothing read, not failed, so its meaning is never held up.

`mv_ai_status` appends `scans_total`, `scans_done`.

---

## Final Cut Pro search (Mac)

**Mac-only, over a shared reader (2026-09-28).** FCP is Mac-only, so the extension and agent have
no Windows twin. Shared on both platforms: the pack's read-only reader (`mv_ai_reader_get`, the same
engine in read-only mode, so results match the app by construction), the wire format, the
thumbnail lookup and the FCPXML writer (`src/nle`, `mv-nle-export`).

**Shipped inside MediaViewer.app, off until turned on (2026-09-28).** (No longer the rule for add-ons: 2026-10-11, "An add-on carries its own code and its own data", under Add-on mechanism.) arm64 only: the `.appex`
(~180 KB) and a launchd job running `MediaViewer --search-agent`, handed over in `main()` before
anything of the viewer starts. A separate agent binary carried ~1.8 MB of copies of code the app
already links; measured cold and warm query times were within noise of it. **Turn on** registers
the agent (`SMAppService`) and elects the extension in (`pluginkit`); off, nothing runs, and the app
elects the extension out once so it is absent until turned on. The agent exits 50 s after its last
client (measured 52 s, inside the 60 s line).

**The extension loads FCP's `ProExtension.framework` (2026-09-28).** FCP traps without its
context class. The extension's `main` `dlopen`s the framework from the installed FCP, then calls
`NSExtensionMain`, and exits with a logged fault if anything is missing. Nothing of Apple's is
shipped. `com.apple.security.cs.disable-library-validation` is on the extension only.

**Panel (2026-09-28).** Scope defaults to the open library's folder and below, read from FCP over
Apple Events; a read-only file exception lets clips play with sound (Developer ID only). An AppKit
grid, not SwiftUI, to keep the extension one Objective-C++ build.

---

## Network copies

**Deep, parallel copies only to or from a share (2026-10-01).** `verified_copy` had one blocking
request per file and one file at a time, so on a share every step waited a round trip. The network
profile keeps several positional requests per file (hashed in order) and several files at once:
depth 8, 2 MiB chunks, 4 files = 80 MiB of buffers, capped at 128 MiB (`kCopyBufferBudget`).
Measured (`copybench`, Mac SSD with a 300 µs simulated round trip, verify on, alternated): 200 KB
files 69–75 → 123–131 files/s; 16 MB 484–488 → 916–941 MB/s; 512 MB 745–758 → 1,620–1,899 MB/s.
Cards are never read deep or by two files at once; local disks keep the sequential path. Full
read-back verify stays the default on a share. `F8` copy keeps the OS copier (unverified, several
files at once to a share); `F8` move across volumes is verified and deep. SMB signing, channels and
MTU are never changed by the app.

---

## Driftwire

Issues #410 and #413, [28](28-driftwire.md).

**Sharepoint is now Driftwire, everywhere (owner, 2026-10-19).** Microsoft already sells a product
called SharePoint, so the add-on was renamed in full: its name on screen, its id (`driftwire`), its
files and folders, the release packages (`mediaviewer-addon-driftwire-*`), the relay tool
(`mv-driftwire-relay`), the code and the docs. The owner chose to rename the key-derivation and
handshake labels too ("MediaViewer Driftwire …"), so a Driftwire computer cannot talk to one still
on Sharepoint, and remembered devices pair again; the wire magic `MVSP` stays (it spells nothing a
person reads). An installed Sharepoint does not update itself into Driftwire (a new id): the share
menu offers Driftwire's one-click install, and Sharepoint is removed in Settings → Add-ons. Its
settings carry over once (`adopt_old_prefs` reads `<root>/sharepoint/data/sharepoint.json`, the one
place the old name stays in the code).

**A first-party add-on may open a local link the person asked for (owner, 2026-10-14).** Driftwire
listens, beacons and uses Bluetooth only while its window is open, receiving is on or a transfer
runs; the idle viewer has no socket. Bytes go only to the peer the sender picked, after the person
there accepted, sealed end to end. Rule 6's line is unchanged: no path, file name, pixel, EXIF,
address or peer name reaches a log, telemetry or a crash report. Other makers' script add-ons
still have no network permission (2026-10-03).

**Discovery is one UDP beacon a second, not Bonjour / DNS-SD (2026-10-15).** One portable socket
file per OS (`net_posix.cpp`, `net_win.cpp`) instead of `dns_sd` on the Mac and `DnsServiceBrowse`
on Windows: the same code finds and is found on both, and it runs only while the window is open.
Sent to an organisation-local multicast group and the subnet broadcast with a TTL of 1.

**Bluetooth is a GATT pipe, and a set over 32 MiB is refused on it (2026-10-15; a budget).**
Windows gives apps no connection-oriented LE channel, so a Mac-to-Windows send has GATT only:
writes without response one way, notifications the other, tens of kilobytes a second. 32 MiB is a
few minutes at that rate. Past it the window says so before Send, and Send waits for the peer on
Wi-Fi and starts by itself when it is there (2026-10-15, owner), rather than failing after Send.

**Commit, then reveal, and the sender confirms the code (2026-10-15).** With both keys sent at
once, a machine in the middle could learn each side's key first and search a few thousand keys
for two six-digit codes that match. The client now commits to a hash of its key before it sees
the server's, and the server shows its key only after that. And the receiver's Accept alone did
not protect the sender: a machine claiming the receiver's id from another address could answer
Accept itself. So the sender's person confirms the code before the offer (even the names) leaves,
and a fresh peer keeps its address. Two clicks, one per side, as Bluetooth's numeric comparison.

**Keep receiving is kept across launches (owner, 2026-10-15, #442; reverses "the toggle is for this
run").** Turned on, it stays on after a relaunch until turned off: the add-on keeps it in its own
data folder (`driftwire.json`), and at load, only when it was on, makes the engine, whose control
thread starts presence. That is after first pixel and off the foreground queue (slot add-ons load
on the add-on queue / a worker), and in one window's process at a time (a lock file). Off at load
is one small read and nothing else. While it is on the command bar shows **Driftwire: receiving**,
so a person knows the machine is discoverable. Measured idle cost with it on, window closed (Mac,
`[.driftwire-idle]`, 15 s): 0.020 % CPU, 6.4 context switches a second, no present; off 0.0003 %.
The earlier call kept the idle viewer silent by default; it still is unless the person asked.

**The sender types the code; it is not compared (owner, 2026-10-15, #442; amends "the sender
confirms the code").** People clicked **Same code, send** without looking at the other screen. Now
the receiver's window shows the code and the sender's person types it; the sender's screen never
shows it. Commit, answer, reveal and the code's derivation are unchanged, so a machine in the
middle still cannot make the two links' codes match, and the offer still leaves only after a right
entry. The check is local to the sender (constant time, digits only, a partial entry is not a
try); three wrong entries send `cancel` and end the link. The wire is unchanged (protocol 1): the
receiver waits for the offer either way. Bluetooth's passkey entry is the pattern.

**Receive when MediaViewer is closed: the same process stays, opt-in, with a notification (owner,
2026-10-16, #452).** Off by default; on, it implies Keep receiving. The add-on asks the host to keep
its process (`keep_alive`, host table, ABI 0.33) rather than shipping a helper app or a login agent:
add-ons carry their own code and there is no second app. Closing the last window, ⌘Q and the
Dock's Quit then hide the viewer (it drops what it showed) and leave a menu-bar item (Mac) or a
notification-area icon (Windows) whose Quit really quits; logout, restart and an update's restart
quit as before. An offer is a system notification (`notice_post`: `UNUserNotificationCenter`,
`AppNotificationManager` under the process's AUMID) that names the sender, then the count and
size, never a file name or the code; a click opens the window with the code, its one button is
Decline, and Accept stays in the window. It shows whenever the window is not the front one, so
Keep receiving gets it too. Without permission (asked when a toggle turns on, never at launch)
the window opens as before. Off changes nothing: no keep-alive, no registration, no ask. Measured
windowless idle: see DEVELOPMENT.md (#452).

**…and it starts at login (owner, 2026-10-17, #452).** Without it a restart left the machine
unreachable until the app was opened by hand ("it does not work when the app is not open"). Still
the same app, not a helper: while the keep-alive list is non-empty the host registers the app
itself as a login item (`SMAppService.mainApp`; a per-user Run value naming the install's stub
with `--background` on Windows) and removes it when the list empties; that launch makes the window
but never shows it, so a person sees only the menu-bar item / tray icon. A login launch that no
add-on keeps within 60 s removes its own item and quits. Not a LaunchAgent with its own plist on
the Mac (the Final Cut Pro agent's way): a login item needs no second launchd job, no bundle
change and no separate approval path, and Login Items names it "MediaViewer".

**Protocol 2: Noise with ML-KEM-768 beside X25519, not a bespoke handshake (2026-10-18, #472 PR 1).**
`Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b`: a published framework whose core is checked against
the Cacophony vectors, and Noise's own hybrid forward secrecy modifier, so a link recorded today
needs both X25519 and ML-KEM-768 broken to read. ML-KEM-768 is libsodium 1.0.22's, already the
vcpkg baseline's: no new dependency. Not X-Wing in place of the DH tokens (PQNoise's shape): that
changes every token, where hfs adds two to a pattern that is already vetted. XX alone lets the
responder choose its key after seeing the initiator's, which would let a machine in the middle
search for matching six-digit codes; so the first message commits to a nonce that only the last
reveals, and the code hashes the handshake hash after the server's message with it (protocol 1's
commit-then-reveal, carried into Noise). Statics are a key made once a run until remembered devices
(#472 PR 4); the LAN and Bluetooth still trust the typed code, as before. A protocol 1 peer is
spoken to in protocol 1 and still works.

**A protocol 2 transfer is a session above its link (2026-10-18, #472 PR 1).** Sequence numbers,
acknowledgements, and a replay of what is unacknowledged, rather than the issue's sketch of a
`resume_at` that names a file and an offset: it resumes at the same chunk boundary (every message
is at most a chunk), but it also covers the control messages in flight (a lost `file_done` or
`done`), and `transfer.cpp` did not change. **The replay is at most 16 MiB a transfer (a budget):**
past it a sender waits; acknowledgements go at least every 1 MiB, and pings carry them. Suspect
after 3 s of silence, ended after 20 s without a link, three rejoins in 10 s end it: the issue's
numbers. After the handshake one bad byte ends the link and not the session (the issue's table:
"one bad byte ends the link"), since the rejoin sends that message again; a sequence gap still
ends the session. Measured in DEVELOPMENT.md (#472).

**The resume secret is fixed for the session; each rejoin's tag is a keyed hash (2026-10-18).** A
secret replaced at every rejoin would leave the two sides on different ones whenever a link dropped
between their two confirmations, and that session could never rejoin again. Fresh link keys come
from each rejoin's fresh X25519 and ML-KEM keys; the tag (8 random bytes, 8 of a keyed hash) is
never the same twice to an observer; and a recorded rejoin played again is stopped at the first
sealed message (`resume_at`), which it cannot make.

**The 32 MiB Bluetooth refusal is for protocol 1 peers only (2026-10-18, #472; amends 2026-10-15).**
To a protocol 2 peer a set of any size starts on Bluetooth when that is all there is and moves to
Wi-Fi when the peer is heard there, with no new code and no new Accept. Waiting for Wi-Fi first
stays for protocol 1 peers.

**The beacon and INFO keep `"v":1` and add `"p"` (2026-10-18, #472).** A protocol 1 client drops a
beacon whose `"v"` is not 1, so old and new would stop seeing each other; `"p"` names the protocol
the sender speaks, and its absence means 1.

**A sealed link to the peer the person picked, over the internet too; no server of ours (owner,
2026-10-17, #472; amends "A first-party add-on may open a local link the person asked for" and 28's
"no internet relay, by design").** Driftwire may reach a peer over the internet, client to client,
with no MediaViewer server of any kind: no rendezvous, no relay, no account. The public BitTorrent
DHT is acceptable, opt-in. A relay is allowed only through the person's own devices or a relay they
configure themselves. Each internet path is opt-in and off by default (find each other over the
internet; continue over the internet if Wi-Fi drops; use my other computers to relay; Tor if its
spike keeps it). Some pairs still cannot connect (two hard NATs and no IPv6, mapping or own relay; a
network that blocks UDP and all TCP but the web) and the window says so honestly. Recorded with PR
2 (2026-10-18), the first change that opens an internet path.

**Internet codes: one toggle, CPace, TCP first (2026-10-18, #472 PR 2).** One toggle, "Send and
receive over the internet", rather than a send toggle and a receive toggle: sending to a code opens
nothing, but with the toggle off the window must be as before, so both live behind it. CPace's
ristretto255 / SHA-512 suite because libsodium (already linked) has the group and the one-way map;
checked against draft-18's vectors. Its key is Noise's psk0, so a wrong code fails at the
receiver's first read, before the receiver's static key goes out. A code is a 48-bit secret
(single use, one live guess, nothing offline: more would only lengthen it), up to four places, and
a two-byte check so a typo dies on the sender, not the code; it is 26–90 characters, not the
issue's 16, which cannot hold an address. With a direct code only the receiver's addresses are
known, so PR 2 is TCP: the paths that need both sides sending, and the QUIC-or-own-UDP spike with
them, move to the DHT PR. A UPnP description is fetched only from a non-public address.

**Word codes on the public DHT: our own small client and our own UDP transport (2026-10-18, #472 PR
3).** The two spikes, decided on what this machine can show: a KRPC + BEP 44/42 client in the add-on
rather than libtorrent-rasterbar (Boost, a torrent engine, its own sockets, for two signed items and
an address vote), read-only (BEP 43) so it answers nothing and stays out of others' tables; and a
reliable stream of our own over the one UDP socket rather than QUIC, because the socket must be
shared with the DHT and the punching on both OSes (msquic owns its sockets and has no macOS; ngtcp2
needs a TLS stack beside Noise) and the session above already seals, orders and resumes. Not
measured on a real network (the live-DHT harness exists; it was not run without the owner, since
it shows this machine's address to the DHT) nor against `iperf3`. The words are BIP-39's English
list (MIT), so four words are exactly the issue's 44 bits and four letters of each are enough.
Argon2id at libsodium's "moderate" (256 MiB) is the issue's "moderate cost". The birthday punch
and TCP simultaneous open are not in this PR: the first needs a hub that polls 256 sockets.

**The receiver's stream takes its peer from the sender's first packet (2026-10-18).** Both sides
can find different working paths (one hears the other's LAN address, the other its public one);
the sender's choice wins. A stream id comes from the code, so only a holder of the words can be
that first packet, and what it carries is the channel's AEAD-sealed frames.

**The birthday punch and TCP simultaneous open; the sender's pick is said, not inferred (2026-10-18,
#472).** Amends the row above. With a birthday socket or a TCP connection in play, "the first
packet" no longer says which socket the receiver's stream runs on, nor which transport: the sender
now offers its pick (*choose*, tagged by the code) until the receiver answers (*chosen*), and the
receiver answers only the first it was offered, UDP or TCP; a sender that offered TCP never falls
back to UDP. Birthday punch only when exactly one side looks hard: per-node votes for ten minutes,
three or more ports and no port with half the voters (a single unverified "ip" field must not set
off 256 sockets and ~1000 probes); two hard NATs would need far more than 256 × 256 probes. 256
extra sockets (64 KiB buffers) on the hard side, birthday probes at most 100 a second and every
probe at most 200 a second each way, all extras but the stream's closed at the punch's end and
the stream's with the stream (an idle hub polls one socket again); on POSIX the soft descriptor
limit is raised to 1024 + 256 for the rest of the process (the Mac starts processes at 256;
lowering it again could fail another part's open). TCP runs beside UDP only in a word code's
meeting, never a remembered device's background one (it listens on every interface, a firewall
prompt on Windows), and the sender takes it only when no UDP path answered in 1.5 s, so a path
that works both ways stays on the stream the PR 3 numbers are for. Tested on loopback with stand-in NATs only; not measured on a real NAT, and the SYN-crossing
case itself does not occur on loopback (a reset answers a SYN there), so it is untested here.

**Wi-Fi → internet: the receiver's places, swapped over the sealed link (2026-10-18, #472 PR 5).**
"Continue over the internet if Wi-Fi drops", off by default, on both sides. The receiver opens a
way in (listener, router mapping, IPv6 pinhole) only for the transfer and sends its places as a
new message, `places` (14), only to a sender whose offer asked: the channel ends the link on a type
it does not know, so a sender before this PR must never see one. The sender races them with the
local bearers on a drop. Not built here: the DHT path for when both sides moved or the receiver's
own network goes, and the configured relay and Tor (their spikes were not run, so neither is
offered).

**Wi-Fi → internet when both sides move: meet on the DHT under the session's keys, pre-started
(2026-10-18, #472; completes PR 5).** When the receiver's network goes too, its swapped places go
with it, and nothing either side knows says where the other is now; a lookup is needed. Both sides
already hold a secret nobody else does (the session's resume secret), so they meet on the DHT the
way four words do, keyed by `handover_secret()` (its own BLAKE2b context, so it gives away neither
the pairing secret nor the rejoins' psk) and the count of rejoins both ends have adopted, through
`crypto_kdf`; no code, no new prompt. The count, not a new secret, because the resume secret stays
fixed for the session (above); without it every drop would reuse the targets (linkable) and the
stream id. Counts that differ after a one-sided rejoin find nobody, and the session ends as it
would with the toggle off. It
needs the DHT toggle on both sides (the offer and `places` say `"dht"`), because joining the
public DHT is what that toggle consents to. The hub and the DHT client are started while the
transfer runs (the receiver at Accept, the sender when the receiver says it meets too), not at the
drop: a cold join (DNS for the routers, a table round) would spend much of `kRejoinMs` before the
first publish. Cost while nothing drops: one join per transfer and a 200 ms look at the link, no
DHT traffic. Every meeting withdraws its items, met or not; the engine never waits for the
handover's thread on a transfer's way out. The handover's lookups retry every 500 ms instead of a
word code's 2 s (both sides start within moments; measured: the 2 s wait was 2.2 of 2.3 s on the
stand-in swarm). Roles stay sender / receiver so neither side meets its own item.

**Remembered devices: the OS's secret store, a pairing secret, hourly DHT keys per direction
(2026-10-18, #472 PR 4).** The identity is an X25519 key kept in the Keychain / Credential Manager
behind a port (`secrets.h`; a folder for the tests), never a plain file; without a store each run
makes its own and nothing is remembered. A remembered device is its name, its static key and a
pairing secret derived from that session's resume secret, so remembering is mutual by construction
(no "remember me" message: both must hold the secret to meet). Remembered devices meet on the DHT
under keys that change every hour and differ per direction (keyed by the waiting device's identity),
found so in testing: with one key set per pair, a device that both waits and sends met its own
item. The waiting side looks for a sender every 15 s rather than every 2 s, to keep the standing
load on the DHT low; the sender punches 15 s longer to cover that. A key that differs from the
remembered one ends the link loudly (TOFU); the own-device relay is not built in this PR.

**A remembered device publishes only when a sender knocks, and meets over TCP too (2026-10-20).**
Amends the two rows above. The owner's Mac and Windows PC met on four words, remembered each
other, and a send between them then never connected: the sender found the waiting side's standing
item, said "Connecting through both networks…", punched UDP alone, and went back to waiting. The
four-word transfer had arrived over TCP simultaneous open (its window said "Internet, straight to
each other", the caption of a link that was not a UDP punch), so UDP could not be punched between
those two networks, and the remembered meeting never tried TCP. TCP could not simply be switched
on there: a rendezvous port is bound per meeting, and the sender read the waiting side's standing
item, published before any meeting had a port. The waiting side now publishes nothing while it
waits; it looks for a sender's knock every 15 s as before, and only then publishes and meets, with
TCP beside UDP, so the first item the sender finds carries a live TCP port and both punch at once
(15 s each, `known::kPunchMs`, rather than the sender's 30 s against an item that may be 4 minutes
old). The meeting's item says "gone" when it ends, met or not, so a later send that hour does not
punch toward it. The firewall reason for keeping TCP out still holds for the wait (nothing listens
then); the port is open only for a meeting a holder of the pair's secret started, and the app
already listens for nearby sends. DHT nodes now see nothing of a waiting device's own until it is
sent to. Tested with two engines behind stand-in hard NATs ("driftwire known: two devices behind
hard NATs…", which failed before the change); no real NAT tried here.

**Relays: through the person's own computer, or a splice server of their own; TURN left out
(2026-10-18, #472).** Both relay paths carry the two ends' handshake and session end to end, so a
relay holds no key and sees ciphertext only. The own-device relay reuses the remembered-device
meeting and asks over that sealed channel (new messages 20–22, a range of their own so other work
on the stack can take 15–19); it serves only when both computers mark each other as the person's
own and the toggle is on, connects onward only to the public places the sender names (never into
the relay's own network) and is the relay's one transfer, so one at a time. It relays direct codes
only: word codes and remembered-device sends would need the relay to hold the DHT meeting's keys.
No rate cap by default (the person's own line), 256 GiB each way, 120 s idle. It runs in the
sender's race beside the direct places rather than after them (a meeting takes up to 45 s), and
never connects to R's own addresses or IPv6 /64s. The relay server is a rendezvous by a random token
carried in the code, not derived from the code's secret: a 48-bit secret behind a hash could be
searched by the server's owner within the code's life. Review (2026-10-18): a server any two
sockets could pair on is an open bandwidth relay, so it has a key of its own (typed with its
address, never in a code) that the waiting side proves; senders never wait; caps per source address
and on handshakes stop one host filling it; and a waiter re-registers every minute (replacing its
own wait) because NATs drop idle TCP after a few minutes. A relay
place joins the sender's race 1.5 s after the direct places, and the receiver hands on a paired
link only once the sender has spoken, so a lost race costs nothing. The relay field is resolved
because the person typed it (the DHT's bootstrap routers are the only other names, and only with
the DHT on); the code carries the address. TURN is left out:
it is a server protocol of its own (allocations, permissions, STUN, credentials) for what the
splice does in a few hundred lines.

**Tor: dropped for #472, on paper (2026-10-18).** C tor as a child process (BSD-3; OpenSSL,
libevent) is the feasible way on both OSes: mature v3 onion services, ephemeral `ADD_ONION` with no
key on disk, about 5–10 MB a platform, but an executable to sign in the add-on, tens of seconds to
a reachable onion and a few MB/s over six hops. Arti (MIT/Apache-2.0) would bring Rust into the
build, and its onion-service hosting was still marked incomplete when last checked. The relays
cover the hard-NAT pair without a third-party network. Nothing was measured: that needs the live
Tor network, so the harness (`tools/driftwire-relay/tor_spike.py`) waits for the owner
([plan](../plans/driftwire-tor-spike.md)). Reopen for address hiding, or if its numbers surprise.

**#472 merges with these asks deferred (owner, 2026-10-18).** The owner chose to land the stack
with the rest of #472 as follow-ups, not to build more before merge: re-gathering candidates and
republishing the DHT item when the OS reports a network change (needs a network-change port on
both hosts); remembered devices keeping last-known direct addresses (a remembered device is met on
the DHT only); bootstrapping the DHT from a LAN peer or a remembered device; a 15 s keepalive while
a wait (not a link) is up; S/Kademlia disjoint lookup paths; the 2x-faster switch between two fast
bearers (only Bluetooth to Wi-Fi switches); port mapping for the UDP socket; per-source rate limits
on the add-on's own listener and on rejoins (the relay server has them); the Wi-Fi Direct soft-AP
spike; AES-256-GCM, larger chunks and parallel streams for throughput; and a word code's failure
naming its reason (it says one generic thing today). The birthday punch also departs from the
issue: it starts with the meeting rather than after paths 1-4 have had 2 s, and punches for 10 s
(about 97 %) rather than up to ~2,048 probes. Everything off by default stays off; the external
review before anything is on by default still stands. [28 "Not built"](28-driftwire.md#not-built)
lists them.

**No "Forwarded port" field: the public address is found, never typed (owner, 2026-10-19).** The
window asked for `public address:port` under "Receive from anywhere", which read as a required step
to someone who had just turned the internet on. A code already finds its own way in (router
mapping by PCP, NAT-PMP or UPnP, public IPv6, the LAN address), and with no public place the DHT
toggle meets without one, so the field, its saved setting and its place in a code are gone. A
hand-forwarded port now helps only if the router maps it; a `forwarded` key in an old
`driftwire.json` is ignored. Reopen only for a network where none of the automatic ways work and
the DHT is blocked too.

**The receiver checks file N while file N+1 arrives (2026-10-15, #442).** Two verifier threads,
at most four whole files waiting, each still flushed, read back uncached, hashed and renamed with
no-replace before its `file_done`; the sender's `done` waits for every check. Measured on loopback
(Mac, alternated): 500 × 2 MiB 124–141 → 158–180 MB/s; one 1 GiB file unchanged (crypto-bound).
Reading ahead on the sender measured no faster and was not kept. `[.driftwire-bench]`.

**Each file's hash comes after its bytes, not in the offer (2026-10-15).** A hash list up front
would make the sender read every file twice before the first byte moved (a large video read
whole first). `file_end` carries the BLAKE3 of the bytes as read; the receiver checks it, then
reads the temporary back uncached and checks again before the rename, as `verified_copy` does.

**The receive is written in the add-on, with `verified_copy`'s rules (2026-10-15).** The bytes
come from a link, not a path, and copying a temporary again would write a large video twice. The
add-on compiles the file port in (exclusive create, durable flush, uncached read-back, no-replace
rename), as Places compiles `io/mapped_file`; it links nothing of the core.
`tools/check-module-graph.ps1` lets `addons/` include `io/` for it.

**A slot add-on's manifest commands are events to it (2026-10-15).** It has no chrome to run
them, so the host sends a `command` event with the payload's files (`run_command`, ABI 0.29).

**The gallery's Shift-click range is the selection while it stands (2026-10-15, #413).** It is
made only by Shift-click, so arrow browsing still never makes one; copy, `F7` / `F8`, `Delete`,
copy path, Share and drag-out act on it ahead of the marks, which stay what `Insert` /
`Shift+Space` edit. `Delete` reaches the gallery for it. On Windows native drops the range once
its anchor is no longer selected, so no selection path needs a hook.

**Gallery drag-out on the Mac starts below SwiftUI (2026-10-15, #413).** In the grid's vertical
`ScrollView` the cell's `DragGesture` never saw the mouse drag; a local event monitor turns a
press on a hovered cell that moves 4 pt into the file drag.

---

## Open add-ons

**Anyone can make an add-on; a contribution model (2026-09-29, owner, issue #79).** "Add-ons can
be made and installed by anyone, not just our repo… if people have a compatible file / URL they
can install their add-on… add-ons can do a wide range of things like add new screens." Designed
in [25](25-open-addons.md) as PRs 55–60; PR 55 is built on both hosts. Not a D-decision, and
none is reversed. [18 "Signed, verified, then loaded"](18-import.md#add-ons-how-import-is-installed)
stays true of MediaViewer's own add-ons (Import, Local search, Voice), whose channel, key,
folder and native code are untouched. Beside them there is a second kind, the **open add-on**:
manifest schema 2, signed by its **publisher's** Ed25519 key, which is a field of the manifest.
The signature proves the files and the continuity of updates, **not who the publisher is**, and
the install sheet says so. The two kinds cannot meet: open ids have a dot and ours do not,
`mediaviewer.` is reserved, the release key is refused as a publisher key, and each kind has its
own folder and store.

**The key is pinned per add-on at first install (2026-09-29).** `publisher.json`, written by the
app outside the version folders. Another key under an installed id is refused; so is an older
version; a folder put in place by hand has no record of consent and is not loaded. **Consent is
bound to bytes:** the sheet shows a package whose SHA-256 the core returned, and install refuses
a file that differs.

**The package is a ZIP with nothing compressed (2026-09-29).** Stored entries only, no ZIP64,
encryption, extra fields, comments or gaps; 64 MB, 2,048 entries. A stranger's file gets the
narrowest reader there is and no inflate code to attack; the cost is download size, which for
themes and scripts is kilobytes. No new dependency.

**Contribution API 1 is data only: themes (2026-09-29).** A manifest that names code (`native`,
`chrome`, `scripts`, `main`) is refused whatever API range it claims. Themes realise the
backlog's "theme" row for the chrome: seven colour tokens per palette and a font family, one
table for both hosts. The canvas's pixels are never themed (rule 2). The host refuses a palette
below a WCAG contrast floor (title 4.5 : 1, body and accent 3 : 1), since Settings is where a
theme is turned off. The canvas colour scheme, F3 overlay and user font file stay in the
backlog.

**Nothing automatic reaches a third party's server (2026-09-29).** Install from a link and Check
for update are clicks; there is no background check, so a publisher cannot learn when the app
runs. The GET is https at every hop, with no cookies and the fixed User-Agent. **Start-up reads
no add-on:** the chrome paints with the tokens it cached and verifies the add-on on a worker
afterwards (Mac: the defaults; Windows: `theme.json` beside `settings.ini`, read once when the
chrome starts, like the font beside the exe). **One wording for both hosts:** what the sheet says
an add-on adds, can and cannot do, and why one is refused, is written by the core
(`src/addon/open_json.cpp`) and shown as given.

**The SDK and the example are MIT; the app stays GPL-3.0-or-later (2026-09-29).** Making an
add-on with `tools/addon-sdk/mvaddon.py` puts no licence on it. The SDK signs with its own
Ed25519 (RFC 8032's reference arithmetic, tested against the RFC's vectors and against libsodium
through the C++ reader), so an author installs nothing but Python. ABI 0.17 (0.16 until main's pages took it, 2026-10-05) adds
`mv_open_addon_inspect`, `_install`, `_list_json`, `_remove`, `_theme_json`, additive
([14](14-abi.md#pr-55--open-add-ons-abi-017)). Measurements (both platforms, base in its own
worktree and build directory, runs alternated) and what is still owed are in
[25 "Implementation notes"](25-open-addons.md#implementation-notes-pr-55-2026-09-29).

**The owner's calls (2026-10-03, on pull request #98;
[25 §17](25-open-addons.md#17-decisions-owner-2026-10-03)).**

- **A stranger's code runs in a sandboxed script, with screens described as data** (as
  recommended; Lua 5.4 unless PR 57's spike shows its sandbox cannot hold the limits).
  Third-party native code in the app's process stays out: rules 1 and 6 could not be promised
  for it, and the Mac would need library validation off for everyone.
- **No `network` permission, ever.** Rule 6 then holds for other people's code by construction:
  the host API has no way to open a connection. The app contacts a third party's server only on
  a click (install from a link, check for update).
- **`.mvaddon` is registered with the OS**, in PR 55: Windows gets a `MediaViewer.Addon` ProgId
  with the extension pointing at it (our own type, so the association is set, not only offered;
  "never silently hijack" is about taking photo types), the Mac a document type with
  `LSHandlerRank` Owner over an exported UTI (`io.github.longtimeno-c.mediaviewer.addon`,
  conforming to `public.data`). The default-viewer prompt and the plist policy check leave that
  type alone. Not yet seen working: neither installer was rebuilt here.
- **Add-ons may carry any licence** (delegated to the writer): `LICENSE-ADDONS.md` is an
  additional permission under GPL-3.0 section 7 for works that reach MediaViewer only through the
  documented add-on interfaces, named from `LICENSE` and the author's guide. Whether a script
  that calls a GPL program's interpreter bindings is a derivative is unsettled, and a theme author
  should not need a lawyer to publish. Only the owner can withdraw the permission for later
  versions; nothing published loses it. MediaViewer's own add-ons, which link the core, stay GPL.
- **"From others"** stays the wording in Settings.

**Import described by its manifest (2026-10-04; PR 56's first half,
[25](25-open-addons.md#pr-56-first-half-import-described-by-its-manifest-2026-10-04)).** Owner:
"can I now convert all of the Import add-on to be an actual add-on? it's still included in app
code mostly". Import's engine and both windows already ship in the add-on package, not the app;
what the app held was the plumbing that named Import: two command rows, their dispatch, the
Settings text, the card hint, and selectors each chrome knew by name. This slice makes the
manifest say those things and the app read them.

- **Schema 1 gains `description`, `contributes.commands` and `contributes.hint`**, all optional
  and additive; an older manifest is the same add-on as before.
- **Eight reserved command ids** (`addon_cmd_0..7`) take the loaded add-ons' rows at load. Kept
  as named keyless placeholders so the wire ids stay dense; a filled slot answers with the
  add-on's own name. Appended after the built-in rows so built-in remaps keep their indices.
- **A manifest's rows supersede the built-in rows of the same add-on** (their key is cleared
  while the add-on's rows are live) rather than being removed: indices never move, and the
  router cannot answer the old row ahead of the new one on the same key.
- **Compatibility first:** an installed Import from before this (the owner's is 0.1.20) has no
  `contributes` and no generic entry; it keeps the built-in rows and the two frozen calls.
  The built-in rows and the fallback go once the published Import carries contributions.
- **The AI pack stays on its built-in rows** for now: Ctrl+F without the pack is the app's file
  search, which a contributed row cannot express. The list of first-party channels stays in the
  app: it is what the app can offer to download.
- **ABI 0.18** (0.17 before the same renumbering): `mv_addon_commands_json`, additive; `mv_addon_installed_json` and
  `mv_addon_check_manifest` carry `description`, `hint_on`, `hint_text`.

Verified on the Mac: the shell suite (contributed rows, the key-label round trip over every
built-in binding, superseding and restoring), the manifest cases, the packer's cross-check,
the full suite and the add-on rig. Not verified: a signed Import carrying the new manifest in
the app (a local build cannot load its own unsigned Import); Windows beyond compiling.

**Built-in themes are host palettes in the Appearance picker (2026-10-12, issue #359).**
Settings → Appearance → Theme lists Default, then Ember, Graphite, Harbour, Linen and Studio,
then every theme of every installed open add-on. The five are one palette each, compiled into
both chromes, with no `font`. A fresh profile shows them with nothing installed. They do not
go through the open-add-on store, a publisher key or the install sheet; `mediaviewer.` stays
reserved, so the selection key `mediaviewer.builtin/<id>` cannot be an add-on's. One palette
keeps that appearance, including system-drawn controls, and applies at once on both hosts
because there is no typeface to defer. Default still clears the override (`mv.theme.*`,
`theme.json`) and follows the system. High contrast still wins. Contribution API 1 remains
the path for a theme someone else ships. The photo, the video, the colour pipeline, the
histogram's channel colours and the F3 overlay are not tokens. The palettes add no work to
the present loop and no file read before first pixel beyond the token cache the chrome
already kept.

**The five built-ins are Midnight, Starfield, Terminal, Ume and Porcelain, each with one
ground effect (2026-10-12, same issue).** The first cut's Ember, Graphite, Harbour, Linen
and Studio were flat palettes. A built-in theme is one palette and one quiet ground, drawn
behind the chrome only: rain, a starfield, fireflies, petals, dots. The effect is not a
token, not in the theme JSON, and not in Contribution API 1, so an add-on still cannot set
one and the photo canvas still receives only the flat canvas colour it already did. The
effect ticks at 10 Hz on the UI thread while the app is in front and input is recent, and
it stops when the app is inactive, when the pointer and keyboard have been still for five
seconds, under Reduce Motion, or when Windows animations are off. High contrast still wins.
Dots do not tick. Mac panes that are a system material stay a system material. Nothing was
added to the present loop.

**Nineteen built-ins, and four more grounds (2026-10-12, same issue).** The picker is Dark,
Light, Midnight, Cyberpunk, Retrowave, Forest, Starfield, Terminal, Organs, Ume, GPT, Claude,
Cute, Eclipse, Porcelain, Arcade, Blueprint, Monolith and Yoyo, in that order. The new grounds
are a circuit (a faint grid and a few pulses along it), embers, sparkles and a slow drift.
Dark, GPT and Claude have no ground, so they do not tick. Organs is the same rain at a crawl:
speed 0.05, size 0.75, colour #451616. Speed and size are host fields on the row, not tokens;
every other theme leaves them at 1, so Midnight, Starfield, Terminal, Ume and Porcelain look
the same as they did. Retrowave's title is #ed5e76, the same hue as #e94560 lifted so it
clears 4.5 : 1 on both its canvas and its surface; the accent stays #e94560. A ground is
still not a token, still not in Contribution API 1, and still absent from the present loop.
The palettes follow the Odysseus set. The photo canvas still receives only the flat canvas
colour.

**Declarative contributions: contribution API 2 (2026-10-11; PR 56's second half,
[25](25-open-addons.md#pr-56-second-half-declarative-contributions-2026-10-11)).** Owner, the
same day: finish the open add-on spine (the rule change made with it, "an add-on carries its own
code and its own data", is its own row under Add-on mechanism, from #348's branch). Settings
pages, keymap packs and theme shape are
data an add-on's manifest carries; both hosts render them natively. None reverses a D-decision.

- **One table of contribution points** (`kContributionPoints`, `open_manifest.h`): every §7
  point with the API that introduces it (settings and keymaps 2, commands 3, screens 4, slots
  and providers 5), the same rows on every branch so PRs 56–59 merge without touching each
  other's lines. A strict manifest (no API past the host's) that names a point the host has not
  built is malformed; one reaching past the host's API keeps it for a later host. Manifest
  schema stays 2.
- **Settings values live in one JSON file per add-on id, written by the core**
  (`io::addon_settings_dir()`: `addon-settings` / `Add-on Settings`, beside the add-on folders),
  not in each host's own store (§7 had "the host's store"). Both hosts, a first-party add-on
  (host table `settings_get`) and a script (PR 57) must read the same values, the core is the
  only thing all four share, and it makes namespacing, persistence and keep-on-remove one
  Catch2-tested store. Values are checked against the page on the way in and again on the way
  out, so a page narrowed by an update hands the add-on its default, not an old value. Nothing
  is created until a value is written.
- **Remove keeps an add-on's settings unless asked** ("Keep its settings", on by default, in the
  Remove panel; Import's "Remove everything" forgets them). Installing again finds them.
- **A first-party (schema 1) manifest may carry a page too**, read by native code through the
  host table's new last row `settings_get` (appended under `struct_size`; `MV_ADDON_HOST_API`
  not bumped). The Places add-on (#344) is its first user.
- **A keymap pack is applied once, then it is the person's table.** Picking a layout resets to
  `default_bindings()` and writes the pack's keys in; from then it persists like any remap,
  survives the add-on's removal, and Reset restores the defaults. Nothing is resolved per key;
  the router stays an array index. Commands are named by their enumerator (`next`,
  `toggle_mark`; `shell/keymap.h`, stable), or `<add-on id>/<command id>` for a contributed row
  (agreed with PR 57). **A binding's keys go to the command's rows in table order**: a pack
  cannot add a row (persisted remaps are by row index), only re-key or clear one. A key the
  pack gives out is taken from any other row holding it in an overlapping mode (left unbound,
  shown so) rather than swapped, so the result does not depend on the order of the pack.
  Unknown commands and keys are skipped and said, not refused: a pack for a newer MediaViewer
  applies what this one knows.
- **A saved key table is put back exactly** (`set_live_key`, no swap), unbound rows included;
  it used to be replayed through `rebind_live`, which swaps and could not express "no key".
  **Reset keeps an add-on's superseded built-in rows without their key** (found here: they got
  it back).
- **Theme shape is three bounded scales on today's literals** (theme file schema 2:
  `radius` 0–2, `density` 0.75–1.25, `type_scale` 0.85–1.25), resolved once when the theme is
  painted. Geometry that native layout or measured gates depend on (gallery tiles, filmstrip,
  transport, the Video Editor's timeline) keeps its literals. On Windows, as for the typeface,
  a new shape shows at the next start.
- **MediaViewer's own add-on windows read the host's tokens** (issue #204). Mac: the app
  publishes them as the class method `+[MVChromeTheme tokens]` and posts
  `MVChromeThemeDidChange`; Import and Local search look the class up by name, so an older app
  leaves them on the system colours and neither bundle links the app. Windows: Import and
  Duplicates use `IAddonHost2` colours, face and `ThemeChanged` (as the AI chrome already did)
  and a new, separate `IAddonThemeShape` interface (no members added to the frozen ones).
- **ABI 0.24**, additive: `mv_addon_settings_json`, `mv_addon_setting_set`,
  `mv_addon_settings_forget`, `mv_open_addon_keymap_json`; the open list carries `settings` and
  `keymaps`, the installed list `settings`. The Windows chrome hands a pack to native through
  the parked-text channel, raised from 16 KB to 128 KB so a 256-binding pack fits.
**Other makers' code runs as Lua 5.4, vendored, in a sandbox per add-on (PR 57, 2026-10-11).**
§9 A of [25](25-open-addons.md) with Lua, as the owner decided; the first spike held every limit,
so WebAssembly was not taken up. What was decided while building it:

- **Lua kept over WASM.** The sandbox holds its limits in about 1,000 lines of runtime: the
  allocator caps memory, a count hook every 10,000 instructions enforces the instruction budget
  and the wall clock, and a stop raised at every instruction afterwards cannot be swallowed by
  `pcall` (each instruction after a caught error raises again). The escape attempts people know
  (`string.dump`, binary `load`, the shared string metatable, `collectgarbage("stop")`, deep and
  C recursion, `string.rep` bombs, coroutine loops) are tested; 563,234 fuzzed inputs found
  nothing. WASM would have meant a runtime many times larger and a toolchain an author must
  install for a first command.
- **Vendored, not vcpkg.** The pinned baseline ships Lua 5.5, and the Windows triplet links ports
  as DLLs; vendoring 5.4.8 (hash from vcpkg's own port) keeps it static on both platforms and
  lets the binary carry no `io`, `os`, `package` or `debug` code at all. Built as C++, so a Lua
  error is an exception and the runtime's RAII holds across it.
- **Local changes to Lua**, marked in `third_party/lua/VENDOR.txt`: pattern matching, a plain
  `string.find`, `table.move`, `table.insert`, `table.remove` and `table.sort` (every
  comparison) check the watchdog's flag (they can run for minutes without one VM instruction,
  so the count hook alone never fires), and `string.rep` of nothing returns at once instead of
  looping 10^15 times. **Added 2026-10-12 by the PR 57 security review:** no `__gc` finalizer
  runs (Lua calls one with hooks off, so a looping finalizer held the worker for good and hung
  the destructor's join at `lua_close`), and `__mode` is ignored, so every table is strong
  (ephemeron convergence is quadratic inside one atomic GC step, where nothing can stop it:
  a 200,000-link chain held the worker past 4 s). Both are in `lgc.c`; scripts lose finalizers
  and weak tables, which glue does not need. The plain find and the `insert` / `remove` /
  sort-comparison checks came from the same review (each held the worker for good).
- **The limits:** 64 MB of memory per add-on, 100 million VM instructions and 1 s of wall clock
  per task (each command call, and the scripts' first run), a 4 MB C stack per worker (macOS
  gives a secondary thread 512 KB, which Lua's 200 C levels can exceed), 256 tasks queued.
  Measured on the Mac (arm64, Release, a loaded machine): a spin stops by the instruction budget
  after about 0.75 s; an allocation bomb at the ceiling within the same bound; a C-side loop by
  the watchdog's 1 s. The numbers are generous for glue (a loop over 10,000 file names is
  thousandths of the budget) and small enough that a stuck add-on costs one core for a second.
- **Over a limit the add-on stops** until Start again or the next launch, rather than retrying:
  a script that ran away once will again, and a stop the person can see beats a core lost
  quietly. A Lua error is not a stop: the call ends, Settings shows the last error.
- **One thread per add-on, one watchdog thread for all,** asleep unless a task runs, so an idle
  add-on costs nothing (§11). Scripts run in every window's process (they never write the
  store), so their keys answer in every window.
- **Permissions are bindings that are not installed.** Without `folder.read`, `mv.folder` is
  `nil`, not a function that refuses. `thumbnails` is accepted and shown but adds nothing until
  PR 58's thumbnail nodes; `files.write` / `metadata.write` are "needs a newer MediaViewer"
  until PR 60; `network` is malformed, always.
- **A script's key gives way.** A clash with the base app's row, or an earlier add-on's, in a
  shared mode drops the add-on's key and keeps the row (Settings says why). First-party rows are
  not held to it: Import supersedes its own built-in rows.
- **Notices name their add-on.** `mv.notify` shows "Folder Count: 4 files in this folder" in the
  bar's notice (four a second at most, 200 bytes), so a person never mistakes a stranger's line
  for MediaViewer's.
- **Windows: the shell's listener, not the completion queue.** The chrome drains the session's
  completions; script notices and start / stop are the shell's (title notice, command table), so
  the core calls a documented any-thread listener and the shell posts to its window (ABI 0.26).
- **Not yet:** slots and screens for scripts (PRs 58–59 built them for native add-ons; script
  bindings register through `add_script_binding`). `mv.settings.get` landed when PR 56 and PR 57
  were integrated (2026-10-12): a binding over the same `settings_values::value_json` the host
  table's `settings_get` reads.

**No canvas background: every screen is the theme's (2026-10-15, issue #408).** Owner call:
remove the canvas background rather than give it a colour scheme of its own. Settings' "Canvas
background" row (System, Grey, White, Checkerboard, Dark), `B` and `cycle_background` are gone
on both hosts. The clear behind the welcome card, behind a still and behind a video, and what
shows through a transparent pixel, is the theme's `canvas` token, the colour the chrome
already paints; only the slideshow blackout differs. The welcome card draws with the theme's
surface, title, body, hairline and accent instead of a grey derived from one RGB, and a
built-in's ground effect moves behind it on the empty window only (static presents by the
panes' rest rule, never `live`, so a still stays at 0 presents). Kept, so nothing else moves:
the command id (`retired_background`, keyless, like the palette's), a key-table row with no key
and no mode (saved rebinds name rows by index), the constant buffer's float (`unused0`, so the
HLSL and MSL layouts do not shift), and the thumbnail matte (changing it would re-encode every
cached thumbnail). View-settings bits 4-6 are read and ignored. The checkerboard is gone with
it: a transparent image now shows the theme's ground, not a pattern. Two rendering details:
ImGui draws into the labs' _SRGB targets, which read a vertex colour as linear, so a token is
decoded first (`home_ink.h`; drawn as is, Midnight's `#161b22` card came out `#555c65`), and a
translucent token or ground mark is mixed over its known solid ground in sRGB, as the panes
blend, because linear blending made the rain several times brighter than in the bar
(`home_over`). On the Mac the chrome paints its theme before the app object exists, so the
bridge keeps the latest theme and applies it when the app registers.

**The ground effect moves faster, and behind other apps (2026-10-15, owner).** Was: 10 frames a
second, only while the app was in front and the pointer or a key had moved in the last five
seconds. The owner found it choppy, and frozen whenever another app was in front, and then
asked for 60 at least. Now: every display frame in front and in use (60 a second, 120 on
ProMotion: the home ticks on each pass of a loop the display paces, the panes on the display link
on the Mac and `CompositionTarget.Rendering` on Windows, since a fixed 60 Hz timer drifts against
the display. On the Mac the panes' marks are Core Animation layers moved on a thread of their
own (`ThemeTicker`): publishing the clock to SwiftUI re-laid out and re-rendered each pane on
every tick, and a main-thread display link woke the main run loop 60 times a second, where every
pass polls all the chrome stores (`ChromePulse`). Profiled 2026-10-15: the native home's own
work per frame is a few samples, and it was SwiftUI that cost); 10 a second while the window is visible (behind another app, or in
front but quiet), for up to a minute after the last input; then rest. Never while hidden,
minimised or covered, never under Reduce Motion or with Windows animations off. One rule in
three places (`home_ground_clock`, both chromes' `ThemeMotion`), and the bar's and panes' input
now reaches the empty window's clock, which only heard the canvas. The app sees no input in
other apps, so a window left behind another app rests a minute after you leave it; moving for
ever while visible was the alternative, at a steady cost with nobody using the app. A tick
behind another app may present (`present_request::ground_tick`); nothing else does. Known gap: the home's
marks do not line up with the panes' (own seed, own rect, own clock).

---

## Video at 4K on Windows

**One decode thread on D3D11VA, and a 1 ms timer while a clip plays (2026-10-06).** A 4K 30 fps
HEVC clip showed ~24 fps on Windows (screen comparison, worst gap 367 ms) while the same clip
plays at 29.7 decoded frames/s on the Mac. `thread_count = 0` had been set on the claim that
it "is ignored on the hardware path"; FFmpeg runs frame threads around a D3D11VA hwaccel too, so
a 16-thread CPU got ~15 frames of start-up latency and contended for the device lock. The
pipeline's short waits (`reserve_slot`, the demuxer's full queue, FFmpeg's `Sleep(2)` on
`E_PENDING`) each lasted a 15.6 ms Windows tick. Now: one thread on D3D11VA (VideoToolbox keeps
auto), and `timeBeginPeriod(1)` held only while a clip is playing, so a still or a paused clip
keeps the default tick. `--av-soak` reports `decoded`, `stale` and `decoder` so decode rate is
told apart from selector drops. Measured 2026-10-15 on the RTX 4070 / 5700X3D desktop (18 s
`--av-soak`, base/new alternated ×3): base was already at rate here (539 presented, 2 dropped),
new presents 540 with 1 dropped and 545 decoded on `d3d11va`, and the worst poll gap falls from
~32.7 ms to ~18.6 ms (A/V error p99 ~16–26 ms → ~2 ms on two of three runs). The ~24 fps of the
screen comparison did not reproduce on this machine; the PR 1 gate and idle are unchanged.

## Launch

**The chrome's .NET runtime loads on its own thread, below normal priority (issue #486,
2026-10-18).** The boot timeline (`core/boot_timeline.h`, `--boot-report`) showed a warm Windows
launch spending ~205 ms of its ~665 ms to interactive with the UI thread parked on the render
thread's device, and the chrome's hostfxr, runtime and ~50 entry points (~100 ms warm, ~260 ms on a
cold first launch) loading only after that, on the same thread. `chrome_host::preload()` now starts
that load right after the crash reporter, and `attach_chrome` joins it. At normal priority the
loader thread delayed the device by a steady 3–5 ms (render start / first frame in every set);
below normal it does not measurably (first frame 252 vs 252 ms, first pixel of a CR3 295 vs 292 ms,
12 alternated runs). Interactive: 663 → 569 ms with no file, 735 → 622 ms with a 42 MP ARW,
763 → 663 ms with a CR3 (RTX 4070 / 5700X3D, warm, `docs/perf/boot/`). Not done, and why: the
command bar's XAML island (~255 ms warm, the largest stage) still attaches after the device,
because overlapping it would put XAML start-up beside device creation and in front of the
`open_paths_early` decode on the one UI thread.

**Mac chrome panes made hidden at launch build their SwiftUI content on first show (issue #486,
2026-10-19).** The Mac boot timeline showed the window's first order-front taking ~184 ms of a
~365 ms launch to interactive (no file, M5 MacBook, warm): AppKit lays the window out before it
shows, and SwiftUI builds and lays out the graph of every `NSHostingView` in it, hidden or not.
Twelve of the thirteen launch-time views start hidden. `LazyHostingView` (`ChromeHost.swift`)
hosts an empty root until it is first shown (or laid out while not hidden), then builds its
content once. Interactive: 365 → 185 ms with no file, 377 → 169 ms with a 42 MP ARW, 372 → 170 ms
with a CR3; first frame and full resolution unchanged; first pixel's slow mode (waiting for the
show) moves from ~400 to ~195 ms (`docs/perf/boot/mac-*.json`). The cost moves to the first show
of each pane, once per launch. The Windows fix does not carry over (no .NET on the Mac), and this
one does not carry back: the Windows islands are attached, not merely hidden, so not attaching the
four launch-time islands there is a separate change.

## Not built

- Voice query add-on (PRs 27–28): no voice add-on or `search_query` host entry exists.
- Editor add-on (PRs 32–47: GPU port, OpenColorIO, out-of-process render): not in the tree.
- Multi-window viewers grouped as OS tabs with `Ctrl+Tab`: Mac windows disallow tabbing; one window.
- Explorer property handler: per-user installs cannot register one.
- Staged rollout (5 % → 25 % → 100 %): one channel per release goes to everyone.
- Keymap import/export by the person (named layouts ship as keymap packs, PR 56); a user font file.
- Open add-ons PR 60 (files), and screens and slots for other makers' scripts (the Lua sandbox, PR 57, and native screens and slots, PRs 58–59, are built).
- Live Photo ContentIdentifier check: pairing is by name only.
- `F2` rename and an `X` reject mark.
- Reverse playback in the Video Editor.
