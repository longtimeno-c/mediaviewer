// SPDX-License-Identifier: GPL-2.0-or-later
// Add-on management over the C ABI (mediaviewer_addon.h, plan/18 "Add-ons"),
// for the Windows chrome. The Mac host drives src/addon directly.
//
// The core never downloads anything: the chrome fetches the manifest, asks
// here whether it is signed, fetches and hashes the archive, extracts it into
// a staging folder, and asks here to verify and install. Loading re-verifies.
#include <mediaviewer/mediaviewer_addon.h>

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "abi/addon_bridge.h"
#include "abi/guard.h"
#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/store.h"
#include "core/json.h"
#include "image/thumb.h"
#include "io/file.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "io/volume.h"
#include "meta/meta.h"

namespace {

using mv::status;

std::atomic<bool> g_present_busy{false};

struct loaded_entry {
  std::unique_ptr<mv::addon::loaded_addon> addon;
  mv_session_t session = nullptr;
};

std::mutex g_mutex;

struct base_watch {
  mv::io::volume_watcher watcher;
  mv_session_t session = nullptr;
};
std::mutex g_watch_mutex;
std::unique_ptr<base_watch> g_watch;

void on_base_volume(void* user, mv::io::volume_event event, const char* root) {
  auto* w = static_cast<base_watch*>(user);
  if (event != mv::io::volume_event::arrived || !root) return;
  // Asks the OS about the volume only (a statfs-level query), never reads it.
  const auto v = mv::io::volume_of(root);
  mv_completion c{};
  c.kind = MV_COMPLETION_ADDON;
  c.status = MV_OK;
  c.generation = MV_ADDON_EVENT_VOLUME_ARRIVED;
  c.payload = v && v->removable ? 1 : 0;
  mv::abi::push_addon_completion(w->session, c);
}
std::map<std::string, loaded_entry> g_loaded;

mv::image::thumb_store& addon_thumbs() {
  static mv::image::thumb_store store;
  return store;
}

mv::result<mv::addon::store> open_store() {
  MV_TRY(std::string root, mv::io::addons_dir());
  auto key = mv::addon::pinned_public_key();
  return mv::addon::store(std::move(root), std::vector<std::uint8_t>(key.begin(), key.end()),
                          MV_ADDON_HOST_API);
}

status write_out(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (!out || cap < need) return status::invalid_arg;
  std::memcpy(out, text.c_str(), need);
  return status::ok;
}

const char* state_name(mv::addon::install_state s) {
  switch (s) {
    case mv::addon::install_state::ok: return "ok";
    case mv::addon::install_state::needs_update: return "needs_update";
    default: return "invalid";
  }
}

// The viewer's capture date and camera for Import's layouts (mv_meta, PR 9).
bool capture_info(const std::string& path, mv_addon_capture& out) {
  auto m = mv::meta::read(path);
  if (!m) return false;
  if (m->s.date_taken_key != 0) {
    out.taken_unix = m->s.date_taken_key;
    out.has_date = 1;
  }
  const std::string& camera = m->s.camera;
  const std::size_t n = std::min(camera.size(), sizeof(out.camera_utf8) - 1);
  std::memcpy(out.camera_utf8, camera.data(), n);
  out.camera_utf8[n] = '\0';
  return true;
}

// The viewer's own JPEG-512 cache: a hit, or made now on the calling worker.
// Stills only; a clip's poster needs the player and shows as a ▶ tile.
mv::result<std::string> thumbnail(const std::string& path) {
  auto& store = addon_thumbs();
  if (!store.is_open()) {
    MV_TRY(std::string dir, mv::io::thumb_cache_dir());
    MV_TRY_VOID(store.open(dir));
  }
  MV_TRY(mv::io::file_stat st, mv::io::stat_path(path));
  const mv::image::thumb_key key{path, st.mtime_unix, st.size};
  MV_TRY(std::string hit, store.lookup(key));
  if (!hit.empty()) return hit;
  MV_TRY(auto bytes, mv::io::read_all(path));
  MV_TRY(auto jpeg, mv::image::make_thumb_jpeg(bytes));
  return store.store(key, jpeg);
}

}  // namespace

