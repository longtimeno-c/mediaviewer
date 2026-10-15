// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/index_locations.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace mv::addon {
namespace {

struct source_state {
  std::mutex m;
  std::condition_variable idle;
  index_location_source source;  // under m
  std::uint32_t in_flight = 0;   // under m
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
}

void retract_index_locations() noexcept {
  source_state& s = state();
  index_location_source gone;
  {
    std::lock_guard lock(s.m);
    gone = std::move(s.source);
    s.source = nullptr;
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
