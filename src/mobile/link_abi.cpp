// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "mobile/link.h"
#include "mobile/link_generation.h"
#include "mobile/link_payload.h"
#include "mobile/link_thumbs.h"
#include "mobile/mv_mobile_link.h"
#include "mobile/sync.h"

using namespace mv;
using namespace mv::mobile;

struct mv_cancel {
  std::shared_ptr<cancel_flag> flag = std::make_shared<cancel_flag>();
};

struct mv_mobile_link {
  service_config service;
  sync_config sync;
  std::string cache_dir;
  std::uint64_t thumb_capacity = 0;
  std::unique_ptr<link_session> session;
  std::unique_ptr<thumb_cache> thumbs;

  std::mutex plan_m;
  sync_plan plan;  // under plan_m: the checked generation

  mutable std::shared_mutex view_m;
  std::unique_ptr<generation_view> view;  // under view_m

  std::mutex roots_m;
  std::map<std::string, std::string> label_to_root;  // under roots_m; empty until asked
  bool roots_known = false;
};

namespace {

template <std::size_t N>
void put(char (&to)[N], const std::string& from) {
  std::snprintf(to, N, "%s", from.c_str());
}

key32 key_at(const std::uint8_t* p) {
  key32 k{};
  std::memcpy(k.data(), p, k.size());
  return k;
}

const char* or_empty(const char* s) { return s ? s : ""; }

void fill_report(const route_report& r, mv_route_report* out) {
  if (!out) return;
  std::memset(out, 0, sizeof *out);
  put(out->address, r.address);
  out->over_internet = r.over_internet ? 1 : 0;
  const std::size_t n = std::min<std::size_t>(r.failures.size(), 4);
  out->failure_count = static_cast<std::int32_t>(n);
  for (std::size_t i = 0; i < n; ++i) {
    put(out->failures[i].address, r.failures[i].address);
    out->failures[i].problem = static_cast<std::int32_t>(r.failures[i].problem);
  }
}

mv_link_status status_of_route(status s, const route_report& r) {
  if (s == status::cancelled) return MV_LINK_CANCELLED;
  if (s == status::unsupported_format) return MV_LINK_TOO_OLD;
  if (s == status::busy) return MV_LINK_BUSY;
  if (s == status::permission_denied) {
    for (const auto& f : r.failures) {
      if (f.problem == route_problem::wrong_service) return MV_LINK_WRONG_SERVICE;
      if (f.problem == route_problem::bad_code) return MV_LINK_BAD_CODE;
    }
    return MV_LINK_NOT_PAIRED;
  }
  return MV_LINK_UNREACHABLE;
}

mv_link_status status_of_outcome(sync_outcome o) {
  switch (o) {
    case sync_outcome::up_to_date: return MV_LINK_UP_TO_DATE;
    case sync_outcome::older_refused: return MV_LINK_OLDER;
    case sync_outcome::copy_available: return MV_LINK_COPY_AVAILABLE;
    case sync_outcome::installed: return MV_LINK_OK;
    case sync_outcome::rejected: return MV_LINK_REJECTED;
    case sync_outcome::no_space: return MV_LINK_NO_SPACE;
    case sync_outcome::unreachable: return MV_LINK_UNREACHABLE;
    case sync_outcome::cancelled: return MV_LINK_CANCELLED;
  }
  return MV_LINK_INTERNAL;
}

mv_link_status status_of_read(status s) {
  switch (s) {
    case status::ok: return MV_LINK_OK;
    case status::cancelled: return MV_LINK_CANCELLED;
    case status::not_found: return MV_LINK_NOT_FOUND;
    case status::permission_denied: return MV_LINK_DENIED;
    default: return MV_LINK_UNREACHABLE;
  }
}

service_config service_of(const mv_pair_info* i, const std::uint8_t* device_key, const char* device_name) {
  service_config c;
  c.local = i->local;
  c.public_name = i->public_name;
  c.identity = key_at(i->identity);
  c.access_key = key_at(i->access_key);
  c.device_key = key_at(device_key);
  c.device_name = or_empty(device_name);
  return c;
}

void reload_view(mv_mobile_link* l) {
  const auto installed = read_installed(l->sync.library_dir);
  std::unique_ptr<generation_view> next;
  if (installed && installed->library_id == l->sync.library_id) {
    auto opened = generation_view::open(l->sync.library_dir + "/gen/" + installed->dir);
    if (opened) next = std::move(*opened);
    if (l->thumbs) l->thumbs->set_catalogue_root(installed->catalogue_root);
  }
  std::unique_lock lock(l->view_m);
  l->view = std::move(next);
}

template <typename Fn>
auto catch_all(Fn fn) noexcept -> decltype(fn()) {
  try {
    return fn();
  } catch (...) {
    return static_cast<decltype(fn())>(MV_LINK_INTERNAL);
  }
}

}  // namespace

