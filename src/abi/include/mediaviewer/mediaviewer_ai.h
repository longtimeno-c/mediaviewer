/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The AI pack's interface to the chrome (plan/17-local-ai-search.md,
 * Milestone H, PRs 20-24), obtained with
 * mv_addon_api.query(addon, MV_AI_INTERFACE). Both chromes call it: the WinUI
 * AI chrome through function pointers in C#, the SwiftUI AI.bundle through
 * this header. Flat C, POD, status codes (plan/14).
 *
 * Rich, read-mostly data (settings, roots, people) crosses as UTF-8 JSON in a
 * caller buffer: `cap` bytes, `needed` reports the full size including the
 * NUL; a short buffer returns MV_ERR_INVALID_ARG with `needed` set and the
 * caller retries. The hot paths (status, results) are POD.
 *
 * Threads: [no-block] returns at once; [worker-thread] may read the index or
 * run the text tower (~10 ms) and belongs off the UI thread. Indexing,
 * searching and the compute self-test run on the add-on's own lowest-priority
 * workers and report through the host completion queue (MV_COMPLETION_ADDON,
 * kinds MV_ADDON_EVENT_AI_*).
 *
 * Privacy (rule 6): paths, queries, embeddings and face data stay in this
 * process and in the add-on's data folder. Nothing here logs one, and the
 * download that installed the pack carried no identifier.
 */
#ifndef MEDIAVIEWER_AI_H
#define MEDIAVIEWER_AI_H

#include <stdint.h>

#include "mediaviewer_addon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MV_AI_INTERFACE "mv.ai.1"

/* Where inference runs (plan/17 "Runtime"). CPU is always underneath. */
typedef enum mv_ai_backend {
  MV_AI_BACKEND_CPU = 0,
  MV_AI_BACKEND_CUDA = 1,      /* Windows vendor piece "ai-cuda" */
  MV_AI_BACKEND_OPENVINO = 2,  /* Windows vendor piece "ai-openvino" */
  MV_AI_BACKEND_COREML = 3     /* macOS, in the Mac Core pack */
} mv_ai_backend;

/* Settings -> Local search -> Compute. */
typedef enum mv_ai_compute {
  MV_AI_COMPUTE_AUTO = 0,
  MV_AI_COMPUTE_CPU_ONLY = 1,
  MV_AI_COMPUTE_CUDA = 2,
  MV_AI_COMPUTE_OPENVINO = 3,
  MV_AI_COMPUTE_COREML = 4
} mv_ai_compute;

/* Settings -> Local search -> Search quality (plan/17 PR 20 spike): which
 * tower indexes. Changing it migrates: the old index answers until the new
 * one completes (PR 23). */
typedef enum mv_ai_quality {
  MV_AI_QUALITY_AUTO = 0,      /* High on an accelerated provider, else Fast */
  MV_AI_QUALITY_FAST = 1,      /* CLIP ViT-B/32 */
  MV_AI_QUALITY_HIGH = 2       /* CLIP ViT-L/14 */
} mv_ai_quality;

typedef enum mv_ai_state {
  MV_AI_STATE_IDLE = 0,        /* every enabled root is indexed */
  MV_AI_STATE_INDEXING = 1,
  MV_AI_STATE_PAUSED = 2,      /* the user paused it */
  MV_AI_STATE_YIELDING = 3,    /* waiting for the viewer; see yield_reason */
  MV_AI_STATE_LOADING = 4,     /* loading the model / running the compute self-test;
                                  yield_reason VIEWER while it waits for a quiet viewer */
  MV_AI_STATE_ERROR = 5        /* the model would not load; see provider_fault */
} mv_ai_state;

typedef enum mv_ai_yield {
  MV_AI_YIELD_NONE = 0,
  MV_AI_YIELD_VIEWER = 1,      /* playing, panning, a slideshow, a transition */
  MV_AI_YIELD_BATTERY = 2,     /* on battery below the threshold */
  MV_AI_YIELD_FRAMES = 3       /* the present loop dropped a frame recently */
} mv_ai_yield;

/* The index's search scope. */
typedef enum mv_ai_scope {
  MV_AI_SCOPE_FOLDER = 0,      /* the given folder only (default) */
  MV_AI_SCOPE_TREE = 1,        /* the folder and below */
  MV_AI_SCOPE_ALL = 2          /* every indexed folder */
} mv_ai_scope;

