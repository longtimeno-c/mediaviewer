/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MediaViewer add-ons — the host function table and the add-on entry point
 * (docs/design/18-import.md "What an add-on is, technically"; docs/design/14-abi.md rules).
 *
 * An add-on is one shared library (mv_import.dll / libmv_import.dylib) that
 * exports exactly one symbol, `mv_addon_get`. The host passes a FUNCTION
 * TABLE: the add-on does not link the core and does not reach into it. Flat
 * C, POD, status codes, correlation ids. Pointers passed into a call are
 * valid for that call only unless stated otherwise; the side that allocates,
 * frees.
 *
 * The same table serves both hosts (D9): the Windows host builds it over the
 * core DLL, the Mac host over the same static core in the app.
 *
 * Nothing about a user's files crosses to a log or the network through this
 * table (rule 6): `log` must never be given a path, a name, or a hash.
 */
#ifndef MEDIAVIEWER_ADDON_H
#define MEDIAVIEWER_ADDON_H

#include <stdint.h>

#include "mediaviewer.h"

#if defined(_WIN32)
#  define MV_ADDON_EXPORT __declspec(dllexport)
#else
#  define MV_ADDON_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The host function table's version. An add-on's manifest declares the range
 * it supports (host_api.min .. host_api.max); outside it the add-on is not
 * loaded and the app says it needs an update. Additive changes append fields
 * and bump this; `struct_size` lets an older add-on read a newer table.
 *
 * Negotiation (Milestone H, 2026-09-26): because every version only APPENDS,
 * this host still serves every layout from MV_ADDON_HOST_API_OLDEST up. An
 * add-on loads when its range meets [OLDEST, MV_ADDON_HOST_API]; mv_addon_get
 * receives min(MV_ADDON_HOST_API, the add-on's max) and the table's host_api
 * says the same. So Import 1.0.0 (host_api 1..1) keeps loading beside the
 * AI pack (2..2) without an update.
 *
 *   1  Milestone G: io, folder model, pairing, thumbnails, scheduling.
 *   2  Milestone H: stills and video frames as pixels, moment thumbnails,
 *      family pieces (docs/design/17 "The AI pack"). */
#define MV_ADDON_HOST_API 2
#define MV_ADDON_HOST_API_OLDEST 1

/* Add-on completion kinds, posted through the host's completion queue with
 * mv_completion.kind = MV_COMPLETION_ADDON. job_id is the add-on's own id,
 * payload is the add-on event below. */
#define MV_COMPLETION_ADDON 100

typedef enum mv_addon_event_kind {
  MV_ADDON_EVENT_NONE = 0,
  MV_ADDON_EVENT_SCAN_DONE = 1,      /* a source scan finished; job_id = scan id */
  MV_ADDON_EVENT_PLAN_READY = 2,     /* a plan was (re)computed */
  MV_ADDON_EVENT_JOB_PROGRESS = 3,   /* throttled to ~4 Hz while copying */
  MV_ADDON_EVENT_JOB_DONE = 4,       /* finished, failed or cancelled; see the summary */
  MV_ADDON_EVENT_VOLUME_ARRIVED = 5,
  MV_ADDON_EVENT_VOLUME_REMOVED = 6,
  MV_ADDON_EVENT_VERIFY_DONE = 7,    /* verify-a-folder finished */
  /* Find duplicates (PR 54): the scan finished (payload = job state), and
   * one trash request settled (id = job; re-read the job's summary). */
  MV_ADDON_EVENT_DUPLICATES_DONE = 8,
  MV_ADDON_EVENT_DUPLICATE_TRASHED = 9,

  /* The AI pack (docs/design/17), 20 and up so a chrome can route by kind alone. */
  MV_ADDON_EVENT_AI_STATUS = 20,     /* indexing progress / state; poll mv.ai.1 status */
  MV_ADDON_EVENT_AI_SEARCH_DONE = 21,/* id = search id; payload = result count */
  MV_ADDON_EVENT_AI_ROOTS = 22,      /* the remembered roots changed */
  MV_ADDON_EVENT_AI_COMPUTE = 23,    /* the compute self-test finished; payload = mv_ai_backend */
  MV_ADDON_EVENT_AI_PEOPLE = 24      /* the people clusters changed (PR 24) */
} mv_addon_event_kind;

/* ---- v2: pixels for the AI pack ------------------------------------------ */

/* Frames from a video, sampled for an index (docs/design/17 "Frame sampling"): a
 * decoder instance of its own, never the playback decoder. */
typedef struct mv_addon_sampler_options {
  uint32_t struct_size;
  uint32_t min_gap_ms;      /* drop keyframes closer than this to the last kept */
  uint32_t max_gap_ms;      /* decode forward to fill a longer keyframe gap */
  uint32_t max_long_edge;   /* frames are box-scaled to fit */
  int64_t start_ms;         /* resume: first frame at or after this time */
} mv_addon_sampler_options;

typedef struct mv_addon_video_info {
  int64_t duration_ms;
  uint32_t width;           /* display size, after rotation */
  uint32_t height;
  uint32_t hdr;             /* 1 = PQ / HLG, tone-mapped to SDR before the caller sees it */
  uint32_t reserved;
} mv_addon_video_info;

#define MV_ADDON_FRAME_KEYFRAME 1u  /* a real keyframe */
#define MV_ADDON_FRAME_GRID_FILL 2u /* decoded forward to honour max_gap_ms */
#define MV_ADDON_FRAME_END 4u       /* no more frames; no pixels written */

typedef struct mv_addon_sampled_frame {
  uint32_t width;
  uint32_t height;
  int64_t pts_ms;           /* from the start of the clip */
  int64_t pts_tb;           /* the same instant in the stream time base */
  int32_t tb_num;
  int32_t tb_den;
  uint32_t flags;           /* MV_ADDON_FRAME_* */
  uint32_t reserved;
} mv_addon_sampled_frame;

typedef struct mv_addon_file_entry {
  const char* path_utf8;      /* absolute */
  const char* relative_utf8;  /* from the walk root, '/'-separated */
  const char* name_utf8;
  uint64_t size;
  int64_t mtime_unix;
} mv_addon_file_entry;

/* Return non-zero to continue the walk, zero to stop it. */
typedef int32_t(MV_CALL* mv_addon_walk_fn)(void* user, const mv_addon_file_entry* entry);

/* walk_files2's entry (2026-10-05): the same file, and what it is. */
#define MV_ADDON_FILE_CLOUD_ONLY 0x1u /* its bytes are only in the cloud (a OneDrive online-only
                                       * file, an evicted iCloud Drive file): reading it downloads it */
