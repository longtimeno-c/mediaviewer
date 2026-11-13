/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The C ABI between the iOS host and the link core (docs/plans/mobile.md §7, §8):
 * pairing, the routes, catalogue sync, thumbnails and range reads. Opaque
 * handles, POD structs, a status from every call. Calls marked BLOCKS wait on the
 * network and are for worker threads only; nothing else in this header waits.
 * Callbacks run on the calling thread or a core worker, never the main thread
 * unless the caller was on it. Thread-safe unless noted.
 */
#ifndef MV_MOBILE_LINK_H
#define MV_MOBILE_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mv_link_status {
  MV_LINK_OK = 0,
  MV_LINK_INVALID = 1,        /* not a code of ours, or a bad argument */
  MV_LINK_EXPIRED = 2,        /* the code's time has passed */
  MV_LINK_UNREACHABLE = 3,    /* no route answered */
  MV_LINK_WRONG_SERVICE = 4,  /* answered with a key that is not the pinned one */
  MV_LINK_NOT_PAIRED = 5,     /* this device is not, or no longer, on the list */
  MV_LINK_BAD_CODE = 6,       /* the single-use code was wrong, spent or expired */
  MV_LINK_TOO_OLD = 7,        /* the service needs updating */
  MV_LINK_BUSY = 8,
  MV_LINK_NOT_FOUND = 9,      /* not on the NAS, or not in this catalogue */
  MV_LINK_DENIED = 10,        /* outside the named folders */
  MV_LINK_CANCELLED = 11,
  MV_LINK_REJECTED = 12,      /* a bad signature, library or hash: nothing was installed */
  MV_LINK_NO_SPACE = 13,
  MV_LINK_OLDER = 14,         /* the NAS's catalogue is older than this phone's: kept out */
  MV_LINK_UP_TO_DATE = 15,
  MV_LINK_COPY_AVAILABLE = 16,
  MV_LINK_INTERNAL = 17,
  MV_LINK_NO_FOLDER = 18,     /* the service has no folder named for this file's share */
  MV_LINK_WAITING = 19,       /* the copy stopped because bytes may not move now (mobile data) */
  MV_LINK_ACCESS_ENDED = 20,  /* this phone's time-limited access is over (not a removal: it may be extended) */
} mv_link_status;

/* Return 0 to stop the read. */
typedef int32_t (*mv_sink_fn)(void* ctx, uint64_t offset, const uint8_t* data, size_t size);

/* ---- pairing ------------------------------------------------------------- */

/* The scanned code, parsed and not yet trusted. */
typedef struct mv_pair_info {
  char library_id[128];
  char library_name[256];
  uint8_t publisher_key[32];
  char local[256];
  char public_name[256];
  uint8_t identity[32];
  uint8_t access_key[32];
  char code[256];
  int64_t expires;
  char fingerprint[32]; /* compare with the desktop's */
  int32_t sample;       /* 1: the sample library's code, or its guest code GUEST (no service; see below) */
  /* A guest code: the access it gives, in seconds from when it is redeemed (0: unlimited), and
   * whether it is to some folders only. Shown before the person confirms. */
  int64_t grant_for_s;
  int32_t grant_scoped;
  /* The person's own relay, "host:port", tried after the direct routes; empty for none (an older
   * code). Keep it with the pairing and pass it to mv_link_config.relay. */
  char relay[256];
} mv_pair_info;

typedef struct mv_route_failure {
  char address[256];
  int32_t problem; /* 0 unreachable, 1 wrong service, 2 not paired, 3 too old, 4 busy, 5 bad code, 6 access ended */
} mv_route_failure;

typedef struct mv_route_report {
  char address[256]; /* the one that answered; empty when none did */
  int32_t over_internet;
  int32_t failure_count;
  mv_route_failure failures[4];
  int32_t relayed;  /* 1: through the person's own relay (over_internet is 1 too) */
  int32_t upgraded; /* 1: moved from the relay to a direct path it punched (address is "direct") */
} mv_route_report;

typedef struct mv_cancel mv_cancel;
mv_cancel* mv_cancel_new(void);
void mv_cancel_fire(mv_cancel* c);
void mv_cancel_free(mv_cancel* c);

/* MV_LINK_EXPIRED when now_unix is past the code's expiry. The sample library's code ("SAMPLE", or
 * mediaviewer://sample) parses too, with sample = 1, its own library and publisher key, and no service. */
int32_t mv_pair_parse(const char* text, int64_t now_unix, mv_pair_info* out);

/* ---- the sample library (docs/plans/mobile.md 11.1) ---------------------------
 * A real signed catalogue in a static HTTPS folder (a GitHub release), opened by
 * mv_link_open with `static_base` and the host's HTTPS GET as `fetch`. Sync,
 * pictures and originals then run exactly as over the link. */

/* The folder every build opens the sample library from, ending in '/'. */
const char* mv_sample_base(void);

