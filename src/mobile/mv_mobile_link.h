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
  MV_LINK_NO_CATALOGUE = 21,  /* (20 is MV_LINK_ACCESS_ENDED, #667) sync: a named folder's library.json names this library, every folder answered,
                                 and none holds a current.json: open it in file mode (see below) */
  MV_LINK_UNSUPPORTED = 22,   /* convert: this service converts nothing, or this file is not a clip */
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

/* Where the service is (docs/design/28-driftwire.md, "Addresses after pairing"): the addresses the link was opened with,
 * each replaced by the one the latest connect's caps gave ("host:port"; empty for none). Keep them
 * with the pairing and pass them to mv_link_open next time, so a DuckDNS name, a relay or a new LAN
 * address set after pairing is dialled without pairing again. Never blocks. 0, or -1 (nothing:
 * the sample library, or no link). */
typedef struct mv_link_addresses {
  char local[256];
  char public_name[256];
  char relay[256];
} mv_link_addresses;
int32_t mv_link_addresses_now(mv_mobile_link* link, mv_link_addresses* out);

/* The server's combined drives (docs/plans/self-host-drives.md 4), as its caps said at connect:
 * 0 healthy or none, 1 rebuilding, 2 degraded (a mirror lost a drive), 3 offline, 4 a drive failing;
 * -1 when there is no live connection (keep what you had). Never blocks. */
int32_t mv_link_storage(mv_mobile_link* link);
/* The sentence the phone shows for a code above ("Your server's mirror lost a drive. ..."), UTF-8,
 * static; "" for 0 or an unknown code. */
const char* mv_link_storage_words(int32_t code);

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

/* ---- file mode (docs/plans/phone-in-the-app.md 4, landing with #668) ---------
 * A library with no catalogue yet is the NAS's named folders. Paths are a service
 * root id plus a '/'-separated path under it ("" for its top). Never logged. */

/* `offline` 1: the folder's drive is not connected to the service now (an external drive unplugged,
 * a share not mounted; docs/plans/self-hosted.md §6). Listed so the phone can say so; every
 * request on it fails until it is back. */
typedef void (*mv_root_fn)(void* ctx, const char* id, const char* label, int32_t offline);
/* BLOCKS. The named folders, without the one that holds the catalogue (`.mediaviewer`). An offline
 * folder is listed as it is, with no look inside it. */
int32_t mv_files_roots(mv_mobile_link* link, mv_root_fn fn, void* ctx);
/* BLOCKS. Only the offline folders (`offline` is always 1), with no look inside any folder: what a
 * library with a catalogue shows beside its own folders. The sample library has one, "Travel drive". */
int32_t mv_files_offline_roots(mv_mobile_link* link, mv_root_fn fn, void* ctx);

typedef struct mv_file_entry {
  const char* name;
  uint64_t size;
  int64_t mtime;
  int32_t dir;  /* 1: a folder */
  int32_t kind; /* files: 0 photo, 1 RAW, 2 clip (the D5 table), 3 any other file; -1 for a folder */
} mv_file_entry;
typedef void (*mv_file_entry_fn)(void* ctx, const mv_file_entry* entry);
/* BLOCKS. One folder: its folders and its files, hidden names and companion files (XMP, THM, LRV) left out, in the
 * service's order. MV_LINK_NOT_FOUND, MV_LINK_DENIED, MV_LINK_UNREACHABLE. */
int32_t mv_files_list(mv_mobile_link* link, const char* root, const char* rel, mv_file_entry_fn fn, void* ctx);
/* BLOCKS. Where a catalogue folder (its key, "/<root uuid>/<path>", from mv_mobile_library_folders)
 * is on the service: the folder id and the path under it, for mv_files_list. So a library with a
 * catalogue can show a folder's other files (PDFs, saves), which the catalogue does not hold.
 * MV_LINK_NOT_FOUND (no catalogue, or not one of its roots), MV_LINK_NO_FOLDER, MV_LINK_UNREACHABLE;
 * MV_LINK_INVALID also when either buffer is too small. */
int32_t mv_files_folder_place(mv_mobile_link* link, const char* folder_key, char* root_out, size_t root_size,
                              char* rel_out, size_t rel_size);
/* BLOCKS. A range of one file, as mv_file_read. */
int32_t mv_files_read(mv_mobile_link* link, const char* root, const char* rel, uint64_t at, uint64_t length, int32_t bulk,
                      mv_cancel* cancel, mv_sink_fn sink, void* ctx);

