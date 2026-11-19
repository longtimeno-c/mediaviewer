// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_vpn's one export, mv_addon_get, and its mv.slots.1 interface
// (docs/plans/vpn-addon.md; docs/design/25 PRs 58-59).
//
// The VPN is one screen, "settings" (place "settings"), its page in Settings ->
// Add-ons, and while the Tunnel is on a menu-bar item / tray icon of its own
// (host table keep_alive / keep_menu) whose rows are the page's switches.
// The service (service.h) is made at load only when the Tunnel was on, or a
// system proxy setting is left to put back; otherwise at the page's first
// open. Phone access is asked through the host (addon_request), never
// directly: add-ons reach each other only that way.
//
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14),
// and nothing here logs a path, a name, an address or a key (rule 6).
#include <mediaviewer/mediaviewer_addon.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>

#include "addons/vpn/service.h"
#include "io/file_port.h"
#include "wire/secrets.h"

#ifndef MV_VPN_VERSION
#define MV_VPN_VERSION "0.0.0-dev"
#endif

namespace {

namespace vpn = mv::driftwire::vpn;
namespace secrets = mv::driftwire::secrets;

constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
constexpr const char* kScreen = "settings";  // manifest.json, place "settings"
constexpr const char* kPhoneAccess = "phone-access";

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

#define MV_VPN_HAS(field) \
  (h_->struct_size >= offsetof(mv_host_api, field) + sizeof(h_->field) && h_->field != nullptr)

class host_bridge final : public vpn::service_host {
 public:
  explicit host_bridge(const mv_host_api* h) : h_(h) {}

  void opened(uint64_t generation) {
    std::lock_guard lock(m_);
    generation_ = generation;
    open_ = true;
    last_.clear();
  }
  void closed() {
    std::lock_guard lock(m_);
    open_ = false;
  }

  bool settings_shown() override {
    std::lock_guard lock(m_);
    return open_;
  }

  void post_settings(const std::string& tree_json) override {
    uint64_t g = 0;
    {
      std::lock_guard lock(m_);
      if (!open_ || tree_json == last_) return;
      last_ = tree_json;
      g = generation_;
    }
    (void)h_->screen_post(h_->host, kScreen, g, tree_json.c_str());
  }

  void log(int level, const char* ascii) override {
    if (h_->log) h_->log(h_->host, level, ascii);
  }

  bool keep_alive(bool on, const std::string& label) override {
    if (!MV_VPN_HAS(keep_alive)) return true;  // an older host: nothing to keep
    return h_->keep_alive(h_->host, on ? 1 : 0, kScreen, label.c_str()) == MV_OK;
  }

  bool post_menu(const std::string& menu_json) override {
    if (!MV_VPN_HAS(keep_menu)) return true;
    return h_->keep_menu(h_->host, menu_json.c_str()) == MV_OK;
  }

  std::optional<std::string> ask_phone(const std::string& method, const std::string& json) override {
    if (!MV_VPN_HAS(addon_request)) return std::nullopt;
    std::optional<std::string> answer;
    const auto keep = [](void* user, const char* reply) {
      auto* out = static_cast<std::optional<std::string>*>(user);
      *out = reply ? std::string(reply) : std::string("{}");
    };
    if (h_->addon_request(h_->host, kPhoneAccess, method.c_str(), json.c_str(), keep, &answer) != MV_OK) {
      return std::nullopt;
    }
    return answer;
  }

  std::string data_dir() {
    char buf[4096];
    if (!h_->data_dir || h_->data_dir(h_->host, buf, sizeof buf) != MV_OK || !*buf) return {};
    return buf;
  }

 private:
  const mv_host_api* h_;
  std::mutex m_;
  uint64_t generation_ = 0;
  bool open_ = false;
  std::string last_;
};
#undef MV_VPN_HAS

struct addon_state {
  const mv_host_api* host = nullptr;
  std::unique_ptr<host_bridge> bridge;
  std::string data_dir, prefs_path;
  bool offline = false;
  std::mutex m;  // the service's making (the host's worker, or load)
  // Declared last: it stops before the bridge it calls goes.
  std::unique_ptr<vpn::service> svc;

  vpn::service& get() {
    std::lock_guard lock(m);
    if (!svc) {
      vpn::service::options o;
      o.offline = offline;
      o.prefs_path = prefs_path;
      o.data_dir = data_dir;
      if (!offline) {
        o.make_secrets = [] { return secrets::os_store(vpn::kSecretService); };
        // The system proxy: this OS's setting, changed only while the Tunnel
        // and Use for all apps are on.
        o.make_sysproxy = [] { return mv::driftwire::sysproxy::os_port(); };
      }
      svc = std::make_unique<vpn::service>(*bridge, std::move(o));
    }
    return *svc;
  }

