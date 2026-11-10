// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/mv_mobile_search.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "core/result.h"
#include "mobile/catalogue.h"
#include "mobile/search.h"

using mv::mobile::catalogue;
using mv::mobile::search_engine;

struct mv_mobile_library {
  std::shared_ptr<catalogue> cat;
  std::string work_dir;
  std::mutex start_m;   // one start at a time; held across it
  std::atomic<bool> starting{false};
  mutable std::mutex engine_m;  // only to read or publish `engine`: never held across work
  std::shared_ptr<search_engine> engine;  // set once

  std::shared_ptr<search_engine> current_engine() const {
    std::lock_guard lock(engine_m);
    return engine;
  }
};

struct mv_mobile_list {
  std::shared_ptr<const catalogue> cat;
  std::shared_ptr<const mv::mobile::listing> listing;
};

struct mv_mobile_results {
  std::vector<mv::mobile::hit> hits;
};

namespace {

mv_mobile_status map_status(mv::status s) noexcept {
  switch (s) {
    case mv::status::ok: return MV_MOBILE_OK;
    case mv::status::invalid_arg: return MV_MOBILE_ERR_INVALID_ARG;
    case mv::status::not_found: return MV_MOBILE_ERR_NOT_FOUND;
    case mv::status::io: return MV_MOBILE_ERR_IO;
    case mv::status::corrupt: return MV_MOBILE_ERR_CORRUPT;
    case mv::status::unsupported_format: return MV_MOBILE_ERR_UNSUPPORTED_FORMAT;
    case mv::status::cancelled: return MV_MOBILE_ERR_CANCELLED;
    case mv::status::timeout: return MV_MOBILE_ERR_TIMEOUT;
    default: return MV_MOBILE_ERR_INTERNAL;
  }
}

// Nothing crosses the ABI as an exception.
template <typename F>
mv_mobile_status guard(F&& f) noexcept {
  try {
    return f();
  } catch (const std::bad_alloc&) {
    return MV_MOBILE_ERR_INTERNAL;
  } catch (...) {
    return MV_MOBILE_ERR_INTERNAL;
  }
}

void fill(char* out, std::size_t capacity, const std::string& text) noexcept {
  const std::size_t n = std::min(text.size(), capacity - 1);
  std::memcpy(out, text.data(), n);
  out[n] = 0;
}

void fill_row(const catalogue& c, const mv::mobile::row& r, mv_mobile_row* out) noexcept {
  const std::string_view name = c.name_of(r);
  const auto [w, h] = c.aspect(r);
  out->asset_id = r.id;
  out->wall_seconds = r.wall_seconds;
  out->duration_ms = r.duration_ms;
  out->file_name = name.data();
  out->file_name_len = static_cast<std::uint32_t>(name.size());
  out->kind = r.kind;
  out->tile_width = w;
  out->tile_height = h;
}

}  // namespace

extern "C" {

const char* mv_mobile_status_message(mv_mobile_status status) {
  switch (status) {
    case MV_MOBILE_OK: return "";
    case MV_MOBILE_ERR_UNSUPPORTED_TOWER:
      return "This library uses the largest search model, which this phone can't run yet.";  // §11.3
    case MV_MOBILE_ERR_CORRUPT: return "What arrived doesn't match what your computer published.";
    case MV_MOBILE_ERR_NOT_FOUND: return "Not found.";
    case MV_MOBILE_ERR_CANCELLED: return "Stopped.";
    case MV_MOBILE_ERR_TIMEOUT: return "Timed out.";
    default: return "Couldn't read this library on the phone.";
  }
}

mv_mobile_status mv_mobile_library_open(const mv_mobile_open_options* o, mv_mobile_library** out) {
  return guard([&]() -> mv_mobile_status {
    if (!o || !out || o->struct_size < sizeof(mv_mobile_open_options) || !o->generation_dir || !o->library_dir || !o->work_dir) {
      return MV_MOBILE_ERR_INVALID_ARG;
    }
    auto opened = catalogue::open(o->generation_dir, o->library_dir);
    if (!opened) return map_status(opened.error());
    auto library = std::make_unique<mv_mobile_library>();
    library->cat = std::shared_ptr<catalogue>(std::move(*opened));
    library->work_dir = o->work_dir;
    *out = library.release();
    return MV_MOBILE_OK;
  });
}

void mv_mobile_library_close(mv_mobile_library* library) { delete library; }

mv_mobile_status mv_mobile_library_info_get(const mv_mobile_library* l, mv_mobile_library_info* out) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !out) return MV_MOBILE_ERR_INVALID_ARG;
    const auto& m = l->cat->info();
    out->epoch = m.epoch;
    out->generation = m.generation;
    out->created_unix = m.created_unix;
    out->assets = static_cast<uint32_t>(l->cat->rows().size());
    out->people = l->cat->people_count();
    out->has_tiny = l->cat->has_tiny() ? 1 : 0;
    fill(out->active_spec, sizeof out->active_spec, m.active_spec);
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_list_open(mv_mobile_library* l, uint32_t scope, const char* key, mv_mobile_list** out) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !out) return MV_MOBILE_ERR_INVALID_ARG;
    auto list = std::make_unique<mv_mobile_list>();
    list->cat = l->cat;
    if (scope == MV_MOBILE_SCOPE_EVERYTHING) {
      list->listing = l->cat->everything();
    } else if (key && (scope == MV_MOBILE_SCOPE_FOLDER || scope == MV_MOBILE_SCOPE_PERSON)) {
      auto got = scope == MV_MOBILE_SCOPE_FOLDER ? l->cat->in_folder(key) : l->cat->of_person(key);
      if (!got) return map_status(got.error());
      list->listing = std::move(*got);
    } else {
      return MV_MOBILE_ERR_INVALID_ARG;
    }
    *out = list.release();
    return MV_MOBILE_OK;
  });
}