/* ---- changing the shared folders (file mode; serve_client.h "changing the shared folders") ----
 * Add, New folder, Move, Rename and Delete, for a device the service lets change files: shown only
 * when mv_files_can_change says the service offers them, and in a folder only when its root is
 * writable for this device (mv_files_writable_roots, asked again after a role change). Paths as
 * mv_files_list; `rel` is never "" (not the folder itself). Each BLOCKS until the service answers
 * (a move across drives copies, which can take minutes). Never logged.
 *
 * A refusal returns MV_LINK_DENIED (or MV_LINK_NOT_FOUND for not_found and offline) with `why`: */
typedef enum mv_change_why {
  MV_CHANGE_NONE = 0,
  MV_CHANGE_UNSUPPORTED = 1,  /* the service is older than these (also MV_LINK_UNSUPPORTED) */
  MV_CHANGE_NOT_ALLOWED = 2,  /* this device may not change files there now */
  MV_CHANGE_PATH = 3,         /* the folder itself, a hidden or unsafe name, a folder into itself */
  MV_CHANGE_EXISTS = 4,       /* the destination is taken; nothing was replaced */
  MV_CHANGE_BUSY = 5,         /* another request is on that name now */
  MV_CHANGE_NAME = 6,         /* the name is too long */
  MV_CHANGE_LIMIT = 7,        /* past a backup folder's storage limit */
  MV_CHANGE_FULL = 8,         /* the server's disk is full */
  MV_CHANGE_OFFLINE = 9,      /* the folder's drive is not connected */
  MV_CHANGE_NOT_FOUND = 10,   /* not there any more (moved or deleted elsewhere) */
  MV_CHANGE_HASH = 11,        /* add: what arrived was not what was sent, again */
  MV_CHANGE_OFFSET = 12,      /* (resumed inside mv_files_add; never returned) */
  MV_CHANGE_FAILED = 13,      /* the server's disk failed it */
  MV_CHANGE_LOCAL = 14,       /* add: the file on the phone could not be read (MV_LINK_INVALID) */
} mv_change_why;
typedef struct mv_change_result {
  int32_t why;       /* mv_change_why */
  int32_t existing;  /* mkdir: the folder was there; add: this very file was there */
  /* Where it is now, under its folder: the new folder, the file (a taken name gets " (2)"), the
   * destination, or its place in the folder's trash on the server ("YYYY-MM-DD/<rel>"). "" when
   * it is too long for this buffer (the change still worked). */
  char rel[4096];
} mv_change_result;
/* BLOCKS. 1 in *out when the service offers the changes (0 from an older one, and the sample library). */
int32_t mv_files_can_change(mv_mobile_link* link, int32_t* out);
typedef void (*mv_writable_fn)(void* ctx, const char* id, int32_t writable);
/* BLOCKS. Every named folder (the catalogue's own too), and whether this device may change files in it now. */
int32_t mv_files_writable_roots(mv_mobile_link* link, mv_writable_fn fn, void* ctx);
typedef void (*mv_change_progress_fn)(void* ctx, uint64_t done_bytes, uint64_t total_bytes);
/* BLOCKS. Sends the phone's file at `local_path` to `rel` in `root` (its folders made). Never replaces.
 * Carries on from this device's unfinished copy after a cancel or a dropped link: call it again. */
int32_t mv_files_add(mv_mobile_link* link, const char* root, const char* rel, const char* local_path, mv_cancel* cancel,
                     mv_change_progress_fn progress, void* ctx, mv_change_result* out);
/* BLOCKS. Makes the folder `rel` (and any missing above it). */
int32_t mv_files_mkdir(mv_mobile_link* link, const char* root, const char* rel, mv_change_result* out);
/* BLOCKS. Moves or renames a file or a folder to `to_rel` in `to_root` (this root or another writable one). */
int32_t mv_files_move(mv_mobile_link* link, const char* root, const char* rel, const char* to_root, const char* to_rel,
                      mv_change_result* out);
/* BLOCKS. Deletes a file or a folder: it goes to the folder's trash on the server, where its owner can restore it. */
int32_t mv_files_delete(mv_mobile_link* link, const char* root, const char* rel, mv_change_result* out);

/* ---- clips this phone can't play (docs/plans/phone-clip-convert.md) ------------------------
 * The service converts them on its graphics hardware: H.264 and AAC in MPEG-TS segments of
 * `segment_ms` of the clip each (the last runs to the end), for an HLS playlist. */
