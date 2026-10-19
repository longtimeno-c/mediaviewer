/* Copyright (C) 2026 longtimeno-c
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mv.agent.1: the AI agents add-on's timeline agent (docs/design/29 "The agent").
 *
 * `MediaViewer --mcp` is an MCP server over stdio for Claude Code, Claude
 * Desktop or any MCP client. The executable is the app's own; the server is
 * the agent add-on's code. The host loads the installed, verified add-on
 * (mv_addon_get, host table v3) and, when Local search is installed, the AI
 * pack's read-only reader; then it calls `serve`, which reads JSON-RPC from
 * `in_fd` and writes to `out_fd` until the client closes its end. No socket
 * is opened, by the host or the add-on.
 *
 * Everything the agent reads or names is scoped to the timeline's own media
 * and the folders the editor gave (docs/design/29 "Scope"); `photos:` keys
 * are refused. The services below run Local search for the agent: the
 * add-on filters every answer to that scope before a client sees it.
 *
 * Flat C, POD, status codes (docs/design/14). Appended only.
 */
#ifndef MEDIAVIEWER_AGENT_H
#define MEDIAVIEWER_AGENT_H

#include <mediaviewer/mediaviewer_addon.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MV_AGENT_INTERFACE "mv.agent.1"

/* What the `--mcp` host does for the agent. Every call may block (a search
 * waits for the pack); the agent calls them from its own thread. A JSON
 * answer longer than `cap` returns MV_ERR_INVALID_ARG with *needed set. */
typedef struct mv_agent_services {
  uint32_t struct_size;
  uint32_t reserved;
  void* ctx;

  /* Local search inside `scope_dir` (MV_AI_SCOPE_FOLDER or _TREE; never
   * _ALL). [{"path":"...","pts_ms":N,"duration_ms":N,"score":0.31,"kind":2,
   * "moments":[ms,...]}], best first. MV_ERR_NOT_FOUND: Local search is not
   * installed; MV_ERR_UNSUPPORTED_FORMAT: the pack is too old. */
  mv_status(MV_CALL* search_json)(void* ctx, const char* query_utf8, const char* scope_dir_utf8,
                                  uint32_t scope, uint32_t kinds, uint32_t max_results, char* out,
                                  uint32_t cap, uint32_t* needed);
  /* Find similar to a still (pts_ms -1) or a clip's frame, inside scope_dir. */
  mv_status(MV_CALL* similar_json)(void* ctx, const char* path_utf8, int64_t pts_ms, const char* scope_dir_utf8,
                                   uint32_t scope, uint32_t kinds, uint32_t max_results, char* out,
                                   uint32_t cap, uint32_t* needed);
  /* A file's transcript as the index has it (mv.ai.1 transcript_json):
   * {"indexed":bool,"segments":[{"start_ms":N,"end_ms":N,"text":"..."}]}.
   * MV_ERR_UNSUPPORTED_FORMAT: the installed pack cannot answer. */
  mv_status(MV_CALL* transcript_json)(void* ctx, const char* path_utf8, char* out, uint32_t cap,
                                      uint32_t* needed);
} mv_agent_services;

typedef struct mv_agent_serve_args {
  uint32_t struct_size;
  uint32_t flags;               /* MV_AGENT_SERVE_* */
  intptr_t in_fd;               /* POSIX fd, or a Windows HANDLE */
  intptr_t out_fd;
  const char* bridge_dir_utf8;  /* the panel's Timeline/ folder (nle/bridge_state.h); may be NULL */
  const char* timeline_utf8;    /* --timeline FILE: an FCPXML to work on instead of the panel's; may be NULL */
  const char* const* folders_utf8;  /* --folder DIR, added to the panel's folders */
  uint32_t folder_count;
  uint32_t reserved;
  const char* host_version;     /* the app's version, for serverInfo */
  const mv_agent_services* services;  /* may be NULL: no Local search */
} mv_agent_serve_args;

#define MV_AGENT_SERVE_NO_HANDOFF 1u  /* commit writes the version but does not ask FCP to open it (tests) */

typedef struct mv_agent_api {
  uint32_t struct_size;
  uint32_t reserved;
  /* Serves one MCP session; returns when the client closes `in_fd` (MV_OK)
   * or on a read error (MV_ERR_IO). One at a time per process. */
  mv_status(MV_CALL* serve)(void* addon, const mv_agent_serve_args* args);
} mv_agent_api;

#ifdef __cplusplus
}
#endif

#endif /* MEDIAVIEWER_AGENT_H */
