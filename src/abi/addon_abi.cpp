// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Add-on management over the C ABI (mediaviewer_addon.h, docs/design/18 "Add-ons"),
// for the Windows chrome. The Mac host drives src/addon directly.
//
// The core never downloads anything: the chrome fetches the manifest, asks
// here whether it is signed, fetches and hashes the archive, extracts it into
// a staging folder, and asks here to verify and install. Loading re-verifies.
#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_ai.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "abi/addon_bridge.h"
#include "abi/guard.h"
#include "addon/host.h"
#include "addon/index_locations.h"
#include "addon/manifest.h"
#include "addon/open_json.h"
#include "abi/addon_media.h"
#include "addon/store.h"
#include "core/json.h"
#include "image/thumb.h"
#include "io/file.h"
#include "io/file_ops.h"
#include "io/file_port.h"
#include "io/paths.h"
#include "io/volume.h"
#include "meta/location.h"
#include "meta/meta.h"
#include "meta/cues.h"
#include "meta/meta_cache.h"

namespace mv::abi {
void scripts_store_changed() noexcept;  // script_abi.cpp (docs/design/25 PR 57)
}  // namespace mv::abi

namespace {

using mv::status;

std::atomic<bool> g_present_busy{false};

struct loaded_entry {
  std::unique_ptr<mv::addon::loaded_addon> addon;
  mv_session_t session = nullptr;
  bool reader = false;  // through MV_AI_READER_ENTRY_SYMBOL (mv_addon_load_reader)
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

// docs/design/25: what a manifest contributes, as the chromes and the shell read it.
void write_contributions(mv::json::writer& w, const mv::addon::manifest& m) {
  w.key("description").string(m.description);
  // docs/design/25 PRs 58-59: it fills screens or slots (load it with
  // MV_SLOTS_INTERFACE to attach them).
  w.key("slots").boolean(!m.slots.empty());
  w.key("hint_on").string(m.hint_on);
  w.key("hint_text").string(m.hint_text);
  // PR 56: whether Settings has a page for it (mv_addon_settings_json).
  w.key("settings").boolean(!m.settings.empty());
  w.key("commands").begin_array();
  for (const mv::addon::manifest_command& c : m.commands) {
    w.begin_object();
    w.key("addon").string(m.id);
    w.key("id").string(c.id);
    w.key("name").string(c.name);
    w.key("windows").string(c.windows);
    w.key("mac").string(c.mac);
    w.key("modes").string(c.modes);
    w.key("payload").string(c.payload);
    w.end_object();
  }
  w.end_array();
}

const char* state_name(mv::addon::install_state s) {
  switch (s) {
    case mv::addon::install_state::ok: return "ok";
    case mv::addon::install_state::needs_update: return "needs_update";
    default: return "invalid";
  }
}

// #291: the host table's tag_facts, from meta_cache.db (a bounded read on a miss).
bool tag_facts(const std::string& path, mv_addon_tag_facts& out) {
  auto row = mv::meta::cached_or_read(mv::meta::default_meta_cache(), path);
  if (!row) return false;
  const mv::meta::search_facts& f = row->facts;
  const auto put = [](char* dst, std::size_t cap, const std::string& src) {
    std::size_t n = std::min(src.size(), cap - 1);
    while (n > 0 && n < src.size() && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) --n;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
  };
  if (f.date_taken_key != 0) {
    out.flags |= MV_ADDON_TAG_HAS_DATE;
    out.taken_unix = f.date_taken_key;
  }
  if (f.where) {
    out.flags |= MV_ADDON_TAG_HAS_LOCATION;
    out.lat = f.where->lat;
    out.lon = f.where->lon;
  }
  if (f.ai_generated) out.flags |= MV_ADDON_TAG_AI_GENERATED;
  put(out.camera_utf8, sizeof(out.camera_utf8), f.camera);
  put(out.lens_utf8, sizeof(out.lens_utf8), f.lens);
  put(out.made_with_utf8, sizeof(out.made_with_utf8), f.made_with);
  put(out.fields_utf8, sizeof(out.fields_utf8), mv::meta::joined_fields(f.fields, sizeof(out.fields_utf8)));
  // #405: rating, label and exposure (the table's struct_size says they are there).
  out.rating = f.rating;
  out.iso = f.iso;
  out.aperture_x100 = f.aperture_x100;
  out.focal_mm = f.focal_mm;
  out.shutter_den = f.shutter_den;
  out.shutter_us = f.shutter_us;
  put(out.label_utf8, sizeof(out.label_utf8), f.label);
  return true;
}

// #406: a clip's chapter titles and text subtitle cues for the AI pack (meta/cues.h).
mv::expected read_cues(const std::string& path, std::int64_t start_ms, std::int64_t max_ms, std::uint32_t max_cues,
                       const std::function<bool(const mv::addon::host_services::cue&)>& visit) {
  mv::meta::cue_limits lim;
  lim.start_ms = start_ms;
  lim.max_ms = max_ms;
  lim.max_cues = max_cues;
  return mv::meta::read_cues(path, lim, [&](const mv::meta::cue& c) {
    mv::addon::host_services::cue out;
    out.kind = c.kind == mv::meta::cue_kind::chapter ? MV_ADDON_CUE_CHAPTER : MV_ADDON_CUE_SUBTITLE;
    out.start_ms = c.start_ms;
    out.end_ms = c.end_ms;
    out.text = c.text;
    return visit(out);
  });
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

// C++ linkage, as addon_bridge.h declares it: inside the extern "C" block
// below it is "a different language linkage" (MSVC C2732, clang-cl error).
namespace mv::abi {
bool present_busy() noexcept { return g_present_busy.load(std::memory_order_relaxed); }
}  // namespace mv::abi

extern "C" {

// docs/design/25 PR 58: a screen's thumbnail / thumbnail_grid tile, by path.
// [worker-thread]. A clip has no poster here (the player makes those), and is
// refused before anything is read.
MV_API mv_status MV_CALL mv_slots_thumbnail_path(const char* path_utf8, char* out, uint32_t cap,
                                                 uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_thumbnail_path", [&] {
    MV_REQUIRE(path_utf8, "path required");
    std::string lower(path_utf8);
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (const char* ext : {".mp4", ".mov", ".m4v", ".mkv", ".webm", ".avi", ".ts", ".mts", ".m2ts"}) {
      const std::size_t n = std::strlen(ext);
      if (lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0) return status::unsupported_format;
    }
    auto made = thumbnail(path_utf8);
    if (!made) return made.error();
    return write_out(*made, out, cap, needed);
  }));
}

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
      // Milestone H: pieces name their parent ("ai-faces" is part_of "ai").
      w.key("part_of").string(i.m.part_of);
      w.key("installed_size").integer(static_cast<std::int64_t>(i.m.installed_size));
      write_contributions(w, i.m);
      w.end_object();
    }
    w.end_array();
    return write_out(w.str(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_addon_slot_addons_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_slot_addons_json", [&] {
    auto s = open_store();
    if (!s) return s.error();
    mv::json::writer w;
    w.begin_array();
    // Manifests only: no file is hashed (issue #402: a later window must not
    // hash the AI pack to find Places). The load verifies the one it loads.
    for (const mv::addon::store::listed& l : s->manifests()) {
      if (!mv::addon::loads_for_slots(l.m)) continue;
      bool loaded = false;
      {
        std::lock_guard lock(g_mutex);
        loaded = g_loaded.count(l.m.id) != 0;
      }
      w.begin_object();
      w.key("id").string(l.m.id);
      w.key("name").string(l.m.name);
      w.key("version").string(l.m.version);
      w.key("description").string(l.m.description);
      w.key("state").string(state_name(l.state));
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
      // docs/design/25: Settings' line and the card hint come from the manifest.
      w.key("description").string(d.m.description);
      w.key("hint_on").string(d.m.hint_on);
      w.key("hint_text").string(d.m.hint_text);
    }
    w.end_object();
    return write_out(w.str(), out, cap, needed);
  }));
}

