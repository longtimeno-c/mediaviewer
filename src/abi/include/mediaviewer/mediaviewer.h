/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MediaViewer core — the flat C ABI between the C# shell and the C++ core.
 *
 * This header is the contract specified in plan/14-abi.md. Read that document
 * before changing anything here; the rules below are load-bearing, not style.
 *
 *   - No C++ types cross this line. No std::, no COM interfaces, no
 *     inheritance, no exceptions, no RTTI.
 *   - Only: opaque handles, POD structs with fixed-width fields and explicit
 *     padding, UTF-8 `const char*` with documented ownership, and function
 *     pointers always paired with a `void* user_data`.
 *   - Every fallible call returns mv_status. Never a bool, never -1.
 *   - The side that allocates, frees.
 *   - Every function carries a thread annotation, and anything the C# UI
 *     thread calls must be [no-block].
 *
 * PIXELS DO NOT CROSS THIS LINE. Not textures, not decoded frames, not
 * ID3D11 anything. The swapchain lives entirely in C++; managed code learns a
 * canvas exists and sends it a size and input events. If you find yourself
 * wanting to marshal a decoded frame, the boundary is in the wrong place.
 */
#ifndef MEDIAVIEWER_MEDIAVIEWER_H
#define MEDIAVIEWER_MEDIAVIEWER_H

#include <stdint.h>

#if defined(_WIN32)
#  if defined(MV_BUILDING_CORE)
#    define MV_API __declspec(dllexport)
#  else
#    define MV_API __declspec(dllimport)
#  endif
#  define MV_CALL __cdecl
#else
#  define MV_API
#  define MV_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Versioning
 *
 * The managed side asserts on this at startup. A mismatched core DLL must fail
 * loudly rather than corrupt memory quietly — the failure mode of getting this
 * wrong is a struct layout change nobody notices until a field reads garbage.
 * ------------------------------------------------------------------------- */
#define MV_ABI_VERSION_MAJOR 0
#define MV_ABI_VERSION_MINOR 4

/* Packed as (major << 16) | minor. [any-thread] */
MV_API uint32_t MV_CALL mv_abi_version(void);

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */
typedef enum mv_status {
  MV_OK = 0,
  MV_ERR_INVALID_ARG = 1,
  MV_ERR_OUT_OF_MEMORY = 2,
  MV_ERR_IO = 3,
  MV_ERR_UNSUPPORTED_FORMAT = 4,
  MV_ERR_CORRUPT = 5,
  MV_ERR_CANCELLED = 6,
  MV_ERR_DEVICE_LOST = 7,
  MV_ERR_INTERNAL = 8
} mv_status;

/* Stable, allocation-free name for a status. Points at a string literal that
 * outlives the process; the caller must not free it. [any-thread] */
MV_API const char* MV_CALL mv_status_name(mv_status status);

/* Detail for the LAST failing call on the CALLING THREAD. The buffer is owned
 * by the core and is valid only until the next call on this thread — C# must
 * copy it immediately with Marshal.PtrToStringUTF8 and never store the pointer.
 * Returns an empty string, never NULL. [any-thread][no-block] */
MV_API const char* MV_CALL mv_last_error_message(void);

/* The correlation id of the last failing call on the calling thread. This is
 * what ties a managed MediaViewerException back to the native minidump that
 * caused it (plan/13-updates-and-telemetry.md). [any-thread][no-block] */
MV_API uint64_t MV_CALL mv_last_error_correlation_id(void);

/* ---------------------------------------------------------------------------
 * Session — the root handle
 * ------------------------------------------------------------------------- */
typedef struct mv_session* mv_session_t;

typedef struct mv_session_config {
  /* 0 selects max(1, cores - 2): the UI thread and the render thread are not
   * the pool's to spend. */
  uint32_t worker_count;
  /* Non-zero registers the ETW provider for this process. */
  uint32_t enable_etw;
} mv_session_config;

