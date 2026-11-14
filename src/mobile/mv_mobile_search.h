/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The phone's catalogue and search (docs/plans/mobile.md §5, §9): a flat C ABI for src.ios.
 * Opaque handles, POD, a status code from every call that can fail. Nothing throws across it.
 *
 * Threads: every call may block (SQLite, the disk, the model) unless it says [no-block], and so
 * never belongs on the main thread. A library is safe to use from several threads at once.
 *
 * No correlation id (docs/design/14's rule for the host ABI): the phone has no crash-report
 * pipeline to tie one to yet (docs/plans/mobile.md §12). Add one with that pipeline, as the
 * last parameter of every call that can fail. */
#ifndef MV_MOBILE_SEARCH_H
#define MV_MOBILE_SEARCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t mv_mobile_status;
#define MV_MOBILE_OK 0
#define MV_MOBILE_ERR_INVALID_ARG 1
#define MV_MOBILE_ERR_NOT_FOUND 2
#define MV_MOBILE_ERR_IO 3
/* A file is not what the manifest says (a truncated copy). */
#define MV_MOBILE_ERR_CORRUPT 4
/* This generation's format or schema is one this build cannot read. */
#define MV_MOBILE_ERR_UNSUPPORTED_FORMAT 5
/* mv_mobile_status_message gives docs/plans/mobile.md §11.3's sentence. */
#define MV_MOBILE_ERR_UNSUPPORTED_TOWER 6
#define MV_MOBILE_ERR_CANCELLED 7
#define MV_MOBILE_ERR_TIMEOUT 8
#define MV_MOBILE_ERR_INTERNAL 9

/* A sentence for a person, or for a log. Static storage. [no-block] */
const char* mv_mobile_status_message(mv_mobile_status status);

typedef struct mv_mobile_library mv_mobile_library;
typedef struct mv_mobile_list mv_mobile_list;
typedef struct mv_mobile_results mv_mobile_results;

/* One installed generation.
 * generation_dir  gen/000123, as the publisher wrote it (manifest.json, catalog.db, ...).
 * library_dir     the folder the manifest's shared paths are relative to ("models/<spec>/...").
 * work_dir        this generation's own folder for what the engine needs laid out: created if
 *                 missing, safe to delete, never inside generation_dir (that folder is signed).
 *
 * Every file of the generation must be the size the manifest says, or MV_MOBILE_ERR_CORRUPT. A
 * new generation is a new library: open it beside the old one, swap, close the old one. */
typedef struct mv_mobile_open_options {
  uint32_t struct_size;
  const char* generation_dir;
  const char* library_dir;
  const char* work_dir;
  /* The sound text tower's folder (sound_tower.h; mv_sound_fetch in mv_mobile_link.h), or NULL.
   * Loaded only when it is verified and the catalogue has clip sounds it answers; the engine then
   * works in work_dir + "+sound", so one opened without it keeps work_dir. */
  const char* sound_dir;
} mv_mobile_open_options;

mv_mobile_status mv_mobile_library_open(const mv_mobile_open_options* options, mv_mobile_library** out);
/* Waits for a search in progress. Lists and results taken from it stay valid after this. */
void mv_mobile_library_close(mv_mobile_library* library);

typedef struct mv_mobile_library_info {
  uint64_t epoch;       /* (epoch, generation) only goes up: compare both */
  uint64_t generation;
  int64_t created_unix;
  uint32_t assets;    /* photos and clips */
  uint32_t people;    /* named */
  uint32_t has_tiny;  /* 1 when tiny.pack is here */
  uint32_t has_sound; /* 1 when search hears clips: the sound tower was given and answers them */
  char active_spec[128];
} mv_mobile_library_info;
mv_mobile_status mv_mobile_library_info_get(const mv_mobile_library* library, mv_mobile_library_info* out);

/* ---- rows ------------------------------------------------------------------------------ */

#define MV_MOBILE_KIND_PHOTO 1u
#define MV_MOBILE_KIND_CLIP 2u

typedef struct mv_mobile_row {
  int64_t asset_id;
  /* The date taken as a wall clock read as UTC, else the file's modification time, as the query
   * language's dates are. */
  int64_t wall_seconds;
  int64_t duration_ms;
  const char* file_name;   /* UTF-8; valid as long as the library or list */
  uint32_t file_name_len;
  uint32_t kind;           /* MV_MOBILE_KIND_* */
  /* The picture's shape from its 64 px tile; 0 until mv_mobile_library_fill_aspects has run or
   * when the asset has no tile. The catalogue has no pixel size. */
  uint16_t tile_width;
  uint16_t tile_height;
} mv_mobile_row;

typedef struct mv_mobile_month {
  int32_t key;               /* yyyymm */
  int64_t wall_seconds;      /* the newest item's */
  uint32_t first;
  uint32_t count;
} mv_mobile_month;

#define MV_MOBILE_SCOPE_EVERYTHING 0u
#define MV_MOBILE_SCOPE_FOLDER 1u  /* scope_key: a folder's key; everything beneath it too */
#define MV_MOBILE_SCOPE_PERSON 2u  /* scope_key: a person's key */

/* Newest first. A folder scope includes every folder beneath it. */
mv_mobile_status mv_mobile_list_open(mv_mobile_library* library, uint32_t scope, const char* scope_key,
                                     mv_mobile_list** out);
void mv_mobile_list_close(mv_mobile_list* list);
/* [no-block] */
uint32_t mv_mobile_list_count(const mv_mobile_list* list);
uint32_t mv_mobile_list_month_count(const mv_mobile_list* list);
/* MV_MOBILE_ERR_INVALID_ARG for an index past the end. [no-block] */
mv_mobile_status mv_mobile_list_month_at(const mv_mobile_list* list, uint32_t index, mv_mobile_month* out);
mv_mobile_status mv_mobile_list_row_at(const mv_mobile_list* list, uint32_t index, mv_mobile_row* out);

/* [no-block] */
mv_mobile_status mv_mobile_library_row(const mv_mobile_library* library, int64_t asset_id, mv_mobile_row* out);
/* Reads the shape of each asset's tile once, for mv_mobile_row. Touches all of tiny.pack, so call
 * it at low priority after the first paint. */
mv_mobile_status mv_mobile_library_fill_aspects(mv_mobile_library* library);

/* ---- folders, people, details, filters -------------------------------------------------- */

typedef struct mv_mobile_folder {
  const char* key;
  const char* name;
  uint32_t item_count;     /* this folder and every folder beneath it */
  uint32_t has_subfolders;
} mv_mobile_folder;
typedef void (*mv_mobile_folder_fn)(void* user, const mv_mobile_folder* folder);
/* parent_key NULL: the library's top level. */
mv_mobile_status mv_mobile_library_folders(mv_mobile_library* library, const char* parent_key, mv_mobile_folder_fn visit,
                                           void* user);

typedef struct mv_mobile_person {
  const char* key;
  const char* name;
  uint32_t item_count;
  int64_t face_asset_id;  /* the item whose picture stands for this person */
} mv_mobile_person;
typedef void (*mv_mobile_person_fn)(void* user, const mv_mobile_person* person);
/* Named people, in name order. None when the generation has no faces.db. */
mv_mobile_status mv_mobile_library_people(mv_mobile_library* library, mv_mobile_person_fn visit, void* user);

typedef struct mv_mobile_details {
  char place[256];
  char camera[128];
  char lens[128];
  char folder[1024];  /* share/sub/.../ as the person knows it */
  char format[32];    /* from the file's name: "HEIC", "MP4" */
  int64_t file_bytes;
  int32_t iso;           /* 0: unknown, as the rest */
  int32_t aperture_x100;
  int32_t focal_mm;
  int32_t shutter_den;   /* 1/den seconds; 0: use shutter_us */
  int64_t shutter_us;
} mv_mobile_details;
mv_mobile_status mv_mobile_library_details(mv_mobile_library* library, int64_t asset_id, mv_mobile_details* out);

typedef void (*mv_mobile_string_fn)(void* user, const char* value);
typedef void (*mv_mobile_year_fn)(void* user, int32_t year);
/* What this library can be narrowed by, most used first; years newest first. */
mv_mobile_status mv_mobile_library_filter_options(mv_mobile_library* library, mv_mobile_string_fn visit_place,
                                                  mv_mobile_string_fn visit_camera, mv_mobile_year_fn visit_year,
                                                  void* user);

/* ---- tiles ----------------------------------------------------------------------------- */

/* The asset's 64 px JPEG from tiny.pack. *out_bytes points into the mapped file and stays valid
 * until the library is closed. MV_MOBILE_ERR_NOT_FOUND when it has none. */
mv_mobile_status mv_mobile_library_tile(const mv_mobile_library* library, int64_t asset_id, const uint8_t** out_bytes,
                                        uint32_t* out_length);

/* ---- search ---------------------------------------------------------------------------- */

#define MV_MOBILE_SEARCH_OFF 0u           /* not started */
#define MV_MOBILE_SEARCH_WARMING 1u       /* the model is loading */
#define MV_MOBILE_SEARCH_READY 2u
#define MV_MOBILE_SEARCH_UNSUPPORTED 3u   /* the library's tower is one a phone cannot run */
#define MV_MOBILE_SEARCH_FAILED 4u        /* the model would not load */

/* Starts the engine: the catalogue is read now, the model loads on its own thread. Returns
 * MV_MOBILE_ERR_UNSUPPORTED_TOWER for a tower a phone cannot run. Idempotent. */
mv_mobile_status mv_mobile_search_start(mv_mobile_library* library);
/* [no-block] */
uint32_t mv_mobile_search_state(const mv_mobile_library* library);

#define MV_MOBILE_SEARCH_PHOTOS 1u
#define MV_MOBILE_SEARCH_CLIPS 2u
#define MV_MOBILE_SEARCH_DOCUMENTS 4u
#define MV_MOBILE_SEARCH_AUDIO 8u

#define MV_MOBILE_MATCH_PICTURE 1u
#define MV_MOBILE_MATCH_SOUND 2u
#define MV_MOBILE_MATCH_SPEECH 4u
#define MV_MOBILE_MATCH_TAGS 8u
#define MV_MOBILE_MATCH_TEXT 16u

typedef struct mv_mobile_hit {
  int64_t asset_id;
  int64_t pts_ms;         /* -1: a photo */
  float score;
  uint32_t kind;          /* MV_MOBILE_SEARCH_* of this hit */
  uint32_t match;         /* MV_MOBILE_MATCH_* */
  uint32_t more_in_clip;
} mv_mobile_hit;

/* Waits for the model if it is still warming up and the query needs it (a name, a date, a place,
 * words said or a file name do not), then answers. One query at a time, newest wins:
 * a run that a newer run or mv_mobile_search_cancel overtakes returns MV_MOBILE_ERR_CANCELLED.
 * kinds 0: every kind. timeout_ms 0: no limit. */
mv_mobile_status mv_mobile_search_run(mv_mobile_library* library, const char* query_utf8, uint32_t kinds,
                                      uint32_t timeout_ms, mv_mobile_results** out);
/* Overtakes the run in progress. [no-block] */
void mv_mobile_search_cancel(mv_mobile_library* library);

void mv_mobile_results_close(mv_mobile_results* results);
/* [no-block] */
uint32_t mv_mobile_results_count(const mv_mobile_results* results);
mv_mobile_status mv_mobile_results_at(const mv_mobile_results* results, uint32_t index, mv_mobile_hit* out);
/* Valid as long as the results. "" when there is none. */
const char* mv_mobile_results_path(const mv_mobile_results* results, uint32_t index);
const char* mv_mobile_results_snippet(const mv_mobile_results* results, uint32_t index);

#ifdef __cplusplus
}
#endif
#endif