extern "C" {

mv_cancel* mv_cancel_new(void) {
  try {
    return new mv_cancel();
  } catch (...) {
    return nullptr;
  }
}
void mv_cancel_fire(mv_cancel* c) {
  if (c) c->flag->stop.store(true);
}
void mv_cancel_free(mv_cancel* c) { delete c; }

int32_t mv_pair_parse(const char* text, int64_t now_unix, mv_pair_info* out) {
  return catch_all([&]() -> int32_t {
    if (!text || !out) return MV_LINK_INVALID;
    auto p = parse_pair_payload(text);
    if (!p) return MV_LINK_INVALID;
    std::memset(out, 0, sizeof *out);
    put(out->library_id, p->library_id);
    put(out->library_name, p->library_name);
    std::memcpy(out->publisher_key, p->publisher_key.data(), 32);
    put(out->local, p->local);
    put(out->public_name, p->public_name);
    std::memcpy(out->identity, p->identity.data(), 32);
    std::memcpy(out->access_key, p->access_key.data(), 32);
    put(out->code, p->code);
    out->expires = p->expires;
    put(out->fingerprint, p->fingerprint);
    if (p->expires <= now_unix) return MV_LINK_EXPIRED;
    return MV_LINK_OK;
  });
}

int32_t mv_device_key_new(uint8_t private_key[32], uint8_t public_key[32]) {
  return catch_all([&]() -> int32_t {
    if (!private_key || !public_key || sodium_init() < 0) return MV_LINK_INVALID;
    randombytes_buf(private_key, 32);
    crypto_scalarmult_base(public_key, private_key);
    return MV_LINK_OK;
  });
}

int32_t mv_pair_redeem(const mv_pair_info* info, const uint8_t device_key[32], const char* device_name,
                       mv_cancel* cancel, mv_route_report* report) {
  return catch_all([&]() -> int32_t {
    if (!info || !device_key) return MV_LINK_INVALID;
    route_report r;
    auto done = pair_device(service_of(info, device_key, device_name), info->code, r, cancel ? cancel->flag.get() : nullptr);
    fill_report(r, report);
    if (done) return MV_LINK_OK;
    return status_of_route(done.error(), r);
  });
}

int32_t mv_pair_public_answers(const mv_pair_info* info, const uint8_t device_key[32], const char* device_name) {
  return catch_all([&]() -> int32_t {
    if (!info || !device_key) return 0;
    return public_route_answers(service_of(info, device_key, device_name)) ? 1 : 0;
  });
}

mv_mobile_link* mv_link_open(const mv_link_config* c) {
  try {
    if (!c || !c->library_dir || !c->cache_dir || !c->library_id || !c->identity || !c->access_key || !c->device_key ||
        !c->publisher_key) {
      return nullptr;
    }
    auto l = std::make_unique<mv_mobile_link>();
    l->service.local = or_empty(c->local);
    l->service.public_name = or_empty(c->public_name);
    l->service.device_name = or_empty(c->device_name);
    l->service.identity = key_at(c->identity);
    l->service.access_key = key_at(c->access_key);
    l->service.device_key = key_at(c->device_key);
    l->sync.library_dir = c->library_dir;
    l->sync.library_id = c->library_id;
    l->sync.publisher_key = key_at(c->publisher_key);
    std::error_code ec;
    std::filesystem::create_directories(l->sync.library_dir, ec);
    l->cache_dir = c->cache_dir;
    l->thumb_capacity = c->thumb_cache_bytes ? c->thumb_cache_bytes : 2ull << 30;
    l->session = std::make_unique<link_session>(l->service);
    l->thumbs = std::make_unique<thumb_cache>(l->cache_dir, l->thumb_capacity, *l->session);
    reload_view(l.get());
    return l.release();
  } catch (...) {
    return nullptr;
  }
}

void mv_link_close(mv_mobile_link* l) { delete l; }

int32_t mv_link_connect(mv_mobile_link* l, mv_cancel* cancel, mv_route_report* report) {
  return catch_all([&]() -> int32_t {
    if (!l) return MV_LINK_INVALID;
    route_report r;
    auto done = l->session->connect(r, cancel ? cancel->flag.get() : nullptr);
    fill_report(r, report);
    {
      std::lock_guard lock(l->roots_m);
      l->roots_known = false;  // the service may have changed its folders
    }
    if (!done) return status_of_route(done.error(), r);
    return MV_LINK_OK;
  });
}

int32_t mv_link_is_connected(const mv_mobile_link* l) { return l && l->session->connected() ? 1 : 0; }

void mv_link_disconnect(mv_mobile_link* l) {
  if (l) l->session->disconnect();
}

int32_t mv_sync_check(mv_mobile_link* l, mv_sync_report* report) {
  return catch_all([&]() -> int32_t {
    if (!l || !report) return MV_LINK_INVALID;
    auto plan = check_catalogue(*l->session, l->sync);
    std::memset(report, 0, sizeof *report);
    report->outcome = status_of_outcome(plan.outcome);
    report->epoch = plan.epoch;
    report->generation = plan.generation;
    report->total_bytes = plan.total_bytes;
    report->done_bytes = plan.done_bytes;
    report->needed_bytes = plan.needed_bytes;
    if (plan.outcome == sync_outcome::copy_available || plan.outcome == sync_outcome::up_to_date) {
      if (!plan.catalogue_root.empty()) l->thumbs->set_catalogue_root(plan.catalogue_root);
    }
    std::lock_guard lock(l->plan_m);
    l->plan = std::move(plan);
    return report->outcome;
  });
}

int32_t mv_sync_copy(mv_mobile_link* l, mv_cancel* cancel, mv_sync_progress_fn progress, void* ctx, mv_sync_report* report) {
  return catch_all([&]() -> int32_t {
    if (!l) return MV_LINK_INVALID;
    sync_plan plan;
    {
      std::lock_guard lock(l->plan_m);
      plan = l->plan;
    }
    if (plan.outcome != sync_outcome::copy_available) {
      plan = check_catalogue(*l->session, l->sync);
      if (plan.outcome != sync_outcome::copy_available) {
        if (report) {
          std::memset(report, 0, sizeof *report);
          report->outcome = status_of_outcome(plan.outcome);
          report->epoch = plan.epoch;
          report->generation = plan.generation;
        }
        return status_of_outcome(plan.outcome);
      }
    }
    const auto outcome = copy_catalogue(
        *l->session, l->sync, plan,
        [&](std::uint64_t done, std::uint64_t total) {
          if (progress) progress(ctx, done, total);
        },
        cancel ? cancel->flag.get() : nullptr);
    if (report) {
      std::memset(report, 0, sizeof *report);
      report->outcome = status_of_outcome(outcome);
      report->epoch = plan.epoch;
      report->generation = plan.generation;
      report->total_bytes = plan.total_bytes;
      report->needed_bytes = plan.needed_bytes;
    }
    if (outcome == sync_outcome::installed) reload_view(l);
    return status_of_outcome(outcome);
  });
}

int32_t mv_catalogue_read(mv_mobile_link* l, mv_catalogue_info* out) {
  return catch_all([&]() -> int32_t {
    if (!l || !out) return MV_LINK_INVALID;
    std::memset(out, 0, sizeof *out);
    const auto installed = read_installed(l->sync.library_dir);
    std::shared_lock lock(l->view_m);
    if (!installed || !l->view) return MV_LINK_OK;
    out->present = 1;
    out->epoch = installed->epoch;
    out->generation = installed->generation;
    out->created_unix = l->view->created();
    out->asset_count = l->view->asset_count();
    put(out->dir, l->view->dir());
    return MV_LINK_OK;
  });
}

void mv_library_remove(const char* dir) {
  if (dir) remove_library(dir);
}

int32_t mv_tiny_get(mv_mobile_link* l, int64_t asset, mv_bytes_fn fn, void* ctx) {
  return catch_all([&]() -> int32_t {
    if (!l || !fn) return MV_LINK_INVALID;
    std::shared_lock lock(l->view_m);
    if (!l->view) return MV_LINK_NOT_FOUND;
    const auto tile = l->view->tiny(asset);
    if (!tile) return MV_LINK_NOT_FOUND;
    fn(ctx, tile->data(), tile->size());
    return MV_LINK_OK;
  });
}

int32_t mv_thumb_request(mv_mobile_link* l, int64_t asset, int32_t lane, mv_done_fn done, void* ctx, mv_cancel** out_cancel) {
  return catch_all([&]() -> int32_t {
    if (!l || !done || !out_cancel) return MV_LINK_INVALID;
    *out_cancel = nullptr;
    std::optional<ai::catalog::thumb_location> where;
    {
      std::shared_lock lock(l->view_m);
      if (!l->view) return MV_LINK_NOT_FOUND;
      where = l->view->thumb(asset);
    }
    if (!where) return MV_LINK_NOT_FOUND;
    auto flag = l->thumbs->request(*where, lane, [done, ctx](result<std::vector<std::uint8_t>> r) {
      if (r) {
        done(ctx, MV_LINK_OK, r->data(), r->size());
      } else {
        done(ctx, status_of_read(r.error()), nullptr, 0);
      }
    });
    auto* handle = new mv_cancel();
    handle->flag = std::move(flag);
    *out_cancel = handle;
    return MV_LINK_OK;
  });
}

uint64_t mv_thumb_bytes_used(const mv_mobile_link* l) { return l ? l->thumbs->bytes_used() : 0; }

int32_t mv_asset_read(mv_mobile_link* l, int64_t asset, mv_asset_info* out) {
  return catch_all([&]() -> int32_t {
    if (!l || !out) return MV_LINK_INVALID;
    std::shared_lock lock(l->view_m);
    if (!l->view) return MV_LINK_NOT_FOUND;
    const auto f = l->view->file(asset);
    if (!f) return MV_LINK_NOT_FOUND;
    out->size = f->size;
    out->kind = f->kind;
    out->duration_ms = f->duration_ms;
    return MV_LINK_OK;
  });
}

int32_t mv_file_read(mv_mobile_link* l, int64_t asset, uint64_t at, uint64_t length, int32_t bulk, mv_cancel* cancel,
                     mv_sink_fn sink, void* ctx) {
  return catch_all([&]() -> int32_t {
    if (!l || !sink) return MV_LINK_INVALID;
    std::optional<asset_file> f;
    std::map<std::string, generation_view::root_ref> roots;
    {
      std::shared_lock lock(l->view_m);
      if (!l->view) return MV_LINK_NOT_FOUND;
      f = l->view->file(asset);
      roots = l->view->roots();
    }
    if (!f) return MV_LINK_NOT_FOUND;
    const auto ref = roots.find(f->uuid);
    if (ref == roots.end()) return MV_LINK_NOT_FOUND;
    std::string root;
    {
      std::lock_guard lock(l->roots_m);
      if (!l->roots_known) {
        auto listed = l->session->roots();
        if (!listed) return status_of_read(listed.error());
        l->label_to_root.clear();
        for (const auto& r : *listed) l->label_to_root[r.label] = r.id;
        l->roots_known = true;
      }
      const auto found = l->label_to_root.find(ref->second.share);
      if (found == l->label_to_root.end()) return MV_LINK_NOT_FOUND;  // the NAS has no folder of that name yet
      root = found->second;
    }
    const std::string rel = ref->second.sub.empty() ? f->rel : ref->second.sub + "/" + f->rel;
    auto done = l->session->read_range(
        root, rel, at, length,
        [&](std::uint64_t offset, std::span<const std::uint8_t> bytes) { return sink(ctx, offset, bytes.data(), bytes.size()) != 0; },
        cancel ? cancel->flag.get() : nullptr, bulk != 0);
    return done ? MV_LINK_OK : status_of_read(done.error());
  });
}

int32_t mv_moments_get(mv_mobile_link* l, int64_t asset, mv_moment_fn fn, void* ctx) {
  return catch_all([&]() -> int32_t {
    if (!l || !fn) return MV_LINK_INVALID;
    std::shared_lock lock(l->view_m);
    if (!l->view) return MV_LINK_NOT_FOUND;
    for (const auto& m : l->view->moments(asset)) fn(ctx, m.start_ms, m.kind, m.text.c_str());
    return MV_LINK_OK;
  });
}

int32_t mv_assets_list(mv_mobile_link* l, mv_listed_asset_fn fn, void* ctx) {
  return catch_all([&]() -> int32_t {
    if (!l || !fn) return MV_LINK_INVALID;
    std::shared_lock lock(l->view_m);
    if (!l->view) return MV_LINK_NOT_FOUND;
    l->view->for_each_asset([&](const generation_view::listed_asset& a) {
      fn(ctx, a.id, a.kind, a.taken_at, a.duration_ms, a.name.c_str());
    });
    return MV_LINK_OK;
  });
}

}  // extern "C"