void mv_mobile_list_close(mv_mobile_list* list) { delete list; }
uint32_t mv_mobile_list_count(const mv_mobile_list* l) { return l ? static_cast<uint32_t>(l->listing->indices.size()) : 0; }
uint32_t mv_mobile_list_month_count(const mv_mobile_list* l) { return l ? static_cast<uint32_t>(l->listing->months.size()) : 0; }

mv_mobile_status mv_mobile_list_month_at(const mv_mobile_list* l, uint32_t index, mv_mobile_month* out) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !out || index >= l->listing->months.size()) return MV_MOBILE_ERR_INVALID_ARG;
    const auto& m = l->listing->months[index];
    *out = mv_mobile_month{m.key, m.wall_seconds, m.first, m.count};
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_list_row_at(const mv_mobile_list* l, uint32_t index, mv_mobile_row* out) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !out || index >= l->listing->indices.size()) return MV_MOBILE_ERR_INVALID_ARG;
    fill_row(*l->cat, l->cat->rows()[l->listing->indices[index]], out);
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_library_row(const mv_mobile_library* l, int64_t id, mv_mobile_row* out) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !out) return MV_MOBILE_ERR_INVALID_ARG;
    const auto* r = l->cat->row_of(id);
    if (!r) return MV_MOBILE_ERR_NOT_FOUND;
    fill_row(*l->cat, *r, out);
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_library_fill_aspects(mv_mobile_library* l) {
  return guard([&]() -> mv_mobile_status {
    if (!l) return MV_MOBILE_ERR_INVALID_ARG;
    l->cat->fill_aspects();
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_library_folders(mv_mobile_library* l, const char* parent, mv_mobile_folder_fn visit, void* user) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !visit) return MV_MOBILE_ERR_INVALID_ARG;
    auto got = l->cat->folders_under(parent ? parent : "");
    if (!got) return map_status(got.error());
    for (const auto& f : *got) {
      const mv_mobile_folder row{f.key.c_str(), f.name.c_str(), f.items, f.has_subfolders ? 1u : 0u};
      visit(user, &row);
    }
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_library_people(mv_mobile_library* l, mv_mobile_person_fn visit, void* user) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !visit) return MV_MOBILE_ERR_INVALID_ARG;
    auto got = l->cat->people();
    if (!got) return map_status(got.error());
    for (const auto& p : *got) {
      const mv_mobile_person row{p.key.c_str(), p.name.c_str(), p.items, p.face_asset};
      visit(user, &row);
    }
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_library_details(mv_mobile_library* l, int64_t id, mv_mobile_details* out) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !out) return MV_MOBILE_ERR_INVALID_ARG;
    auto got = l->cat->details_of(id);
    if (!got) return map_status(got.error());
    fill(out->place, sizeof out->place, got->place);
    fill(out->camera, sizeof out->camera, got->camera);
    fill(out->lens, sizeof out->lens, got->lens);
    fill(out->folder, sizeof out->folder, got->folder);
    fill(out->format, sizeof out->format, got->format);
    out->file_bytes = got->file_bytes;
    out->iso = got->iso;
    out->aperture_x100 = got->aperture_x100;
    out->focal_mm = got->focal_mm;
    out->shutter_den = got->shutter_den;
    out->shutter_us = got->shutter_us;
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_library_filter_options(mv_mobile_library* l, mv_mobile_string_fn visit_place,
                                                  mv_mobile_string_fn visit_camera, mv_mobile_year_fn visit_year, void* user) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !visit_place || !visit_camera || !visit_year) return MV_MOBILE_ERR_INVALID_ARG;
    const auto got = l->cat->filters();
    for (const auto& s : got.places) visit_place(user, s.c_str());
    for (const auto& s : got.cameras) visit_camera(user, s.c_str());
    for (const auto y : got.years) visit_year(user, y);
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_library_tile(const mv_mobile_library* l, int64_t id, const uint8_t** bytes, uint32_t* length) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !bytes || !length) return MV_MOBILE_ERR_INVALID_ARG;
    const auto t = l->cat->tile(id);
    if (!t) return MV_MOBILE_ERR_NOT_FOUND;
    *bytes = t->data();
    *length = static_cast<uint32_t>(t->size());
    return MV_MOBILE_OK;
  });
}