/* Polled while a fetch waits: 1 stop now. */
typedef int32_t (*mv_stopped_fn)(void* ctx);
/* The host's HTTPS GET. BLOCKS (a core worker calls it). Bytes [at, at + length) of `url` (length
 * UINT64_MAX: to the end) go to `sink` in order, offsets from 0 at `at`; a 0 from the sink stops the
 * read. Returns MV_LINK_OK, MV_LINK_NOT_FOUND (a 404), MV_LINK_CANCELLED (stopped or the sink said
 * stop) or MV_LINK_UNREACHABLE. Must poll `stopped` while it waits for the network. */
typedef int32_t (*mv_fetch_fn)(void* ctx, const char* url, uint64_t at, uint64_t length, mv_sink_fn sink, void* sink_ctx,
                               mv_stopped_fn stopped, void* stopped_ctx);

/* A new X25519 device key. */
int32_t mv_device_key_new(uint8_t private_key[32], uint8_t public_key[32]);

/* BLOCKS. Redeems the code on the first route that answers. The sample library's needs none: MV_LINK_OK. */
int32_t mv_pair_redeem(const mv_pair_info* info, const uint8_t device_key[32], const char* device_name,
                       mv_cancel* cancel, mv_route_report* report);

/* ---- the library's connection -------------------------------------------- */

typedef struct mv_mobile_link mv_mobile_link;

typedef struct mv_link_config {
  const char* library_dir; /* Application Support/library */
  const char* cache_dir;   /* Caches/thumbs */
  const char* library_id;
  const char* local;
  const char* public_name;
  const char* device_name;
  const uint8_t* identity;      /* 32 bytes: the pinned service key */
  const uint8_t* access_key;    /* 32 */
  const uint8_t* device_key;    /* 32: private */
  const uint8_t* publisher_key; /* 32: pinned at pairing */
  uint64_t thumb_cache_bytes;
  /* The sample library: non-NULL reads the static folder at this URL through `fetch`, in place of
   * the service; identity, access_key, device_key, local and public_name are then unused. */
  const char* static_base;
  mv_fetch_fn fetch;
  void* fetch_ctx; /* outlives the link */
  const char* relay; /* the person's own relay from the pairing, or NULL / "" for none */
} mv_link_config;

mv_mobile_link* mv_link_open(const mv_link_config* config);
void mv_link_close(mv_mobile_link* link);

/* The live connection's limit, as the service gave it: 1 when this phone's access is limited
 * (`expires_unix` its end on the service's clock, 0 none; `scoped` 1 for some folders only), 0 when
 * it is unlimited, -1 when there is no live connection (nothing is known: keep what you had).
 * Never blocks. */
int32_t mv_link_grant(mv_mobile_link* link, int64_t* expires_unix, int32_t* scoped);

/* A direct path after the relay (docs/plans/nas-relay.md 9): once the link is through the person's
 * own relay, the core tries to punch a direct one and moves to it. On Wi-Fi always; on mobile data
 * only when `allowed_on_cellular` (Settings, "Faster direct connection on mobile data", default on).
 * Call it at open and whenever the network or the setting changes. Never blocks. */
void mv_link_set_upgrade(mv_mobile_link* link, int32_t on_cellular, int32_t allowed_on_cellular);

/* BLOCKS. Keeps a live connection (the report says how it was reached); races the routes only when
 * there is none. Never cuts reads in flight. The sample library: its folder's host answered with a
 * file list signed by the pinned key; MV_LINK_REJECTED when it answered with one that is not. */
int32_t mv_link_connect(mv_mobile_link* link, mv_cancel* cancel, mv_route_report* report);
/* BLOCKS. The network changed: races again and swaps the winner in; reads in flight finish on the old one. */
int32_t mv_link_reconnect(mv_mobile_link* link, mv_cancel* cancel, mv_route_report* report);
/* "Forget this library": cuts every read and refuses every later connect. Returns once nothing the
 * core writes for this library (JPEG-512 tiles, the sample library's verified copies) is being
 * written, and none is written later, so the folders can be removed. Close it afterwards. */
void mv_link_shut_down(mv_mobile_link* link);
int32_t mv_link_is_connected(const mv_mobile_link* link);
void mv_link_disconnect(mv_mobile_link* link);

/* ---- catalogue sync ------------------------------------------------------ */

typedef struct mv_sync_report {
  int32_t outcome; /* mv_link_status: UP_TO_DATE, OLDER, COPY_AVAILABLE, OK (installed), or the failure */
  uint64_t epoch;
  uint64_t generation;
  uint64_t total_bytes;
  uint64_t done_bytes;
  uint64_t needed_bytes; /* with NO_SPACE */
} mv_sync_report;

typedef void (*mv_sync_progress_fn)(void* ctx, uint64_t done_bytes, uint64_t total_bytes);
/* Asked before each file and with each chunk: 1 bytes may move now, 0 they may not (MV_LINK_WAITING). */
typedef int32_t (*mv_sync_allowed_fn)(void* ctx);

