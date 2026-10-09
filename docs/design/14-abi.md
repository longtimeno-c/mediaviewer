# 14 — The C ABI

The flat C ABI between the hosts' chrome and the C++ core, as declared in
[`src/abi/include/mediaviewer/`](../../src/abi/include/mediaviewer/) (current version **0.18**).

The Windows host is C# WinUI over `mediaviewer_core.dll` and calls this ABI through P/Invoke
([`src.managed/MediaViewer.Interop`](../../src.managed/MediaViewer.Interop/)). The Mac host links
the same core statically and calls it directly; it does not create an ABI session, but it uses
the same portable pieces (`abi/clip_session`, the add-on host table, `player/playback_hold.h`).

| Header | Contents |
|---|---|
| `mediaviewer.h` | Version, status, session, generations, completions, image open, folder model, result listings, video transport |
| `mediaviewer_clip.h` | Clip jobs and the keyframe index (trim, remux, extract) |
| `mediaviewer_crash.h` | Crash-report context slots |
| `mediaviewer_addon.h` | Add-on entry point, host function table, host-side add-on management |
| `mediaviewer_import.h` | The Import add-on's interface (`mv.import.1`) |
| `mediaviewer_ai.h` | The AI pack's interface (`mv.ai.1`) and the read-only reader entry |

## Shape

A **flat C header**. No C++ types cross the line — no `std::`, no exceptions, no RTTI, no COM
interfaces, no inheritance. Only:

- opaque handles (`typedef struct mv_session* mv_session_t;`)
- POD structs with fixed-width fields and explicit padding (`reserved` fields)
- `const char*` UTF-8 strings with documented ownership
- function pointers, always paired with a `void* user_data` / `ctx`

Everything is `extern "C"`. `MV_API` is `__declspec(dllexport/dllimport)` on Windows (export
when `MV_BUILDING_CORE`) and `MV_CALL` is `__cdecl`; both are empty elsewhere.

**Versioning:** `MV_ABI_VERSION_MAJOR 0`, `MV_ABI_VERSION_MINOR 15`. `mv_abi_version()` returns
`(major << 16) | minor`. `MediaViewerSession.Create` throws unless the loaded DLL's major equals
`ExpectedAbiMajor`. Minor bumps are additive; existing struct layouts do not change (new meaning
goes into a former `reserved` field or a new flag bit).

## Error model

```c
typedef enum mv_status {
  MV_OK = 0,
  MV_ERR_INVALID_ARG = 1, MV_ERR_OUT_OF_MEMORY = 2, MV_ERR_IO = 3,
  MV_ERR_UNSUPPORTED_FORMAT = 4, MV_ERR_CORRUPT = 5, MV_ERR_CANCELLED = 6,
  MV_ERR_DEVICE_LOST = 7, MV_ERR_INTERNAL = 8,
  /* 0.12 (issue #42, eject categories) */
  MV_ERR_BUSY = 9, MV_ERR_NOT_REMOVABLE = 10, MV_ERR_PERMISSION_DENIED = 11,
  MV_ERR_NOT_FOUND = 12, MV_ERR_TIMEOUT = 13
} mv_status;

const char* mv_status_name(mv_status);          /* string literal; never freed */
const char* mv_last_error_message(void);        /* calling thread's last failure; "" never NULL */
uint64_t    mv_last_error_correlation_id(void);
```

Every fallible call returns `mv_status` (`core/status.h` mirrors the values). The last-error
message is owned by the core, valid until the next call on that thread; C# copies it at once.

**No exception crosses the boundary.** Every export is wrapped by `mv::abi::guard`
([`src/abi/guard.h`](../../src/abi/guard.h)):

```cpp
extern "C" mv_status MV_CALL mv_thing_do(...) noexcept {
  return mv::abi::guard("mv_thing_do", [&] { ...; return status::ok; });
}
```