typedef struct mv_convert_plan {
  int64_t duration_ms;
  uint32_t segment_ms;
  uint32_t segments;
  uint32_t width, height; /* the converted picture's */
} mv_convert_plan;
/* BLOCKS. `out` 1 when the service converts, 0 when not (an older one, turned off, the sample). */
int32_t mv_link_can_convert(mv_mobile_link* link, int32_t* out);
/* BLOCKS. A clip's plan: MV_LINK_UNSUPPORTED when not offered or not a clip. */
int32_t mv_files_convert_plan(mv_mobile_link* link, const char* root, const char* rel, mv_convert_plan* out);
int32_t mv_asset_convert_plan(mv_mobile_link* link, int64_t asset, mv_convert_plan* out);
/* BLOCKS. One segment's bytes, in order; the first wait for the server's encoder (up to a minute). */
int32_t mv_files_convert_read(mv_mobile_link* link, const char* root, const char* rel, uint32_t segment, mv_cancel* cancel,
                              mv_sink_fn sink, void* ctx);
int32_t mv_asset_convert_read(mv_mobile_link* link, int64_t asset, uint32_t segment, mv_cancel* cancel, mv_sink_fn sink,
                              void* ctx);

/* File search: the desktop's fields, fold and match (docs/design/16 "File search", 06 "Search text").
 * Nothing here blocks. */
typedef struct mv_file_tag {
  const char* raw_tag; /* as Exiv2 names it: "Exif.Image.Software", "Iptc.Application2.Keywords", "Xmp.dc.subject" */
  const char* value;   /* a list as "a, b" */
} mv_file_tag;
typedef void (*mv_text_fn)(void* ctx, const char* text);
/* A file's search fields in the desktop's why-order, each handed to `fn`. Any string may be NULL. */
int32_t mv_files_fields(const char* make, const char* model, const char* lens, const char* comment, const mv_file_tag* tags,
                        size_t count, mv_text_fn fn, void* ctx);

/* The folder on screen. Thread-safe. */
typedef struct mv_file_index mv_file_index;
mv_file_index* mv_file_index_new(void);
void mv_file_index_free(mv_file_index* index);
/* A new listing; every file's fields are forgotten. */
int32_t mv_file_index_names(mv_file_index* index, const char* const* names, size_t count);
int32_t mv_file_index_fields(mv_file_index* index, size_t i, const char* const* fields, size_t count);
/* `why` is NULL for a name match, else the field the first word matched in ("Canon EOS R5"). Names
 * first, in listing order, then tags. */
typedef void (*mv_file_hit_fn)(void* ctx, size_t index, const char* why);
int32_t mv_file_index_match(const mv_file_index* index, const char* typed, mv_file_hit_fn fn, void* ctx);
/* How many files have their fields, of how many. */
int32_t mv_file_index_counts(const mv_file_index* index, size_t* tagged, size_t* count);

/* ---- phone backup, release 1.1 (docs/plans/mobile.md 10b) ---------------------
 * The phone's photos and videos to a backup folder on the NAS, while the app is open.
 * The host lists Photos (newest first) with mv_backup_add_item and exports one
 * resource at a time when asked; the core keeps the ledger, files by date, and
 * writes through the service. With the sample library the "NAS" is a folder on
 * this phone, labelled "Sample server on this iPhone": nothing is sent anywhere. */

typedef struct mv_backup mv_backup;

typedef struct mv_backup_config {
  const char* ledger_path;   /* Application Support, excluded from iCloud backup */
  const char* scratch_dir;   /* one exported resource at a time */
  const char* library_id;
  const char* sample_folder; /* the sample library only: its on-phone "NAS" folder */
} mv_backup_config;

/* Holds a reference to `link`: close the backup before the link. */
mv_backup* mv_backup_open(mv_mobile_link* link, const mv_backup_config* config);
/* Waits for a run in flight to return (fire its cancel first). */
void mv_backup_close(mv_backup* b);

/* A backup folder the service offers. writable 0: not one this phone may write to. */
typedef void (*mv_backup_folder_fn)(void* ctx, const char* id, const char* label, int32_t writable);
/* BLOCKS. MV_LINK_TOO_OLD: the service is from before 1.1. */
int32_t mv_backup_folders(mv_backup* b, mv_backup_folder_fn fn, void* ctx);