mv_mobile_status mv_mobile_search_start(mv_mobile_library* l) {
  return guard([&]() -> mv_mobile_status {
    if (!l) return MV_MOBILE_ERR_INVALID_ARG;
    if (!mv::mobile::tower_supported(l->cat->info().active_spec)) return MV_MOBILE_ERR_UNSUPPORTED_TOWER;
    std::lock_guard lock(l->start_m);
    if (l->current_engine()) return MV_MOBILE_OK;
    l->starting = true;
    auto started = search_engine::start(l->cat, l->work_dir);
    l->starting = false;
    if (!started) return map_status(started.error());
    std::lock_guard publish(l->engine_m);
    l->engine = std::shared_ptr<search_engine>(std::move(*started));
    return MV_MOBILE_OK;
  });
}

uint32_t mv_mobile_search_state(const mv_mobile_library* l) {
  if (!l) return MV_MOBILE_SEARCH_OFF;
  if (!mv::mobile::tower_supported(l->cat->info().active_spec)) return MV_MOBILE_SEARCH_UNSUPPORTED;
  try {
    const auto engine = l->current_engine();
    if (!engine) return l->starting ? MV_MOBILE_SEARCH_WARMING : MV_MOBILE_SEARCH_OFF;
    switch (engine->current()) {
      case search_engine::state::warming: return MV_MOBILE_SEARCH_WARMING;
      case search_engine::state::ready: return MV_MOBILE_SEARCH_READY;
      default: return MV_MOBILE_SEARCH_FAILED;
    }
  } catch (...) {
    return MV_MOBILE_SEARCH_FAILED;
  }
}

mv_mobile_status mv_mobile_search_run(mv_mobile_library* l, const char* query, uint32_t kinds, uint32_t timeout_ms, mv_mobile_results** out) {
  return guard([&]() -> mv_mobile_status {
    if (!l || !query || !out) return MV_MOBILE_ERR_INVALID_ARG;
    if (!mv::mobile::tower_supported(l->cat->info().active_spec)) return MV_MOBILE_ERR_UNSUPPORTED_TOWER;
    const auto engine = l->current_engine();
    if (!engine) return MV_MOBILE_ERR_NOT_FOUND;
    auto run = engine->run(query, kinds, timeout_ms);
    if (!run) return map_status(run.error());
    auto r = std::make_unique<mv_mobile_results>();
    r->hits = std::move(*run);
    *out = r.release();
    return MV_MOBILE_OK;
  });
}

void mv_mobile_search_cancel(mv_mobile_library* l) {
  if (!l) return;
  try {
    if (const auto engine = l->current_engine()) engine->cancel();
  } catch (...) {
  }
}

void mv_mobile_results_close(mv_mobile_results* r) { delete r; }
uint32_t mv_mobile_results_count(const mv_mobile_results* r) { return r ? static_cast<uint32_t>(r->hits.size()) : 0; }

mv_mobile_status mv_mobile_results_at(const mv_mobile_results* r, uint32_t i, mv_mobile_hit* out) {
  if (!r || !out || i >= r->hits.size()) return MV_MOBILE_ERR_INVALID_ARG;
  const auto& h = r->hits[i];
  *out = mv_mobile_hit{h.asset, h.pts_ms, h.score, h.kind, h.match, h.more_in_clip};
  return MV_MOBILE_OK;
}

const char* mv_mobile_results_path(const mv_mobile_results* r, uint32_t i) {
  return r && i < r->hits.size() ? r->hits[i].path.c_str() : "";
}
const char* mv_mobile_results_snippet(const mv_mobile_results* r, uint32_t i) {
  return r && i < r->hits.size() ? r->hits[i].snippet.c_str() : "";
}

}  // extern "C"