typedef struct mv_addon_file_entry2 {
  uint32_t struct_size;
  uint32_t flags;             /* MV_ADDON_FILE_* */
  const char* path_utf8;      /* absolute */
  const char* relative_utf8;  /* from the walk root, '/'-separated */
  const char* name_utf8;
  uint64_t size;
  int64_t mtime_unix;
} mv_addon_file_entry2;

typedef int32_t(MV_CALL* mv_addon_walk2_fn)(void* user, const mv_addon_file_entry2* entry);

typedef struct mv_addon_volume {
  char volume_id[128];
  char root_utf8[1024];
  char label_utf8[256];
  char device_key[256];
  uint64_t total_bytes;
  uint64_t free_bytes;
  uint32_t removable;
  uint32_t network;
  uint32_t read_only;
  uint32_t reserved;
} mv_addon_volume;

/* What a file says about when and on what it was taken (the host's metadata
 * read; docs/design/06). has_date = 0 means the file carried no capture time and the
 * caller falls back to the file time, labelled as such (docs/design/18 "Date source"). */
typedef struct mv_addon_capture {
  int64_t taken_unix;       /* local wall-clock time as if it were UTC */
  uint32_t has_date;
  uint32_t reserved;
  char camera_utf8[128];    /* "Canon EOS R5"; empty when absent */
} mv_addon_capture;

#define MV_ADDON_COPY_MAX_TARGETS 4

typedef enum mv_addon_copy_outcome {
  MV_COPY_VERIFIED = 0,
  MV_COPY_WRITTEN_UNVERIFIED = 1,
  MV_COPY_NAME_TAKEN = 2,
  MV_COPY_WRITE_FAILED = 3,
  MV_COPY_VERIFY_FAILED = 4,
  MV_COPY_CANCELLED = 5
} mv_addon_copy_outcome;

/* One read of `source_utf8`, written and verified to every target
 * (io/verified_copy.h). Targets are final paths in existing directories and
 * are never overwritten. */
