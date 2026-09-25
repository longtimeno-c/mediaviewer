/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Import add-on's interface to the chrome (plan/18-import.md), obtained
 * with mv_addon_api.query(addon, MV_IMPORT_INTERFACE). Both chromes call it:
 * the WinUI Import window through function pointers in C#, the SwiftUI
 * Import.bundle through this header. Flat C, POD, status codes (plan/14).
 *
 * Rich, read-mostly data (sources, plans, presets, summaries, history)
 * crosses as UTF-8 JSON written into a caller buffer: `cap` bytes, `needed`
 * reports the full size including the terminating NUL; a short buffer
 * returns MV_ERR_INVALID_ARG with `needed` set, and the caller retries. The
 * hot path (progress, polled at display rate while copying) is a POD struct.
 *
 * Thread annotations as in mediaviewer.h. Everything marked [no-block]
 * returns at once; scans, plans, jobs and verifies run on the add-on's own
 * I/O threads and report through the host completion queue
 * (MV_COMPLETION_ADDON, mediaviewer_addon.h). Nothing here deletes from a
 * source, formats a card, overwrites a destination, or uploads (plan/18
 * "Never offered at any setting").
 */
#ifndef MEDIAVIEWER_IMPORT_H
#define MEDIAVIEWER_IMPORT_H

#include <stdint.h>

#include "mediaviewer_addon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MV_IMPORT_INTERFACE "mv.import.1"

typedef enum mv_import_job_state {
  MV_IMPORT_JOB_NONE = 0,
  MV_IMPORT_JOB_QUEUED = 1,
  MV_IMPORT_JOB_RUNNING = 2,
  MV_IMPORT_JOB_PAUSED = 3,
  MV_IMPORT_JOB_DONE = 4,         /* every selected unit copied or skipped */
  MV_IMPORT_JOB_FAILED = 5,       /* finished with at least one failed unit */
  MV_IMPORT_JOB_CANCELLED = 6,
  MV_IMPORT_JOB_INTERRUPTED = 7   /* card or destination went away; resumable */
} mv_import_job_state;

/* What a card insert did (MV_ADDON_EVENT_VOLUME_ARRIVED payload). */
typedef enum mv_import_arrival_action {
  MV_IMPORT_ARRIVAL_NOTHING = 0,
  MV_IMPORT_ARRIVAL_OPEN = 1,     /* open the Import window on this source */
  MV_IMPORT_ARRIVAL_AUTO = 2      /* auto-import started; event id = job id */
} mv_import_arrival_action;

#define MV_IMPORT_MAX_DESTINATIONS 2

typedef struct mv_import_progress {
  uint64_t job_id;
  uint32_t state;                 /* mv_import_job_state */
  uint32_t destination_count;
  uint32_t units_total;
  uint32_t units_done;            /* copied and verified */
  uint32_t units_skipped;         /* duplicates decided up front */
  uint32_t units_failed;
  uint64_t bytes_total;           /* source bytes to read */
  uint64_t bytes_read;
  uint64_t bytes_verified[MV_IMPORT_MAX_DESTINATIONS];
  double bytes_per_second;        /* measured over the last ~10 s */
  int64_t eta_seconds;            /* -1 until there is a measurement */
  int64_t elapsed_ms;
  /* The file being copied, for display only; never logged (rule 6). */
  char current_name_utf8[256];
} mv_import_progress;