The guard takes a fresh process-wide **correlation id**, stamps the thread-local last-error
slot, records the id as the last in-flight call for crash reports, traces the call, and maps
`std::bad_alloc` → `MV_ERR_OUT_OF_MEMORY` and anything else → `MV_ERR_INTERNAL` with the
message kept. The correlation id ties a managed `MediaViewerException` to the native minidump
([13-updates-and-telemetry.md](13-updates-and-telemetry.md)); every completion carries one too.

## Ownership — the side that allocates, frees

| Direction | Rule |
|---|---|
| Core → managed, strings | Either a core-owned pointer valid until the next call on that thread (last error) or a static literal (`mv_status_name`), or — the common case — **copied into a caller buffer** (below). |
| Core → managed, data | POD structs filled by value; scalars in completion payloads. Never a pointer the caller frees. |
| Managed → core, strings | Caller owns; the core copies before returning and never retains the pointer. |
| Managed → core, buffers | Read during the call only (e.g. `mv_clip_request.ranges_ns`, `mv_folder_open_list` paths). |

**Caller-buffer strings** (`mv_folder_item_name` and every `…_path` / `…_name` call):
UTF-8, `out_bytes` (optional) is the required size including the NUL; `cap == 0` returns the size
and `MV_ERR_INVALID_ARG`; a short buffer is truncated with a NUL and still reports the full size.
JSON-returning add-on calls use the same idea with `cap` / `needed`: short → `MV_ERR_INVALID_ARG`
with `needed` set, and the caller retries.

## Handles and `SafeHandle`

The one opaque handle is the **session**. It is reference-counted:
`mv_session_create` returns a count of one; `mv_session_retain` / `mv_session_release` adjust
it. On the C# side it is `MvSessionHandle : SafeHandleZeroOrMinusOneIsInvalid`, whose
`ReleaseHandle` calls `mv_session_release`; the wrapper never calls `retain` — it owns what the
factory returned and releases exactly once.

