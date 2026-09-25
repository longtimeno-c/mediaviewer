// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/host.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "io/content_hash.h"
#include "io/file_port.h"
#include "io/pairing.h"
#include "io/replace.h"
#include "io/verified_copy.h"

namespace mv::addon {
namespace {

host_table& self(void* host) { return *static_cast<host_table*>(host); }

mv_status to_mv(status s) noexcept { return static_cast<mv_status>(s); }

template <typename Fn>
mv_status guarded(Fn&& fn) noexcept {
  try {
    return fn();
  } catch (...) {
    return MV_ERR_INTERNAL;
  }
}

mv_status copy_out(const std::string& text, char* out, uint32_t cap) {
  if (!out || cap == 0 || text.size() + 1 > cap) return MV_ERR_INVALID_ARG;
  std::memcpy(out, text.c_str(), text.size() + 1);
  return MV_OK;
}

template <std::size_t N>
void fill(char (&dst)[N], const std::string& src) {
  const std::size_t n = std::min(src.size(), N - 1);
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

void to_volume(const io::volume_info& v, mv_addon_volume& out) {
  out = mv_addon_volume{};
  fill(out.volume_id, v.volume_id);
  fill(out.root_utf8, v.root_utf8);
  fill(out.label_utf8, v.label_utf8);
  fill(out.device_key, v.device_key);
  out.total_bytes = v.total_bytes;
  out.free_bytes = v.free_bytes;
  out.removable = v.removable ? 1u : 0u;
  out.network = v.network ? 1u : 0u;
  out.read_only = v.read_only ? 1u : 0u;
}

// ---- the table's thunks ------------------------------------------------------

mv_status MV_CALL t_walk(void*, const char* root, int32_t depth, mv_addon_walk_fn visit, void* user) {
  return guarded([&] {
    if (!root || !visit) return MV_ERR_INVALID_ARG;
    auto r = io::walk_files(root, depth, [&](const io::tree_entry& e) {
      mv_addon_file_entry fe{e.path_utf8.c_str(), e.relative_utf8.c_str(), e.name_utf8.c_str(),
                             e.size, e.mtime_unix};
      return visit(user, &fe) != 0;
    });
    return r ? MV_OK : to_mv(r.error());
  });
}

mv_status MV_CALL t_stat(void*, const char* path, uint64_t* size, int64_t* mtime, uint32_t* is_dir) {
  return guarded([&] {
    if (!path) return MV_ERR_INVALID_ARG;
    auto st = io::stat_path(path);
    if (!st) return to_mv(st.error());
    if (size) *size = st->size;
    if (mtime) *mtime = st->mtime_unix;
    if (is_dir) *is_dir = st->is_directory ? 1u : 0u;
    return MV_OK;
  });
}

mv_status MV_CALL t_mkdirs(void*, const char* dir) {
  return guarded([&] {
    if (!dir) return MV_ERR_INVALID_ARG;
    auto r = io::make_directories(dir);
    return r ? MV_OK : to_mv(r.error());
  });
}

mv_status MV_CALL t_remove(void*, const char* path) {
  return guarded([&] {
    if (!path) return MV_ERR_INVALID_ARG;
    auto r = io::remove_file(path);
    return r ? MV_OK : to_mv(r.error());
  });
}

mv_status MV_CALL t_hash(void*, const char* path, uint32_t uncached,
                         int32_t(MV_CALL* is_cancelled)(void*), void(MV_CALL* yield)(void*),
                         void* user, uint8_t out[32]) {
  return guarded([&] {
    if (!path || !out) return MV_ERR_INVALID_ARG;
    std::atomic<bool> cancel{false};
    const std::function<void()> y = [&] {
      if (yield) yield(user);
      if (is_cancelled && is_cancelled(user)) cancel = true;
    };
    auto h = io::hash_file(path, uncached ? io::read_mode::uncached : io::read_mode::sequential,
                           &cancel, y);
    if (!h) return to_mv(h.error());
    std::memcpy(out, h->bytes.data(), 32);
    return MV_OK;
  });
}

mv_status MV_CALL t_copy(void* host, const mv_addon_copy_request* req, mv_addon_copy_result* out) {
  return guarded([&] {
    if (!req || !out || !req->source_utf8 || !req->targets_utf8 || req->target_count == 0 ||
        req->target_count > MV_ADDON_COPY_MAX_TARGETS) {
      return MV_ERR_INVALID_ARG;
    }
    std::vector<std::string> targets;
    for (uint32_t i = 0; i < req->target_count; ++i) {
      if (!req->targets_utf8[i]) return MV_ERR_INVALID_ARG;
      targets.emplace_back(req->targets_utf8[i]);
    }
    std::atomic<bool> cancel{false};
    io::copy_options o;
    o.read_back = req->read_back != 0;
    o.retries = static_cast<int>(std::min<uint32_t>(req->retries, 3));
    o.cancel = &cancel;
    o.yield = [&] {
      if (req->yield) req->yield(req->user);
      if (req->is_cancelled && req->is_cancelled(req->user)) cancel = true;
    };
    if (req->on_progress) {
      o.on_progress = [&](std::uint64_t d) { req->on_progress(req->user, d); };
    }
    io::copy_fault fault;
    if (self(host).services().test_hooks && req->fault_times > 0) {
      fault.target = req->fault_target;
      fault.offset = req->fault_offset;
      fault.times = static_cast<int>(req->fault_times);
      o.fault = &fault;
    }
    auto r = io::verified_copy(req->source_utf8, targets, o);
    *out = mv_addon_copy_result{};
    out->target_count = req->target_count;
    if (!r) {
      for (uint32_t i = 0; i < req->target_count; ++i) {
        out->outcome[i] = r.error() == status::cancelled ? MV_COPY_CANCELLED : MV_COPY_WRITE_FAILED;
      }
      return to_mv(r.error());
    }
    std::memcpy(out->source_hash, r->source_hash.bytes.data(), 32);
    out->bytes = r->bytes;
    out->source_unstable = r->source_unstable ? 1u : 0u;
    for (uint32_t i = 0; i < req->target_count; ++i) {
      out->outcome[i] = static_cast<uint32_t>(r->targets[i].outcome);
      out->retried[i] = r->targets[i].retried ? 1u : 0u;
    }
    return MV_OK;
  });
}

mv_status MV_CALL t_write_new(void*, const char* path, const void* bytes, uint64_t length) {
  return guarded([&] {
    if (!path || (!bytes && length)) return MV_ERR_INVALID_ARG;
    auto r = io::write_new(path, std::span<const std::uint8_t>(
                                     static_cast<const std::uint8_t*>(bytes),
                                     static_cast<std::size_t>(length)));
    return r ? MV_OK : to_mv(r.error());
  });
}

mv_status MV_CALL t_volume_of(void*, const char* path, mv_addon_volume* out) {
  return guarded([&] {
    if (!path || !out) return MV_ERR_INVALID_ARG;
    auto v = io::volume_of(path);
    if (!v) return to_mv(v.error());
    to_volume(*v, *out);
    return MV_OK;
  });
}

mv_status MV_CALL t_list_volumes(void*, mv_addon_volume* out, uint32_t cap, uint32_t* count) {
  return guarded([&] {
    if (!out && cap) return MV_ERR_INVALID_ARG;
    auto v = io::list_volumes();
    if (!v) return to_mv(v.error());
    const auto n = static_cast<uint32_t>(std::min<std::size_t>(v->size(), cap));
    for (uint32_t i = 0; i < n; ++i) to_volume((*v)[i], out[i]);
    if (count) *count = n;
    return MV_OK;
  });
}

mv_status MV_CALL t_eject(void*, const char* root) {
  return guarded([&] {
    if (!root) return MV_ERR_INVALID_ARG;
    auto r = io::eject_volume(root);
    return r ? MV_OK : to_mv(r.error());
  });
}

void on_io_volume(void* user, io::volume_event event, const char* root) {
  auto& w = *static_cast<host_table::watch_state*>(user);
  void(MV_CALL* cb)(void*, std::uint32_t, const char*) = nullptr;
  void* cb_user = nullptr;
  {
    std::lock_guard lock(w.m);
    cb = w.cb;
    cb_user = w.user;
  }
  if (cb) cb(cb_user, static_cast<std::uint32_t>(event), root);
}

mv_status MV_CALL t_watch(void* host, void(MV_CALL* cb)(void*, std::uint32_t, const char*),
                          void* user) {
  return guarded([&] {
    auto& w = self(host).watch;
    if (!cb) {
      self(host).stop_watch();
      return MV_OK;
    }
    // `control` serialises start / stop; `m` only guards the callback, so
    // the watcher thread never waits on a start or stop in progress.
    std::lock_guard control(w.control);
    {
      std::lock_guard lock(w.m);
      w.cb = cb;
      w.user = user;
    }
    if (!w.running) {
      if (!w.watcher.start(&on_io_volume, &w)) return MV_ERR_IO;
      w.running = true;
    }
    return MV_OK;
  });
}

mv_status MV_CALL t_capture(void* host, const char* path, mv_addon_capture* out) {
  return guarded([&] {
    if (!path || !out) return MV_ERR_INVALID_ARG;
    *out = mv_addon_capture{};
    const auto& capture = self(host).services().capture;
    if (!capture) return MV_ERR_UNSUPPORTED_FORMAT;
    return capture(path, *out) ? MV_OK : MV_ERR_UNSUPPORTED_FORMAT;
  });
}

mv_status MV_CALL t_pair(void*, const char* const* names, uint32_t count, uint32_t* partner,
                         uint32_t* kind) {
  return guarded([&] {
    if ((!names || !partner || !kind) && count) return MV_ERR_INVALID_ARG;
    // The viewer's own pairing (io/pairing.h). The index rides in path_utf8
    // so the result maps back to the caller's positions.
    std::vector<io::dir_entry> entries(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (!names[i]) return MV_ERR_INVALID_ARG;
      entries[i].name_utf8 = names[i];
      entries[i].path_utf8 = std::to_string(i);
      partner[i] = UINT32_MAX;
      kind[i] = 0;
    }
    for (const io::listed_item& item : io::pair_listing(std::move(entries))) {
      if (item.kind == io::pair_kind::none) continue;
      const auto a = static_cast<uint32_t>(std::stoul(item.primary.path_utf8));
      const auto b = static_cast<uint32_t>(std::stoul(item.secondary.path_utf8));
      partner[a] = b;
      partner[b] = a;
      kind[a] = kind[b] = static_cast<uint32_t>(item.kind);
    }
    return MV_OK;
  });
}

mv_status MV_CALL t_thumbnail(void* host, const char* path, char* out, uint32_t cap) {
  return guarded([&] {
    if (!path) return MV_ERR_INVALID_ARG;
    const auto& thumb = self(host).services().thumbnail;
    if (!thumb) return MV_ERR_UNSUPPORTED_FORMAT;
    auto r = thumb(path);
    return r ? copy_out(*r, out, cap) : to_mv(r.error());
  });
}

int32_t MV_CALL t_should_yield(void* host) {
  try {
    const auto& fn = self(host).services().should_yield;
    return fn && fn() ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

void MV_CALL t_post(void* host, const mv_addon_event* e) {
  try {
    const auto& fn = self(host).services().post;
    if (fn && e) fn(*e);
  } catch (...) {
  }
}

uint64_t MV_CALL t_corr(void*) {
  static std::atomic<uint64_t> next{1ull << 48};  // apart from the ABI's own ids
  return next.fetch_add(1);
}

mv_status MV_CALL t_data_dir(void* host, char* out, uint32_t cap) {
  return guarded([&] {
    const std::string& dir = self(host).services().data_dir;
    if (dir.empty()) return MV_ERR_IO;
    if (auto made = io::make_directories(dir); !made) return to_mv(made.error());
    return copy_out(dir, out, cap);
  });
}

mv_status MV_CALL t_library(void* host, char* out, uint32_t cap) {
  return guarded([&] {
    const std::string& dir = self(host).services().default_library;
    if (dir.empty()) return MV_ERR_IO;
    return copy_out(dir, out, cap);
  });
}

void MV_CALL t_log(void*, int32_t, const char*) {
  // Deliberately nowhere yet: an add-on's messages are for a developer's
  // debugger, and the app has no log file that could leak a name (rule 6).
}

}  // namespace

host_table::host_table(host_services services) : svc_(std::move(services)) {
  api_.struct_size = sizeof(mv_host_api);
  api_.host_api = MV_ADDON_HOST_API;
  api_.host = this;
  api_.walk_files = &t_walk;
  api_.stat_file = &t_stat;
  api_.make_directories = &t_mkdirs;
  api_.remove_file = &t_remove;
  api_.hash_file = &t_hash;
  api_.copy_verified = &t_copy;
  api_.write_new_file = &t_write_new;
  api_.volume_of = &t_volume_of;
  api_.list_volumes = &t_list_volumes;
  api_.eject_volume = &t_eject;
  api_.watch_volumes = &t_watch;
  api_.capture_info = &t_capture;
  api_.pair_names = &t_pair;
  api_.thumbnail_path = &t_thumbnail;
  api_.should_yield = &t_should_yield;
  api_.post_event = &t_post;
  api_.next_correlation_id = &t_corr;
  api_.data_dir = &t_data_dir;
  api_.default_library_dir = &t_library;
  api_.log = &t_log;
}

host_table::~host_table() { stop_watch(); }

void host_table::stop_watch() noexcept {
  std::lock_guard control(watch.control);
  {
    std::lock_guard lock(watch.m);
    watch.cb = nullptr;
    watch.user = nullptr;
  }
  // Not under `m`: stop() drains the watcher thread, which may be inside
  // on_io_volume waiting for `m`.
  if (watch.running) watch.watcher.stop();
  watch.running = false;
}

// ---------------------------------------------------------------------------

shared_library::~shared_library() { close(); }

shared_library::shared_library(shared_library&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

shared_library& shared_library::operator=(shared_library&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

loaded_addon::~loaded_addon() {
  // The add-on stops its threads before its code goes away, and the host
  // table (whose thunks it calls) outlives both.
  if (api_.shutdown && api_.addon) api_.shutdown(api_.addon);
  // An add-on that left its volume callback registered must not be called
  // after its code is unmapped.
  if (table_) table_->stop_watch();
  lib_.close();
  table_.reset();
}

const void* loaded_addon::query(const char* interface_id) const noexcept {
  return api_.query && api_.addon ? api_.query(api_.addon, interface_id) : nullptr;
}

result<std::unique_ptr<loaded_addon>> loaded_addon::load(const store& s, const std::string& id,
                                                         host_services services) {
  MV_TRY(installed info, s.find(id));
  if (info.state == install_state::needs_update) return err(status::unsupported_format);
  if (info.state != install_state::ok) return err(status::corrupt);
  if (services.data_dir.empty()) services.data_dir = info.data_dir;
  std::unique_ptr<loaded_addon> out(new loaded_addon());
  out->info_ = info;
  out->table_ = std::make_unique<host_table>(std::move(services));
  MV_TRY(shared_library lib,
         shared_library::open(io::join_path(info.dir, io::native_relative(info.m.native))));
  out->lib_ = std::move(lib);
  auto get = reinterpret_cast<mv_addon_get_fn>(out->lib_.symbol(MV_ADDON_ENTRY_SYMBOL));
  if (!get) return err(status::corrupt);
  mv_addon_api api{};
  const mv_status st = get(MV_ADDON_HOST_API, out->table_->api(), &api);
  if (st != MV_OK) return err(static_cast<status>(st));
  if (api.struct_size < sizeof(mv_addon_api) || !api.query || !api.shutdown) {
    if (api.shutdown && api.addon) api.shutdown(api.addon);
    return err(status::corrupt);
  }
  out->api_ = api;
  return out;
}

}  // namespace mv::addon
