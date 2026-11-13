// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_driftwire's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/design/28; docs/design/25 PRs 58-59).
//
// Driftwire is a window of its own ("driftwire"), opened from its row in
// Settings -> Add-ons, from its command, from the gallery's share icon and
// from the "Send with Driftwire..." context entry. Its page in Settings ->
// Add-ons shows its settings ("settings", place "settings": the window's
// controls, the same engine), open while that page shows. The host renders the view
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

#include "addons/driftwire/desk.h"
#include "addons/driftwire/engine.h"
#include "addons/driftwire/known.h"
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
constexpr const char* kSettingsScreen = "settings";  // manifest.json, place "settings"
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
  void settings_opened(uint64_t generation) {
    std::lock_guard lock(m_);
    settings_generation_ = generation;
    settings_open_ = true;
    settings_last_.clear();
  }
  void settings_closed() {
    std::lock_guard lock(m_);
    settings_open_ = false;
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

  bool settings_shown() override {
    std::lock_guard lock(m_);
    return settings_open_;
  }

  // Posted only when it changed: a transfer's progress moves the window ten
  // times a second and the settings not at all.
  void post_settings(const std::string& tree_json) override {
    uint64_t g = 0;
    {
      std::lock_guard lock(m_);
      if (!settings_open_ || tree_json == settings_last_) return;
      settings_last_ = tree_json;
      g = settings_generation_;
    }
    (void)h_->screen_post(h_->host, kSettingsScreen, g, tree_json.c_str());
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
    return post_offer_notice(title, body, 0);
  }
  bool post_offer_notice(const std::string& title, const std::string& body, std::uint64_t offer) override {
    if (!MV_SP_HAS(notice_post)) return false;
    if (title.empty()) return h_->notice_post(h_->host, kNotice, nullptr) == MV_OK;
    mv_addon_notice n{};
    n.struct_size = sizeof n;
    n.screen_id = kScreen;
    n.title_utf8 = title.c_str();
    n.body_utf8 = body.c_str();
    // The window's own Decline, for this offer only (#502: the host hands
    // the id back, and a notification left over from an earlier offer must
    // not decline the next).
    const std::string action = "decline:" + std::to_string(offer);
    n.action_id = action.c_str();
    n.action_label = "Decline";
    return h_->notice_post(h_->host, kNotice, &n) == MV_OK;
  }

  void ask_to_notify() override {
    if (MV_SP_HAS(notice_authorize)) (void)h_->notice_authorize(h_->host);
  }

  bool post_menu(const std::string& menu_json) override {
    if (!MV_SP_HAS(keep_menu)) return true;  // an older host: the four fixed rows
    return h_->keep_menu(h_->host, menu_json.c_str()) == MV_OK;
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
  uint64_t settings_generation_ = 0;
  bool settings_open_ = false;
  std::string settings_last_;
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
  // Phone access (desk.h), made at Local search's first request; its secrets
  // are this machine's OS store, as the engine's are.
  std::unique_ptr<mv::driftwire::secrets::store> desk_store;
  std::unique_ptr<mv::driftwire::desk::desk> desk;

  mv::driftwire::desk::desk* get_desk() {
    if (desk) return desk.get();
    desk_store = mv::driftwire::secrets::os_store();
    if (!desk_store) return nullptr;
    mv::driftwire::desk::desk::options o;
    o.store = desk_store.get();
    o.device_name = mv::driftwire::net::machine_name();
    desk = std::make_unique<mv::driftwire::desk::desk>(std::move(o));
    return desk.get();
  }

  engine& get(std::optional<mv::driftwire::prefs> initial = std::nullopt) {
    if (!eng) {
      engine::options o;
      // The loader test opens the window without touching the network.
      const char* offline = std::getenv("MV_DRIFTWIRE_OFFLINE");
      o.offline = offline && offline[0] == '1';
      o.prefs_path = prefs_path;
      o.initial = std::move(initial);
      // #472 PR 4: the identity and remembered devices, in the Keychain /
      // Credential Manager / the Secret Service (none without one: nothing
      // is remembered).
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
    if (r && r->screen_id && std::strcmp(r->screen_id, kSettingsScreen) == 0) {
      g->bridge->settings_opened(r->generation);
      g->get().settings_opened();
      return MV_OK;
    }
    if (!r || !r->screen_id || std::strcmp(r->screen_id, kScreen) != 0) return MV_ERR_INVALID_ARG;
    g->bridge->opened(r->generation);
    g->get().screen_opened(r->path_utf8 ? r->path_utf8 : "");
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id) return MV_OK;
    const bool settings = e->screen_id && std::strcmp(e->screen_id, kSettingsScreen) == 0;
    if (e->kind == MV_SLOT_EVENT_CLOSE && settings) {
      g->bridge->settings_closed();
      return MV_OK;
    }
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
    if (settings) {
      // The settings page's controls are the window's (the same ids).
      g->get().on_event(e->node_id, e->kind, e->value_json ? e->value_json : "");
      return MV_OK;
    }
    if (!e->screen_id || std::strcmp(e->screen_id, kScreen) != 0) return MV_OK;
    if (e->kind == MV_SLOT_EVENT_MENU) {
      // A row of the menu-bar item / tray icon's menu (build_menu). Its ids are
      // the window's, so a toggle or Cancel is the window's own change; Send
      // Files... puts the chosen files on the Send card and opens the window.
      const std::string node = e->node_id;
      const std::string value = e->value_json ? e->value_json : "";
      if (node == "send_files") {
        const auto paths = paths_of(e->value_json);
        if (paths.empty()) return MV_OK;
        g->get().set_files(paths);
        (void)g->host->screen_show(g->host->host, kScreen, 1);
      } else if (node == "out_cancel" || node == "in_cancel") {
        g->get().on_event(node, MV_SLOT_EVENT_CLICK, "");
      } else if (node == "receiving" || node == "internet" || node == "background") {
        g->get().on_event(node, MV_SLOT_EVENT_CHANGE, value);
      }
      return MV_OK;
    }
    if (e->kind == MV_SLOT_EVENT_NOTICE) {
      // #452: Decline in the system notification is the window's Decline.
      // "decline:<offer>" (#502) reaches only that offer; a bare "decline"
      // (posted before) any.
      if (g->eng && std::strncmp(e->node_id, "decline", 7) == 0) {
        const char* rest = e->node_id + 7;
        if (*rest == '\0') {
          g->eng->decline_notice(0);
        } else if (*rest == ':') {
          g->eng->decline_notice(std::strtoull(rest + 1, nullptr, 10));
        }
      }
      return MV_OK;
    }
    g->get().on_event(e->node_id, e->kind, e->value_json ? e->value_json : "");
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

// Local search reaching the service on the NAS and the nearby service, through
// the host (docs/design/18 "Add-ons calling add-ons"). On this add-on's worker.
mv_status MV_CALL handle_request(void*, const char* caller, const char* method, const char* request_json,
                                 mv_addon_reply_fn reply, void* user) {
  return guard([&] {
    if (!caller || !method || !reply) return MV_ERR_INVALID_ARG;
    auto* d = g->get_desk();
    if (!d) return MV_ERR_UNSUPPORTED_FORMAT;  // this machine has no secret store
    auto r = mv::driftwire::desk::route_request(*d, caller, method, request_json ? request_json : "{}");
    if (!r) return static_cast<mv_status>(r.error());
    reply(user, r->c_str());
    return MV_OK;
  });
}

const mv_addon_request_api kRequests{sizeof(mv_addon_request_api), 0, &handle_request};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_REQUEST_INTERFACE) == 0) return &kRequests;
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

// #507: removed without keeping its data. The identity and the remembered
// devices are in the OS's secret store, not the data folder the host deletes.
// Not loaded: no engine runs.
extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_forget(const char* /*data_dir_utf8*/) {
  return guard([&] {
    auto store = mv::driftwire::secrets::os_store();
    if (store && !mv::driftwire::known::forget(*store)) return MV_ERR_IO;
    return MV_OK;
  });
}
