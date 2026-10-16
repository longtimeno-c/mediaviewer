// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Screens and slots over the C ABI (mediaviewer_addon.h mv_slots_*, docs/design/25
// PRs 58-59) for the Windows chrome: the process's slot_registry
// (src/addon/slots.h). The Mac twin is src/shell/slots_mac.mm. Every call is a
// table copy or a post onto an add-on's worker; none waits on an add-on.
#include <mediaviewer/mediaviewer_addon.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "abi/guard.h"
#include "addon/slots.h"

namespace {

using mv::status;

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

status write_out(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (!out || cap < need) return status::invalid_arg;
  std::memcpy(out, text.c_str(), need);
  return status::ok;
}

mv_slot_theme theme_or_default(const mv_slot_theme* t) { return t ? *t : mv_slot_theme{}; }

std::mutex g_listener_m;
mv_slots_changed_fn g_listener = nullptr;
void* g_listener_user = nullptr;

}  // namespace

extern "C" {

MV_API mv_status MV_CALL mv_slots_set_listener(mv_slots_changed_fn fn, void* user) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_set_listener", [&] {
    {
      std::lock_guard lock(g_listener_m);
      g_listener = fn;
      g_listener_user = user;
    }
    if (!fn) {
      mv::addon::default_slots().set_listener({});
      return status::ok;
    }
    mv::addon::default_slots().set_listener([](std::uint32_t bits) {
      mv_slots_changed_fn cb = nullptr;
      void* u = nullptr;
      {
        std::lock_guard lock(g_listener_m);
        cb = g_listener;
        u = g_listener_user;
      }
      if (cb) cb(u, bits);
    });
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_slots_contributions_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_contributions_json", [&] {
    return write_out(mv::addon::default_slots().contributions_json(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_slots_info_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_info_json", [&] {
    return write_out(mv::addon::default_slots().info_json(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_slots_screens_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_screens_json", [&] {
    return write_out(mv::addon::default_slots().screens_json(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_slots_search_json(uint32_t scope, char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_search_json", [&] {
    return write_out(mv::addon::default_slots().search_json(scope), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_slots_badges_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_badges_json", [&] {
    return write_out(mv::addon::default_slots().badges_json(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_slots_status_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_status_json", [&] {
    return write_out(mv::addon::default_slots().status_json(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_slots_notices_json(char* out, uint32_t cap, uint32_t* needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_notices_json", [&] {
    return write_out(mv::addon::default_slots().notices_json(), out, cap, needed);
  }));
}

MV_API mv_status MV_CALL mv_slots_notice_action(const char* addon, const char* notice_id, const char* action_id) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_notice_action", [&] {
    MV_REQUIRE(addon && notice_id && action_id, "addon, notice and action required");
    return mv::addon::default_slots().notice_action(addon, notice_id, action_id);
  }));
}

MV_API mv_status MV_CALL mv_slots_request_info(const char* path_utf8, const char* location_text,
                                               const mv_slot_theme* theme, float width_pt, float scale,
                                               uint64_t* out_generation) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_request_info", [&] {
    MV_REQUIRE(path_utf8, "path required");
    const auto g = mv::addon::default_slots().request_info(path_utf8, str(location_text), theme_or_default(theme),
                                                           width_pt, scale);
    if (out_generation) *out_generation = g;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_slots_clear_info(void) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_clear_info", [&] {
    mv::addon::default_slots().clear_info();
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_slots_search(uint32_t scope, const char* query_utf8, const char* folder_utf8,
                                         uint64_t* out_generation) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_search", [&] {
    MV_REQUIRE(scope == MV_SLOT_SCOPE_FILES || scope == MV_SLOT_SCOPE_LOCAL, "unknown scope");
    const auto g = mv::addon::default_slots().search(scope, str(query_utf8), str(folder_utf8));
    if (out_generation) *out_generation = g;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_slots_request_badges(const char* folder_utf8, uint64_t* out_generation) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_request_badges", [&] {
    const auto g = mv::addon::default_slots().request_badges(str(folder_utf8));
    if (out_generation) *out_generation = g;
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_slots_open_screen(const char* addon, const char* screen, const char* path_utf8,
                                              const mv_slot_theme* theme, float width_pt, float height_pt,
                                              float scale) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_open_screen", [&] {
    MV_REQUIRE(addon && screen, "addon and screen required");
    return mv::addon::default_slots().open_screen(addon, screen, str(path_utf8), theme_or_default(theme), width_pt,
                                                  height_pt, scale);
  }));
}

MV_API mv_status MV_CALL mv_slots_close_screen(const char* addon, const char* screen) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_close_screen", [&] {
    MV_REQUIRE(addon && screen, "addon and screen required");
    return mv::addon::default_slots().close_screen(addon, screen);
  }));
}

MV_API mv_status MV_CALL mv_slots_event(const char* addon, const char* screen, const char* node, uint32_t kind,
                                        const char* value_json) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_event", [&] {
    MV_REQUIRE(addon && screen && node, "addon, screen and node required");
    return mv::addon::default_slots().send_event(addon, screen, node, kind, str(value_json));
  }));
}

MV_API mv_status MV_CALL mv_slots_invoke(const char* addon, const char* item, const char* paths_json) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_invoke", [&] {
    MV_REQUIRE(addon && item, "addon and item required");
    return mv::addon::default_slots().invoke_item(addon, item, paths_json ? std::string(paths_json) : "[]");
  }));
}

MV_API mv_status MV_CALL mv_slots_command(const char* addon, const char* command, const char* paths_json) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_command", [&] {
    MV_REQUIRE(addon && command, "addon and command required");
    return mv::addon::default_slots().run_command(addon, command, paths_json ? std::string(paths_json) : "[]");
  }));
}

MV_API mv_status MV_CALL mv_slots_surface(const char* addon, const char* screen, const char* surface,
                                          uint8_t* out, uint64_t cap, uint32_t* out_width,
                                          uint32_t* out_height, uint64_t* out_revision,
                                          uint64_t* out_needed) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_surface", [&] {
    MV_REQUIRE(addon && screen && surface, "addon, screen and surface required");
    if (!out) {
      // The size call copies no pixels (a surface is up to 64 MB).
      uint32_t w = 0;
      uint32_t h = 0;
      uint64_t rev = 0;
      if (!mv::addon::default_slots().surface_size(addon, screen, surface, w, h, rev)) return status::not_found;
      if (out_width) *out_width = w;
      if (out_height) *out_height = h;
      if (out_revision) *out_revision = rev;
      if (out_needed) *out_needed = static_cast<uint64_t>(w) * h * 4;
      return status::invalid_arg;
    }
    mv::addon::surface_pixels px;
    if (!mv::addon::default_slots().surface(addon, screen, surface, px)) return status::not_found;
    if (out_width) *out_width = px.width;
    if (out_height) *out_height = px.height;
    if (out_revision) *out_revision = px.revision;
    if (out_needed) *out_needed = px.bgra.size();
    if (!out || cap < px.bgra.size()) return status::invalid_arg;
    std::memcpy(out, px.bgra.data(), px.bgra.size());
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_slots_place_terms(const char* query_utf8, uint32_t* out_flags) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_place_terms", [&] {
    MV_REQUIRE(out_flags != nullptr, "out_flags");
    *out_flags = mv::addon::place_terms(mv::addon::default_slots(), str(query_utf8));
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_slots_local_search_provider(uint32_t on) {
  return static_cast<mv_status>(mv::abi::guard("mv_slots_local_search_provider", [&] {
    mv::addon::set_local_search_provider(mv::addon::default_slots(), on != 0);
    return status::ok;
  }));
}

}  // extern "C"