// ---- open add-ons (docs/design/25) ---------------------------------------------------

MV_API mv_status MV_CALL mv_open_addon_inspect(const char* package_utf8, char* out, uint32_t cap,
                                               uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_open_addon_inspect", [&] {
    MV_REQUIRE(package_utf8 && *package_utf8, "package path is empty");
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    return write_out(mv::addon::open_inspect_json(*s, package_utf8), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_open_addon_install(const char* package_utf8,
                                               const char* approved_sha256, char* out,
                                               uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_open_addon_install", [&] {
    MV_REQUIRE(package_utf8 && *package_utf8, "package path is empty");
    MV_REQUIRE(approved_sha256, "approved_sha256 is null");
    MV_REQUIRE(out && cap >= 1024, "the result buffer is too small to say what happened");
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    const status written =
        write_out(mv::addon::open_install_json(*s, package_utf8, approved_sha256), out, cap, needed);
    mv::abi::scripts_store_changed();  // a script add-on starts, an update replaces the old one
    return written;
  }));
}

MV_API mv_status MV_CALL mv_open_addon_list_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_open_addon_list_json", [&] {
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    return write_out(mv::addon::open_list_json(*s), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_open_addon_remove(const char* folder) {
  return static_cast<mv_status>(mv::abi::guard("mv_open_addon_remove", [&] {
    MV_REQUIRE(folder && *folder, "folder is empty");
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    auto removed = s->remove(folder);
    if (removed) mv::abi::scripts_store_changed();  // its script stops and its commands go
    return removed ? status::ok : removed.error();
  }));
}

MV_API mv_status MV_CALL mv_open_addon_theme_json(const char* addon_id, const char* theme_id,
                                                  char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_open_addon_theme_json", [&] {
    MV_REQUIRE(addon_id && theme_id, "addon_id and theme_id are required");
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    auto theme = s->theme_json(addon_id, theme_id);
    if (!theme) return theme.error();
    return write_out(*theme, out, cap, needed);
  }));
}

