# 14 — The C ABI

**D1** put a language boundary through the middle of the app, and said "design the ABI once, in
PR 1." Naming it is not designing it. This is the design.

Get this wrong and the symptom is not a compile error — it is the filmstrip and the canvas fighting
over texture lifetime in PR 3–4, VRAM climbing until the app dies, and a crash you can only
reproduce on someone else's machine.

## Shape

A **flat C header**. No C++ types across the line — no `std::string`, no `std::vector`, no
exceptions, no RTTI, no COM interfaces, no inheritance. Only:

- opaque handles (`typedef struct mv_session* mv_session_t;`)
- POD structs with explicit fixed-width fields and explicit padding
- `const char*` UTF-8 strings with documented ownership
- function pointers for callbacks, always paired with a `void* user_data`

Everything is `extern "C"` and `__cdecl`. Version the header with a
`mv_abi_version()` the C# side asserts on startup — a mismatched core DLL must fail loudly, not
corrupt memory quietly.

## Error model

```c
typedef enum mv_status {
  MV_OK = 0,
  MV_ERR_INVALID_ARG, MV_ERR_OUT_OF_MEMORY, MV_ERR_IO, MV_ERR_UNSUPPORTED_FORMAT,
  MV_ERR_CORRUPT, MV_ERR_CANCELLED, MV_ERR_DEVICE_LOST, MV_ERR_INTERNAL
} mv_status;

// Every fallible call returns mv_status. Never a bool, never -1.
mv_status mv_image_open(mv_session_t s, const char* utf8_path, mv_image_t* out_image);

// Last error detail for the calling thread; valid until the next call on that thread.
const char* mv_last_error_message(void);
uint64_t    mv_last_error_correlation_id(void);
```

**No exception may cross the boundary.** Every exported function is wrapped:

```cpp
extern "C" mv_status mv_image_open(...) noexcept {
  return mv_guard([&]{ /* real work */ });   // catches everything, maps to mv_status
}
```

An exception escaping into managed code through P/Invoke is undefined behaviour, and it will not
look like the bug it is. The `mv_guard` wrapper is not optional politeness — it is the boundary.

The **correlation id** matters because of D1's two-language crash story
([13-updates-and-telemetry.md](13-updates-and-telemetry.md)): it is what ties a managed
`MediaViewerException` back to the native minidump that caused it.

## Ownership — the rule that prevents the leak

**The side that allocates, frees.** No exceptions, no "just call `free` on it."

| Direction | Rule |
|---|---|
| Core → managed, strings | Core owns the buffer; valid until the next call on that handle. C# **must copy immediately** (`Marshal.PtrToStringUTF8`) and never store the pointer. |
| Core → managed, buffers | Returned via a handle + explicit `mv_*_release`. Never a raw pointer C# is expected to free. |
| Managed → core, strings | Caller owns; core copies before returning. Core never retains the pointer past the call. |
| Managed → core, buffers | Pinned for the duration of the call only. Core never retains. |

## Handles and `SafeHandle` — non-negotiable on the C# side

Every opaque handle gets a `SafeHandle` subclass. Not an `IntPtr`, not a finalizer on a wrapper
class.

```csharp
sealed class MvImageHandle : SafeHandleZeroOrMinusOneIsInvalid {
    protected override bool ReleaseHandle() => NativeMethods.mv_image_release(handle) == 0;
}
```

An `IntPtr` that the GC loses is a leaked decoded image — tens or hundreds of MB of VRAM, per
occurrence, invisible until the app falls over on a long browsing session. `SafeHandle` also gets
you correct behaviour if the process is torn down mid-call, which a hand-rolled finalizer does not.

**Handles are reference-counted in the core**, so the filmstrip holding a thumbnail and the canvas
holding the same image do not fight. `mv_*_retain` / `mv_*_release`, and the C# wrapper never calls
`retain` — it takes ownership of what the factory returned and releases exactly once.

## Thread contract — write it on every function

Each exported function is annotated in the header with exactly one of:

| Annotation | Meaning |
|---|---|
| `[any-thread]` | Safe from any thread, concurrently |
| `[ui-thread]` | Must be called on the thread that created the session (window/input/canvas) |
| `[no-block]` | Returns without I/O, decode, or lock contention — safe from the UI thread |

The rule that follows from [02-architecture.md](02-architecture.md): **anything the C# UI thread
calls must be `[no-block]`.** Opening an image is a request that returns immediately with a job id;
it is never a call that decodes before returning. If a function cannot honour that, it does not get
called from managed code — it gets an async form that does.

## Completions — the core never touches the dispatcher

This is the one people get wrong, and it is worth being blunt about: **C++ must never call
`DispatcherQueue.TryEnqueue`, or any managed callback, from a worker thread.**