/* [any-thread] Creates a session. `config` may be NULL for defaults.
 * The returned handle is reference-counted with a count of one. */
MV_API mv_status MV_CALL mv_session_create(const mv_session_config* config,
                                           mv_session_t* out_session);

/* [any-thread] Handles are reference-counted so the filmstrip holding a
 * thumbnail and the canvas holding the same image do not fight over lifetime.
 * The C# wrapper never calls retain: it takes ownership of what the factory
 * returned and releases exactly once. */
MV_API mv_status MV_CALL mv_session_retain(mv_session_t session);
MV_API mv_status MV_CALL mv_session_release(mv_session_t session);

/* ---------------------------------------------------------------------------
 * Generations — cancellation, tied to view intent
 *
 * Every job carries the generation current when it was submitted. Navigating
 * away bumps the generation and everything queued at the old one is abandoned
 * at its next check. Without this, fast arrow-key browsing queues two hundred
 * dead decodes (plan/02-architecture.md).
 * ------------------------------------------------------------------------- */

/* [any-thread][no-block] Returns the new generation. */
MV_API mv_status MV_CALL mv_session_bump_generation(mv_session_t session,
                                                    uint32_t* out_generation);

/* [any-thread][no-block] */
MV_API mv_status MV_CALL mv_session_current_generation(mv_session_t session,
                                                       uint32_t* out_generation);

/* ---------------------------------------------------------------------------
 * Completions — the core never touches the dispatcher
 *
 * C++ must never call DispatcherQueue.TryEnqueue, or any managed callback, from
 * a worker thread. Instead the core owns a completion queue and C# drains it:
 * no marshalling policy baked into the core, no managed code running on a
 * decode worker, completions naturally batched (a folder scan finishing 400
 * thumbnails is ONE drain, not 400 marshalling hops), and the core stays
 * testable headlessly with no dispatcher at all.
 *
 * IMAGE_OPENED means pixels may be ready on the native canvas. Draining this
 * queue does not wake an idle render thread; the lab/shell must
 * (plan/03-rendering.md rule 4).
 * ------------------------------------------------------------------------- */
typedef enum mv_completion_kind {
  MV_COMPLETION_NONE = 0,
  MV_COMPLETION_ECHO = 1,          /* PR 1's round-trip proof. */
  MV_COMPLETION_IMAGE_OPENED = 2,  /* PR 2. payload = (width << 32) | height. */
  MV_COMPLETION_FOLDER_READY = 3,  /* PR 4. payload = item count. */
  MV_COMPLETION_FOLDER_CHANGED = 4,/* watcher; payload = item count. */
  MV_COMPLETION_THUMB_READY = 5,   /* payload = item index. */
  MV_COMPLETION_FOLDER_SELECTED = 6,/* payload = selected index. */
  /* PR 5. payload = duration in nanoseconds. */
  MV_COMPLETION_VIDEO_OPENED = 7,
  /* Reached the end of the clip. payload = 0. */
  MV_COMPLETION_VIDEO_ENDED = 8,
  /* Play/pause/stop changed. payload = mv_play_state. Pushed on EVERY
   * transition, including ones the host asked for: telling the host about a
   * change it requested is redundant but never wrong, and the alternative is
   * threading "who caused this" through the command queue for no gain. Treat
   * these as notifications, not as acknowledgements of your own calls. The
   * ones that matter are the transitions the core makes on its own — reaching
   * the end of a clip, device loss. */
  MV_COMPLETION_VIDEO_STATE = 9
} mv_completion_kind;

typedef struct mv_completion {
  uint32_t kind;        /* mv_completion_kind */
  uint32_t status;      /* mv_status */
  uint64_t job_id;
  uint64_t correlation_id;
  uint32_t generation;
  uint32_t reserved;    /* explicit padding; keeps the struct 8-byte aligned
                         * and the layout obvious on both sides */
  int64_t  payload;     /* kind-specific scalar. Never a pointer. */
} mv_completion;