// PR 56 (docs/design/25): settings pages and keymap packs.

MV_API mv_status MV_CALL mv_addon_settings_json(const char* addon_id, char* out, uint32_t cap,
                                                uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_settings_json", [&] {
    MV_REQUIRE(addon_id && *addon_id, "addon_id is empty");
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    auto first = open_store();
    const std::string page =
        mv::addon::addon_settings_page_json(*s, first ? &*first : nullptr, addon_id);
    if (page.empty()) return status::not_found;
    return write_out(page, out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_addon_setting_set(const char* addon_id, const char* key,
                                              const char* value_json, char* out, uint32_t cap,
                                              uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_setting_set", [&] {
    MV_REQUIRE(addon_id && key && value_json, "addon_id, key and value_json are required");
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    auto first = open_store();
    return write_out(mv::addon::addon_setting_set_json(*s, first ? &*first : nullptr, addon_id,
                                                       key, value_json),
                     out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_addon_settings_forget(const char* addon_id) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_settings_forget", [&] {
    MV_REQUIRE(addon_id && *addon_id, "addon_id is empty");
    return mv::addon::addon_settings_forget(addon_id) ? status::ok : status::io;
  }));
}

MV_API mv_status MV_CALL mv_open_addon_keymap_json(const char* addon_id, const char* pack_id,
                                                   char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_open_addon_keymap_json", [&] {
    MV_REQUIRE(addon_id && pack_id, "addon_id and pack_id are required");
    auto s = mv::addon::default_open_store();
    if (!s) return s.error();
    const std::string pack = mv::addon::open_keymap_json(*s, addon_id, pack_id);
    if (pack.empty()) return status::not_found;
    return write_out(pack, out, cap, needed);
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

MV_API mv_status MV_CALL mv_addon_family_usage(const char* family, uint64_t* out_used,
                                               uint64_t* out_ceiling) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_family_usage", [&] {
    MV_REQUIRE(family && out_used && out_ceiling, "bad arguments");
    auto s = open_store();
    if (!s) return s.error();
    const auto room = s->family_usage(family);
    *out_used = room.used;
    *out_ceiling = room.ceiling;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_addon_commands_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_commands_json", [&] {
    mv::json::writer w;
    w.begin_array();
    {
      std::lock_guard lock(g_mutex);
      for (const auto& [id, entry] : g_loaded) {
        if (!entry.addon) continue;
        const mv::addon::manifest& m = entry.addon->info().m;
        for (const mv::addon::manifest_command& c : m.commands) {
          w.begin_object();
          w.key("addon").string(m.id);
          w.key("id").string(c.id);
          w.key("name").string(c.name);
          w.key("windows").string(c.windows);
          w.key("mac").string(c.mac);
          w.key("modes").string(c.modes);
          w.key("payload").string(c.payload);
          w.end_object();
        }
      }
    }
    w.end_array();
    return write_out(w.str(), out, cap, needed);
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
    // #398: the AI pack's index stops serving other add-ons' locations_of_index
    // first; a walk in flight stops at its next row, and none is inside the
    // pack when its library goes.
    if (std::string_view(id) == "ai") {
      mv::addon::retract_index_locations();
      mv::addon::wait_index_locations_idle();
    }
    gone.addon.reset();  // shutdown, then unload; outside the lock
    if (gone.session) (void)mv_session_release(gone.session);
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_addon_quit(void) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_quit", [&] {
    std::map<std::string, loaded_entry> all;
    {
      std::lock_guard lock(g_mutex);
      all.swap(g_loaded);  // static destruction finds nothing left to join
    }
    // #398: no index walk starts now; one in flight stops at its next row.
    // Not waited on: the pack is abandoned, never unmapped.
    mv::addon::retract_index_locations();
    for (auto& [id, entry] : all) {
      // The entry's session stays retained: a pack left running still posts.
      const bool ai_family = id == "ai" || id.rfind("ai-", 0) == 0;
      if (ai_family) {
        mv::addon::abandon_for_exit(std::move(entry.addon));
      } else {
        mv::addon::stop_for_exit(std::move(entry.addon));
      }
    }
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_addon_quit_wait(uint32_t timeout_ms) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_quit_wait", [&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    return mv::addon::wait_stopped_for_exit(deadline) ? status::ok : status::timeout;
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

}  // extern "C"