typedef struct mv_addon_copy_request {
  const char* source_utf8;
  const char* const* targets_utf8;
  uint32_t target_count;          /* 1 .. MV_ADDON_COPY_MAX_TARGETS */
  uint32_t read_back;             /* 1 = full uncached read-back; 0 = hash-on-read only */
  uint32_t retries;               /* docs/design/18: 1 */
  uint32_t reserved;
  /* Polled between buffers; non-zero cancels and leaves no temporary. */
  int32_t(MV_CALL* is_cancelled)(void* user);
  /* Bytes just read from the source (the ETA's input). May be NULL. */
  void(MV_CALL* on_progress)(void* user, uint64_t delta_bytes);
  /* Between buffers: blocks while the viewer needs the CPU / disk. May be NULL. */
  void(MV_CALL* yield)(void* user);
  void* user;
  /* Fault injection, honoured only by a host built with MV_ADDON_TEST_HOOKS
   * (tests and the PR 16 verify rig). fault_times = 0 disables it. */
  int32_t fault_target;
  uint32_t fault_times;
  uint64_t fault_offset;
} mv_addon_copy_request;

typedef struct mv_addon_copy_result {
  uint8_t source_hash[32];  /* BLAKE3-256 of the bytes read */
  uint64_t bytes;
  uint32_t source_unstable; /* the source read differently on the retry */
  uint32_t target_count;
  uint32_t outcome[MV_ADDON_COPY_MAX_TARGETS]; /* mv_addon_copy_outcome */
  uint32_t retried[MV_ADDON_COPY_MAX_TARGETS];
} mv_addon_copy_result;

typedef struct mv_addon_event {
  uint32_t kind;     /* mv_addon_event_kind */
  uint32_t status;   /* mv_status */
  uint64_t id;       /* scan / job id */
  int64_t payload;   /* kind-specific scalar. Never a pointer. */
} mv_addon_event;