#define MV_AI_KIND_PHOTOS 1u
#define MV_AI_KIND_VIDEOS 2u
#define MV_AI_KIND_ALL 3u

/* What a search looks for, OR'ed into `kinds` (2026-09-27, audio). None of
 * these set means all three. */
#define MV_AI_FIND_PICTURES 0x10u     /* what a photo or frame shows (CLIP) */
#define MV_AI_FIND_SOUNDS 0x20u       /* what a clip sounds like (CLAP): "dog barking" */
#define MV_AI_FIND_SPEECH 0x40u       /* what is said in a clip (Whisper transcripts) */

/* What a folder's VIDEOS are indexed for (Settings "Index videos for", and per
 * folder). Photos are always pictures. Sound needs the ai-audio piece and
 * covers both sounds and speech. */
#define MV_AI_MEDIA_DEFAULT 0u        /* per folder: follow the setting */
#define MV_AI_MEDIA_PICTURES 1u
#define MV_AI_MEDIA_SOUND 2u
#define MV_AI_MEDIA_BOTH 3u

/* Why a result matched (mv_ai_result.match), a bit set. */
#define MV_AI_MATCH_PICTURE 1u
#define MV_AI_MATCH_SOUND 2u
#define MV_AI_MATCH_SPEECH 4u

#define MV_AI_STATUS_INDEX_FULL 1u     /* over the index size cap: indexing stopped */
#define MV_AI_STATUS_FACES_ON 2u       /* the People opt-in is on */
#define MV_AI_STATUS_FACES_READY 4u    /* ...and the ai-faces piece is loaded */
#define MV_AI_STATUS_NO_MODELS 8u      /* the Core pack's models failed to load */
#define MV_AI_STATUS_AUDIO_READY 16u   /* the ai-audio piece is loaded (sounds + speech) */
#define MV_AI_STATUS_FIRST_COMPILE 32u /* LOADING, and the model is being prepared for this
                                          machine for the first time (Core ML's first compile,
                                          minutes; later starts read its cache) */

/* Polled by the status line (~4 Hz while visible). [no-block] */
typedef struct mv_ai_status {
  uint32_t struct_size;
  uint32_t state;              /* mv_ai_state */
  uint32_t yield_reason;       /* mv_ai_yield */
  uint32_t backend;            /* mv_ai_backend actually running */
  uint32_t provider_fault;     /* why Auto / the chosen provider fell back: 0 none,
                                * 1 not in this build, 2 runtime missing (CUDA / cuDNN),
                                * 3 failed, 4 mismatch vs CPU, 5 slower than CPU */
  uint32_t quality;            /* mv_ai_quality in effect (never AUTO) */
  uint64_t assets_total;       /* known in enabled roots */
  uint64_t assets_done;
  uint64_t assets_failed;
  uint64_t frames_indexed;     /* rows searchable now, current model */
  double assets_per_second;    /* measured over the last minute of work; 0 unknown */
  double frames_per_second;
  double eta_low_seconds;      /* a range from completed work; -1 unknown */
  double eta_high_seconds;
  uint64_t index_bytes;        /* index.db + faces.db on disk */
  uint64_t migrate_total;      /* assets a quality change must re-embed; 0 none */
  uint64_t migrate_done;
  uint64_t faces_total;        /* 0 unless Faces is on */
  uint32_t people;             /* clusters */
  uint32_t flags;              /* MV_AI_STATUS_* */
  char active_root_utf8[1024]; /* display only */
  char model_utf8[64];         /* "CLIP ViT-L/14 fp16" */
  /* Audio (2026-09-27): clips to index for sound / speech, and done. */
  uint64_t sound_total;
  uint64_t sound_done;
  uint64_t speech_total;
  uint64_t speech_done;
} mv_ai_status;

/* One result: a photo, or the best moment of a clip with the others grouped
 * under it (plan/17 "Ranking"). */
typedef struct mv_ai_result {
  uint64_t asset_id;
  int64_t pts_ms;              /* -1 for a photo */
  float score;                 /* cosine similarity, higher is closer */
  uint32_t kind;               /* MV_AI_KIND_PHOTOS or MV_AI_KIND_VIDEOS */
  uint32_t more_in_clip;       /* other matching moments in the same clip */
  uint32_t match;              /* MV_AI_MATCH_*: what matched at pts_ms (picture, sound, speech) */
} mv_ai_result;

