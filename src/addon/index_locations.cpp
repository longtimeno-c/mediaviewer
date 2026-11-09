// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/index_locations.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mv::addon {
namespace {

struct source_state {
  std::mutex m;
  std::condition_variable idle;
  index_location_source source;  // under m
  eyes_source eyes;              // under m (slice 6)
  faces_source faces;            // under m (edit-refresh S10)
  search_source search;          // under m (docs/design/30)
  std::uint32_t in_flight = 0;   // under m
  std::condition_variable changed;  // a publish or a decline
  index_wanted_fn wanted;           // under m
  std::uint64_t declines = 0;       // under m
  // Bumped by every publish and retract; a call in flight compares it per row
  // without the lock.
  std::atomic<std::uint64_t> epoch{0};
};

// Never destroyed: a pack abandoned at quit may still be inside a call when
// static destructors would run (addon/host.h "Quit").
source_state& state() {
  static source_state* s = new source_state;
  return *s;
}

struct visit_ctx {
  const index_location_visit* visit = nullptr;
};

int32_t MV_CALL visit_thunk(void* user, const mv_addon_location_entry* e) {
  try {
    const auto& c = *static_cast<visit_ctx*>(user);
    if (!e || !e->path_utf8) return 1;
    return (*c.visit)(*e) ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

}  // namespace

void publish_index_locations(index_location_source source) {
  source_state& s = state();
  std::lock_guard lock(s.m);
  s.source = std::move(source);
  s.epoch.fetch_add(1, std::memory_order_acq_rel);
  s.changed.notify_all();
}

void set_index_wanted_listener(index_wanted_fn fn) {
  source_state& s = state();
  std::lock_guard lock(s.m);
  s.wanted = std::move(fn);
}

void decline_index_request() noexcept {
  source_state& s = state();
  std::lock_guard lock(s.m);
  ++s.declines;
  s.changed.notify_all();
}

expected each_index_location_or_wait(const index_location_visit& visit, const std::function<bool()>& keep_waiting,
                                     std::chrono::milliseconds max) {
  expected r = each_index_location(visit);
  if (r || r.error() != status::not_found) return r;
  source_state& s = state();
  index_wanted_fn wanted;
  std::uint64_t declines = 0;
  {
    std::lock_guard lock(s.m);
    if (!s.wanted || s.source) return s.source ? each_index_location(visit) : r;
    wanted = s.wanted;
    declines = s.declines;
  }
  wanted();  // the host starts its pack (never blocks)
  const auto deadline = std::chrono::steady_clock::now() + max;
  for (;;) {
    {
      std::unique_lock lock(s.m);
      s.changed.wait_for(lock, std::chrono::milliseconds(100),
                         [&] { return s.source != nullptr || s.declines != declines; });
      if (s.source) break;
      if (s.declines != declines) return err(status::not_found);
    }
    // Outside the lock: keep_waiting may take the caller's own locks.
    if (keep_waiting && !keep_waiting()) return err(status::cancelled);
    if (std::chrono::steady_clock::now() >= deadline) return err(status::not_found);
  }
  return each_index_location(visit);
}

void retract_index_locations() noexcept {
  source_state& s = state();
  index_location_source gone;
  {
    std::lock_guard lock(s.m);
    gone = std::move(s.source);
    s.source = nullptr;
    s.eyes = nullptr;
    s.faces = nullptr;
    s.search = nullptr;
    s.epoch.fetch_add(1, std::memory_order_acq_rel);
  }
}

void wait_index_locations_idle() noexcept {
  source_state& s = state();
  std::unique_lock lock(s.m);
  s.idle.wait(lock, [&] { return s.in_flight == 0; });
}

expected each_index_location(const index_location_visit& visit) {
  source_state& s = state();
  index_location_source source;
  std::uint64_t mine = 0;
  {
    std::lock_guard lock(s.m);
    if (!s.source) return err(status::not_found);
    source = s.source;
    mine = s.epoch.load(std::memory_order_acquire);
    ++s.in_flight;
  }
  struct leave {
    source_state& s;
    ~leave() {
      std::lock_guard lock(s.m);
      if (--s.in_flight == 0) s.idle.notify_all();
    }
  } on_leave{s};
  bool retracted = false;
  const expected r = source([&](const mv_addon_location_entry& e) {
    if (s.epoch.load(std::memory_order_acquire) != mine) {
      retracted = true;
      return false;
    }
    return visit(e);
  });
  if (retracted) return err(status::cancelled);
  return r;
}

void publish_ai_eyes(eyes_source source) {
  source_state& s = state();
  std::lock_guard lock(s.m);
  s.eyes = std::move(source);
}

result<std::string> ai_eyes_json(const std::string& path, const std::string& candidates) {
  source_state& s = state();
  eyes_source eyes;
  {
    std::lock_guard lock(s.m);
    if (!s.eyes) return err(status::not_found);
    eyes = s.eyes;
    ++s.in_flight;
  }
  struct leave {
    source_state& s;
    ~leave() {
      std::lock_guard lock(s.m);
      if (--s.in_flight == 0) s.idle.notify_all();
    }
  } on_leave{s};
  return eyes(path, candidates);
}

eyes_source ai_eyes(const mv_ai_api* api) {
  if (!api || api->struct_size < offsetof(mv_ai_api, eyes_json) + sizeof(api->eyes_json) || !api->eyes_json) {
    return nullptr;
  }
  return [api](const std::string& path, const std::string& candidates) -> result<std::string> {
    std::string out(64 * 1024, '\0');
    std::uint32_t needed = 0;
    mv_status st = api->eyes_json(api->ctx, path.c_str(), candidates.c_str(), out.data(),
                                  static_cast<std::uint32_t>(out.size()), &needed);
    if (st == MV_ERR_INVALID_ARG && needed > out.size()) {
      out.assign(needed, '\0');
      st = api->eyes_json(api->ctx, path.c_str(), candidates.c_str(), out.data(),
                          static_cast<std::uint32_t>(out.size()), &needed);
    }
    if (st != MV_OK) return err(static_cast<status>(st));
    out.resize(std::strlen(out.c_str()));
    return out;
  };
}

void publish_ai_faces(faces_source source) {
  source_state& s = state();
  std::lock_guard lock(s.m);
  s.faces = std::move(source);
}

result<std::string> ai_faces_json(const std::uint8_t* rgb, std::uint32_t width, std::uint32_t height,
                                  std::uint32_t stride, const std::uint8_t* other) {
  source_state& s = state();
  faces_source faces;
  {
    std::lock_guard lock(s.m);
    if (!s.faces) return err(status::not_found);
    faces = s.faces;
    ++s.in_flight;
  }
  struct leave {
    source_state& s;
    ~leave() {
      std::lock_guard lock(s.m);
      if (--s.in_flight == 0) s.idle.notify_all();
    }
  } on_leave{s};
  return faces(rgb, width, height, stride, other);
}

faces_source ai_faces(const mv_ai_api* api) {
  if (!api || api->struct_size < offsetof(mv_ai_api, faces_json) + sizeof(api->faces_json) || !api->faces_json) {
    return nullptr;
  }
  return [api](const std::uint8_t* rgb, std::uint32_t width, std::uint32_t height, std::uint32_t stride,
               const std::uint8_t* other) -> result<std::string> {
    std::string out(16 * 1024, '\0');
    std::uint32_t needed = 0;
    mv_status st = api->faces_json(api->ctx, rgb, width, height, stride, other, out.data(),
                                   static_cast<std::uint32_t>(out.size()), &needed);
    if (st == MV_ERR_INVALID_ARG && needed > out.size()) {
      out.assign(needed, '\0');
      st = api->faces_json(api->ctx, rgb, width, height, stride, other, out.data(),
                           static_cast<std::uint32_t>(out.size()), &needed);
    }
    // The pack's invalid_arg with room enough: People is off.
    if (st == MV_ERR_INVALID_ARG && needed <= out.size()) return err(status::permission_denied);
    if (st != MV_OK) return err(static_cast<status>(st));
    out.resize(std::strlen(out.c_str()));
    return out;
  };
}

void publish_ai_search(search_source source) {
  source_state& s = state();
  std::lock_guard lock(s.m);
  s.search = std::move(source);
}

result<std::vector<std::string>> ai_search_paths(const std::string& query, const std::string& scope) {
  source_state& s = state();
  search_source search;
  bool asked = false;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  for (;;) {
    {
      std::lock_guard lock(s.m);
      if (s.search) {
        search = s.search;
        ++s.in_flight;
        break;
      }
      if (asked && std::chrono::steady_clock::now() >= until) return err(status::not_found);
      if (!asked && !s.wanted) return err(status::not_found);
    }
    if (!asked) {
      asked = true;
      index_wanted_fn wanted;
      {
        std::lock_guard lock(s.m);
        wanted = s.wanted;
      }
      if (wanted) wanted();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  struct leave {
    source_state& s;
    ~leave() {
      std::lock_guard lock(s.m);
      if (--s.in_flight == 0) s.idle.notify_all();
    }
  } on_leave{s};
  return search(query, scope);
}

search_source ai_search(const mv_ai_api* api) {
  if (!api || !api->search_text || !api->result_count || !api->result_path || !api->search_release) return nullptr;
  return [api](const std::string& query, const std::string& scope) -> result<std::vector<std::string>> {
    const std::uint32_t kind = MV_AI_KIND_PHOTOS | MV_AI_KIND_VIDEOS;
    const std::uint32_t where = scope.empty() ? MV_AI_SCOPE_ALL : MV_AI_SCOPE_FOLDER;
    std::uint64_t id = 0;
    const mv_status st = api->search_text(api->ctx, query.c_str(), scope.empty() ? nullptr : scope.c_str(), where, kind, &id);
    if (st != MV_OK) return err(static_cast<status>(st));
    // search_text runs on the pack's worker and reports through the session's
    // queue, which an add-on does not read. The rows are sorted when it ends:
    // a count that has held for three polls is the end.
    std::uint32_t count = 0;
    int held = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (held < 3 && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      std::uint32_t now = 0;
      (void)api->result_count(api->ctx, id, &now);
      held = now > 0 && now == count ? held + 1 : 0;
      count = now;
    }
    std::vector<std::string> out;
    std::set<std::string> seen;
    char path[4096];
    for (std::uint32_t i = 0; i < count; ++i) {
      if (api->result_path(api->ctx, id, i, path, sizeof path) == MV_OK && seen.insert(path).second) out.emplace_back(path);
    }
    (void)api->search_release(api->ctx, id);
    return out;
  };
}

index_location_source ai_index_locations(const mv_ai_api* api) {
  if (!api || api->struct_size < offsetof(mv_ai_api, each_location) + sizeof(api->each_location) ||
      !api->each_location) {
    return nullptr;
  }
  return [api](const index_location_visit& visit) -> expected {
    visit_ctx c{&visit};
    const mv_status st = api->each_location(api->ctx, &visit_thunk, &c);
    if (st == MV_OK) return {};
    return err(static_cast<status>(st));
  };
}

}  // namespace mv::addon