typedef struct mv_host_api {
  uint32_t struct_size;
  uint32_t host_api;          /* MV_ADDON_HOST_API of the host */
  void* host;                 /* opaque; the first argument of every call */

  /* ---- io (worker threads only) ---------------------------------------- */
  mv_status(MV_CALL* walk_files)(void* host, const char* root_utf8, int32_t max_depth,
                                 mv_addon_walk_fn visit, void* user);
  mv_status(MV_CALL* stat_file)(void* host, const char* path_utf8, uint64_t* out_size,
                                int64_t* out_mtime_unix, uint32_t* out_is_directory);
  mv_status(MV_CALL* make_directories)(void* host, const char* dir_utf8);
  mv_status(MV_CALL* remove_file)(void* host, const char* path_utf8);
  /* uncached = 1 reads from the device (verify-a-folder, read-back). */
  mv_status(MV_CALL* hash_file)(void* host, const char* path_utf8, uint32_t uncached,
                                int32_t(MV_CALL* is_cancelled)(void* user),
                                void(MV_CALL* yield)(void* user), void* user,
                                uint8_t out_hash[32]);
  mv_status(MV_CALL* copy_verified)(void* host, const mv_addon_copy_request* request,
                                    mv_addon_copy_result* out_result);
  /* Creates `path_utf8` with `bytes` and flushes it; fails if anything is
   * already there (the add-on's own reports; never a user's file). */
  mv_status(MV_CALL* write_new_file)(void* host, const char* path_utf8, const void* bytes,
                                     uint64_t length);
  mv_status(MV_CALL* volume_of)(void* host, const char* path_utf8, mv_addon_volume* out);
  mv_status(MV_CALL* list_volumes)(void* host, mv_addon_volume* out, uint32_t capacity,
                                   uint32_t* out_count);
  mv_status(MV_CALL* eject_volume)(void* host, const char* root_utf8);
  /* Calls `on_volume` (on the host's watcher thread; must not block) when a
   * volume mounts (event 0) or goes away (event 1). NULL stops watching. One
   * watcher per add-on. */
  mv_status(MV_CALL* watch_volumes)(void* host,
                                    void(MV_CALL* on_volume)(void* user, uint32_t event,
                                                             const char* root_utf8),
                                    void* user);

  /* ---- the folder model, metadata, pairing, thumbnails ------------------ */
  mv_status(MV_CALL* capture_info)(void* host, const char* path_utf8, mv_addon_capture* out);
  /* The viewer's own pairing (io/pairing.h, PR 7): for each name, the index
   * of its partner (UINT32_MAX when unpaired) and the pair kind (0 none,
   * 1 RAW+JPEG, 2 Live Photo). Names are one directory's file names. */
  mv_status(MV_CALL* pair_names)(void* host, const char* const* names_utf8, uint32_t count,
                                 uint32_t* out_partner, uint32_t* out_kind);
  /* The viewer's JPEG-512 thumbnail for a file (cache hit or made now, on
   * the calling worker). Writes the thumbnail's path. */
  mv_status(MV_CALL* thumbnail_path)(void* host, const char* path_utf8, char* out_utf8,
                                     uint32_t capacity);

  /* ---- scheduling ------------------------------------------------------- */
  /* Non-zero while the present loop is busy (panning, playing, loading): a
   * background import waits between buffers (docs/design/18 "Priority"). */
  int32_t(MV_CALL* should_yield)(void* host);
  /* Posts to the host's completion queue; the host's chrome drains it. Any
   * thread; never blocks. */
  void(MV_CALL* post_event)(void* host, const mv_addon_event* event);
  uint64_t(MV_CALL* next_correlation_id)(void* host);

  /* ---- places ----------------------------------------------------------- */
  /* The add-on's per-user data folder (import.db lives here), created. */
  mv_status(MV_CALL* data_dir)(void* host, char* out_utf8, uint32_t capacity);
  /* The user's default photo folder: Pictures\MediaViewer / ~/Pictures/MediaViewer. */
  mv_status(MV_CALL* default_library_dir)(void* host, char* out_utf8, uint32_t capacity);

  /* Never a path, a file name, or a hash (rule 6). level: 0 info, 1 warn, 2 error. */
  void(MV_CALL* log)(void* host, int32_t level, const char* message_ascii);

  /* ---- v2 (Milestone H): pixels. Worker threads only; every call may read
   * and decode for tens of milliseconds. Pixels are 8-bit sRGB RGB, tightly
   * packed (stride = width * 3). A buffer too small returns
   * MV_ERR_INVALID_ARG with the size written, so the caller can grow it. -- */

  /* A still at first-pixel quality: the embedded RAW preview or a DCT-scaled
   * JPEG, never a full RAW develop (docs/design/17 step 6). Colour-managed to sRGB,
   * EXIF-oriented, long edge <= max_long_edge. */
  mv_status(MV_CALL* decode_still_rgb)(void* host, const char* path_utf8, uint32_t max_long_edge,
                                       uint8_t* out_rgb, uint64_t cap, uint32_t* out_width,
                                       uint32_t* out_height);
  /* The sampler: keyframes, min/max gaps, rotation, the SDR tone-map for
   * PQ/HLG (docs/design/17 steps 1, 2 and 4). `out_sampler` is closed with
   * sampler_close on the thread that uses it. */
  mv_status(MV_CALL* sampler_open)(void* host, const char* path_utf8,
                                   const mv_addon_sampler_options* options,
                                   mv_addon_video_info* out_info, void** out_sampler);
  /* The next sampled frame. MV_OK with MV_ADDON_FRAME_END at the end. */
  mv_status(MV_CALL* sampler_next)(void* host, void* sampler, uint8_t* out_rgb, uint64_t cap,
                                   mv_addon_sampled_frame* out_frame);
  void(MV_CALL* sampler_close)(void* host, void* sampler);
  /* The frame shown at `pts_ms` (decoded forward from the keyframe before
   * it), for "find similar" on a paused frame that was never sampled. */
  mv_status(MV_CALL* video_frame_rgb)(void* host, const char* path_utf8, int64_t pts_ms,
                                      uint32_t max_long_edge, uint8_t* out_rgb, uint64_t cap,
                                      uint32_t* out_width, uint32_t* out_height);
  /* A result tile for a moment inside a clip, in the viewer's own JPEG-512
   * cache (keyed by the file and the moment): `rgb` may be NULL to look one
   * up, else it is encoded and stored. Writes the JPEG's path. MV_ERR_IO on a
   * lookup miss. */
  mv_status(MV_CALL* moment_thumbnail)(void* host, const char* path_utf8, int64_t pts_ms,
                                       const uint8_t* rgb, uint32_t width, uint32_t height,
                                       char* out_utf8, uint32_t capacity);
  /* The version folder of an installed, verified piece of this add-on's
   * family ("ai-faces", "ai-cuda"; docs/design/17 per-piece Install/Remove).
   * MV_ERR_IO when the piece is not installed; MV_ERR_CORRUPT when it fails
   * verification (its files are never handed out). */
  mv_status(MV_CALL* piece_dir)(void* host, const char* piece_id, char* out_utf8,
                                uint32_t capacity);

  /* A clip's soundtrack for the audio index (sounds and speech, docs/design/17
   * "Audio", 2026-09-27): mono float PCM at `sample_rate`, from `start_ms`,
   * its own decoder (never the player's). MV_ERR_UNSUPPORTED_FORMAT: the file
   * has no audio. `out_duration_ms` may be NULL. */
  mv_status(MV_CALL* audio_open)(void* host, const char* path_utf8, uint32_t sample_rate,
                                 int64_t start_ms, int64_t* out_duration_ms, void** out_audio);
  /* Up to `max_samples` more samples into `out`; `*out_count` 0 is the end.
   * `*out_start_ms` is the first sample's time on the player's timeline. */
  mv_status(MV_CALL* audio_read)(void* host, void* audio, float* out, uint32_t max_samples,
                                 uint32_t* out_count, int64_t* out_start_ms);
  void(MV_CALL* audio_close)(void* host, void* audio);

  /* ---- sharing an index (2026-09-28, docs/design/17 "Sharing an index") ---------- */
  /* The JPEG-512 the viewer's cache already holds for a file (pts_ms < 0) or
   * for one of its moments, as bytes. Never makes one: MV_ERR_IO on a miss.
   * A short buffer returns MV_ERR_INVALID_ARG with *out_size set. */
  mv_status(MV_CALL* thumbnail_jpeg)(void* host, const char* path_utf8, int64_t pts_ms,
                                     uint8_t* out, uint64_t cap, uint64_t* out_size);
  /* Stores a JPEG-512 made on another machine under this machine's stamp of
   * the file (it must exist). The bytes are untrusted: the host decodes them
   * and refuses anything but a JPEG whose long edge is at most 512. */
  mv_status(MV_CALL* thumbnail_store_jpeg)(void* host, const char* path_utf8, int64_t pts_ms,
                                           const uint8_t* jpeg, uint64_t size);

  /* ---- the Recycle Bin / Trash (2026-10-03, PR 54 find duplicates) -------- */
  /* [worker-thread] Moves one file to the Recycle Bin (Windows) or the Trash
   * (macOS). Never a permanent delete: where the location has no bin (a
   * network share, some removable drives, the bin turned off) nothing is
   * removed and *out_refused is set to 1. Appended, so an add-on reads it
   * only when `struct_size` covers it and the pointer is non-NULL. */
  mv_status(MV_CALL* recycle_file)(void* host, const char* path_utf8, uint32_t* out_refused);

  /* ---- cloud files (2026-10-05, docs/plans/document-search.md slice 0) ------- */
  /* [worker-thread] walk_files, with the files only a cloud provider has
   * (OneDrive online-only, evicted iCloud Drive) listed too and flagged
   * MV_ADDON_FILE_CLOUD_ONLY. walk_files leaves those out: everything it lists
   * can be read without the network. Appended: read it only when
   * `struct_size` covers it and the pointer is non-NULL. */
  mv_status(MV_CALL* walk_files2)(void* host, const char* root_utf8, int32_t max_depth,
                                  mv_addon_walk2_fn visit, void* user);
} mv_host_api;

