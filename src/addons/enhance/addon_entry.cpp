// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_enhance's one export, mv_addon_get, and its mv.slots.1 interface (#297).
//
// Enhance opens two screens the hosts render from its trees: its page in
// Settings -> Add-ons (place "settings"), where each kind's model is
// downloaded or removed, and the sheet over the viewer (place "sheet"),
// opened by the "Enhance…" command or the context entry for the file on
// screen. Both run on the host's worker for this add-on, never the UI or
// render thread (rule 1). The add-on links nothing of the core: the host
// function table is its only way in.
//
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14),
// and nothing here logs a path or a file name (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "addons/ai/fetch.h"
#include "addons/enhance/engine.h"
#include "addons/places/self_dir.h"
#include "core/json.h"

#ifndef MV_ENHANCE_VERSION
#define MV_ENHANCE_VERSION "0.0.0-dev"
#endif

namespace {

using mv::enhance::engine;

// v3 carries the screen and slot calls.
constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;

struct addon_state {
  const mv_host_api* host = nullptr;
  std::unique_ptr<engine> eng;
};

addon_state* g = nullptr;

template <typename Fn>
mv_status guard(Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return MV_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return MV_ERR_INTERNAL;
  }
}

std::string data_dir(const mv_host_api* h) {
  char buf[4096] = {};
  if (!h->data_dir || h->data_dir(h->host, buf, sizeof buf) != MV_OK) return {};
  return buf;
}

// What a clip holds, from the host's own decoders (never the player's): a
// picture when its sampler opens, sound when its audio does.
mv::enhance::media_facts probe(const mv_host_api* h, const std::string& path) {
  mv::enhance::media_facts f;
  if (h->sampler_open && h->sampler_close) {
    mv_addon_sampler_options o{};
    o.struct_size = sizeof o;
    o.max_long_edge = 64;
    mv_addon_video_info info{};
    void* s = nullptr;
    if (h->sampler_open(h->host, path.c_str(), &o, &info, &s) == MV_OK && s) {
      f.picture = info.width > 0 && info.height > 0;
      h->sampler_close(h->host, s);
    }
  }
  if (h->audio_open && h->audio_close) {
    void* a = nullptr;
    if (h->audio_open(h->host, path.c_str(), 16000, 0, nullptr, &a) == MV_OK && a) {
      f.sound = true;
      h->audio_close(h->host, a);
    }
  }
  return f;
}

mv::enhance::engine_port make_port(const mv_host_api* h) {
  mv::enhance::engine_port p;
  p.post = [h](const char* screen, uint64_t gen, const std::string& tree) {
    return h->screen_post(h->host, screen, gen, tree.c_str()) == MV_OK;
  };
  p.diff = [h](const char* screen, uint64_t gen, const std::string& diff) {
    return h->screen_diff && h->screen_diff(h->host, screen, gen, diff.c_str()) == MV_OK;
  };
  p.show = [h](const char* screen, bool show) { (void)h->screen_show(h->host, screen, show ? 1 : 0); };
  p.probe = [h](const std::string& path) { return probe(h, path); };
  p.free_disk = [h](const std::string& dir) -> std::optional<uint64_t> {
    if (!h->volume_of) return std::nullopt;
    mv_addon_volume v{};
    if (h->volume_of(h->host, dir.c_str(), &v) != MV_OK) return std::nullopt;
    return v.free_bytes;
  };
  p.transport = [] { return mv::ai::make_http_transport(); };
  p.log = [h](int level, const char* message) {
    if (h->log) h->log(h->host, level, message);
  };
  return p;
}

std::vector<std::string> paths_of(const char* value_json) {
  std::vector<std::string> out;
  const auto doc = mv::json::parse(value_json ? value_json : "", 4);
  if (!doc) return out;
  if (doc->k == mv::json::kind::string && !doc->s.empty()) out.push_back(doc->s);
  if (doc->k != mv::json::kind::array) return out;
  for (const auto& v : doc->a) {
    if (v.k == mv::json::kind::string && !v.s.empty()) out.push_back(v.s);
  }
  return out;
}

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id) return MV_ERR_INVALID_ARG;
    if (std::strcmp(r->screen_id, mv::enhance::kSettingsScreen) == 0) {
      g->eng->settings_opened(r->generation);
      return MV_OK;
    }
    if (std::strcmp(r->screen_id, mv::enhance::kSheetScreen) != 0) return MV_ERR_INVALID_ARG;
    g->eng->sheet_opened(r->generation, r->path_utf8 ? r->path_utf8 : "");
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id) return MV_OK;
    const std::string screen = e->screen_id ? e->screen_id : "";
    if (e->kind == MV_SLOT_EVENT_CLOSE) {
      g->eng->closed(screen);
      return MV_OK;
    }
    if (e->kind == MV_SLOT_EVENT_COMMAND) {
      // The command ("open") or the context entry ("enhance"): this file,
      // then the sheet.
      const std::string node = e->node_id;
      if (node == "open" || node == "enhance") g->eng->command(paths_of(e->value_json));
      return MV_OK;
    }
    g->eng->event(screen, e->node_id, e->kind, e->value_json ? e->value_json : "");
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  // The engine stops its downloads (what arrived is kept for Resume) and any
  // job, joining their threads, before the library can go.
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (host_api < kHostApiMin || host_api > kHostApiMax || !host || !out ||
        host->struct_size < offsetof(mv_host_api, screen_show) + sizeof(host->screen_show) || !host->screen_post ||
        !host->screen_show) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (g) return MV_ERR_INVALID_ARG;  // one instance per process
    auto state = std::make_unique<addon_state>();
    state->host = host;
    state->eng = std::make_unique<engine>(make_port(host), mv::places::self_dir(), data_dir(host));
    g = state.release();
    g->eng->resume_wanted();
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "enhance";
    out->version = MV_ENHANCE_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}
