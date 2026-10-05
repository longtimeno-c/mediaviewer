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

**The Video Editor moves the one canvas (2026-09-26, 2026-09-27).** While the editor is open the
viewer's canvas moves into its window and back, so there is no second renderer. Mac: the one
`CAMetalLayer` view moves and refuses first responder. Windows: `gfx::swapchain::retarget` moves
the same DComp visual between frames, requested through the opaque
`input_snapshot::canvas_window`; the editor window is destroyed only after the swapchain has left.

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

**Thumbnails are JPEG files on disk (2026-09-07).** Spec `jpg512.3` (was `.1`; bumped
2026-09-24 when JPEG orientation was applied, and 2026-10-03 when clip posters began following
the display matrix), keyed in SQLite by `(path, mtime, size, spec)`. The
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

**File search and the path-bar icon (2026-09-28).** The gallery search bar is gone. A magnifier at
the end of the folder path runs `Ctrl+F` / `⌘F`: with Local search loaded (or starting) it opens the
search panel; without it, **file search**, a name filter over the listed folder with no index (the
Windows chrome entry is still named `GallerySearch`). `/` in the gallery is the folder row's find.

---

## Windows chrome (XAML islands)

**Island layout (2026-09-07).** Top island: command bar (48 DIP). Bottom island: filmstrip with
a virtualising `ItemsRepeater`. `input_snapshot` carries `chrome_bottom_px` / `chrome_left_px` /
`chrome_right_px`, and `usable_canvas` subtracts them. Flyouts use
`ShouldConstrainToRootBounds = false` so they are siblings of the swapchain. Decoded pixels never
go to a XAML `Image`.

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

**Developer key (2026-09-26).** `MV_ADDON_DEV_PUBLIC_KEY` lets a developer build trust a local key
so a pack can be sideloaded; with it, `MV_DEV_ADDONS_DIR` / `MV_DEV_THUMBS_DIR` keep the run out of
the user's install. CMake warns and refuses it under GitHub Actions.

**Files are hashed once per process (2026-09-27).** Re-hashing on list, load and each piece lookup
cost ~6 GB of reads per launch with the whole pack. The first check hashes; later checks stat every
file (size and `io::file_stat::mtime_ns`) and re-walk for extra files.

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
outside it. CI rejects an oversized combination and the installer refuses it.

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

**People follow the open folder (2026-10-03).** While a folder is open the People grid offers
"This folder | + Subfolders" only; everyone shows only with no folder open. A folder open is the
user saying what they are looking at, and everyone-from-everywhere inside one read as a leak. A
search is a question rather than a place, so the search panel keeps "Everywhere".

**The Photos library as a folder, its backup, upright clips (2026-10-03, owner).** Once the
library is added in Settings it opens as a listing of *virtual items* (`photos:<id>` keys, tiled
from PhotoKit's cache, resolved to a file only as shown), and search results take the same path.
An original is downloaded when viewed (after a 400 ms stay), copied out, or backed up — each is the
user asking for the file; the index still never downloads. The backup copies originals into
`YYYY/YYYY-MM-DD` through `io::verified_copy` with a manifest, so a re-run writes nothing for what is
there; not a mirror, no schedule. Both platforms: the player honours a clip's display matrix
(`video_stream_info::rotation`), because portrait phone clips played sideways. See
[26](26-photos-library.md).

**The Mac Photos library as a source (2026-09-28).** Read-only: no PhotoKit write API is called,
and `shell/write_guard.h` refuses every write, move, bin, rotate, export and trim of a Photos file
or listed preview. Indexing never downloads; an iCloud original is fetched only when its preview is
viewed (400 ms) and deleted when the next list opens and at quit. `photos:<localIdentifier>` is
treated like a path (never logged). The base app carries `NSPhotoLibraryUsageDescription` and the
photos-library entitlement, because TCC attributes the request to the app; access is asked only on
a click. A full re-enumeration of 23,089 assets takes 0.43 s, so no change tokens.

---

## Final Cut Pro search (Mac)

**Mac-only, over a shared reader (2026-09-28).** FCP is Mac-only, so the extension and agent have
no Windows twin. Shared on both platforms: the pack's read-only reader (`mv_ai_reader_get`, the same
engine in read-only mode, so results match the app by construction), the wire format, the
thumbnail lookup and the FCPXML writer (`src/nle`, `mv-nle-export`).

**Shipped inside MediaViewer.app, off until turned on (2026-09-28).** arm64 only: the `.appex`
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

---

## Not built

- Voice query add-on (PRs 27–28): no voice add-on or `search_query` host entry exists.
- Editor add-on (PRs 32–47: GPU port, OpenColorIO, out-of-process render): not in the tree.
- Multi-window viewers grouped as OS tabs with `Ctrl+Tab`: Mac windows disallow tabbing; one window.
- Explorer property handler: per-user installs cannot register one.
- Staged rollout (5 % → 25 % → 100 %): one channel per release goes to everyone.
- Keymap import/export and named layouts (planned as keymap packs, PR 56); a user font file.
- Open add-ons PRs 56–60: settings pages, keymap packs, the Lua sandbox, screens, slots, files.
- Live Photo ContentIdentifier check: pairing is by name only.
- `F2` rename and an `X` reject mark.
- Reverse playback in the Video Editor.
