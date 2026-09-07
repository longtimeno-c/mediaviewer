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
submission and progress, transport commands, settings.

**Does not cross:** pixels, textures, decoded frames, `ID3D11*` anything. The swapchain lives
entirely in C++ ([03-rendering.md](03-rendering.md)); managed code learns a canvas *exists* and
sends it a size and input events. A single decoded frame must never be marshalled — if you find a
design where it is, the boundary is in the wrong place.

## PR 1 deliverable

A header, a `mv_guard`, one round-tripping call, a `SafeHandle`, and a completion drain — proving
the shape end to end before anything is built on it. Roughly a day, and it is the day that decides
whether PR 3–4 are pleasant or miserable.