typedef struct mv_addon_api {
  uint32_t struct_size;
  uint32_t reserved;
  const char* id;        /* "import"; static storage in the add-on */
  const char* version;   /* "1.0.0" */
  void* addon;           /* opaque */
  /* Stops the add-on's threads and closes its files. The library is unloaded
   * only after this returns. [ui-thread] */
  void(MV_CALL* shutdown)(void* addon);
  /* A named interface ("mv.import.1"), or NULL. The pointer lives until
   * shutdown. */
  const void*(MV_CALL* query)(void* addon, const char* interface_id);
} mv_addon_api;

/* The one export. Returns MV_ERR_UNSUPPORTED_FORMAT when `host_api` is
 * outside the add-on's supported range (the host then reports "needs an
 * update" rather than loading it). */
typedef mv_status(MV_CALL* mv_addon_get_fn)(uint32_t host_api, const mv_host_api* host,
                                            mv_addon_api* out);
#define MV_ADDON_ENTRY_SYMBOL "mv_addon_get"

/* ---------------------------------------------------------------------------
 * Host-side management, exported by the Windows core DLL for the C# chrome
 * (the Mac host calls src/addon directly). [worker-thread] unless noted:
 * these read and hash files. JSON out follows mediaviewer_import.h's buffer
 * rule (cap / needed, MV_ERR_INVALID_ARG when short).
 *
 * Add-on events arrive on the session's completion queue as
 * mv_completion{kind = MV_COMPLETION_ADDON, job_id = event id,
 * generation = mv_addon_event_kind, status, payload}.
 * ------------------------------------------------------------------------- */

