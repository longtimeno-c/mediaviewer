// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Script add-ons over the C ABI (mediaviewer_addon.h "Script add-ons", docs/design/25
// §9 A, PR 57), for the Windows shell and chrome. The Mac host drives
// src/addon/script_host.h directly (shell/addons_mac.mm); both read the same
// rows and words.
//
// One script_host per process. Its scripts read the viewer through the
// session's folder model ([any-thread][no-block] calls) and talk back through
// the shell's listener (mv_script_set_listener), a documented any-thread
// callback the shell marshals to its UI thread (docs/design/14 "C ABI"): 1 when
// a notice is waiting (mv_script_take_notices_json), 2 when the set or a state
// changed (re-read mv_script_commands_json and mv_script_status_json). Not the
// completion queue: the chrome drains that, and these are the shell's.
#include <mediaviewer/mediaviewer.h>
#include <mediaviewer/mediaviewer_addon.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "abi/guard.h"
#include "addon/open_json.h"
#include "addon/script_host.h"
#include "core/json.h"
#include "io/paths.h"

namespace {

using mv::status;

struct script_state {
  std::mutex m;  // reload / restart / stop: one at a time
  std::unique_ptr<mv::addon::script_host> host;
  // Made once and never replaced: the UI thread reads through this without
  // waiting on a reload (rule 1); the host locks itself, briefly.
  std::atomic<mv::addon::script_host*> live{nullptr};
  mv_session_t session = nullptr;
  std::mutex notices_m;
  std::deque<std::pair<std::string, std::string>> notices;  // addon, text
  std::mutex listener_m;
  void(MV_CALL* listener)(void*, uint32_t) = nullptr;
  void* listener_user = nullptr;
};

script_state& state() {
  static script_state* const s = new script_state;  // workers may outlive statics at exit
  return *s;
}

status write_out(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (!out || cap < need) return status::invalid_arg;
  std::memcpy(out, text.c_str(), need);
  return status::ok;
}

void post(uint32_t what) {
  script_state& st = state();
  std::lock_guard<std::mutex> lock(st.listener_m);
  if (st.listener) st.listener(st.listener_user, what);
}

// A string the session hands out by the caller-buffer rule (mv_folder_item_*):
// one try on the stack, a second at the size it reported.
template <typename Get>
std::string session_string(Get get) {
  char small[512] = {};
  uint32_t need = 0;
  const mv_status first = get(small, static_cast<uint32_t>(sizeof(small)), &need);
  if (need <= sizeof(small)) return first == MV_OK ? std::string(small) : std::string();
  if (need > (1u << 16)) return {};
  std::string big(need, '\0');
  if (get(big.data(), need, &need) != MV_OK) return {};
  big.resize(std::strlen(big.c_str()));
  return big;
}

mv::addon::script_services services(mv_session_t session) {
  mv::addon::script_services s;
  s.folder_count = [session] {
    uint32_t n = 0;
    return mv_folder_count(session, &n) == MV_OK ? n : 0u;
  };
  s.folder_path = [session] {
    return session_string([session](char* b, uint32_t c, uint32_t* n) {
      return mv_folder_directory(session, b, c, n);
    });
  };
  s.folder_item_name = [session](uint32_t i) {
    return session_string([session, i](char* b, uint32_t c, uint32_t* n) {
      return mv_folder_item_name(session, i, b, c, n);
    });
  };
  s.current_path = [session] {
    uint32_t i = 0;
    if (mv_folder_selected(session, &i) != MV_OK) return std::string();
    return session_string([session, i](char* b, uint32_t c, uint32_t* n) {
      return mv_folder_item_path(session, i, b, c, n);
    });
  };
  s.notify = [](const std::string& addon, const std::string& text) {
    script_state& st = state();
    {
      std::lock_guard<std::mutex> lock(st.notices_m);
      if (st.notices.size() >= 64) st.notices.pop_front();
      st.notices.emplace_back(addon, text);
    }
    post(1);
  };
  // The add-on's own words stay on this machine (rule 6). Windows keeps no log
  // of them; Settings shows the last error.
  return s;
}

mv::addon::script_host& host_locked(mv_session_t session) {
  script_state& st = state();
  if (!st.host) {
    st.session = session;
    (void)mv_session_retain(session);
    st.host = std::make_unique<mv::addon::script_host>(services(session));
    st.host->set_on_change([] { post(2); });
    st.live.store(st.host.get());
  }
  return *st.host;
}

// A verify rig's environment variable (docs/design/25 PR 57); "" when unset.
std::string rig_env(const char* name) {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

}  // namespace

namespace mv::abi {
// An open add-on was installed or removed (addon_abi.cpp): the running set
// follows the store, if scripts were ever started. The caller's thread is a
// worker (those calls hash files).
void scripts_store_changed() noexcept {
  try {
    script_state& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    if (!st.host) return;
    auto store = mv::addon::default_open_store();
    if (store) st.host->reload(*store);
  } catch (...) {
  }
}
}  // namespace mv::abi

extern "C" {

MV_API mv_status MV_CALL mv_script_set_listener(void(MV_CALL* listener)(void* user, uint32_t what),
                                                void* user) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_set_listener", [&] {
    script_state& st = state();
    std::lock_guard<std::mutex> lock(st.listener_m);
    st.listener = listener;
    st.listener_user = user;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_script_reload(mv_session_t session) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_reload", [&] {
    MV_REQUIRE(session, "session is null");
    script_state& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    MV_REQUIRE(!st.host || st.session == session, "scripts run against one session");
    // The verify rigs: a scratch store (MV_SCRIPT_STORE) instead of the
    // profile's, and MV_SCRIPT_SPIN=<addon>/<command> kept running back to back
    // for the present-loop gates. Inert unless set.
    if (const std::string dir = rig_env("MV_SCRIPT_STORE"); !dir.empty()) {
      mv::io::set_open_addons_dir_override(dir);
    }
    auto store = mv::addon::default_open_store();
    if (!store) return store.error();
    host_locked(session).reload(*store);
    if (const std::string spin = rig_env("MV_SCRIPT_SPIN"); spin.find('/') != std::string::npos) {
      host_locked(session).start_spin(spin.substr(0, spin.find('/')), spin.substr(spin.find('/') + 1));
    }
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_script_restart(const char* addon_id) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_restart", [&] {
    MV_REQUIRE(addon_id && *addon_id, "addon_id is empty");
    script_state& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    if (!st.host) return status::not_found;
    auto store = mv::addon::default_open_store();
    if (!store) return store.error();
    st.host->restart(*store, addon_id);
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_script_stop_all(void) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_stop_all", [&] {
    script_state& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    if (st.host) st.host->stop_all();
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_script_commands_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_commands_json", [&] {
    script_state& st = state();
    mv::json::writer w;
    w.begin_array();
    // The host never goes away once made; it locks itself, briefly.
    if (mv::addon::script_host* host = st.live.load()) {
      for (const mv::addon::script_command_row& r : host->commands()) {
        w.begin_object();
        w.key("addon").string(r.addon);
        w.key("id").string(r.id);
        w.key("name").string(r.name);
        w.key("key").string(r.key);
        w.key("modes").string(r.modes);
        w.key("payload").string(r.payload);
        w.end_object();
      }
    }
    w.end_array();
    return write_out(w.take(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_script_run_command(const char* addon_id, const char* command_id,
                                               const char* payload_json) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_run_command", [&] {
    MV_REQUIRE(addon_id && command_id, "addon_id and command_id are required");
    mv::addon::script_host* host = state().live.load();
    if (!host) return status::not_found;
    return host->run_command(addon_id, command_id, payload_json ? payload_json : "null")
               ? status::ok
               : status::busy;
  }));
}

MV_API mv_status MV_CALL mv_script_set_key_notes(const char* notes_json) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_set_key_notes", [&] {
    MV_REQUIRE(notes_json, "notes_json is null");
    if (mv::addon::script_host* host = state().live.load()) host->set_key_notes(notes_json);
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_script_status_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_status_json", [&] {
    mv::addon::script_host* host = state().live.load();
    return write_out(host ? host->status_json() : std::string("[]"), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_script_take_notices_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_script_take_notices_json", [&] {
    script_state& st = state();
    std::lock_guard<std::mutex> lock(st.notices_m);
    mv::json::writer w;
    w.begin_array();
    for (const auto& [addon, text] : st.notices) {
      w.begin_object().key("addon").string(addon).key("text").string(text).end_object();
    }
    w.end_array();
    const std::string json = w.take();
    const status s = write_out(json, out, cap, needed);
    if (s == status::ok) st.notices.clear();  // taken only when the caller had room
    return s;
  }));
}

}  // extern "C"
