/* Copyright (C) 2026 longtimeno-c
/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Clip editing (PR 13 two-path trim, PR 14 extract & remux): the keyframe
 * index for the scrub-bar grid, and the clip job queue the Jobs pane shows.
 * plan/08-video-editing.md, plan/14-abi.md. ABI 0.10.
 *
 * Same rules as mediaviewer.h: opaque session, POD structs with explicit
 * padding, status codes, UTF-8 caller buffers, nothing retained. No pixels.
 *
 * Every call here is [any-thread][no-block]. The index is read on a pool
 * worker and each job runs on the queue's own worker; answers arrive as
 * completions on the session queue:
 *
 *   MV_COMPLETION_CLIP_INDEX  job_id = the request id, payload = keyframe
 *                             count (status != MV_OK: none, see status)
 *   MV_COMPLETION_CLIP_JOB    job_id = the clip job id, payload =
 *                             mv_clip_job_state, pushed on every transition
 *
 * Rule 5: a job never writes to its source. Outputs are new files beside the
 * source under a free name ("IMG_0001_trimmed.mp4", then " (2)"), written to
 * a hidden temporary first, so a cancel or a failure leaves nothing behind.
 * Rule 6: nothing here logs a path.
 */
#ifndef MEDIAVIEWER_CLIP_H
#define MEDIAVIEWER_CLIP_H

#include <stdint.h>

#include "mediaviewer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MV_COMPLETION_CLIP_INDEX 11
#define MV_COMPLETION_CLIP_JOB 12

typedef enum mv_clip_op {
  MV_CLIP_TRIM_KEYFRAME = 1,  /* Path 1: stream copy between keyframes (instant) */
  MV_CLIP_TRIM_REENCODE = 2,  /* Path 2: frame-accurate, hardware encoder (slower) */
  MV_CLIP_ROTATE = 3,         /* lossless: container matrix only */
  MV_CLIP_SPLIT = 4,          /* two files at the keyframe nearest in_ns */
  MV_CLIP_REMOVE_MIDDLE = 5,  /* one file without [in, out) */
  MV_CLIP_REMUX = 6,          /* MKV <-> MP4, no re-encode */
  MV_CLIP_FRAME = 7,          /* the frame at in_ns as PNG / JPEG */
  MV_CLIP_AUDIO = 8,          /* first audio track: copy / WAV / FLAC */
  MV_CLIP_ANIMATION = 9       /* [in, out) as GIF / WebP, at most 60 s */
} mv_clip_op;

typedef enum mv_clip_job_state {
  MV_CLIP_JOB_QUEUED = 1,
  MV_CLIP_JOB_RUNNING = 2,
  MV_CLIP_JOB_DONE = 3,
  MV_CLIP_JOB_FAILED = 4,
  MV_CLIP_JOB_CANCELLED = 5
} mv_clip_job_state;

/* `option` by op: ROTATE 1 = 90° clockwise, 2 = 90° counter-clockwise,
 * 3 = 180°; REMUX 1 = MP4, 2 = MKV; FRAME 1 = PNG, 2 = JPEG; AUDIO 1 = copy,
 * 2 = WAV, 3 = FLAC; ANIMATION 1 = GIF, 2 = WebP. Others ignore it.
 * Times are nanoseconds on the player's timeline (mv_video_position);
 * out_ns < 0 means the end of the clip. */
typedef struct mv_clip_request {
  uint32_t struct_size;       /* sizeof(mv_clip_request) */
  uint32_t op;                /* mv_clip_op */
  int64_t in_ns;
  int64_t out_ns;
  uint32_t option;
  uint32_t animation_width;   /* long edge, 0 = 480 */
  uint32_t animation_fps;     /* 0 = 15 */
  uint32_t reserved;
} mv_clip_request;

typedef struct mv_clip_progress {
  uint64_t job_id;
  uint32_t state;             /* mv_clip_job_state */
  uint32_t op;                /* mv_clip_op */
  double fraction;            /* 0..1 */
  int64_t elapsed_ms;
  int64_t eta_ms;             /* -1 until measured */
  uint32_t error;             /* mv_status when FAILED */
  uint32_t output_count;
  char title_utf8[64];        /* "Trim (re-encode, NVENC)" */
  char source_name_utf8[256]; /* file name only, for display */
} mv_clip_progress;

/* The MediaViewerClipJob helper (UTF-8 path; copied). Once set, every job
 * that opens a decoder or an encoder (re-encode, frame, WAV / FLAC, GIF /
 * WebP) runs in that helper process, so a crash in a driver's encoder fails
 * the job and not the viewer; if the helper cannot be started those jobs
 * fail with MV_ERR_IO and are never run in the viewer's process. Stream-copy
 * jobs always run in process. Hosts set it once, before the first submit.
 * [any-thread][no-block] */
MV_API mv_status MV_CALL mv_clip_set_helper(mv_session_t session, const char* utf8_helper_path);

/* Reads the clip's keyframe index on a pool worker (packets, never decoded).
 * `out_request_id` names the MV_COMPLETION_CLIP_INDEX that answers. */
MV_API mv_status MV_CALL mv_clip_index_request(mv_session_t session, const char* utf8_path,
                                               uint64_t* out_request_id);

/* The answer to an index request, once its completion has arrived. Writes up
 * to `cap` keyframe times; `out_count` is the full count. MV_ERR_INVALID_ARG
 * for an unknown or unanswered id. The session keeps the last few answers. */
MV_API mv_status MV_CALL mv_clip_index_get(mv_session_t session, uint64_t request_id,
                                           int64_t* keyframes_ns, uint32_t cap,
                                           uint32_t* out_count, int64_t* out_duration_ns);

/* Queues a job on `utf8_source`. Returns at once with its id. */
MV_API mv_status MV_CALL mv_clip_submit(mv_session_t session, const char* utf8_source,
                                        const mv_clip_request* request, uint64_t* out_job_id);

/* Cancels a queued or running job: nothing it would have written remains.
 * MV_ERR_INVALID_ARG if it is unknown or already finished. */
MV_API mv_status MV_CALL mv_clip_cancel(mv_session_t session, uint64_t job_id);

/* A failed or cancelled job again, as a new job. */
MV_API mv_status MV_CALL mv_clip_retry(mv_session_t session, uint64_t job_id,
                                       uint64_t* out_job_id);

/* Job ids, oldest first. `out_count` is the full count. */
MV_API mv_status MV_CALL mv_clip_jobs(mv_session_t session, uint64_t* ids, uint32_t cap,
                                      uint32_t* out_count);

MV_API mv_status MV_CALL mv_clip_job_progress(mv_session_t session, uint64_t job_id,
                                              mv_clip_progress* out_progress);

/* Output `index` of a DONE job (split has two). Buffer rules as
 * mv_folder_item_name. */
MV_API mv_status MV_CALL mv_clip_job_output(mv_session_t session, uint64_t job_id, uint32_t index,
                                            char* utf8, uint32_t cap, uint32_t* out_bytes);

/* Forgets finished jobs (the Jobs pane's Clear). */
MV_API mv_status MV_CALL mv_clip_clear_finished(mv_session_t session);

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_CLIP_H */