typedef struct mv_backup_space {
  uint64_t limit; /* the folder's storage limit */
  uint64_t used;
  uint64_t room;  /* what may still be written: the limit or the NAS's free space, whichever is less */
} mv_backup_space;
/* BLOCKS. */
int32_t mv_backup_space_of(mv_backup* b, const char* folder_id, mv_backup_space* out);

/* The photos to back up, newest first. MV_LINK_INVALID for no resources, a name that is a path, or a
 * year outside 1900-9999. resources_json: [{"kind":"photo|video|live_video|raw|other","name":"IMG_0412.HEIC"}].
 * key: PHCloudIdentifier where iCloud Photos gives one, else "local:" + the local identifier.
 * year, month, day: the capture date where it was taken. */
int32_t mv_backup_clear_items(mv_backup* b);
int32_t mv_backup_add_item(mv_backup* b, const char* key, const char* local_id, int32_t year, int32_t month, int32_t day,
                           const char* resources_json);

/* BLOCKS (the core's run thread calls it). Writes resource `index` of the photo `local_id`, the
 * original as shot (from iCloud first if need be), to `path`. MV_LINK_OK, MV_LINK_NOT_FOUND (gone from
 * Photos), MV_LINK_CANCELLED, or MV_LINK_INTERNAL. Must poll `cancel` (mv_backup_cancelled).
 * ymd[3] holds the item's year, month and day; set it to the capture date read from the file, where
 * it was taken, when the file says (it then files the rest of the photo too). */
typedef int32_t (*mv_backup_export_fn)(void* ctx, const char* local_id, int32_t index, const char* path, mv_cancel* cancel,
                                       int32_t ymd[3]);
int32_t mv_backup_cancelled(mv_cancel* cancel);

typedef enum mv_backup_state {
  MV_BACKUP_IDLE = 0,
  MV_BACKUP_CHECKING = 1,
  MV_BACKUP_RUNNING = 2,
  MV_BACKUP_DONE = 3,
  MV_BACKUP_PAUSED_WIFI = 4,
  MV_BACKUP_NOT_ALLOWED = 5, /* the folder is not one this phone may write to */
  MV_BACKUP_LIMIT = 6,       /* the folder's storage limit is reached */
  MV_BACKUP_FULL = 7,        /* the NAS is full */
  MV_BACKUP_UNREACHABLE = 8,
  MV_BACKUP_TOO_OLD = 9,
  MV_BACKUP_NO_FOLDER = 10,
  MV_BACKUP_NO_ACCESS = 11,
  MV_BACKUP_DONE_FAILURES = 12, /* everything it could copy is backed up; mv_backup_failures says what not */
} mv_backup_state;

typedef struct mv_backup_status {
  int32_t state; /* mv_backup_state */
  uint64_t total;
  uint64_t backed_up;
  uint64_t files_done;
  uint64_t copied;          /* files this run */
  uint64_t bytes;           /* sent this run */
  double current_fraction;
  char current[256];        /* the file being copied: this phone's own screen only */
  uint64_t failed;
} mv_backup_status;
typedef void (*mv_backup_status_fn)(void* ctx, const mv_backup_status* status);

/* BLOCKS, until done, a state that stops it, or `cancel`. layout: 1 year, 2 year/month/day,
 * 4 one folder, 5 year/month (the default). MV_LINK_OK with the last mv_backup_state in *state_out,
 * or a mv_link_status for a bad argument. */
int32_t mv_backup_run(mv_backup* b, const char* folder_id, int32_t layout, mv_cancel* cancel,
                      mv_backup_export_fn export_fn, mv_backup_status_fn status_fn, void* ctx, int32_t* state_out);

/* The last run's failures: the photo's key, the file, and why ("gone", "export", "hash", "error"). */
typedef void (*mv_backup_failure_fn)(void* ctx, const char* key, const char* name, const char* why);
int32_t mv_backup_failures(mv_backup* b, mv_backup_failure_fn fn, void* ctx);
/* Keys of the photos not backed up yet, as of the last run. */
typedef void (*mv_backup_key_fn)(void* ctx, const char* key);
int32_t mv_backup_remaining(mv_backup* b, mv_backup_key_fn fn, void* ctx);

/* Forget: removes the sample library's on-phone "NAS" and a ledger. Nothing on a real NAS is touched. */
void mv_backup_remove_local(const char* sample_folder, const char* ledger_path);

#ifdef __cplusplus
}
#endif

#endif /* MV_MOBILE_LINK_H */
