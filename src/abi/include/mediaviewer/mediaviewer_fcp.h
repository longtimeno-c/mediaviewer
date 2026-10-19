/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mv.fcp.1: the Final Cut Pro add-on's search agent (docs/design/23; moved out of
 * MediaViewer.app into the add-on 2026-10-19, issue #485).
 *
 * launchd starts `MediaViewer --search-agent` when the add-on's workflow
 * extension (MediaViewerSearch.appex, inside the add-on) looks up its Mach
 * service. The host loads the installed, verified add-on and serves Local
 * search through the AI pack's read-only reader; `run_agent` listens on the
 * Mach service, checks each peer's signature and answers until it has been
 * idle, then the process exits. Mac only (D9 exception: Final Cut Pro).
 *
 * Flat C, POD, status codes (docs/design/14). Appended only.
 */
#ifndef MEDIAVIEWER_FCP_H
#define MEDIAVIEWER_FCP_H

#include <mediaviewer/mediaviewer_addon.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MV_FCP_INTERFACE "mv.fcp.1"

/* Bytes handed back by the host: the add-on copies them before the call returns. */
typedef void(MV_CALL* mv_fcp_bytes_fn)(void* user, const void* bytes, uint64_t size);

typedef struct mv_fcp_services {
  uint32_t struct_size;
  uint32_t reserved;
  void* ctx;
  /* One search: `request` is nle/search_wire's encoded request, `text` the
   * query (or the file for find similar), `scope_dir` "" for every indexed
   * folder. `out` gets the encoded reply; it always gets one, with its status
   * set when there are no rows. [blocks: the pack's load, the search] */
  mv_status(MV_CALL* run)(void* ctx, const void* request, uint64_t request_size, const char* text_utf8,
                          const char* scope_dir_utf8, mv_fcp_bytes_fn out, void* user);
  /* mv.ai.1 roots_json / suggest_json, into `out` as UTF-8; MV_ERR_BUSY while the
   * pack loads, MV_ERR_NOT_FOUND without Local search. */
  mv_status(MV_CALL* roots_json)(void* ctx, mv_fcp_bytes_fn out, void* user);
  mv_status(MV_CALL* suggest_json)(void* ctx, const char* text_utf8, mv_fcp_bytes_fn out, void* user);
  /* The viewer's thumbnail cache folder: the only folder `thumbnail` serves from. */
  const char* thumbs_dir_utf8;
} mv_fcp_services;

typedef struct mv_fcp_api {
  uint32_t struct_size;
  uint32_t reserved;
  /* Listens on `mach_service` and serves until idle; returns only on failure
   * (the idle exit ends the process). `allow_unsigned`: a developer build's
   * agent accepts peers that are not signed by its team. */
  mv_status(MV_CALL* run_agent)(void* addon, const char* mach_service, const mv_fcp_services* services,
                                uint32_t allow_unsigned);
} mv_fcp_api;

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_FCP_H */