typedef struct mv_import_api {
  uint32_t struct_size;
  uint32_t reserved;
  void* ctx;

  /* ---- sources ---------------------------------------------------------- */
  /* [worker-thread] Cards, drives and added folders. Asks the OS about every
   * mounted volume (a dead network share can take seconds) and reads
   * import.db, so both chromes call it off the UI thread, as they do the other
   * *_json reads below (presets, history, unfinished):
   * [{"root","label","volume_id","kind":"card|drive|network|folder",
   *   "removable","total_bytes","free_bytes","new_count":-1|n,"preset"}]. */
  mv_status(MV_CALL* sources_json)(void* ctx, char* out, uint32_t cap, uint32_t* needed);
  mv_status(MV_CALL* add_folder_source)(void* ctx, const char* dir_utf8);
  mv_status(MV_CALL* remove_folder_source)(void* ctx, const char* dir_utf8);
  /* The root of arrival `seq` (MV_ADDON_EVENT_VOLUME_ARRIVED's id). */
  mv_status(MV_CALL* arrival_root)(void* ctx, uint64_t seq, char* out, uint32_t cap);

  /* ---- scan and plan ---------------------------------------------------- */
  /* [ui-thread][no-block] Walks `root`, groups units, reads capture dates,
   * and looks every file up in the card memory. MV_ADDON_EVENT_SCAN_DONE. */
  mv_status(MV_CALL* scan)(void* ctx, const char* root_utf8, uint64_t* out_scan_id);
  /* [ui-thread][no-block] Explicit files (the viewer's marks, Ctrl+Shift+F7):
   * a JSON array of paths. */
  mv_status(MV_CALL* scan_files)(void* ctx, const char* paths_json, uint64_t* out_scan_id);
  /* [ui-thread][no-block] A plan from a finished scan and a preset (JSON, see
   * plan/18 "Configurability"; NULL = the last used preset). `marked_json`
   * is the viewer's marked paths for selection "marked", or NULL.
   * Duplicate tests that need a hash run here. MV_ADDON_EVENT_PLAN_READY. */
  mv_status(MV_CALL* plan)(void* ctx, uint64_t scan_id, const char* preset_json,
                           const char* marked_json, uint64_t* out_plan_id);
  /* [ui-thread][no-block] The plan: totals, days, units (state, selection,
   * destination names, what a duplicate matched), and the "Where files go"
   * folder counts. */
  mv_status(MV_CALL* plan_json)(void* ctx, uint64_t plan_id, char* out, uint32_t cap,
                                uint32_t* needed);
  /* [ui-thread][no-block] Select or clear one unit (unit >= 0), a whole day
   * ("YYYY-MM-DD", unit = -1), or everything (unit = -1, day = NULL). */
  mv_status(MV_CALL* select)(void* ctx, uint64_t plan_id, int32_t unit, const char* day,
                             uint32_t selected);
  /* [worker-thread] The viewer's thumbnail for a unit's primary file. */
  mv_status(MV_CALL* thumbnail)(void* ctx, uint64_t plan_id, uint32_t unit, char* out_utf8,
                                uint32_t cap);

  /* ---- jobs ------------------------------------------------------------- */
  mv_status(MV_CALL* start)(void* ctx, uint64_t plan_id, uint64_t* out_job_id);
  /* Ctrl+Shift+F7: scan_files + plan with the last preset + start. */
  mv_status(MV_CALL* import_now)(void* ctx, const char* paths_json, uint64_t* out_job_id);
  mv_status(MV_CALL* pause)(void* ctx, uint64_t job_id, uint32_t paused);
  mv_status(MV_CALL* cancel)(void* ctx, uint64_t job_id);
  /* 0 = background (yields to the viewer), 1 = fast. */
  mv_status(MV_CALL* set_priority)(void* ctx, uint64_t job_id, uint32_t fast);
  /* [any-thread][no-block] */
  mv_status(MV_CALL* progress)(void* ctx, uint64_t job_id, mv_import_progress* out);
  /* Copied / skipped (with what each matched) / failed (with the reason). */
  mv_status(MV_CALL* summary_json)(void* ctx, uint64_t job_id, char* out, uint32_t cap,
                                   uint32_t* needed);
  mv_status(MV_CALL* retry_failed)(void* ctx, uint64_t job_id, uint64_t* out_job_id);
  /* Jobs a crash or an unplug left unfinished: [{"job","source","label",
   * "pending","done","volume_id"}]. */
  mv_status(MV_CALL* unfinished_json)(void* ctx, char* out, uint32_t cap, uint32_t* needed);
  /* Re-queues an unfinished job's unverified units; verified ones stay done. */
  mv_status(MV_CALL* resume)(void* ctx, uint64_t job_id);
  /* The saved local report (a text file beside import.db). */
  mv_status(MV_CALL* report_path)(void* ctx, uint64_t job_id, char* out, uint32_t cap);
  mv_status(MV_CALL* eject)(void* ctx, const char* root_utf8);

  /* ---- presets ---------------------------------------------------------- */
  mv_status(MV_CALL* presets_json)(void* ctx, char* out, uint32_t cap, uint32_t* needed);
  mv_status(MV_CALL* save_preset)(void* ctx, const char* preset_json);
  mv_status(MV_CALL* delete_preset)(void* ctx, const char* name_utf8);
  /* Binds a preset to a card; auto_import = 1 imports on insert (opt-in). */
  mv_status(MV_CALL* bind_card)(void* ctx, const char* volume_id, const char* preset_name,
                                uint32_t auto_import);
  /* A layout / rename preview for a preset without a scan (Settings). */
  mv_status(MV_CALL* preview_names_json)(void* ctx, const char* preset_json, char* out,
                                         uint32_t cap, uint32_t* needed);

  /* ---- library tools (PR 19) -------------------------------------------- */
  mv_status(MV_CALL* history_json)(void* ctx, char* out, uint32_t cap, uint32_t* needed);
  /* Re-hash every indexed file under `dir` (uncached) against import.db.
   * MV_ADDON_EVENT_VERIFY_DONE; result via summary_json(job). */
  mv_status(MV_CALL* verify_folder)(void* ctx, const char* dir_utf8, uint64_t* out_job_id);
} mv_import_api;

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_IMPORT_H */
