/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MediaViewer add-ons — the host function table and the add-on entry point
 * (plan/18-import.md "What an add-on is, technically"; plan/14-abi.md rules).
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
 * and bump this; `struct_size` lets an older add-on read a newer table. */
#define MV_ADDON_HOST_API 1

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
  MV_ADDON_EVENT_VERIFY_DONE = 7     /* verify-a-folder finished */
} mv_addon_event_kind;

typedef struct mv_addon_file_entry {
  const char* path_utf8;      /* absolute */
  const char* relative_utf8;  /* from the walk root, '/'-separated */
  const char* name_utf8;
  uint64_t size;
  int64_t mtime_unix;
} mv_addon_file_entry;

/* Return non-zero to continue the walk, zero to stop it. */
typedef int32_t(MV_CALL* mv_addon_walk_fn)(void* user, const mv_addon_file_entry* entry);

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
 * read; plan/06). has_date = 0 means the file carried no capture time and the
 * caller falls back to the file time, labelled as such (plan/18 "Date source"). */
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
  uint32_t retries;               /* plan/18: 1 */
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
   * background import waits between buffers (plan/18 "Priority"). */
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

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_ADDON_H */
