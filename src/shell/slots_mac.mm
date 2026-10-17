// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Mac chrome's door to screens and slots (docs/design/25 PRs 58-59): the
// process's slot_registry (src/addon/slots.h) behind the C bridge
// (mv_chrome_bridge.h, mv_chrome_slots_*). The Windows twin is
// src/abi/slots_abi.cpp (mv_slots_*), over the same registry.
//
// Every call here is a table read or a post onto an add-on's worker: none
// waits on an add-on (rule 1). The registry's change listener runs on any
// thread; it is coalesced into one main-queue hop that hands the Swift chrome
// the OR of what changed.
#import <Foundation/Foundation.h>

#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "addon/slots.h"
#include "shell/addons_mac.h"
#include "shell/present_busy.h"

#include "mv_chrome_bridge.h"

namespace {

std::atomic<mv_chrome_slots_changed_fn> g_callback{nullptr};
std::atomic<std::uint32_t> g_pending{0};

int32_t copy_out(const std::string& text, char* buf, int32_t size) {
  const auto need = static_cast<int32_t>(text.size());
  if (buf && size > need) {
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
  }
  return need;
}

mv_slot_theme theme_of(const mv_chrome_slot_theme* t) {
  mv_slot_theme out{};
  if (t) {
    out.canvas = t->canvas;
    out.surface = t->surface;
    out.title = t->title;
    out.body = t->body;
    out.disabled = t->disabled;
    out.hairline = t->hairline;
    out.accent = t->accent;
    out.dark = t->dark;
  }
  return out;
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

extern "C" void mv_chrome_slots_set_callback(mv_chrome_slots_changed_fn fn) {
  g_callback.store(fn);
  mv::addon::default_slots().set_listener([](std::uint32_t bits) {
    // One hop per burst: a search posting rows 50 times in a frame wakes the
    // main queue once.
    if (g_pending.fetch_or(bits) != 0) return;
    dispatch_async(dispatch_get_main_queue(), ^{
      const std::uint32_t all = g_pending.exchange(0);
      if (auto cb = g_callback.load()) cb(all);
    });
  });
}

extern "C" int32_t mv_chrome_slots_contributions(char* buf, int32_t size) {
  return copy_out(mv::addon::default_slots().contributions_json(), buf, size);
}
extern "C" int32_t mv_chrome_slots_info(char* buf, int32_t size) {
  return copy_out(mv::addon::default_slots().info_json(), buf, size);
}
extern "C" int32_t mv_chrome_slots_screens(char* buf, int32_t size) {
  return copy_out(mv::addon::default_slots().screens_json(), buf, size);
}
extern "C" int32_t mv_chrome_slots_search_results(uint32_t scope, char* buf, int32_t size) {
  return copy_out(mv::addon::default_slots().search_json(scope), buf, size);
}
extern "C" int32_t mv_chrome_slots_badges(char* buf, int32_t size) {
  return copy_out(mv::addon::default_slots().badges_json(), buf, size);
}
extern "C" int32_t mv_chrome_slots_status(char* buf, int32_t size) {
  return copy_out(mv::addon::default_slots().status_json(), buf, size);
}

extern "C" int32_t mv_chrome_slots_notices(char* buf, int32_t size) {
  return copy_out(mv::addon::default_slots().notices_json(), buf, size);
}
extern "C" bool mv_chrome_slots_notice_action(const char* addon, const char* notice, const char* action) {
  return mv::addon::default_slots().notice_action(str(addon), str(notice), str(action)) == mv::status::ok;
}

extern "C" uint64_t mv_chrome_slots_request_info(const char* path, const char* location_text,
                                                 const mv_chrome_slot_theme* theme, float width,
                                                 float scale) {
  return mv::addon::default_slots().request_info(str(path), str(location_text), theme_of(theme), width, scale);
}
extern "C" void mv_chrome_slots_clear_info(void) { mv::addon::default_slots().clear_info(); }
extern "C" uint64_t mv_chrome_slots_search(uint32_t scope, const char* query, const char* folder) {
  return mv::addon::default_slots().search(scope, str(query), str(folder));
}
extern "C" uint64_t mv_chrome_slots_request_badges(const char* folder) {
  return mv::addon::default_slots().request_badges(str(folder));
}
extern "C" bool mv_chrome_slots_open_screen(const char* addon, const char* screen, const char* path,
                                            const mv_chrome_slot_theme* theme, float width, float height,
                                            float scale) {
  return mv::addon::default_slots().open_screen(str(addon), str(screen), str(path), theme_of(theme), width,
                                                height, scale) == mv::status::ok;
}
extern "C" bool mv_chrome_slots_close_screen(const char* addon, const char* screen) {
  return mv::addon::default_slots().close_screen(str(addon), str(screen)) == mv::status::ok;
}
extern "C" bool mv_chrome_slots_event(const char* addon, const char* screen, const char* node, uint32_t kind,
                                      const char* value_json) {
  return mv::addon::default_slots().send_event(str(addon), str(screen), str(node), kind, str(value_json)) ==
         mv::status::ok;
}
extern "C" bool mv_chrome_slots_invoke(const char* addon, const char* item, const char* paths_json) {
  return mv::addon::default_slots().invoke_item(str(addon), str(item),
                                                paths_json ? std::string(paths_json) : "[]") == mv::status::ok;
}

extern "C" bool mv_chrome_slots_command(const char* addon, const char* command, const char* paths_json) {
  return mv::addon::default_slots().run_command(str(addon), str(command),
                                                paths_json ? std::string(paths_json) : "[]") == mv::status::ok;
}

extern "C" int64_t mv_chrome_slots_surface(const char* addon, const char* screen, const char* surface,
                                           uint8_t* out, int64_t cap, uint32_t* width, uint32_t* height,
                                           uint64_t* revision) {
  if (!out) {
    // The size call: no pixel copy (the chrome skips the fetch when it already
    // holds this revision).
    uint32_t w = 0, h = 0;
    uint64_t rev = 0;
    if (!mv::addon::default_slots().surface_size(str(addon), str(screen), str(surface), w, h, rev)) return 0;
    if (width) *width = w;
    if (height) *height = h;
    if (revision) *revision = rev;
    return static_cast<int64_t>(w) * h * 4;
  }
  mv::addon::surface_pixels px;
  if (!mv::addon::default_slots().surface(str(addon), str(screen), str(surface), px)) return 0;
  if (width) *width = px.width;
  if (height) *height = px.height;
  if (revision) *revision = px.revision;
  const auto need = static_cast<int64_t>(px.bgra.size());
  if (out && cap >= need) std::memcpy(out, px.bgra.data(), px.bgra.size());
  return need;
}

// ---- thumbnails for a screen's thumbnail / thumbnail_grid nodes ------------------
//
// The viewer's JPEG-512 cache, by path (the add-on names files, not indices).
// Requests come from the tiles the grid has on screen (SwiftUI's LazyVGrid
// builds only those); one utility-QoS serial queue makes them, newest first is
// not needed because a tile that scrolled away cancels its request.

namespace {
std::mutex g_thumb_m;
std::set<std::string> g_thumb_wanted;
std::atomic<mv_chrome_slots_thumb_fn> g_thumb_cb{nullptr};

dispatch_queue_t thumb_queue() {
  static dispatch_queue_t q = dispatch_queue_create(
      "MediaViewer.slots.thumbs", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  return q;
}
}  // namespace

extern "C" void mv_chrome_slots_set_thumb_callback(mv_chrome_slots_thumb_fn fn) { g_thumb_cb.store(fn); }

extern "C" void mv_chrome_slots_request_thumbnail(const char* path) {
  if (!path || !*path) return;
  const std::string p(path);
  {
    std::lock_guard<std::mutex> lock(g_thumb_m);
    if (!g_thumb_wanted.insert(p).second) return;  // already queued
  }
  dispatch_async(thumb_queue(), ^{
    {
      std::lock_guard<std::mutex> lock(g_thumb_m);
      if (!g_thumb_wanted.count(p)) return;  // scrolled away
    }
    // A slow read waits behind the viewer's own work (docs/design/18 "Priority").
    for (int i = 0; i < 100 && mv::shell::g_present_busy.load(std::memory_order_relaxed); ++i) usleep(10'000);
    std::string jpeg = MvAddonsThumbnailPath(p);
    {
      std::lock_guard<std::mutex> lock(g_thumb_m);
      g_thumb_wanted.erase(p);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if (auto cb = g_thumb_cb.load()) cb(p.c_str(), jpeg.empty() ? nullptr : jpeg.c_str());
    });
  });
}

extern "C" void mv_chrome_slots_cancel_thumbnail(const char* path) {
  if (!path) return;
  std::lock_guard<std::mutex> lock(g_thumb_m);
  g_thumb_wanted.erase(path);
}