namespace {

// How a process loads an add-on (docs/design/18 "One host process").
enum class load_door {
  host,    // mv_addon_load: the window that hosts the add-ons; the first load sweeps the store
  reader,  // mv_addon_load_reader: the AI pack's read-only door, in a later window
  guest,   // mv_addon_load_guest: the add-on's own entry in a later window (issue #402)
};

// mv_addon_load, mv_addon_load_reader and mv_addon_load_guest. A reader or a
// guest is a window whose process does not host the add-ons: it leaves the
// store alone (never sweeps it: the host window may be installing into it).
mv::status load_addon(mv_session_t session, const char* id, const char* interface_id,
                      const void** out_interface, char* out_chrome_utf8, uint32_t chrome_cap,
                      load_door door) {
  const bool reader = door == load_door::reader;
  MV_REQUIRE(session && id && interface_id && out_interface, "bad arguments");
  std::lock_guard lock(g_mutex);
  auto it = g_loaded.find(id);
  // A process loads an add-on one way: the host's writer or a reader.
  MV_REQUIRE(it == g_loaded.end() || it->second.reader == reader, "loaded the other way in this process");
  if (it == g_loaded.end()) {
    auto s = open_store();
    if (!s) return s.error();
    // Once per process, at the first load: a later load (after an install)
    // must not sweep .staging while the chrome is downloading into it. A
    // reader never sweeps: the host window may be installing into it.
    static bool cleaned = false;  // guarded by g_mutex
    if (!cleaned && door == load_door::host) {
      s->startup_cleanup();
      cleaned = true;
    }
    mv::addon::host_services svc;
    svc.capture = &capture_info;
    svc.tag_facts = &tag_facts;
    svc.read_cues = &read_cues;
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
    // Host table v3 (docs/design/25 PR 59): a file's coordinates, from #291's
    // meta_cache.db (the folder's tag pass may have read it already; a miss is
    // one bounded read, stored for next time).
    svc.location = [](const std::string& path) -> std::optional<mv_addon_location> {
      auto row = mv::meta::cached_or_read(mv::meta::default_meta_cache(), path);
      if (!row || !row->facts.where) return std::nullopt;
      const mv::meta::geo_point* fix = &*row->facts.where;
      mv_addon_location l{};
      l.has = 1;
      l.lat = fix->lat;
      l.lon = fix->lon;
      if (fix->altitude) {
        l.has_altitude = 1;
        l.altitude_m = *fix->altitude;
      }
      return l;
    };
    if (auto lib = mv::io::default_library_dir()) svc.default_library = *lib;
    // Host table v2 (Milestone H): pixels from the viewer's own decoders.
    svc.still_rgb = &mv::addon::media::decode_still;
    svc.open_sampler = &mv::addon::media::open_sampler;
    svc.video_frame = &mv::addon::media::video_frame;
    svc.moment_thumbnail = &mv::addon::media::moment_thumbnail;
    svc.open_audio = &mv::addon::media::open_audio;
    svc.thumbnail_jpeg = &mv::addon::media::thumbnail_jpeg;
    svc.store_thumbnail_jpeg = &mv::addon::media::store_thumbnail_jpeg;
    // Find duplicates (PR 54): the Recycle Bin, refused where there is none.
    svc.recycle = [](const std::string& path) -> mv::result<bool> {
      auto r = mv::io::recycle_file(path);
      if (!r) return mv::err(r.error());
      return *r == mv::io::recycle_outcome::recycled;
    };
    auto loaded = mv::addon::loaded_addon::load(*s, id, std::move(svc),
                                                reader ? MV_AI_READER_ENTRY_SYMBOL : MV_ADDON_ENTRY_SYMBOL);
    if (!loaded) {
      // Why a library that verified would not load, for the person (no path).
      const std::string& why = mv::addon::shared_library::last_error();
      if (loaded.error() == mv::status::io && !why.empty()) {
        mv::abi::set_last_error(mv::abi::current_correlation_id(), why.c_str());
      }
      return loaded.error();
    }
    (void)mv_session_retain(session);
    it = g_loaded.emplace(id, loaded_entry{std::move(*loaded), session, reader}).first;
    // #398: while the AI pack is loaded (the writer or a reader: both read
    // index.db), its index's coordinates serve every add-on's
    // locations_of_index (Places' place: over the whole index).
    if (std::string_view(id) == "ai") {
      mv::addon::publish_index_locations(mv::addon::ai_index_locations(
          static_cast<const mv_ai_api*>(it->second.addon->query(MV_AI_INTERFACE))));
    }
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
}

}  // namespace

extern "C" {

MV_API mv_status MV_CALL mv_addon_load(mv_session_t session, const char* id,
                                       const char* interface_id, const void** out_interface,
                                       char* out_chrome_utf8, uint32_t chrome_cap) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_load", [&] {
    return load_addon(session, id, interface_id, out_interface, out_chrome_utf8, chrome_cap, load_door::host);
  }));
}

MV_API mv_status MV_CALL mv_addon_load_reader(mv_session_t session, const char* id,
                                              const char* interface_id, const void** out_interface,
                                              char* out_chrome_utf8, uint32_t chrome_cap) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_load_reader", [&] {
    return load_addon(session, id, interface_id, out_interface, out_chrome_utf8, chrome_cap, load_door::reader);
  }));
}

MV_API mv_status MV_CALL mv_addon_load_guest(mv_session_t session, const char* id,
                                             const char* interface_id, const void** out_interface,
                                             char* out_chrome_utf8, uint32_t chrome_cap) {
  return static_cast<mv_status>(mv::abi::guard("mv_addon_load_guest", [&] {
    return load_addon(session, id, interface_id, out_interface, out_chrome_utf8, chrome_cap, load_door::guest);
  }));
}

}  // extern "C"