The completion wait handle is session-owned; C# wraps it in a `SafeWaitHandle` with
`ownsHandle: false`. Images, frames and textures have no ABI handle at all (see
[What crosses](#what-crosses-and-what-does-not)).

## Thread contract

Each function is annotated in the header:

| Annotation | Meaning |
|---|---|
| `[any-thread]` | Safe from any thread, concurrently |
| `[no-block]` | Returns without I/O, decode, or lock contention — safe from the UI thread |
| `[ui-thread]` | Call on the chrome's UI thread (add-on `shutdown`, `mv_addon_unload`, `mv_addon_quit`) |
| `[worker-thread]` | May read files, hash or run a model; call off the UI thread |

Almost every session call is `[any-thread][no-block]`: requests return a job id immediately and
the answer arrives as a completion. The exceptions that touch the disk directly
(`mv_list_subdirectories`, the add-on management calls) are documented as worker-only.

## Generations

```c
mv_status mv_session_bump_generation(mv_session_t, uint32_t* out_generation);
mv_status mv_session_current_generation(mv_session_t, uint32_t* out_generation);
```

Every job carries the generation current at submission; a bump abandons everything queued at
the old one at its next check ([02-architecture.md](02-architecture.md)). `mv_image_open` submits
at the **current** generation (the caller bumps first when it replaces the view; without a bump
it replaces rather than cancels). `mv_folder_select` and `mv_video_open` bump the view
generation themselves. Thumbnail jobs ride a separate **folder** generation so arrow-key bumps
do not cancel the filmstrip.

## Completions — the core never touches the dispatcher

The core never calls a host dispatcher or managed callback from a worker. It owns a completion
queue and the host drains it:

```c
typedef struct mv_completion {     /* 40 bytes */
  uint32_t kind;                   /* mv_completion_kind */
  uint32_t status;                 /* mv_status */
  uint64_t job_id;
  uint64_t correlation_id;
  uint32_t generation;
  uint32_t reserved;
  int64_t  payload;                /* kind-specific scalar, never a pointer */
} mv_completion;

void*    mv_completion_wait_handle(mv_session_t);   /* manual-reset event; do not CloseHandle */
uint32_t mv_completion_drain(mv_session_t, mv_completion* out, uint32_t capacity);
```

| Kind | Value | Payload |
|---|---|---|
| `ECHO` | 1 | byte length of the echoed text |
| `IMAGE_OPENED` | 2 | `(width << 32) \| height` |
| `FOLDER_READY` | 3 | stop count |
| `FOLDER_CHANGED` | 4 | stop count (watcher relist, re-sort) |
| `THUMB_READY` | 5 | item index |
| `FOLDER_SELECTED` | 6 | selected index |
| `VIDEO_OPENED` | 7 | duration (ns) |
| `VIDEO_ENDED` | 8 | 0 |
| `VIDEO_STATE` | 9 | `mv_play_state`, pushed on every transition including host-requested ones |
| `FOLDER_SUMMARY` | 10 | subfolder index |
| `CLIP_INDEX` | 11 | keyframe count (`mediaviewer_clip.h`) |
| `CLIP_JOB` | 12 | `mv_clip_job_state` (`mediaviewer_clip.h`) |
| `ADDON` | 100 | add-on event payload; `job_id` = event id, `generation` = `mv_addon_event_kind` |

Draining is non-blocking and batched (a folder's 400 thumbnails are one drain). `IMAGE_OPENED`
means pixels may be ready on the native canvas; draining does not wake an idle render thread —
the shell wakes the presenter ([03-rendering.md](03-rendering.md)). The queue is capped at
65,536 entries: a host that stops draining loses the oldest quarter at the cap, and every drop
is counted and logged, never a `std::terminate` (issue #236). Both drain calls go through
`mv_guard` like every other export.

`mv_session_echo` is the minimal round trip (validate, copy, submit, return a job id, answer as
`ECHO`), and `mv_session_job_stats` reports submitted / completed / cancelled / queue depth /
workers / generation for the F3 overlay and tests.

`mv_session_config`: `worker_count` 0 → `max(1, cores − 2)`; `enable_etw` registers the ETW
provider.

## What crosses, and what does not

**Crosses:** open/close, navigation intent, folder item records, sort order, transport commands,
clip-job requests and progress, UTF-8 paths (including paths to on-disk JPEG thumbnails),
add-on JSON and POD status.

**Does not cross:** pixels, textures, decoded frames, `ID3D11*` anything. The swapchain lives
in C++; managed code tells the canvas its size and sends input. Edits do not cross either: the
edit stack, crop mode and lossless/export jobs live in `shell/edit_session` (shared with the Mac
host), and geometry reaches the render thread through the input snapshot. Key bindings and
`command_id` stay in the shell ([16-commands.md](16-commands.md)).

The add-on host table is the one place 8-bit RGB pixels are handed over — to an add-on, inside
the process, for indexing (below).

**Native-only side door** ([`src/abi/native.h`](../../src/abi/native.h)): exported from the
DLL for the C++ present lab, never P/Invoked — `attach_device` / `detach_device`,
`take_ready_image` / `release_gpu_image` (wait-free handoff of the latest uploaded image),
`image_ready_wait_handle`, `tiles_frame` / `tiles_stats`, the animation frame ring
(`take_animation_frame`, `animation_*`), `poll_video`, `take_ready_video_frame` /
`release_video_frame`, `video_open`, and `open_failed`. The navigation GPU LRU is native-only.

## Image open

```c
typedef struct mv_image_info {   /* 24 bytes */
  uint32_t width, height;
  uint32_t format;               /* codec::format_family */
  uint32_t icc_tagged;           /* an ICC profile (or sRGB chunk) was used */
  uint32_t transfer_intent;      /* 0 = display-referred */
  uint32_t page_count;           /* 0.16: pages in the file; 1 for a single-page still */
} mv_image_info;

mv_status mv_image_open(mv_session_t, const char* utf8_path, uint64_t* out_job_id);
/* 0.16: page `page` of the selected stop; IMAGE_OPENED follows. Page 0 = mv_folder_select. */
mv_status mv_folder_select_page(mv_session_t, uint32_t page, uint64_t* out_job_id);
mv_status mv_session_image_info(mv_session_t, mv_image_info* out);   /* last opened */
```

## PR 4 — folder, thumbs, prefetch

```c
typedef struct mv_folder_item {  /* 32 bytes */
  uint32_t index;
  uint32_t flags;        /* bit 0 selected; bit 1 primary is a RAW (0.5);
                            bit 2 primary is a video, by extension (0.15) */
  uint64_t size_bytes;   /* of the primary */
  int64_t  mtime_unix;   /* of the primary */
  uint32_t pair_kind;    /* mv_pair_kind (0.5) */
  uint32_t reserved1;
} mv_folder_item;

mv_status mv_folder_open(mv_session_t, const char* utf8_dir, const char* utf8_select_path,
                         uint64_t* out_job_id);          /* FOLDER_READY */
mv_status mv_folder_count(mv_session_t, uint32_t* out_count);
mv_status mv_folder_item_at(mv_session_t, uint32_t index, mv_folder_item* out);
mv_status mv_folder_item_name / _path / _thumb_path / _pair_path(mv_session_t, uint32_t index,
                         char* utf8, uint32_t cap, uint32_t* out_bytes);
mv_status mv_folder_select(mv_session_t, uint32_t index, uint64_t* out_job_id);
mv_status mv_folder_selected(mv_session_t, uint32_t* out_index);
mv_status mv_folder_thumbs_visible(mv_session_t, uint32_t first, uint32_t count);
mv_status mv_folder_close(mv_session_t);
```

- `utf8_select_path` may be NULL (index 0). Either half of a pair selects the pair's stop.
- The thumb path is empty until `THUMB_READY` for that index.
- `mv_folder_select` bumps the view generation, publishes an LRU hit or opens, prefetches ±2,
  and pushes `FOLDER_SELECTED` ([04-image-pipeline.md](04-image-pipeline.md)).
- `mv_folder_thumbs_visible` makes on-screen thumbs first; skipping it still generates all.
- The folder is watched; a change relists and pushes `FOLDER_CHANGED`.

## PR 7 — pairs are one stop (ABI 0.5)

A folder item is a **navigation stop**, not a file. RAW+JPEG (or RAW+HEIC) and Live Photos
(HEIC+MOV, JPG+MOV) are paired at scan time; ambiguous groups stay separate.

```c
typedef enum mv_pair_kind { MV_PAIR_NONE = 0, MV_PAIR_RAW_JPEG = 1, MV_PAIR_LIVE_PHOTO = 2 } mv_pair_kind;
```

- Name, path, size, mtime, thumbs, decode, prefetch and video detection use the **primary**
  (the JPEG/HEIC still). `mv_folder_item_pair_path` returns the secondary (the RAW, or the MOV);
  empty (`out_bytes` 1) when unpaired.
- A Live Photo stop is a still; the host opens its motion with `mv_video_open` on the pair path.
- `FOLDER_READY` / `FOLDER_CHANGED` payloads count stops. Copy, move, drag-out and Recycle Bin
  expand to both halves on the host side.

## PR 9 — sort and the folder tree (ABI 0.6)

```c
/* key in bits 0-2 (0 name, 1 modified, 2 size, 3 type, 4 date taken), descending in bit 3 */
mv_status mv_folder_set_sort(mv_session_t, int32_t packed);
mv_status mv_folder_get_sort(mv_session_t, int32_t* out_packed);
/* 0.19: MV_FOLDER_HIDE_AUDIO | MV_FOLDER_HIDE_DOCUMENTS; the opened file and current stop stay */
mv_status mv_folder_set_hidden_kinds(mv_session_t, uint32_t mask);
mv_status mv_list_subdirectories(const char* utf8_dir, char* utf8, uint32_t cap, uint32_t* out_bytes);
```

- The session owns the sort order, so filmstrip, gallery and arrow keys read one list. `set`
  re-sorts on a worker, keeps the current stop, pushes `FOLDER_CHANGED`, and applies to later
  opens. Unknown key bits mean name. Date-taken behaviour is in
  [06-metadata.md](06-metadata.md).
- `mv_list_subdirectories` takes no session: one directory read for the folder tree, worker
  threads only. Writes `"name\tpath\n"` per subfolder (hidden, system and dot directories
  skipped, case-insensitive order; tabs/newlines in names flattened to spaces). `MV_ERR_IO` if
  unreadable. Managed: `MediaViewerSession.ListSubdirectories`.

## PR 10 — forgetting a rewritten file (ABI 0.7)

```c
mv_status mv_folder_forget(mv_session_t, const char* utf8_path);
```

Drops a path's decoded pixels from the navigation LRU (the LRU is keyed by path) after a
lossless rotate rewrote the file. `MV_OK` when it was not cached. Does not select, decode or
touch the file.

## PR 26 — child folders for gallery tiles (ABI 0.8)

```c
mv_status mv_folder_directory(mv_session_t, char* utf8, uint32_t cap, uint32_t* out_bytes);
mv_status mv_folder_subfolder_count(mv_session_t, uint32_t* out_count);
mv_status mv_folder_subfolder_name / _path(mv_session_t, uint32_t index, char*, uint32_t, uint32_t*);

typedef struct mv_folder_summary {   /* 16 bytes */
  uint32_t media_count;   /* media files directly in the folder */
  uint32_t subdir_count;  /* direct child folders */
  uint32_t flags;         /* bit 0 loaded, bit 1 has cover, bit 2 photos in a descendant,
                             bit 3 bounded walk stopped early */
  uint32_t reserved;
} mv_folder_summary;

mv_status mv_folder_summary_at(mv_session_t, uint32_t index, mv_folder_summary* out);
mv_status mv_folder_summary_cover_thumb_path(mv_session_t, uint32_t index, char*, uint32_t, uint32_t*);
mv_status mv_folder_request_summary(mv_session_t, uint32_t index);   /* FOLDER_SUMMARY */
```

Child directories ride the same relist as media items (`io::list_subfolders`: natural order,
NAS/OS housekeeping folders dropped). A folder whose subfolders cannot be read still shows its
files. Summaries are requested lazily and computed on a pool thread by a bounded walk;
`mv_folder_request_summary` is a no-op when loaded or out of range.

## Result listings (ABI 0.14)

```c
mv_status mv_folder_open_list(mv_session_t, const char* title_utf8, const char* const* paths_utf8,
                              const int64_t* moments_ms, uint32_t count, uint32_t select_index,
                              uint64_t* out_job_id);
mv_status mv_folder_list_title(mv_session_t, char* utf8, uint32_t cap, uint32_t* out_bytes);
mv_status mv_folder_item_moment(mv_session_t, uint32_t index, int64_t* out_ms);
```

A listing that is not a directory — search results — shown by the same gallery, filmstrip,
selection, keyboard model and thumbnail cache. Items keep the given order (sort does not
apply), are never paired and are not watched. An item with a moment ≥ 0 is a clip that opens
**paused** on that frame (exact seek). `moments_ms` may be NULL. While a list is open
`mv_folder_directory` reports `""`; `FOLDER_READY` carries the count; `mv_folder_open` of a
directory ends the list. The Mac host has a direct twin.

## PR 5 — video

Times are `int64` nanoseconds ([05-video-pipeline.md](05-video-pipeline.md)).

```c
typedef enum mv_play_state { MV_PLAY_STOPPED, MV_PLAY_PLAYING, MV_PLAY_PAUSED, MV_PLAY_ENDED } mv_play_state;
typedef enum mv_decoder_kind { MV_DECODER_NONE, MV_DECODER_D3D11VA, MV_DECODER_SOFTWARE } mv_decoder_kind;

typedef struct mv_video_info {
  int64_t duration_ns; uint32_t width, height; double frame_rate;   /* nominal only */
  uint32_t audio_tracks, video_tracks, decoder /* mv_decoder_kind */, flags /* bit 0 audio, bit 1 10-bit */;
  char codec_name[32];
} mv_video_info;

typedef struct mv_video_stats {
  int64_t position_ns, audio_clock_ns;
  double err_ms_p50, err_ms_p99, drift_slope_ms_per_min, playback_rate;
  uint64_t frames_presented, frames_dropped_late, holds_cadence, holds_starved,
           device_rebuilds, position_discontinuities;
  uint32_t audio_master /* 0 = host-clock fallback */, fallback_reason;
} mv_video_stats;
```

| Call | Notes |
|---|---|
| `mv_video_open(s, path, &job)` | Bumps the view generation; answer `VIDEO_OPENED` |
| `mv_video_close(s)` | Idempotent |
| `mv_video_play` / `mv_video_pause` | |
| `mv_video_set_hold(s, hold, resume)` | 0.11. Gallery covers the canvas: pause, adopt new clips paused on their poster; on release resume only the clip that was playing (if still on screen and `resume` is 1). Play/pause while held are kept. Rule: `player/playback_hold.h` |
| `mv_video_seek(s, ns, exact)` | `exact` 0 = nearest keyframe (scrub drag), 1 = decode to the frame |
| `mv_video_step(s, ±1)` | Paused only; backward is a keyframe seek + forward decode |
| `mv_video_set_rate(s, r)` | 0.25–4.0, clamped, pitch-corrected `atempo` chain |
| `mv_video_set_volume` / `_set_muted` / `_select_audio_track` | volume 0.0–1.0 |
| `mv_video_set_loop(s, a, b)` | A–B loop; `b < 0` clears |
| `mv_video_position` / `_state` / `_get_info` / `_get_stats` | |
| `mv_probe_is_video(s, path, &is)` | Magic bytes, not extension |

## PR 13 / 14 — clip jobs and the keyframe index (ABI 0.10, 0.13)

`mediaviewer_clip.h`. Every call is `[any-thread][no-block]`. The index is read from packets
(never decoded) on a pool worker; only the newest request is read, and a superseded one is
answered `MV_ERR_CANCELLED`. Jobs run on the clip queue's single worker. The logic is the
portable `abi/clip_session`, which the Mac host links directly — one queue on both platforms.
See [08-video-editing.md](08-video-editing.md).

```c
typedef enum mv_clip_op {
  MV_CLIP_TRIM_KEYFRAME = 1, MV_CLIP_TRIM_REENCODE = 2, MV_CLIP_ROTATE = 3, MV_CLIP_SPLIT = 4,
  MV_CLIP_REMOVE_MIDDLE = 5, MV_CLIP_REMUX = 6, MV_CLIP_FRAME = 7, MV_CLIP_AUDIO = 8,
  MV_CLIP_ANIMATION = 9, MV_CLIP_KEEP_RANGES = 10 /* 0.13 */
} mv_clip_op;
typedef enum mv_clip_job_state { QUEUED = 1, RUNNING, DONE, FAILED, CANCELLED } mv_clip_job_state;

typedef struct mv_clip_request {
  uint32_t struct_size, op;
  int64_t  in_ns, out_ns;              /* player timeline; out < 0 = the end */
  uint32_t option;                     /* per op: rotate dir, remux target, frame/audio/anim format, cut mode */
  uint32_t animation_width;            /* long edge, 0 = 480 */
  uint32_t animation_fps;              /* 0 = 15 */
  uint32_t reserved;
  const int64_t* ranges_ns;            /* 0.13 KEEP_RANGES: flattened ascending [in,out) pairs */
  uint32_t range_count, reserved2;
} mv_clip_request;                     /* a request of the 0.10 size is still accepted */

typedef struct mv_clip_progress {
  uint64_t job_id; uint32_t state, op; double fraction; int64_t elapsed_ms, eta_ms /* -1 unknown */;
  uint32_t error, output_count; char title_utf8[64]; char source_name_utf8[256];
} mv_clip_progress;
```

| Call | Notes |
|---|---|
| `mv_clip_set_helper(s, path)` | The `MediaViewerClipJob` helper process. Jobs that open a decoder or encoder run there; if it cannot start they fail `MV_ERR_IO`. Stream-copy jobs run in process |
| `mv_clip_index_request(s, path, &req)` / `mv_clip_index_get(...)` | Keyframe times + duration; the session keeps the last few answers |
| `mv_clip_submit(s, source, &req, &job)` | Returns at once |
| `mv_clip_cancel` / `mv_clip_retry` / `mv_clip_jobs` / `mv_clip_clear_finished` | Cancel leaves nothing behind; retry makes a new job |
| `mv_clip_job_progress` / `mv_clip_job_output(s, job, index, …)` | Split has two outputs |

Outputs are new files beside the source under a free name (`IMG_0001_trimmed.mp4`, then
`" (2)"`), written to a hidden temporary first; a job never writes to its source and nothing logs
a path. No pixel crosses: a frame export is a file the job writes.

## Play badge — clips read apart from stills (ABI 0.15)

`mv_folder_item.flags` bit 2 is set when the stop's primary is a video **by extension**
(`io::is_video_name`, the same list the directory scan and the hosts' routing use). The gallery
and filmstrip draw a play badge. A Live Photo stop is its still and never carries it. Nothing is
opened or probed to set it.

## Crash context

`mediaviewer_crash.h`: `mv_crash_context(&info)` returns stable addresses — `slot_count`
fixed-size NUL-padded ASCII slots (one per decode worker, `slot_bytes` each) and a pointer to the
last ABI call's correlation id. The host registers them as annotations with its out-of-process
crash reporter; the core links no reporter. Slots hold only format family, decoder name and
version, dimensions, bit depth and correlation id — never a path, filename, bytes or EXIF
([13-updates-and-telemetry.md](13-updates-and-telemetry.md)).

## Add-ons

`mediaviewer_addon.h` ([18-import.md](18-import.md) has the mechanism). An add-on is one shared
library exporting `mv_addon_get(host_api, const mv_host_api*, mv_addon_api* out)`. It links
nothing of the core and reaches it only through the host function table.

- **Host API version** `MV_ADDON_HOST_API 2`, oldest served `MV_ADDON_HOST_API_OLDEST 1`.
  Versions only append; `struct_size` lets an older add-on read a newer table. An add-on loads
  when its manifest range meets `[OLDEST, HOST_API]` and receives `min(HOST_API, its max)`;
  otherwise `mv_addon_get` returns `MV_ERR_UNSUPPORTED_FORMAT` and the app reports "needs an
  update".
- **`mv_host_api`** (v1): file walk / stat / mkdir / remove / hash (BLAKE3-256, optionally
  uncached) / verified multi-target copy / write-new-file; volumes (list, of-path, eject, watch);
  `capture_info`, the viewer's own pairing (`pair_names`) and JPEG-512 `thumbnail_path`;
  scheduling (`should_yield` while the present loop is busy, `post_event` to the completion
  queue, `next_correlation_id`); `data_dir`, `default_library_dir`; `log` (never a path, name or
  hash). **v2 appends** pixels for the AI pack: `decode_still_rgb` (first-pixel quality,
  colour-managed, oriented), a frame `sampler_*` with HDR tone-mapped to SDR, `video_frame_rgb`,
  `moment_thumbnail`, `piece_dir`, `audio_*` (mono float PCM), `thumbnail_jpeg` /
  `thumbnail_store_jpeg`, and `recycle_file` (Recycle Bin / Trash only). Pixels are 8-bit sRGB
  RGB, stride `width * 3`, worker threads only.
- **`mv_addon_api`**: `id`, `version`, `shutdown` (`[ui-thread]`), and `query(interface_id)`
  returning a named interface — `"mv.import.1"` (`mv_import_api`, `mediaviewer_import.h`) or
  `"mv.ai.1"` (`mv_ai_api`, `mediaviewer_ai.h`, [17-local-ai-search.md](17-local-ai-search.md)).
  The AI pack also exports `mv_ai_reader_get`, a read-only instance of the same table for the
  Final Cut Pro search agent.
- **Events** post as `mv_completion{kind = MV_COMPLETION_ADDON (100), job_id = event id,
  generation = mv_addon_event_kind, status, payload}`. Kinds 1–9 are Import (scan, plan, job
  progress ~4 Hz, done, volume arrived/removed, verify, duplicates); 20–24 are the AI pack.
- **Host-side management** (exported by the Windows DLL for C#; the Mac host calls `src/addon`
  directly), worker-thread unless noted: `mv_addon_installed_json`, `mv_addon_check_manifest`
  (Ed25519 signature against the pinned key), `mv_addon_sha256_file`, `mv_addon_make_staging`,
  `mv_addon_install`, `mv_addon_family_usage`, `mv_addon_remove`, `mv_addon_load` (re-verifies,
  loads, returns the interface and the chrome entry path), `mv_addon_unload`, `mv_addon_quit` /
  `mv_addon_quit_wait(timeout_ms)` (`MV_ERR_TIMEOUT` → the host terminates without static
  destructors), `mv_volume_watch` (card-arrival hint), and `mv_present_set_busy` (the render
  loop's busy flag for `should_yield`).

## PR 55 — open add-ons (ABI 0.17)

Minor bump, additive, in `mediaviewer_addon.h`: `mv_open_addon_inspect`, `mv_open_addon_install`,
`mv_open_addon_list_json`, `mv_open_addon_remove`, `mv_open_addon_theme_json`
([25](25-open-addons.md)). All **[worker-thread]** (they read and hash files), all JSON out with
the `cap` / `needed` buffer rule. `inspect` answers `MV_OK` whenever it wrote its JSON, whatever
the JSON says: a refused package is an answer, not an error. `install` takes the SHA-256 that
`inspect` returned, so what is installed is what the person was shown; it is never called twice
for a size. The Mac host reaches the same code through `mv_open_addons_*` in the chrome bridge.
Both are thin wrappers over `src/addon/open_json.h`, which writes the one JSON both chromes read.

## PR 56 — contributed commands (ABI 0.18)

Minor bump, additive: `mv_addon_commands_json` returns the rows the loaded first-party add-ons'
manifests contribute (`[{"addon","id","name","windows","mac","modes","payload"}]`), which the
Windows shell turns into live command rows ([25 §7](25-open-addons.md#7-the-contribution-model)).
`mv_addon_installed_json` and `mv_addon_check_manifest` gain `description`, `hint_on` and
`hint_text`. The Mac host reads the same manifests directly.

## Version history

| Minor | Change |
|---|---|
| 0.5 | Pairs: `pair_kind` (was `reserved0`), flags bit 1, `mv_folder_item_pair_path` |
| 0.6 | `mv_folder_set_sort` / `_get_sort`, `mv_list_subdirectories` |
| 0.7 | `mv_folder_forget` |
| 0.8 | Child folders and summaries, `FOLDER_SUMMARY` |
| 0.10 | `mediaviewer_clip.h`, `CLIP_INDEX` / `CLIP_JOB` |
| 0.11 | `mv_video_set_hold` |
| 0.12 | Status codes 9–13 |
| 0.13 | `MV_CLIP_KEEP_RANGES`, `ranges_ns` |
| 0.14 | Result listings (`mv_folder_open_list`, `_list_title`, `_item_moment`) |
| 0.15 | `mv_folder_item.flags` bit 2 (video) |
| 0.16 | `mv_image_info.page_count` (was `reserved`), `mv_folder_select_page` — pages of a multi-page still |
| 0.17 | Open add-ons: `mv_open_addon_inspect` / `_install` / `_list_json` / `_remove` / `_theme_json` |
| 0.18 | Contributed commands: `mv_addon_commands_json`; `description`, `hint_on`, `hint_text` in the installed / check JSON |
| 0.19 | `mv_folder_set_hidden_kinds` — Settings can leave audio and/or documents out of the listing |

## Not built

- A direct callback form (`mv_set_callback`) for the canvas input path; input reaches the core
  through the published input snapshot instead.
- Per-object image handles (`mv_image_t` with `retain` / `release`); `mv_image_open` returns a job
  id and the only ABI handle is the session.
