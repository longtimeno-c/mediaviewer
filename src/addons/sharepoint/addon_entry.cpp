// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_sharepoint's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/design/28; docs/design/25 PRs 58-59).
//
// Sharepoint is a window of its own ("sharepoint"), opened from its row in
// Settings -> Add-ons, from its command, from the gallery's share icon and
// from the "Send with Sharepoint..." context entry. The host renders the view
// tree the engine posts; the host's worker for this add-on brings every open,
// close, click and command here. The engine is made at the first of those,
// never at load: until then the add-on has no thread and no socket.
//
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14),
// and nothing here logs a path, a name or an address (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "addons/sharepoint/engine.h"
#include "core/json.h"
#include "io/file_port.h"

#ifndef MV_SHAREPOINT_VERSION
#define MV_SHAREPOINT_VERSION "0.0.0-dev"
#endif

namespace {

using mv::sharepoint::engine;

constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
constexpr const char* kScreen = "sharepoint";

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

// The engine's view of the host table.
class host_bridge final : public mv::sharepoint::engine_host {
 public:
  explicit host_bridge(const mv_host_api* h) : h_(h) {}

  void opened(uint64_t generation) {
    std::lock_guard lock(m_);
    generation_ = generation;
    open_ = true;
  }
  void closed() {
    std::lock_guard lock(m_);
    open_ = false;
  }

  void post_tree(const std::string& tree_json) override {
    uint64_t g = 0;
    {
      std::lock_guard lock(m_);
      if (!open_) return;
      g = generation_;
    }
    (void)h_->screen_post(h_->host, kScreen, g, tree_json.c_str());
  }

  void show_screen(bool show) override { (void)h_->screen_show(h_->host, kScreen, show ? 1 : 0); }

  std::string default_destination() override {
    char buf[4096];
    if (!h_->default_library_dir || h_->default_library_dir(h_->host, buf, sizeof buf) != MV_OK || !*buf) return {};
    return mv::io::join_path(buf, mv::sharepoint::kReceivedFolder);
  }

  std::optional<std::uint64_t> free_bytes(const std::string& dir) override {
    mv_addon_volume v{};
    if (!h_->volume_of || h_->volume_of(h_->host, dir.c_str(), &v) != MV_OK || v.total_bytes == 0) {
      return std::nullopt;
    }
    return v.free_bytes;
  }

  bool should_yield() override { return h_->should_yield && h_->should_yield(h_->host) != 0; }

  std::vector<std::uint32_t> pair_partners(const std::vector<std::string>& names) override {
    if (!h_->pair_names || names.empty()) return {};
    std::vector<const char*> ptrs;
    ptrs.reserve(names.size());
    for (const auto& n : names) ptrs.push_back(n.c_str());
    std::vector<std::uint32_t> partner(names.size(), UINT32_MAX), kind(names.size(), 0);
    if (h_->pair_names(h_->host, ptrs.data(), static_cast<uint32_t>(ptrs.size()), partner.data(), kind.data()) !=
        MV_OK) {
      return {};
    }
    return partner;
  }

  void log(int level, const char* ascii) override {
    if (h_->log) h_->log(h_->host, level, ascii);
  }

 private:
  const mv_host_api* h_;
  std::mutex m_;
  uint64_t generation_ = 0;
  bool open_ = false;
};

struct addon_state {
  const mv_host_api* host = nullptr;
  std::unique_ptr<host_bridge> bridge;
  std::unique_ptr<engine> eng;  // made on first use (the host's worker, one call at a time)

  engine& get() {
    if (!eng) {
      engine::options o;
      // The loader test opens the window without touching the network.
      const char* offline = std::getenv("MV_SHAREPOINT_OFFLINE");
      o.offline = offline && offline[0] == '1';
      eng = std::make_unique<engine>(*bridge, std::move(o));
    }
    return *eng;
  }
};

addon_state* g = nullptr;

std::vector<std::string> paths_of(const char* value_json) {
  std::vector<std::string> out;
  const auto doc = mv::json::parse(value_json ? value_json : "", 4);
  if (!doc || doc->k != mv::json::kind::array) return out;
  for (const auto& v : doc->a) {
    if (v.k == mv::json::kind::string && !v.s.empty()) out.push_back(v.s);
  }
  return out;
}

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id || std::strcmp(r->screen_id, kScreen) != 0) return MV_ERR_INVALID_ARG;
    g->bridge->opened(r->generation);
    g->get().screen_opened(r->path_utf8 ? r->path_utf8 : "");
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id) return MV_OK;
    if (e->kind == MV_SLOT_EVENT_CLOSE) {
      g->bridge->closed();
      if (g->eng) g->eng->screen_closed();
      return MV_OK;
    }
    if (e->kind == MV_SLOT_EVENT_COMMAND) {
      // The command ("open"), the context entry or the gallery's share icon
      // ("send_files"): these files, then the window.
      const std::string node = e->node_id;
      if (node != "open" && node != "send_files") return MV_OK;
      const auto paths = paths_of(e->value_json);
      if (!paths.empty()) g->get().set_files(paths);
      (void)g->host->screen_show(g->host->host, kScreen, 1);
      return MV_OK;
    }
    if (!e->screen_id || std::strcmp(e->screen_id, kScreen) != 0) return MV_OK;
    g->get().on_event(e->node_id, e->kind, e->value_json ? e->value_json : "");
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  // The engine stops its transfer (the other side is told), its presence and
  // every thread before this returns, so the library can go.
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
    state->bridge = std::make_unique<host_bridge>(host);
    g = state.release();
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "sharepoint";
    out->version = MV_SHAREPOINT_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}