typedef struct mv_ai_api {
  uint32_t struct_size;
  uint32_t reserved;
  void* ctx;

  /* ---- state and settings (PR 20, 23) ------------------------------------ */
  mv_status(MV_CALL* status)(void* ctx, mv_ai_status* out);                      /* [no-block] */
  /* {"compute":0..4,"quality":0..2,"pause_on_battery_percent":30,
   *  "video_index":1..3 (in effect: an unset choice is Pictures, or Both once
   *  ai-audio is installed), "video_index_setting":0..3, "audio_ready":bool,
   *  "index_cap_bytes":N,"faces":false,"min_score":0.2,
   *  "available":{"cuda":bool,"openvino":bool,"coreml":bool},
   *  "models":[{"quality":1,"name":"CLIP ViT-B/32","dim":512},...],
   *  "runtime":"1.30.0"}  [no-block] */
  mv_status(MV_CALL* settings_json)(void* ctx, char* out, uint32_t cap, uint32_t* needed);
  /* key: "compute" | "quality" | "pause_on_battery_percent" | "index_cap_bytes"
   * | "min_score" | "reload" | "video_index" (MV_AI_MEDIA_*, 0 = Pictures, plus
   * Sound once the ai-audio piece is installed); value: a JSON number. Compute re-creates the
   * sessions (no re-index); quality starts a migration. "reload" (any value)
   * re-reads the installed pieces after one is installed or removed: People
   * (ai-faces) and Sound (ai-audio) are picked up at once, without reopening
   * the picture towers (indexing and search go on); a vendor piece (ai-cuda) carries its own
   * runtime and takes effect the next time the app starts. [no-block] */
  mv_status(MV_CALL* set_setting)(void* ctx, const char* key, const char* value_json);
  /* Pause / resume all indexing. [no-block] */
  mv_status(MV_CALL* pause)(void* ctx, uint32_t paused);

  /* ---- remembered roots (PR 21, 23) --------------------------------------- */
  /* [{"id":1,"path":"...","recursive":true,"enabled":true,"assets":N,
   *   "done":N,"frames":N,"bytes":N,"last_scan":unix}] [worker-thread] */
  mv_status(MV_CALL* roots_json)(void* ctx, char* out, uint32_t cap, uint32_t* needed);
  /* "Index this folder" / "... and subfolders": remembers a root and starts
   * work in the background; results become searchable as they commit. A
   * folder already covered returns that root's id. [no-block] */
  mv_status(MV_CALL* index_folder)(void* ctx, const char* dir_utf8, uint32_t recursive,
                                   uint64_t* out_root_id);
  mv_status(MV_CALL* root_set_enabled)(void* ctx, uint64_t root_id, uint32_t enabled);
  mv_status(MV_CALL* root_rescan)(void* ctx, uint64_t root_id);
  /* Forgets the root and deletes its rows (and its faces). [worker-thread] */
  mv_status(MV_CALL* root_remove)(void* ctx, uint64_t root_id);
  /* 0 not covered, 1 covered and indexing, 2 covered and complete. [no-block] */
  mv_status(MV_CALL* folder_coverage)(void* ctx, const char* dir_utf8, uint32_t* out_state);
  /* The viewer opened `dir`: if a root covers it, queue its delta. [no-block] */
  mv_status(MV_CALL* note_folder_opened)(void* ctx, const char* dir_utf8);
  /* Deletes every row and vector (the thumbnail cache is untouched); waits
   * for the indexer to let go. [worker-thread] */
  mv_status(MV_CALL* clear_index)(void* ctx);

  /* ---- search (PR 22, 23) ------------------------------------------------- */
  /* Natural-language query. `scope_dir` may be NULL for MV_AI_SCOPE_ALL.
   * Runs on a worker; MV_ADDON_EVENT_AI_SEARCH_DONE carries the id and the
   * result count (0: "nothing found" — the min-score cutoff, not the
   * least-bad ten). A query naming a person (PR 24) narrows to that person.
   * [no-block] */
  mv_status(MV_CALL* search_text)(void* ctx, const char* query_utf8, const char* scope_dir_utf8,
                                  uint32_t scope, uint32_t kinds, uint64_t* out_search_id);
  /* Find similar: a still (pts_ms = -1) or the frame of a clip at pts_ms,
   * embedded on demand when it was never sampled. [no-block] */
  mv_status(MV_CALL* search_similar)(void* ctx, const char* path_utf8, int64_t pts_ms,
                                     const char* scope_dir_utf8, uint32_t scope, uint32_t kinds,
                                     uint64_t* out_search_id);
  mv_status(MV_CALL* result_count)(void* ctx, uint64_t search_id, uint32_t* out_count);
  mv_status(MV_CALL* result_at)(void* ctx, uint64_t search_id, uint32_t index,
                                mv_ai_result* out);
  mv_status(MV_CALL* result_path)(void* ctx, uint64_t search_id, uint32_t index, char* out_utf8,
                                  uint32_t cap);
  /* The tile: the moment's JPEG in the viewer's cache (made on a miss).
   * [worker-thread] */
  mv_status(MV_CALL* result_thumb)(void* ctx, uint64_t search_id, uint32_t index,
                                   char* out_utf8, uint32_t cap);
  /* Every matching moment of the clip at `path`, in time order: the scrub
   * bar's markers and N / Shift+N. `out_ms` may be NULL to count. */
  mv_status(MV_CALL* clip_matches)(void* ctx, uint64_t search_id, const char* path_utf8,
                                   int64_t* out_ms, float* out_scores, uint32_t cap,
                                   uint32_t* out_count);
  mv_status(MV_CALL* search_release)(void* ctx, uint64_t search_id);

  /* ---- people (PR 24; everything here is inert until faces are on) -------- */
  /* The separate opt-in ("Find people in your photos"). Off deletes every
   * face vector, crop and name at once. [no-block] */
  mv_status(MV_CALL* faces_enable)(void* ctx, uint32_t enable);
  /* [{"id":7,"name":"","faces":42,"cover_face":9,"cover_path":"...","cover_ms":-1,
   *   "cover_box":[x,y,w,h]}]  (box in 0..1 of the image) [worker-thread] */
  mv_status(MV_CALL* people_json)(void* ctx, char* out, uint32_t cap, uint32_t* needed);
  /* [{"face":9,"path":"...","pts_ms":-1,"box":[x,y,w,h],"score":0.9}] */
  mv_status(MV_CALL* person_faces_json)(void* ctx, uint64_t person_id, char* out, uint32_t cap,
                                        uint32_t* needed);
  mv_status(MV_CALL* person_rename)(void* ctx, uint64_t person_id, const char* name_utf8);
  /* [worker-thread] */
  mv_status(MV_CALL* person_merge)(void* ctx, uint64_t into_id, uint64_t from_id);
  /* "Not this person": the face leaves the cluster and never rejoins it.
   * [worker-thread] */
  mv_status(MV_CALL* face_reject)(void* ctx, uint64_t face_id);
  /* Split: these faces become a new person. [worker-thread] */
  mv_status(MV_CALL* face_split)(void* ctx, const uint64_t* face_ids, uint32_t count,
                                 uint64_t* out_person_id);
  /* Photos and moments of a person. [no-block] */
  mv_status(MV_CALL* search_person)(void* ctx, uint64_t person_id, const char* scope_dir_utf8,
                                    uint32_t scope, uint64_t* out_search_id);
  /* "This person": the largest face in the still / frame on screen. The
   * search's result count is 0 when no face is found there. [no-block] */
  mv_status(MV_CALL* search_this_person)(void* ctx, const char* path_utf8, int64_t pts_ms,
                                         uint64_t* out_search_id);
  /* The JPEG a face was found in: the still's JPEG-512, or the moment's
   * (made on a miss), for every format the viewer decodes (RAW, HEIC, clips).
   * The chrome crops it in the view with the face's box; no crop is ever
   * written (PR 24 "never ... crops"). people_json's "cover_face" names the
   * cover's face id. [worker-thread] */
  mv_status(MV_CALL* face_thumb)(void* ctx, uint64_t face_id, char* out_utf8, uint32_t cap);

  /* ---- audio (2026-09-27) ------------------------------------------------- */
  /* What a remembered folder's videos are indexed for (MV_AI_MEDIA_*; 0
   * follows Settings "video_index"). Adding Sound queues its clips; removing
   * it keeps what was indexed until the folder is removed. [no-block] */
  mv_status(MV_CALL* root_set_media)(void* ctx, uint64_t root_id, uint32_t media);
  /* The words that matched, for a speech result ("...we are now landing in
   * Lisbon..."), empty otherwise. */
  mv_status(MV_CALL* result_snippet)(void* ctx, uint64_t search_id, uint32_t index, char* out_utf8,
                                     uint32_t cap);
} mv_ai_api;

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_AI_H */