Instead, the core owns a completion queue and C# drains it:

```c
// [any-thread] Signalled when at least one completion is pending.
void*     mv_completion_wait_handle(mv_session_t);
// [any-thread] Non-blocking drain. Returns count written; 0 when empty.
uint32_t  mv_completion_drain(mv_session_t, mv_completion* out, uint32_t cap);
```

C# waits on the handle (or ticks it from the render loop) and dispatches on its own thread. This
means:

- no marshalling policy baked into the core,
- no managed code running on a decode worker,
- completions are naturally batched, so a folder scan finishing 400 thumbnails is one drain, not
  400 marshalling hops,
- the core stays testable headlessly, with no dispatcher at all.

`MV_COMPLETION_IMAGE_OPENED` means new pixels may be ready on the native canvas. The UI thread
draining the queue does **not** wake an idle render thread by itself — the lab (and later the
shell) must `wake()` the presenter. See [03](03-rendering.md) rule 4.

`mv_image_open` is submitted at the **current** generation. The caller bumps first when the open
is a new view intent (the native lab does; C# `OpenImage` must too). Opening without a bump
replaces rather than cancels.

A direct callback form (`mv_set_callback` with `user_data`) may exist for the **canvas input path
only**, where per-mouse-move latency matters and the call is already on the UI thread. Everywhere
else: drain.

## What crosses, and what does not

**Crosses:** open/close, navigation intent, edit-stack parameters, metadata property lists, job
submission and progress, transport commands, settings, folder item records, **UTF-8 paths to
on-disk thumbnails**. A `command_id` integer enum may be shared so C# and C++ agree on
canvas-owned effects. **Bindings never cross** — the keymap is host chrome
([16-commands.md](16-commands.md)).

**Does not cross:** pixels, textures, decoded frames, `ID3D11*` anything. The swapchain lives
entirely in C++ ([03-rendering.md](03-rendering.md)); managed code learns a canvas *exists* and
sends it a size and input events. A single decoded frame must never be marshalled — if you find a
design where it is, the boundary is in the wrong place. A thumbnail JPEG *file path* is a path,
not a frame; a `uint8_t*` of decoded RGBA for the filmstrip is a frame and is forbidden.

## PR 4 — folder, thumbs, prefetch

Same shape as `mv_image_open`: requests return a job id, answers arrive as completions.
Bump **view** generation on `mv_folder_select` (it is a new view intent). Thumb jobs ride a
separate **folder** generation so arrow-key bumps do not cancel the filmstrip.

```c
typedef enum mv_completion_kind {
  /* ... PR 1–2 ... */
  MV_COMPLETION_FOLDER_READY  = 3,  /* payload = item count */
  MV_COMPLETION_FOLDER_CHANGED = 4, /* watcher; payload = item count */
  MV_COMPLETION_THUMB_READY   = 5   /* payload = item index */
} mv_completion_kind;

typedef struct mv_folder_item {
  uint32_t index;
  uint32_t flags;          /* bit 0 = selected */
  uint64_t size_bytes;
  int64_t  mtime_unix;
  uint32_t reserved0;
  uint32_t reserved1;
} mv_folder_item;

/* [any-thread][no-block] Copy the directory path. Completions: FOLDER_READY. */
mv_status mv_folder_open(mv_session_t, const char* utf8_dir, uint64_t* out_job_id);

/* [any-thread][no-block] */
mv_status mv_folder_count(mv_session_t, uint32_t* out_count);
mv_status mv_folder_item_at(mv_session_t, uint32_t index, mv_folder_item* out);
/* Name / path / thumb path: caller buffer, UTF-8, NUL-terminated if cap allows.
 * out_bytes is the required size including NUL. MV_ERR_INVALID_ARG if cap is 0.
 * Thumb path is empty until THUMB_READY for that index. */
mv_status mv_folder_item_name(mv_session_t, uint32_t index, char* utf8, uint32_t cap, uint32_t* out_bytes);
mv_status mv_folder_item_path(mv_session_t, uint32_t index, char* utf8, uint32_t cap, uint32_t* out_bytes);
mv_status mv_folder_item_thumb_path(mv_session_t, uint32_t index, char* utf8, uint32_t cap, uint32_t* out_bytes);

/* [any-thread][no-block] Bump view generation, publish LRU hit or open, prefetch ±2. */
mv_status mv_folder_select(mv_session_t, uint32_t index, uint64_t* out_job_id);

mv_status mv_folder_close(mv_session_t);
```

Visible-first thumbs: after `FOLDER_READY`, the host tells the core which indices are on
screen with `mv_folder_thumbs_visible(session, first, count)` (`[no-block]`). The core
submits those first, then the rest at the folder generation. Skipping this call still
generates every thumb; it just is not visible-first.

The five-slot GPU LRU is native-only (`native.h`), same as `take_ready_image`. C# never
sees a texture.

## PR 7 — pairs are one stop (ABI 0.5)

A folder item is a **navigation stop**, not a file. RAW+JPEG (or RAW+HEIC) and Live Photos
(HEIC+MOV, and JPG+MOV from an iPhone set to "Most Compatible") are paired at scan time
([04](04-image-pipeline.md)); ambiguous groups stay separate. Minor bump, **no layout change**:
`mv_folder_item` stays 32 bytes.

```c
typedef enum mv_pair_kind { MV_PAIR_NONE = 0, MV_PAIR_RAW_JPEG = 1, MV_PAIR_LIVE_PHOTO = 2 } mv_pair_kind;

typedef struct mv_folder_item {
  uint32_t index;
  uint32_t flags;          /* bit 0 = selected; bit 1 = primary is a RAW (badge only) */
  uint64_t size_bytes;     /* primary */
  int64_t  mtime_unix;     /* primary */
  uint32_t pair_kind;      /* mv_pair_kind; was reserved0 (always 0 before 0.5) */
  uint32_t reserved1;
} mv_folder_item;

/* [any-thread][no-block] The secondary (RAW, or the Live Photo's MOV). Empty when unpaired. */
mv_status mv_folder_item_pair_path(mv_session_t, uint32_t index, char* utf8, uint32_t cap, uint32_t* out_bytes);
```

- Name, path, size, mtime, thumbs, decode, prefetch and video detection all use the
  **primary**. A Live Photo stop is a still; its motion is opened only by `;`
  (`mv_video_open` on the pair path, from the host).
- `mv_folder_open`'s `utf8_select_path` may name either half: opening the `.NEF` selects the
  JPEG+NEF stop, and the canvas shows the primary. A watcher refresh keeps the stop by either
  half. `FOLDER_READY` / `FOLDER_CHANGED` payloads count stops.
- Copy, move, drag-out and Recycle Bin act on **both halves** of a paired stop; that
  expansion is host-side, the ABI only reports the pair.

## PR 9 — sort and the folder tree (ABI 0.6)

Minor bump, **no layout change**. The Windows folder listing is owned by the session, so the
sort order is too: every consumer (filmstrip, gallery, arrow keys) reads one list.

```c
/* Packed: key in bits 0-2 (0 name, 1 modified, 2 size, 3 type, 4 date taken), descending in bit 3.
 * set re-sorts on a worker, keeps the current stop, pushes MV_COMPLETION_FOLDER_CHANGED. It also
 * applies to every later mv_folder_open. Unknown key bits mean name. [any-thread][no-block] */
mv_status mv_folder_set_sort(mv_session_t, int32_t packed);
mv_status mv_folder_get_sort(mv_session_t, int32_t* out_packed);

/* The folder tree's one directory read: "name\tpath\n" per visible subfolder, sorted. Worker
 * threads only. Buffer rules as mv_folder_item_name; MV_ERR_IO if the directory cannot be read. */
mv_status mv_list_subdirectories(const char* utf8_dir, char* utf8, uint32_t cap, uint32_t* out_bytes);
```

- Date taken is read once per file (bounded prefix, background job) and remembered per
  (path, mtime, size). Until a stamp is known the file sorts by mtime, and the listing re-sorts once
  the stamps land, so the order is total while they arrive. A file with no stamp stays on mtime.
- `mv_list_subdirectories` takes no session: it is a pure directory read, and the managed tree
  calls it through `MediaViewerSession.ListSubdirectories`.

## PR 10 — forgetting a rewritten file (ABI 0.7)

Minor bump, **no layout change**. The viewer's lossless rotate rewrites the JPEG on disk; the
navigation LRU is keyed by path, so without this a reselect republishes the old pixels.

```c
/* Drop `utf8_path`'s decoded pixels from the navigation LRU. MV_OK when it was not cached.
 * Does not select, decode or touch the file. [any-thread][no-block] */
mv_status mv_folder_forget(mv_session_t, const char* utf8_path);
```

Edits themselves never cross the ABI: the edit stack, crop mode and the lossless / export jobs
live in `shell/edit_session` (shared with the Mac host), and the geometry reaches the render
thread through the input snapshot, like every other view state.

## PR 1 deliverable

A header, a `mv_guard`, one round-tripping call, a `SafeHandle`, and a completion drain — proving
the shape end to end before anything is built on it. Roughly a day, and it is the day that decides
whether PR 3–4 are pleasant or miserable.