/* [any-thread] A manual-reset event, signalled while at least one completion is
 * pending. Owned by the session; the caller must NOT CloseHandle it. Returns
 * NULL on a bad handle. C# wraps this in a SafeWaitHandle with
 * ownsHandle:false. */
MV_API void* MV_CALL mv_completion_wait_handle(mv_session_t session);

/* [any-thread][no-block] Non-blocking drain into `out`. Returns the count
 * written; 0 when empty. Safe to call from the render loop. */
MV_API uint32_t MV_CALL mv_completion_drain(mv_session_t session, mv_completion* out,
                                            uint32_t capacity);

/* ---------------------------------------------------------------------------
 * The PR 1 round-trip
 *
 * plan/14-abi.md's PR 1 deliverable is "a header, an mv_guard, one call, a
 * SafeHandle, and a completion drain — proving the shape end to end before
 * anything is built on it."
 *
 * mv_session_echo IS that call, and it is deliberately shaped like the real
 * ones that follow: it does not compute the answer before returning. It
 * validates, copies the caller's string, submits a job at the current
 * generation, and returns a job id immediately. The answer arrives as a
 * completion. Opening an image in PR 2 has exactly this shape; if this call
 * were synchronous it would teach the wrong pattern on day one.
 * ------------------------------------------------------------------------- */

/* [any-thread][no-block] `utf8_text` is owned by the caller and copied before
 * this returns; the core never retains the pointer. The completion carries
 * MV_COMPLETION_ECHO with `payload` set to the byte length of the text. */
MV_API mv_status MV_CALL mv_session_echo(mv_session_t session, const char* utf8_text,
                                         uint64_t* out_job_id);

/* [any-thread][no-block] Diagnostics for the F3 overlay and the tests. */
typedef struct mv_job_stats {
  uint64_t submitted;
  uint64_t completed;
  uint64_t cancelled;
  uint64_t queue_depth;
  uint32_t worker_count;
  uint32_t generation;
} mv_job_stats;

MV_API mv_status MV_CALL mv_session_job_stats(mv_session_t session, mv_job_stats* out_stats);

/* ---------------------------------------------------------------------------
 * Image open — same shape as echo: returns a job id immediately, the answer
 * arrives as MV_COMPLETION_IMAGE_OPENED. Pixels never cross this line
 * (plan/14). The native present lab binds the D3D device out of band and
 * takes the resulting texture on the render thread.
 * ------------------------------------------------------------------------- */

typedef struct mv_image_info {
  uint32_t width;
  uint32_t height;
  uint32_t format;          /* 1 JPEG, 2 PNG, 3 BMP — matches codec::format_family */
  uint32_t icc_tagged;      /* non-zero if an ICC profile (or sRGB chunk) was used */
  uint32_t transfer_intent; /* 0 = display-referred (no tone map) */
  uint32_t reserved;
} mv_image_info;

/* [any-thread][no-block] `utf8_path` is owned by the caller and copied before
 * this returns. Submit at the current generation; bump first when replacing
 * the view. */
MV_API mv_status MV_CALL mv_image_open(mv_session_t session, const char* utf8_path,
                                       uint64_t* out_job_id);

/* [any-thread][no-block] Last successfully opened image, if any. */
MV_API mv_status MV_CALL mv_session_image_info(mv_session_t session, mv_image_info* out_info);

/* ---------------------------------------------------------------------------
 * Folder — listing, watch, thumbs, select (PR 4)
 *
 * Same shape as image open: the scan returns a job id, FOLDER_READY carries
 * the count. Item strings are copied into a caller buffer (never a pointer
 * the core retains). Thumbnails are on-disk JPEG paths, never pixels.
 * ------------------------------------------------------------------------- */