/* [{"id","name","version","dir","size","state":"ok|needs_update|invalid",
 *   "why","loaded","description","hint_on","hint_text","commands":[...]}] for
 * every add-on folder present (the last four from docs/design/25's contributions;
 * empty for an older manifest). Empty array when none. */
MV_API mv_status MV_CALL mv_addon_installed_json(char* out, uint32_t cap, uint32_t* needed);

/* Checks a downloaded manifest and its signature against the pinned key
 * before anything else is fetched: {"ok","why","id","name","version",
 * "installed_size","description","hint_on","hint_text",
 * "archive":{"path","sha256","size"}}. [any-thread] */
MV_API mv_status MV_CALL mv_addon_check_manifest(const void* manifest, uint32_t manifest_len,
                                                 const void* signature, uint32_t signature_len,
                                                 char* out, uint32_t cap, uint32_t* needed);

/* SHA-256 of a file, lowercase hex (the archive, before it is opened). */
MV_API mv_status MV_CALL mv_addon_sha256_file(const char* path_utf8, char out[65]);

/* A fresh staging folder under the add-ons folder: extract there, put
 * manifest.json and manifest.json.sig beside the files, then install. */
MV_API mv_status MV_CALL mv_addon_make_staging(char* out_utf8, uint32_t cap);

/* Verifies the staged folder (signature, every file, nothing extra) and moves
 * it into place. The staging folder is consumed either way. */
MV_API mv_status MV_CALL mv_addon_install(const char* staged_dir_utf8);

/* Milestone H: a family's installed bytes and its ceiling (0 = none), for
 * "Install local search, downloads ~N GB, uses ~N GB" and the 3 GB rule
 * (docs/design/17): the chrome refuses before downloading a piece that would not
 * fit, and mv_addon_install refuses it again (MV_ERR_UNSUPPORTED_FORMAT).
 * `family` is the parent's id ("ai"). [worker-thread] */
MV_API mv_status MV_CALL mv_addon_family_usage(const char* family, uint64_t* out_used,
                                               uint64_t* out_ceiling);

/* Unloads if loaded, then removes every version; keep_data = 0 also deletes
 * the add-on's data (import.db). Locked files are removed at next start. */
MV_API mv_status MV_CALL mv_addon_remove(const char* id, uint32_t keep_data);

/* Re-verifies, loads the native library, and returns the named interface
 * ("mv.import.1") plus the absolute path of the add-on's chrome entry (for
 * Windows, the chrome assembly). Events post to `session`'s queue. A second
 * load of a loaded add-on returns the same interface. MV_ERR_UNSUPPORTED_FORMAT:
 * the add-on needs an update. MV_ERR_CORRUPT: it failed verification. */
MV_API mv_status MV_CALL mv_addon_load(mv_session_t session, const char* id,
                                       const char* interface_id, const void** out_interface,
                                       char* out_chrome_utf8, uint32_t chrome_cap);

/* docs/design/25 (2026-10-03): the commands the LOADED add-ons' manifests
 * contribute, for the host's command table:
 *   [{"addon","id","name","windows","mac","modes","payload"}]
 * "[]" when none is loaded or none declares any. [any-thread] (no file is
 * read: the manifests were verified at load). */
MV_API mv_status MV_CALL mv_addon_commands_json(char* out, uint32_t cap, uint32_t* needed);

/* Shuts the add-on down (its jobs stop, resumable) and unloads it. [ui-thread] */
MV_API mv_status MV_CALL mv_addon_unload(const char* id);

/* Quit (not Remove), once, after the chrome has gone: every loaded add-on is
 * taken out of the loaded set and nothing is unloaded. Import's stop (its jobs
 * cancel and clean their temporaries) starts on a thread of its own; the AI
 * family is not stopped at all, since a chrome read may still be inside it.
 * Returns at once. [ui-thread][no-block] */
MV_API mv_status MV_CALL mv_addon_quit(void);