extern "C" {

MV_API void MV_CALL mv_present_set_busy(uint32_t busy) {
  g_present_busy.store(busy != 0, std::memory_order_relaxed);
}

MV_API mv_status MV_CALL mv_volume_watch(mv_session_t session, uint32_t enable) {
  return static_cast<mv_status>(mv::abi::guard("mv_volume_watch", [&] {
    MV_REQUIRE(session || !enable, "session is required");
    std::unique_ptr<base_watch> old;
    std::lock_guard lock(g_watch_mutex);
    if (g_watch) {
      g_watch->watcher.stop();
      if (g_watch->session) (void)mv_session_release(g_watch->session);
      old = std::move(g_watch);
    }
    if (!enable) return status::ok;
    auto w = std::make_unique<base_watch>();
    w->session = session;
    (void)mv_session_retain(session);
    if (auto started = w->watcher.start(&on_base_volume, w.get()); !started) {
      (void)mv_session_release(session);
      return started.error();
    }
    g_watch = std::move(w);
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_addon_installed_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_installed_json", [&] {
    auto s = open_store();
    if (!s) return s.error();
    mv::json::writer w;
    w.begin_array();
    for (const mv::addon::installed& i : s->list()) {
      bool loaded = false;
      {
        std::lock_guard lock(g_mutex);
        loaded = g_loaded.count(i.id) != 0;
      }
      w.begin_object();
      w.key("id").string(i.id);
      w.key("name").string(i.name);
      w.key("version").string(i.version);
      w.key("dir").string(i.dir);
      w.key("size").integer(static_cast<std::int64_t>(i.size));
      w.key("state").string(state_name(i.state));
      w.key("why").string(mv::addon::rejection_name(i.why));
      w.key("loaded").boolean(loaded);
      w.end_object();
    }
    w.end_array();
    return write_out(w.str(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_addon_check_manifest(const void* manifest, uint32_t manifest_len,
                                                 const void* signature, uint32_t signature_len,
                                                 char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_check_manifest", [&] {
    MV_REQUIRE(manifest && manifest_len > 0, "manifest is empty");
    const auto key = mv::addon::pinned_public_key();
    const auto d = mv::addon::check_manifest(
        std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(manifest), manifest_len),
        std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(signature),
                                      signature ? signature_len : 0),
        key, MV_ADDON_HOST_API);
    mv::json::writer w;
    w.begin_object();
    w.key("ok").boolean(d.trusted());
    w.key("why").string(mv::addon::rejection_name(d.why));
    // Nothing unauthenticated is echoed back.
    const bool signed_ok = d.trusted() || d.why == mv::addon::rejection::needs_update ||
                           d.why == mv::addon::rejection::wrong_platform ||
                           d.why == mv::addon::rejection::unsupported_schema;
    if (signed_ok) {
      w.key("id").string(d.m.id);
      w.key("name").string(d.m.name);
      w.key("version").string(d.m.version);
      w.key("installed_size").integer(static_cast<std::int64_t>(d.m.installed_size));
      w.key("archive").begin_object();
      w.key("path").string(d.m.archive.path);
      w.key("sha256").string(d.m.archive.sha256);
      w.key("size").integer(static_cast<std::int64_t>(d.m.archive.size));
      w.end_object();
    }
    w.end_object();
    return write_out(w.str(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_addon_sha256_file(const char* path_utf8, char out[65]) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_sha256_file", [&] {
    MV_REQUIRE(path_utf8 && out, "path and out are required");
    const std::string hex = mv::addon::sha256_file(path_utf8);
    if (hex.size() != 64) return status::io;
    std::memcpy(out, hex.c_str(), 65);
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_addon_make_staging(char* out_utf8, uint32_t cap) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_make_staging", [&] {
    auto s = open_store();
    if (!s) return s.error();
    auto dir = s->make_staging();
    if (!dir) return dir.error();
    return write_out(*dir, out_utf8, cap, nullptr);
  }));
}

MV_API mv_status MV_CALL mv_addon_install(const char* staged_dir_utf8) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_install", [&] {
    MV_REQUIRE(staged_dir_utf8, "staged_dir is required");
    auto s = open_store();
    if (!s) return s.error();
    auto installed = s->install(staged_dir_utf8);
    return installed ? status::ok : installed.error();
  }));
}

MV_API mv_status MV_CALL mv_addon_unload(const char* id) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_unload", [&] {
    MV_REQUIRE(id, "id is required");
    loaded_entry gone;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_loaded.find(id);
      if (it == g_loaded.end()) return status::ok;
      gone = std::move(it->second);
      g_loaded.erase(it);
    }
    gone.addon.reset();  // shutdown, then unload; outside the lock
    if (gone.session) (void)mv_session_release(gone.session);
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_addon_remove(const char* id, uint32_t keep_data) {
  (void)mv_addon_unload(id);
  return static_cast<mv_status>(mv::abi::guard("mv_addon_remove", [&] {
    MV_REQUIRE(id, "id is required");
    auto s = open_store();
    if (!s) return s.error();
    auto removed = s->remove(id, keep_data != 0);
    return removed ? status::ok : removed.error();
  }));
}

MV_API mv_status MV_CALL mv_addon_load(mv_session_t session, const char* id,
                                       const char* interface_id, const void** out_interface,
                                       char* out_chrome_utf8, uint32_t chrome_cap) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_load", [&] {
    MV_REQUIRE(session && id && interface_id && out_interface, "bad arguments");
    std::lock_guard lock(g_mutex);
    auto it = g_loaded.find(id);
    if (it == g_loaded.end()) {
      auto s = open_store();
      if (!s) return s.error();
      // Once per process, at the first load: a later load (after an install)
      // must not sweep .staging while the chrome is downloading into it.
      static bool cleaned = false;  // guarded by g_mutex
      if (!cleaned) {
        s->startup_cleanup();
        cleaned = true;
      }
      mv::addon::host_services svc;
      svc.capture = &capture_info;
      svc.thumbnail = &thumbnail;
      svc.should_yield = [] { return g_present_busy.load(std::memory_order_relaxed); };
      svc.post = [session](const mv_addon_event& e) {
        mv_completion c{};
        c.kind = MV_COMPLETION_ADDON;
        c.status = e.status;
        c.job_id = e.id;
        c.generation = e.kind;
        c.payload = e.payload;
        mv::abi::push_addon_completion(session, c);
      };
      if (auto lib = mv::io::default_library_dir()) svc.default_library = *lib;
      auto loaded = mv::addon::loaded_addon::load(*s, id, std::move(svc));
      if (!loaded) return loaded.error();
      (void)mv_session_retain(session);
      it = g_loaded.emplace(id, loaded_entry{std::move(*loaded), session}).first;
    }
    const void* iface = it->second.addon->query(interface_id);
    if (!iface) return status::unsupported_format;
    *out_interface = iface;
    if (out_chrome_utf8 && chrome_cap > 0) {
      const auto& info = it->second.addon->info();
      const std::string chrome =
          mv::io::join_path(info.dir, mv::io::native_relative(info.m.chrome));
      if (write_out(chrome, out_chrome_utf8, chrome_cap, nullptr) != status::ok) {
        return status::invalid_arg;
      }
    }
    return status::ok;
  }));
}

}  // extern "C"