typedef struct mv_folder_item {
  uint32_t index;
  uint32_t flags;       /* bit 0 = selected */
  uint64_t size_bytes;
  int64_t  mtime_unix;
  uint32_t reserved0;
  uint32_t reserved1;
} mv_folder_item;

/* [any-thread][no-block] `utf8_dir` is copied. `utf8_select_path` may be NULL;
 * when set, that file is selected after the scan (otherwise index 0). */
MV_API mv_status MV_CALL mv_folder_open(mv_session_t session, const char* utf8_dir,
                                        const char* utf8_select_path, uint64_t* out_job_id);

/* [any-thread][no-block] */
MV_API mv_status MV_CALL mv_folder_count(mv_session_t session, uint32_t* out_count);
MV_API mv_status MV_CALL mv_folder_item_at(mv_session_t session, uint32_t index,
                                           mv_folder_item* out_item);

/* Caller buffer, UTF-8. `out_bytes` (optional) is the required size including
 * NUL. `cap == 0` returns the size and MV_ERR_INVALID_ARG. Truncates with NUL
 * if cap is too small and still reports the full size in out_bytes. */
MV_API mv_status MV_CALL mv_folder_item_name(mv_session_t session, uint32_t index, char* utf8,
                                             uint32_t cap, uint32_t* out_bytes);
MV_API mv_status MV_CALL mv_folder_item_path(mv_session_t session, uint32_t index, char* utf8,
                                             uint32_t cap, uint32_t* out_bytes);
MV_API mv_status MV_CALL mv_folder_item_thumb_path(mv_session_t session, uint32_t index,
                                                   char* utf8, uint32_t cap, uint32_t* out_bytes);

/* [any-thread][no-block] Bump view generation, publish an LRU hit or open,
 * prefetch ±2. Pushes MV_COMPLETION_FOLDER_SELECTED. */
MV_API mv_status MV_CALL mv_folder_select(mv_session_t session, uint32_t index,
                                          uint64_t* out_job_id);

/* [any-thread][no-block] */
MV_API mv_status MV_CALL mv_folder_selected(mv_session_t session, uint32_t* out_index);

/* [any-thread][no-block] Visible-first thumbs. Skipping still generates every
 * thumb, just not visible-first. */
MV_API mv_status MV_CALL mv_folder_thumbs_visible(mv_session_t session, uint32_t first,
                                                  uint32_t count);

MV_API mv_status MV_CALL mv_folder_close(mv_session_t session);

/* -------------------------------------------------------------------------
 * PR 5 — video. plan/05-video-pipeline.md, plan/14-abi.md.
 *
 * Times are int64 NANOSECONDS everywhere, matching player::time_ns. No frame,
 * texture or ID3D11* ever crosses this line (plan/14 "What crosses, and what
 * does not"): the host learns a clip is open and sends it transport commands.
 * Pixels stay in C++.
 * ------------------------------------------------------------------------- */

typedef enum mv_play_state {
  MV_PLAY_STOPPED = 0,
  MV_PLAY_PLAYING = 1,
  MV_PLAY_PAUSED  = 2,
  MV_PLAY_ENDED   = 3
} mv_play_state;

typedef enum mv_decoder_kind {
  MV_DECODER_NONE = 0,
  MV_DECODER_D3D11VA = 1, /* hardware, on our own device */
  MV_DECODER_SOFTWARE = 2 /* CPU fallback. Must be visible, never silent. */
} mv_decoder_kind;

typedef struct mv_video_info {
  int64_t  duration_ns;
  uint32_t width;
  uint32_t height;
  double   frame_rate;      /* nominal only; presentation is on PTS */
  uint32_t audio_tracks;
  uint32_t video_tracks;
  uint32_t decoder;         /* mv_decoder_kind */
  uint32_t flags;           /* bit 0 = has audio, bit 1 = 10-bit */
  char     codec_name[32];  /* NUL-terminated, e.g. "hevc" */
} mv_video_info;