  // At load: the Tunnel was on at the last launch (or a crash left the
  // system proxy pointing at it), so it comes back now. Otherwise one small
  // read and no thread or socket until the page opens.
  void start_if_kept() {
    if (prefs_path.empty()) return;
    const auto kept = vpn::load_prefs(prefs_path);
    const bool leftover = mv::io::stat_path(mv::io::join_path(data_dir, vpn::kProxyRecordFile)).has_value();
    if ((kept && kept->tunnel) || leftover) (void)get();
  }
};

addon_state* g = nullptr;

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id || std::strcmp(r->screen_id, kScreen) != 0) return MV_ERR_INVALID_ARG;
    g->bridge->opened(r->generation);
    g->get().settings_opened();
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id || !e->screen_id || std::strcmp(e->screen_id, kScreen) != 0) return MV_OK;
    if (e->kind == MV_SLOT_EVENT_CLOSE) {
      g->bridge->closed();
      if (g->svc) g->svc->settings_closed();
      return MV_OK;
    }
    if (e->kind == MV_SLOT_EVENT_MENU) {
      // A row of the kept menu: the page's switch of the same id.
      const std::string node = e->node_id;
      if (node == "tunnel" || node == "all_traffic") {
        g->get().on_event(node, 2 /* change */, e->value_json ? e->value_json : "");
      }
      return MV_OK;
    }
    g->get().on_event(e->node_id, e->kind, e->value_json ? e->value_json : "");
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

// What another add-on may ask (mv.request.1, through the host):
//   status   {"tunnel": bool}: this add-on is here, and whether the Tunnel is
//            on. Driftwire asks, to stop pointing at the VPN add-on once it is.
mv_status MV_CALL handle_request(void*, const char* /*caller_id*/, const char* method, const char* /*request_json*/,
                                 mv_addon_reply_fn reply, void* user) {
  return guard([&] {
    if (!method || std::strcmp(method, "status") != 0 || !reply) return MV_ERR_NOT_FOUND;
    bool on = false;
    {
      std::lock_guard lock(g->m);
      if (g->svc) {
        on = g->svc->snapshot().tunnel;
      } else if (const auto kept = vpn::load_prefs(g->prefs_path)) {
        on = kept->tunnel;
      }
    }
    reply(user, on ? "{\"tunnel\":true}" : "{\"tunnel\":false}");
    return MV_OK;
  });
}

const mv_addon_request_api kRequests{sizeof(mv_addon_request_api), 0, &handle_request};

const void* MV_CALL query(void*, const char* id) {
  if (id && std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  if (id && std::strcmp(id, MV_REQUEST_INTERFACE) == 0) return &kRequests;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  // The proxy or the VPN stops, the system proxy goes back and every thread
  // ends before this returns, so the library can go.
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (host_api < kHostApiMin || host_api > kHostApiMax || !host || !out ||
        host->struct_size < offsetof(mv_host_api, screen_show) + sizeof(host->screen_show) || !host->screen_post) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (g) return MV_ERR_INVALID_ARG;  // one instance per process
    auto state = std::make_unique<addon_state>();
    state->host = host;
    state->bridge = std::make_unique<host_bridge>(host);
    // The loader test opens the page without touching the network or the keychain.
    const char* offline = std::getenv("MV_VPN_OFFLINE");
    state->offline = offline && offline[0] == '1';
    if (const std::string dir = state->bridge->data_dir(); !dir.empty()) {
      state->data_dir = dir;
      state->prefs_path = mv::io::join_path(dir, vpn::kPrefsFile);
      // The Tunnel was Driftwire's until 2026-11-19: its settings come over once.
      if (!state->offline) {
        auto theirs = secrets::os_store(secrets::kAppService);
        auto mine = secrets::os_store(vpn::kSecretService);
        (void)vpn::adopt_driftwire(vpn::driftwire_data_dir(dir), dir, state->prefs_path, theirs.get(), mine.get());
      }
    }
    g = state.release();
    g->start_if_kept();
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "vpn";
    out->version = MV_VPN_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}

// Removed without keeping its data: the VPN server's key, which is in the OS's
// secret store, not the data folder the host deletes. Not loaded: nothing runs.
extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_forget(const char* /*data_dir_utf8*/) {
  return guard([&] {
    if (auto store = secrets::os_store(vpn::kSecretService)) (void)store->erase(vpn::kServerKeyName);
    return MV_OK;
  });
}
