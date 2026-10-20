// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_driftwire's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/design/28; docs/design/25 PRs 58-59).
//
// Driftwire is a window of its own ("driftwire"), opened from its row in
// Settings -> Add-ons, from its command, from the gallery's share icon and
// from the "Send with Driftwire..." context entry. The host renders the view
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
#include <optional>
#include <string>
#include <vector>

#include "addons/driftwire/engine.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/secrets.h"
#include "core/json.h"
#include "io/file_port.h"

#ifndef MV_DRIFTWIRE_VERSION
#define MV_DRIFTWIRE_VERSION "0.0.0-dev"
#endif

namespace {

using mv::driftwire::engine;

constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
constexpr const char* kScreen = "driftwire";
constexpr const char* kStatusItem = "receiving";  // manifest.json status_items
constexpr const char* kNotice = "offer";          // the one notice (#452): an offer waiting here

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
class host_bridge final : public mv::driftwire::engine_host {
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

  // Downloads, as AirDrop (#442); Save to picks another.
  std::string default_destination() override { return mv::driftwire::net::downloads_folder(); }

  // The add-on's own data folder (made by the host), or "".
  std::string data_dir() {
    char buf[4096];
    if (!h_->data_dir || h_->data_dir(h_->host, buf, sizeof buf) != MV_OK || !*buf) return {};
    return buf;
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

  bool post_status(const std::string& text) override {
    return h_->status_post && h_->status_post(h_->host, kStatusItem, text.c_str()) == MV_OK;
  }

  // #452: entries appended to the host table; an older host has none.
#define MV_SP_HAS(field) \
  (h_->struct_size >= offsetof(mv_host_api, field) + sizeof(h_->field) && h_->field != nullptr)

  bool post_notice(const std::string& title, const std::string& body) override {
    if (!MV_SP_HAS(notice_post)) return false;
    if (title.empty()) return h_->notice_post(h_->host, kNotice, nullptr) == MV_OK;
    mv_addon_notice n{};
    n.struct_size = sizeof n;
    n.screen_id = kScreen;
    n.title_utf8 = title.c_str();
    n.body_utf8 = body.c_str();
    n.action_id = "decline";  // the window's own Decline (engine::on_event)
    n.action_label = "Decline";
    return h_->notice_post(h_->host, kNotice, &n) == MV_OK;
  }

  void ask_to_notify() override {
    if (MV_SP_HAS(notice_authorize)) (void)h_->notice_authorize(h_->host);
  }

  bool keep_alive(bool on) override {
    if (!MV_SP_HAS(keep_alive)) return true;  // an older host: nothing to keep
    return h_->keep_alive(h_->host, on ? 1 : 0, kScreen, mv::driftwire::kReceivingStatus) == MV_OK;
  }

  void post_busy(const std::string& what) override {
    if (MV_SP_HAS(busy_post)) (void)h_->busy_post(h_->host, what.c_str());  // an older host: nothing waits
  }
#undef MV_SP_HAS

 private:
  const mv_host_api* h_;
  std::mutex m_;
  uint64_t generation_ = 0;
  bool open_ = false;
};

struct addon_state {
  const mv_host_api* host = nullptr;
  std::unique_ptr<host_bridge> bridge;
  std::string prefs_path;  // "" without a data folder: nothing is kept
  // This process keeps receiving at launch (one window's process at a time).
  std::unique_ptr<mv::driftwire::net::instance_lock> background;
  // Made on first use (the host's worker, one call at a time), or at load
  // when Keep receiving was on (the loader's thread, after first pixel).
  // Declared last: it stops before the bridge it calls goes.
  std::unique_ptr<engine> eng;

  engine& get(std::optional<mv::driftwire::prefs> initial = std::nullopt) {
    if (!eng) {
      engine::options o;
      // The loader test opens the window without touching the network.
      const char* offline = std::getenv("MV_DRIFTWIRE_OFFLINE");
      o.offline = offline && offline[0] == '1';
      o.prefs_path = prefs_path;
      o.initial = std::move(initial);
      // #472 PR 4: the identity and remembered devices, in the Keychain /
      // Credential Manager (none on other systems: nothing is remembered).
      if (!o.offline) o.make_secrets = [] { return mv::driftwire::secrets::os_store(); };
      eng = std::make_unique<engine>(*bridge, std::move(o));
    }
    return *eng;
  }

  // At load (#442): Keep receiving was on at the last launch, so the engine
  // comes up now and listens; the host loads slot add-ons on its add-on
  // queue / a worker after first pixel, never the foreground queue. Off: one
  // small read, and no engine, thread or socket until the window opens.
  void start_if_kept() {
    if (prefs_path.empty()) return;
    auto kept = mv::driftwire::load_prefs(prefs_path);
    if (!kept || !(kept->receiving || kept->background)) return;
    background = mv::driftwire::net::try_lock_instance(prefs_path + ".lock");
    if (!background) return;  // another window's process receives
    (void)get(kept);
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
    if (e->kind == MV_SLOT_EVENT_NOTICE) {
      // #452: Decline in the system notification is the window's Decline.
      if (g->eng && std::strcmp(e->node_id, "decline") == 0) g->eng->on_event("decline", MV_SLOT_EVENT_CLICK, "");
      return MV_OK;
    }
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
    if (const std::string dir = state->bridge->data_dir(); !dir.empty()) {
      state->prefs_path = mv::io::join_path(dir, mv::driftwire::kPrefsFile);
      mv::driftwire::adopt_old_prefs(dir, state->prefs_path);
    }
    g = state.release();
    g->start_if_kept();
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "driftwire";
    out->version = MV_DRIFTWIRE_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}