/* The F3 overlay and any managed diagnostics read this. Counters are separated
 * deliberately: a cadence hold is CORRECT on 24p content at 60 Hz, and merging
 * it with a starvation hold makes healthy playback look broken. */
typedef struct mv_video_stats {
  int64_t  position_ns;
  int64_t  audio_clock_ns;
  double   err_ms_p50;
  double   err_ms_p99;
  double   drift_slope_ms_per_min;
  double   playback_rate;
  uint64_t frames_presented;
  uint64_t frames_dropped_late;
  uint64_t holds_cadence;
  uint64_t holds_starved;
  uint64_t device_rebuilds;
  uint64_t position_discontinuities;
  uint32_t audio_master;    /* 0 => host-clock fallback */
  uint32_t fallback_reason;
} mv_video_stats;

/* [any-thread][no-block] Opens a clip. Returns immediately with a job id; the
 * answer arrives as MV_COMPLETION_VIDEO_OPENED. Bumps the view generation, so
 * in-flight work for the previous item is cancelled. */
MV_API mv_status MV_CALL mv_video_open(mv_session_t session, const char* utf8_path,
                                       uint64_t* out_job_id);

/* [any-thread][no-block] Idempotent; closing nothing is not an error. */
MV_API mv_status MV_CALL mv_video_close(mv_session_t session);

/* [any-thread][no-block] */
MV_API mv_status MV_CALL mv_video_play(mv_session_t session);
MV_API mv_status MV_CALL mv_video_pause(mv_session_t session);

/* [any-thread][no-block] `exact` 0 while dragging the scrubber (nearest
 * keyframe, no decode — instant); 1 on release or a typed position (decode
 * forward to the exact frame). plan/05's fast-then-accurate rule. */
MV_API mv_status MV_CALL mv_video_seek(mv_session_t session, int64_t position_ns,
                                       int32_t exact);

/* [any-thread][no-block] Paused only. +1 / -1. Backward is a keyframe seek plus
 * a forward decode, so it is not as cheap as forward. */
MV_API mv_status MV_CALL mv_video_step(mv_session_t session, int32_t frames);

/* [any-thread][no-block] 0.25 .. 4.0, clamped. Pitch-corrected via a chained
 * atempo filter (one instance only covers 0.5-2.0). */
MV_API mv_status MV_CALL mv_video_set_rate(mv_session_t session, double rate);

/* [any-thread][no-block] volume 0.0 .. 1.0. */
MV_API mv_status MV_CALL mv_video_set_volume(mv_session_t session, float volume);
MV_API mv_status MV_CALL mv_video_set_muted(mv_session_t session, int32_t muted);
MV_API mv_status MV_CALL mv_video_select_audio_track(mv_session_t session, uint32_t index);

/* [any-thread][no-block] A-B loop. b_ns < 0 clears it. */
MV_API mv_status MV_CALL mv_video_set_loop(mv_session_t session, int64_t a_ns, int64_t b_ns);

/* [any-thread][no-block] */
MV_API mv_status MV_CALL mv_video_position(mv_session_t session, int64_t* out_position_ns);
MV_API mv_status MV_CALL mv_video_state(mv_session_t session, uint32_t* out_state);
MV_API mv_status MV_CALL mv_video_get_info(mv_session_t session, mv_video_info* out_info);
MV_API mv_status MV_CALL mv_video_get_stats(mv_session_t session, mv_video_stats* out_stats);

/* [any-thread][no-block] True when the path is a clip we can open, by MAGIC
 * BYTES not extension (CLAUDE.md: "Probe by magic bytes, never extension").
 * Lets the shell decide image-vs-video without opening anything. */
MV_API mv_status MV_CALL mv_probe_is_video(mv_session_t session, const char* utf8_path,
                                           int32_t* out_is_video);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MEDIAVIEWER_MEDIAVIEWER_H */
