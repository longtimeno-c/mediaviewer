/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Crash-report context (plan/13 Part 2). The host registers these addresses
 * with its out-of-process crash reporter as annotations. The core never links
 * a crash reporter (D9: the reporter's client is a host concern).
 *
 * What the slots may contain: format family, bundled decoder name + version,
 * dimensions, bit depth, correlation id. Never a path, filename, bytes or
 * EXIF — the text is built from literals and integers inside the core.
 */
#ifndef MEDIAVIEWER_MEDIAVIEWER_CRASH_H
#define MEDIAVIEWER_MEDIAVIEWER_CRASH_H

#include "mediaviewer/mediaviewer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mv_crash_context_info {
  /* slot_count fixed-size, NUL-padded ASCII slots, one per decode worker. */
  const char* slots;
  uint32_t slot_count;
  uint32_t slot_bytes;
  /* Correlation id of the most recent ABI call, any thread. */
  const uint64_t* last_call_correlation_id;
} mv_crash_context_info;

/* Addresses are stable for the life of the process. [any-thread][no-block] */
MV_API mv_status MV_CALL mv_crash_context(mv_crash_context_info* out_info);

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_MEDIAVIEWER_CRASH_H */