/* After mv_addon_quit: waits up to `timeout_ms` for the stops it started.
 * MV_OK when no add-on code can run any more; MV_ERR_TIMEOUT when some still
 * may (a model load, an inference batch, a copy step), and the host must then
 * end the process without running static destructors or DLL detach
 * (TerminateProcess), which that code may still be using. Its data is safe to
 * lose mid-step: SQLite WAL transactions and temporaries renamed into place.
 * [any-thread] */
MV_API mv_status MV_CALL mv_addon_quit_wait(uint32_t timeout_ms);

/* ---------------------------------------------------------------------------
 * Open add-ons (docs/design/25): add-ons from other makers, one `.mvaddon` file
 * each, signed by its publisher. Data only under contribution API 1 (themes):
 * nothing here loads code. They live beside the add-ons above, under their
 * own folder, and neither set of calls sees the other's.
 *
 * The core downloads nothing: the chrome fetches a link into a temporary file
 * and hands its path here, exactly as it hands over a file the user picked.
 * [worker-thread]: every call reads and hashes files. JSON out follows the
 * buffer rule above (cap / needed, MV_ERR_INVALID_ARG when short).
 * ------------------------------------------------------------------------- */

/* The contribution API this host serves (an add-on's manifest declares the
 * range it was written for).  1  themes. */
#define MV_ADDON_CONTRIBUTION_API 1

/* What a package is, for the consent sheet; installs and creates nothing.
 * MV_OK whenever the JSON was written, whatever it says:
 *   {"ok","why","detail","message","sha256","relation":"fresh|update|repair|
 *    downgrade|other_publisher","installed_version","adds","can":[...],
 *    "cannot","id","name","version",
 *    "description","licence","size","update_url",
 *    "publisher":{"name","url","key","fingerprint"},"api":{"min","max"},
 *    "themes":[{"id","name"}]}
 * The add-on's fields are present whenever its publisher's signature held,
 * so a refusal can still say whose add-on it was. */
MV_API mv_status MV_CALL mv_open_addon_inspect(const char* package_utf8, char* out, uint32_t cap,
                                               uint32_t* needed);

/* Installs the package the user agreed to. `approved_sha256` is the "sha256"
 * mv_open_addon_inspect returned for the sheet that was shown; a file that
 * has changed since is refused ("changed"). {"ok","why","message","id",
 * "version"}. Call it ONCE, with `cap` of 1024 or more (less is
 * MV_ERR_INVALID_ARG and nothing is installed): the ask-for-the-size-first
 * pattern would install twice. */
MV_API mv_status MV_CALL mv_open_addon_install(const char* package_utf8,
                                               const char* approved_sha256, char* out,
                                               uint32_t cap, uint32_t* needed);

/* [{"folder","id","name","version","state":"ok|needs_update|invalid","why",
 *   "description","licence","size","update_url","publisher":{...},
 *   "themes":[{"id","name"}]}], re-verified. "[]" when none are installed;
 * the folder is not created. */
MV_API mv_status MV_CALL mv_open_addon_list_json(char* out, uint32_t cap, uint32_t* needed);

/* Removes every version of the add-on in `folder` (the list's "folder"). */
MV_API mv_status MV_CALL mv_open_addon_remove(const char* folder);

/* A theme of an installed, verified add-on, as both chromes read it:
 *   {"addon","id","name","theme":{"font","dark":{"canvas":"#rrggbbaa",...}
 *    |null,"light":{...}|null}}
 * MV_ERR_NOT_FOUND: no such add-on or theme. MV_ERR_CORRUPT: the add-on no
 * longer verifies, and the chrome returns to its default. */
MV_API mv_status MV_CALL mv_open_addon_theme_json(const char* addon_id, const char* theme_id,
                                                  char* out, uint32_t cap, uint32_t* needed);

/* The base app's own card watch, for the one-time "Install Import?" hint
 * when a card appears and Import is not installed (docs/design/18). Posts
 * MV_ADDON_EVENT_VOLUME_ARRIVED (payload 1 for removable media, 0 otherwise)
 * to `session`. enable = 0 stops it. Reads nothing on the card. */
MV_API mv_status MV_CALL mv_volume_watch(mv_session_t session, uint32_t enable);

/* The render loop says whether it is presenting frames (panning, zooming,
 * playing, loading). Background add-on work waits between buffers while it
 * is (docs/design/18 "Priority"). [any-thread][no-block] */
MV_API void MV_CALL mv_present_set_busy(uint32_t busy);

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_ADDON_H */