/* BLOCKS. Reads current.json and, when newer, the manifest. Safe on any network. */
int32_t mv_sync_check(mv_mobile_link* link, mv_sync_report* report);
/* BLOCKS. Copies the newest generation with resume and swaps it in. One copy per library at a
 * time: a second waits, then finds it up to date. `allowed` may be NULL (always). */
int32_t mv_sync_copy(mv_mobile_link* link, mv_cancel* cancel, mv_sync_allowed_fn allowed, mv_sync_progress_fn progress,
                     void* ctx, mv_sync_report* report);

typedef struct mv_catalogue_info {
  int32_t present;
  uint64_t epoch;
  uint64_t generation;
  uint64_t created_unix;
  uint64_t asset_count;
  char dir[1024]; /* the installed generation's folder */
} mv_catalogue_info;
int32_t mv_catalogue_read(mv_mobile_link* link, mv_catalogue_info* out);

/* Removes the library's files; forget the keys separately. */
void mv_library_remove(const char* library_dir);

/* ---- the sound text tower (docs/plans/mobile.md 9, "Sound search"; sound_tower.h) -------
 * The catalogue carries every clip's sound vectors; the text half that puts a typed query in
 * their space is one pinned download, kept in <library_dir>/sound/ (so Forget removes it).
 * Every file is hashed against the pin in the app before it is kept. */

/* The release it comes from, ending in '/'. */
const char* mv_sound_base(void);
/* Its size in all, for "the size first" (8.4). */
uint64_t mv_sound_total_bytes(void);
/* 1 when it is here and verified. Reads a small file. */
int32_t mv_sound_ready(const char* library_dir);
/* Bytes it holds on the phone, a partial download included: part of the cache size shown. */
uint64_t mv_sound_bytes(const char* library_dir);
/* 1 when this generation's clips have sounds the pinned tower answers (one indexed probe). */
int32_t mv_sound_wanted(const char* generation_dir);
/* BLOCKS. Downloads what is missing through `fetch`, resuming, and verifies it. base_url NULL:
 * mv_sound_base(). `allowed` (NULL: always) is asked before each file and with each chunk.
 * Returns MV_LINK_OK (ready), MV_LINK_WAITING, MV_LINK_CANCELLED, MV_LINK_UNREACHABLE,
 * MV_LINK_NOT_FOUND (a pinned file is gone from the release), MV_LINK_REJECTED (a file was not
 * the pinned one and is deleted), MV_LINK_NO_SPACE or MV_LINK_INTERNAL. */
int32_t mv_sound_fetch(const char* library_dir, const char* base_url, mv_fetch_fn fetch, void* fetch_ctx, mv_cancel* cancel,
                       mv_sync_allowed_fn allowed, mv_sync_progress_fn progress, void* ctx);

/* ---- pictures and files, by assets.id ------------------------------------ */

typedef void (*mv_bytes_fn)(void* ctx, const uint8_t* data, size_t size);
typedef void (*mv_done_fn)(void* ctx, int32_t status, const uint8_t* data, size_t size);
typedef void (*mv_moment_fn)(void* ctx, int64_t start_ms, int32_t kind, const char* text);

/* Local and fast: the 64 px tile. MV_LINK_NOT_FOUND when none exists. */
int32_t mv_tiny_get(mv_mobile_link* link, int64_t asset, mv_bytes_fn fn, void* ctx);
/* Queues a JPEG-512. lane 0 on screen, 1 prefetch. *out_cancel is the caller's to free. */
int32_t mv_thumb_request(mv_mobile_link* link, int64_t asset, int32_t lane, mv_done_fn done, void* ctx,
                         mv_cancel** out_cancel);
uint64_t mv_thumb_bytes_used(const mv_mobile_link* link);

typedef struct mv_asset_info {
  uint64_t size;
  int32_t kind;
  int64_t duration_ms;
} mv_asset_info;
int32_t mv_asset_read(mv_mobile_link* link, int64_t asset, mv_asset_info* out);

/* BLOCKS. Range read of an original. length UINT64_MAX means to the end. */
int32_t mv_file_read(mv_mobile_link* link, int64_t asset, uint64_t at, uint64_t length, int32_t bulk, mv_cancel* cancel,
                     mv_sink_fn sink, void* ctx);

/* BLOCKS. Whether the service names the folder this asset is in: OK or MV_LINK_NO_FOLDER. */
int32_t mv_asset_folder_check(mv_mobile_link* link, int64_t asset);

int32_t mv_moments_get(mv_mobile_link* link, int64_t asset, mv_moment_fn fn, void* ctx);

/* Until the search session's library store lands: every photo and clip, newest first. */
typedef void (*mv_listed_asset_fn)(void* ctx, int64_t asset, int32_t kind, int64_t taken_at, int64_t duration_ms, const char* name);
int32_t mv_assets_list(mv_mobile_link* link, mv_listed_asset_fn fn, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* MV_MOBILE_LINK_H */
