// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_fcp's one export, mv_addon_get, and its one interface, mv.fcp.1 (the search
// agent, agent_mac.mm). The add-on contributes nothing to the viewer's
// window, so the app never loads it (manifest.h loads_for_slots): only
// `MediaViewer --search-agent` (shell/fcp_host_mac.mm) does, when launchd
// starts it for the Final Cut Pro panel.
#include <cstring>

#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_fcp.h>

#ifndef MV_FCP_VERSION
#define MV_FCP_VERSION "0.0.0-dev"
#endif

const mv_fcp_api* mv_fcp_agent_api();

namespace {

constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
int g_instance = 0;

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_FCP_INTERFACE) == 0) return mv_fcp_agent_api();
  return nullptr;
}

void MV_CALL shutdown(void*) {}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  if (host_api < kHostApiMin || host_api > kHostApiMax || !host || !out) return MV_ERR_UNSUPPORTED_FORMAT;
  std::memset(out, 0, sizeof *out);
  out->struct_size = sizeof *out;
  out->id = "fcp";
  out->version = MV_FCP_VERSION;
  out->addon = &g_instance;
  out->shutdown = &shutdown;
  out->query = &query;
  return MV_OK;
}
