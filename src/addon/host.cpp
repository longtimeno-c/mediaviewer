// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <string_view>
#include <span>
#include <thread>
#include <vector>

#include "addon/settings_values.h"
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
      // Table v1's promise: every file listed is on this machine. A copy
      // from it must never start a download.
      if (e.cloud_only) return true;
      mv_addon_file_entry fe{e.path_utf8.c_str(), e.relative_utf8.c_str(), e.name_utf8.c_str(),
                             e.size, e.mtime_unix};
      return visit(user, &fe) != 0;
    });
    return r ? MV_OK : to_mv(r.error());
  });
}

mv_status MV_CALL t_walk2(void*, const char* root, int32_t depth, mv_addon_walk2_fn visit, void* user) {
  return guarded([&] {
    if (!root || !visit) return MV_ERR_INVALID_ARG;
    auto r = io::walk_files(root, depth, [&](const io::tree_entry& e) {
      mv_addon_file_entry2 fe{sizeof(mv_addon_file_entry2), e.cloud_only ? MV_ADDON_FILE_CLOUD_ONLY : 0u,
                              e.path_utf8.c_str(), e.relative_utf8.c_str(), e.name_utf8.c_str(),
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
    // Deep writes to a share, deep reads only from one: a card is still read
    // one request at a time (docs/design/18 "Throughput"; docs/design/12 2026-10-01).
    io::copy_profile_for(req->source_utf8, targets.front()).apply(o);
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

mv_status MV_CALL t_tag_facts(void* host, const char* path, mv_addon_tag_facts* out) {
  return guarded([&] {
    // An add-on built before #405 passes the shorter struct: it gets what it
    // knows, and struct_size says how much was written.
    if (!path || !out || out->struct_size < MV_ADDON_TAG_FACTS_V1_SIZE) return MV_ERR_INVALID_ARG;
    const std::uint32_t size = std::min<std::uint32_t>(out->struct_size, sizeof(mv_addon_tag_facts));
    mv_addon_tag_facts full{};
    full.struct_size = sizeof(mv_addon_tag_facts);
    const auto& facts = self(host).services().tag_facts;
    if (!facts) return MV_ERR_UNSUPPORTED_FORMAT;
    const bool ok = facts(path, full);
    full.struct_size = size;
    std::memcpy(out, &full, size);
    return ok ? MV_OK : MV_ERR_IO;
  });
}

mv_status MV_CALL t_read_cues(void* host, const char* path, int64_t start_ms, int64_t max_ms, uint32_t max_cues,
                              mv_addon_cue_fn visit, void* user) {
  return guarded([&] {
    if (!path || !visit) return MV_ERR_INVALID_ARG;
    const auto& read = self(host).services().read_cues;
    if (!read) return MV_ERR_UNSUPPORTED_FORMAT;
    const expected r = read(path, start_ms, max_ms, max_cues, [&](const host_services::cue& c) {
      mv_addon_cue out{};
      out.struct_size = sizeof(out);
      out.kind = c.kind;
      out.start_ms = c.start_ms;
      out.end_ms = c.end_ms;
      out.text_utf8 = c.text.c_str();
      return visit(user, &out) != 0;
    });
    return r ? MV_OK : to_mv(r.error());
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

// ---- v2: pixels -----------------------------------------------------------

// Copies an image into the caller's buffer; a short buffer reports its size.
mv_status give_rgb(const rgb_image& img, uint8_t* out, uint64_t cap, uint32_t* w, uint32_t* h) {
  if (w) *w = img.width;
  if (h) *h = img.height;
  if (!out || cap < img.rgb.size()) return MV_ERR_INVALID_ARG;
  std::memcpy(out, img.rgb.data(), img.rgb.size());
  return MV_OK;
}

mv_status MV_CALL t_still_rgb(void* host, const char* path, uint32_t max_edge, uint8_t* out,
                              uint64_t cap, uint32_t* w, uint32_t* h) {
  return guarded([&] {
    if (!path || max_edge == 0) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().still_rgb;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    auto img = fn(path, max_edge);
    return img ? give_rgb(*img, out, cap, w, h) : to_mv(img.error());
  });
}

// A sampler handed across the table: the host's object plus the one frame
// that did not fit the caller's buffer, kept until it asks again.
struct sampler_box {
  std::unique_ptr<video_sampler> s;
  bool pending = false;
  sampled_frame held;
};

mv_status MV_CALL t_sampler_open(void* host, const char* path, const mv_addon_sampler_options* o,
                                 mv_addon_video_info* info, void** out) {
  return guarded([&] {
    if (!path || !out) return MV_ERR_INVALID_ARG;
    *out = nullptr;
    const auto& fn = self(host).services().open_sampler;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    sampler_options opts;
    if (o && o->struct_size >= sizeof(mv_addon_sampler_options)) {
      opts.min_gap_ms = o->min_gap_ms;
      opts.max_gap_ms = o->max_gap_ms;
      opts.max_long_edge = o->max_long_edge ? o->max_long_edge : 512;
      opts.start_ms = o->start_ms;
    }
    auto s = fn(path, opts);
    if (!s) return to_mv(s.error());
    if (info) {
      const video_facts& f = (*s)->facts();
      *info = mv_addon_video_info{};
      info->duration_ms = f.duration_ms;
      info->width = f.width;
      info->height = f.height;
      info->hdr = f.hdr ? 1u : 0u;
    }
    auto box = std::make_unique<sampler_box>();
    box->s = std::move(*s);
    *out = box.release();
    return MV_OK;
  });
}

mv_status MV_CALL t_sampler_next(void*, void* sampler, uint8_t* out, uint64_t cap,
                                 mv_addon_sampled_frame* frame) {
  return guarded([&] {
    if (!sampler || !frame) return MV_ERR_INVALID_ARG;
    auto& box = *static_cast<sampler_box*>(sampler);
    if (!box.pending) {
      auto f = box.s->next();
      if (!f) return to_mv(f.error());
      box.held = std::move(*f);
      box.pending = true;
    }
    const sampled_frame& f = box.held;
    *frame = mv_addon_sampled_frame{};
    frame->width = f.image.width;
    frame->height = f.image.height;
    frame->pts_ms = f.pts_ms;
    frame->pts_tb = f.pts_tb;
    frame->tb_num = f.tb_num;
    frame->tb_den = f.tb_den;
    frame->flags = f.end ? MV_ADDON_FRAME_END
                         : (f.keyframe ? MV_ADDON_FRAME_KEYFRAME : MV_ADDON_FRAME_GRID_FILL);
    if (f.end) {
      box.pending = false;
      return MV_OK;
    }
    if (!out || cap < f.image.rgb.size()) return MV_ERR_INVALID_ARG;  // kept for the retry
    std::memcpy(out, f.image.rgb.data(), f.image.rgb.size());
    box.pending = false;
    return MV_OK;
  });
}

void MV_CALL t_sampler_close(void*, void* sampler) {
  try {
    delete static_cast<sampler_box*>(sampler);
  } catch (...) {
  }
}

mv_status MV_CALL t_video_frame(void* host, const char* path, int64_t pts_ms, uint32_t max_edge,
                                uint8_t* out, uint64_t cap, uint32_t* w, uint32_t* h) {
  return guarded([&] {
    if (!path || max_edge == 0) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().video_frame;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    auto img = fn(path, pts_ms, max_edge);
    return img ? give_rgb(*img, out, cap, w, h) : to_mv(img.error());
  });
}

mv_status MV_CALL t_moment_thumb(void* host, const char* path, int64_t pts_ms, const uint8_t* rgb,
                                 uint32_t width, uint32_t height, char* out, uint32_t cap) {
  return guarded([&] {
    if (!path) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().moment_thumbnail;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    rgb_image img;
    if (rgb) {
      if (width == 0 || height == 0 || width > 16384 || height > 16384) return MV_ERR_INVALID_ARG;
      img.width = width;
      img.height = height;
      img.rgb.assign(rgb, rgb + static_cast<std::size_t>(width) * height * 3);
    }
    auto r = fn(path, pts_ms, rgb ? &img : nullptr);
    return r ? copy_out(*r, out, cap) : to_mv(r.error());
  });
}

mv_status MV_CALL t_piece_dir(void* host, const char* piece, char* out, uint32_t cap) {
  return guarded([&] {
    if (!piece) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().piece_dir;
    if (!fn) return MV_ERR_IO;
    auto r = fn(piece);
    return r ? copy_out(*r, out, cap) : to_mv(r.error());
  });
}

struct audio_box {
  std::unique_ptr<audio_stream> s;
};

mv_status MV_CALL t_audio_open(void* host, const char* path, uint32_t rate, int64_t start_ms,
                               int64_t* duration_ms, void** out) {
  return guarded([&] {
    if (!path || !out || rate < 8000 || rate > 96000) return MV_ERR_INVALID_ARG;
    *out = nullptr;
    const auto& fn = self(host).services().open_audio;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    auto s = fn(path, rate, start_ms);
    if (!s) return to_mv(s.error());
    if (duration_ms) *duration_ms = (*s)->duration_ms();
    auto box = std::make_unique<audio_box>();
    box->s = std::move(*s);
    *out = box.release();
    return MV_OK;
  });
}

mv_status MV_CALL t_audio_read(void*, void* audio, float* out, uint32_t max_samples, uint32_t* count,
                               int64_t* start_ms) {
  return guarded([&] {
    if (!audio || !out || !count || max_samples == 0) return MV_ERR_INVALID_ARG;
    std::int64_t t = 0;
    auto r = static_cast<audio_box*>(audio)->s->read(max_samples, t);
    if (!r) return to_mv(r.error());
    std::memcpy(out, r->data(), r->size() * sizeof(float));
    *count = static_cast<uint32_t>(r->size());
    if (start_ms) *start_ms = t;
    return MV_OK;
  });
}

void MV_CALL t_audio_close(void*, void* audio) {
  try {
    delete static_cast<audio_box*>(audio);
  } catch (...) {
  }
}

mv_status MV_CALL t_thumbnail_jpeg(void* host, const char* path, int64_t pts_ms, uint8_t* out,
                                   uint64_t cap, uint64_t* out_size) {
  return guarded([&] {
    if (!path || !out_size) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().thumbnail_jpeg;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    auto r = fn(path, pts_ms);
    if (!r) return to_mv(r.error());
    *out_size = r->size();
    if (!out || cap < r->size()) return MV_ERR_INVALID_ARG;
    std::memcpy(out, r->data(), r->size());
    return MV_OK;
  });
}

mv_status MV_CALL t_thumbnail_store_jpeg(void* host, const char* path, int64_t pts_ms, const uint8_t* jpeg,
                                         uint64_t size) {
  return guarded([&] {
    if (!path || !jpeg || size == 0) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().store_thumbnail_jpeg;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    auto r = fn(path, pts_ms, std::span<const std::uint8_t>(jpeg, static_cast<std::size_t>(size)));
    return r ? MV_OK : to_mv(r.error());
  });
}

mv_status MV_CALL t_recycle(void* host, const char* path, uint32_t* out_refused) {
  return guarded([&] {
    if (!path || !out_refused) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().recycle;
    if (!fn) return MV_ERR_UNSUPPORTED_FORMAT;
    auto r = fn(path);
    if (!r) return to_mv(r.error());
    *out_refused = *r ? 0u : 1u;
    return MV_OK;
  });
}

mv_status MV_CALL t_settings_get(void* host, const char* key, char* out, uint32_t cap,
                                 uint32_t* out_size) {
  return guarded([&] {
    if (!key || !out_size) return MV_ERR_INVALID_ARG;
    const auto& fn = self(host).services().setting;
    if (!fn) return MV_ERR_IO;
    auto r = fn(key);
    if (!r) return r.error() == status::invalid_arg ? MV_ERR_IO : to_mv(r.error());
    *out_size = static_cast<uint32_t>(r->size() + 1);
    if (!out || cap < r->size() + 1) return MV_ERR_INVALID_ARG;
    std::memcpy(out, r->c_str(), r->size() + 1);
    return MV_OK;
  });
}

// ---- v3: screens and slots (docs/design/25 PRs 58-59) ------------------------------

std::string str_of(const char* s) { return s ? std::string(s) : std::string(); }

mv_status MV_CALL t_screen_post(void* host, const char* screen, uint64_t g, const char* tree) {
  return guarded([&] {
    if (!screen || !tree) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    return to_mv(t.slots().post_screen(t.addon_id(), screen, g, tree));
  });
}

mv_status MV_CALL t_screen_diff(void* host, const char* screen, uint64_t g, const char* diff) {
  return guarded([&] {
    if (!screen || !diff) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    return to_mv(t.slots().post_diff(t.addon_id(), screen, g, diff));
  });
}

mv_status MV_CALL t_surface_post(void* host, const char* screen, const char* surface, uint64_t g,
                                 const uint8_t* bgra, uint32_t w, uint32_t h, uint32_t stride) {
  return guarded([&] {
    if (!screen || !surface) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    return to_mv(t.slots().post_surface(t.addon_id(), screen, surface, g, bgra, w, h, stride));
  });
}

mv_status MV_CALL t_search_post(void* host, const char* provider, uint64_t g,
                                const mv_slot_search_row* rows, uint32_t count, uint32_t done) {
  return guarded([&] {
    if (!provider || (count && !rows)) return MV_ERR_INVALID_ARG;
    std::vector<slot_registry::search_row> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (!rows[i].path_utf8) continue;
      out.push_back({rows[i].path_utf8, str_of(rows[i].why_utf8)});
    }
    host_table& t = self(host);
    return to_mv(t.slots().post_search(t.addon_id(), provider, g, std::move(out), done != 0));
  });
}

void MV_CALL t_search_progress(void* host, const char* provider, uint64_t g, uint32_t done,
                               uint32_t total) {
  (void)guarded([&] {
    if (!provider) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    t.slots().search_progress(t.addon_id(), provider, g, done, total);
    return MV_OK;
  });
}

int32_t MV_CALL t_is_stale(void* host, uint64_t g) {
  try {
    host_table& t = self(host);
    return t.slots().is_stale(t.addon_id(), g) ? 1 : 0;
  } catch (...) {
    return 1;
  }
}

mv_status MV_CALL t_badges_post(void* host, const char* badge, uint64_t g, const char* const* paths,
                                uint32_t count, uint32_t replace) {
  return guarded([&] {
    if (!badge || (count && !paths)) return MV_ERR_INVALID_ARG;
    std::vector<std::string> out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (paths[i]) out.emplace_back(paths[i]);
    }
    host_table& t = self(host);
    return to_mv(t.slots().post_badges(t.addon_id(), badge, g, std::move(out), replace != 0));
  });
}

mv_status MV_CALL t_status_post(void* host, const char* item, const char* text) {
  return guarded([&] {
    if (!item) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    return to_mv(t.slots().post_status(t.addon_id(), item, str_of(text)));
  });
}

mv_status MV_CALL t_screen_show(void* host, const char* screen, uint32_t show) {
  return guarded([&] {
    if (!screen) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    return to_mv(t.slots().show_screen(t.addon_id(), screen, show != 0));
  });
}

mv_status MV_CALL t_location_of(void* host, const char* path, mv_addon_location* out) {
  return guarded([&] {
    if (!path || !out) return MV_ERR_INVALID_ARG;
    *out = mv_addon_location{};
    auto st = io::stat_path(path);
    if (!st || st->is_directory) return MV_ERR_IO;
    *out = cached_location(path, st->size, st->mtime_unix, self(host).services().location);
    return MV_OK;
  });
}

mv_status MV_CALL t_locations_in(void* host, const char* folder, uint64_t g, mv_addon_location_fn visit,
                                 void* user) {
  return guarded([&] {
    if (!folder || !visit) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    mv_status result = MV_OK;
    // Not recursive (depth 0): the folder the person is looking at. Files only
    // a cloud provider holds are skipped: reading them would download them.
    auto walked = io::walk_files(folder, 0, [&](const io::tree_entry& e) {
      if (e.cloud_only) return true;
      // Stale also once the add-on is being removed: detach joins this worker,
      // so the walk must notice and stop rather than finish the folder.
      const auto stale = [&] { return t.slots().is_stale(t.addon_id(), g); };
      if (stale()) {
        result = MV_ERR_CANCELLED;
        return false;
      }
      mv_addon_location_entry entry{};
      entry.path_utf8 = e.path_utf8.c_str();
      // A cache hit costs no read, so it does not wait.
      if (!peek_cached_location(e.path_utf8, e.size, e.mtime_unix, entry.location)) {
        // Background priority: a metadata read waits while the viewer presents
        // (docs/design/18 "Priority"), bounded, and still hears a cancel.
        if (t.services().should_yield) {
          for (int spins = 0; spins < 200 && t.services().should_yield(); ++spins) {
            if (stale()) {
              result = MV_ERR_CANCELLED;
              return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
        }
        entry.location = cached_location(e.path_utf8, e.size, e.mtime_unix, t.services().location);
      }
      return visit(user, &entry) != 0;
    });
    if (result != MV_OK) return result;
    if (!walked && walked.error() != status::cancelled) return to_mv(walked.error());
    return MV_OK;
  });
}

// #398: the Local search index's located assets. No file is read: the source
// serves what the index holds, a batch at a time.
mv_status MV_CALL t_locations_of_index(void* host, uint64_t g, mv_addon_location_fn visit, void* user) {
  return guarded([&] {
    if (!visit) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    // Stale also once the add-on is being removed: detach joins this worker.
    if (t.slots().is_stale(t.addon_id(), g)) return MV_ERR_CANCELLED;
    bool stale = false;
    bool stopped = false;  // the add-on's visit returned 0: as locations_in, MV_OK
    std::uint32_t n = 0;
    const index_location_visit each = [&](const mv_addon_location_entry& e) {
      // is_stale takes the registry's lock: ask once every 64 rows.
      if ((n++ & 63u) == 0 && t.slots().is_stale(t.addon_id(), g)) {
        stale = true;
        return false;
      }
      if (!e.path_utf8 || !e.location.has) return true;
      if (visit(user, &e) != 0) return true;
      stopped = true;
      return false;
    };
    const auto& svc = t.services().index_locations;
    const expected r = svc ? svc(each) : each_index_location(each);
    if (stale) return MV_ERR_CANCELLED;
    if (r || stopped) return MV_OK;
    // Cancelled by neither: the index went mid-walk (the pack is going).
    if (r.error() == status::cancelled) return MV_ERR_NOT_FOUND;
    return to_mv(r.error());
  });
}

// #452: a notice (a system notification the hosts draw), the permission ask,
// and keeping the process with every window closed. Tables, never a wait.
mv_status MV_CALL t_notice_post(void* host, const char* notice_id, const mv_addon_notice* n) {
  return guarded([&] {
    if (!notice_id) return MV_ERR_INVALID_ARG;
    std::optional<slot_registry::notice_fields> f;
    if (n) {
      if (n->struct_size < sizeof(mv_addon_notice) || !n->screen_id || !n->title_utf8) return MV_ERR_INVALID_ARG;
      f = slot_registry::notice_fields{n->screen_id, n->title_utf8, str_of(n->body_utf8), str_of(n->action_id),
                                       str_of(n->action_label)};
    }
    host_table& t = self(host);
    return to_mv(t.slots().post_notice(t.addon_id(), notice_id, std::move(f)));
  });
}

mv_status MV_CALL t_notice_authorize(void* host) {
  return guarded([&] {
    host_table& t = self(host);
    return to_mv(t.slots().ask_notice_permission(t.addon_id()));
  });
}

mv_status MV_CALL t_keep_alive(void* host, uint32_t on, const char* screen, const char* label) {
  return guarded([&] {
    if (on && !screen) return MV_ERR_INVALID_ARG;
    host_table& t = self(host);
    return to_mv(t.slots().keep_alive(t.addon_id(), on != 0, str_of(screen), str_of(label)));
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
  api_.decode_still_rgb = &t_still_rgb;
  api_.sampler_open = &t_sampler_open;
  api_.sampler_next = &t_sampler_next;
  api_.sampler_close = &t_sampler_close;
  api_.video_frame_rgb = &t_video_frame;
  api_.moment_thumbnail = &t_moment_thumb;
  api_.piece_dir = &t_piece_dir;
  api_.audio_open = &t_audio_open;
  api_.audio_read = &t_audio_read;
  api_.audio_close = &t_audio_close;
  api_.thumbnail_jpeg = &t_thumbnail_jpeg;
  api_.thumbnail_store_jpeg = &t_thumbnail_store_jpeg;
  api_.recycle_file = svc_.recycle ? &t_recycle : nullptr;
  api_.walk_files2 = &t_walk2;
  api_.settings_get = &t_settings_get;
  api_.screen_post = &t_screen_post;
  api_.screen_diff = &t_screen_diff;
  api_.surface_post = &t_surface_post;
  api_.search_post = &t_search_post;
  api_.search_progress = &t_search_progress;
  api_.is_stale = &t_is_stale;
  api_.badges_post = &t_badges_post;
  api_.status_post = &t_status_post;
  api_.screen_show = &t_screen_show;
  api_.location_of = &t_location_of;
  api_.locations_in = &t_locations_in;
  api_.tag_facts = svc_.tag_facts ? &t_tag_facts : nullptr;
  api_.read_cues = svc_.read_cues ? &t_read_cues : nullptr;
  // Always set: whether an index is attached changes while the add-on runs
  // (MV_ERR_NOT_FOUND meanwhile).
  api_.locations_of_index = &t_locations_of_index;
  api_.notice_post = &t_notice_post;
  api_.notice_authorize = &t_notice_authorize;
  api_.keep_alive = &t_keep_alive;
}

void host_table::set_negotiated(std::uint32_t version) noexcept { api_.host_api = version; }

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
  // Its screens and slots go first: on return the registry's worker is not
  // inside the add-on and never will be again (docs/design/25: removing an
  // add-on removes every item, no restart).
  if (slots_) slots_->detach(info_.id);
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
                                                         host_services services, const char* entry) {
  MV_TRY(installed info, s.find(id));
  if (info.state == install_state::needs_update) return err(status::unsupported_format);
  if (info.state != install_state::ok) return err(status::corrupt);
  if (services.data_dir.empty()) services.data_dir = info.data_dir;
  if (!services.piece_dir) {
    // Only this add-on's own verified pieces; a store copy is a root, a key
    // and a version, and every call re-verifies (docs/design/18 verify-before-load).
    services.piece_dir = [s, parent = info.id](const std::string& piece) -> result<std::string> {
      MV_TRY(installed p, s.find(piece));
      if (p.m.part_of != parent) return err(status::invalid_arg);
      if (p.state != install_state::ok) return err(status::corrupt);
      return p.dir;
    };
  }
  if (!services.setting) {
    // Its own page only: the id and the page are the verified manifest's.
    services.setting = [id = info.m.id, page = info.m.settings](const std::string& key)
        -> result<std::string> {
      MV_TRY(settings_values values, default_settings_values());
      return values.value_json(id, /*publisher: the app's own*/ "", page, key);
    };
  }
  std::unique_ptr<loaded_addon> out(new loaded_addon());
  out->info_ = info;
  out->table_ = std::make_unique<host_table>(std::move(services));
  out->table_->set_addon_id(info.id);
  // The newest table both sides know (mediaviewer_addon.h "Negotiation").
  const std::uint32_t version = negotiated_host_api(info.m, MV_ADDON_HOST_API);
  out->table_->set_negotiated(version);
  MV_TRY(shared_library lib,
         shared_library::open(io::join_path(info.dir, io::native_relative(info.m.native))));
  out->lib_ = std::move(lib);
  if (!entry) return err(status::invalid_arg);
  auto get = reinterpret_cast<mv_addon_get_fn>(out->lib_.symbol(entry));
  if (!get) {
    // The one export missing is a broken add-on; another door missing is an
    // add-on older than the host asking for it.
    return err(std::string_view(entry) == MV_ADDON_ENTRY_SYMBOL ? status::corrupt : status::unsupported_format);
  }
  mv_addon_api api{};
  const mv_status st = get(version, out->table_->api(), &api);
  if (st != MV_OK) return err(static_cast<status>(st));
  if (api.struct_size < sizeof(mv_addon_api) || !api.query || !api.shutdown) {
    if (api.shutdown && api.addon) api.shutdown(api.addon);
    return err(status::corrupt);
  }
  out->api_ = api;
  // Screens and slots (docs/design/25 §7): resolved once, here, on the loader's
  // thread. Only for a table that carries the v3 calls the add-on answers with.
  if (!info.m.slots.empty() && version >= kSlotsHostApi) {
    if (const auto* table = static_cast<const mv_addon_slots*>(out->query(MV_SLOTS_INTERFACE))) {
      slot_registry& reg = out->table_->slots();
      contributions c = info.m.slots;
      for (const manifest_command& cmd : info.m.commands) c.commands.push_back(cmd.id);
      if (reg.attach(info.id, info.m.name, std::move(c), native_slot_client(api.addon, table), info.dir) ==
          status::ok) {
        out->slots_ = &reg;
      }
    }
  }
  return out;
}

// ---- Quit ------------------------------------------------------------------------

namespace {

// Heap-held and never freed: a stop thread may still finish, and the host's
// exit guard may still ask, once static destructors have begun.
struct exit_state {
  std::mutex m;
  std::condition_variable cv;
  int stopping = 0;   // stop_for_exit threads not yet finished
  int abandoned = 0;  // left running for the exit
};

exit_state& exits() {
  static exit_state* const s = new exit_state;
  return *s;
}

}  // namespace

void stop_for_exit(std::unique_ptr<loaded_addon> addon) noexcept {
  if (!addon) return;
  exit_state& s = exits();
  {
    std::lock_guard lock(s.m);
    ++s.stopping;
  }
  loaded_addon* raw = addon.release();
  try {
    std::thread([raw] {
      delete raw;  // the add-on's shutdown (joins its threads), then its library
      exit_state& st = exits();
      std::lock_guard lock(st.m);
      --st.stopping;
      st.cv.notify_all();
    }).detach();
  } catch (...) {
    // No thread to stop it on: it runs until the exit reclaims it.
    std::lock_guard lock(s.m);
    --s.stopping;
    ++s.abandoned;
  }
}

void abandon_for_exit(std::unique_ptr<loaded_addon> addon) noexcept {
  if (!addon) return;
  (void)addon.release();
  exit_state& s = exits();
  std::lock_guard lock(s.m);
  ++s.abandoned;
}

bool wait_stopped_for_exit(std::chrono::steady_clock::time_point deadline) noexcept {
  exit_state& s = exits();
  std::unique_lock lock(s.m);
  (void)s.cv.wait_until(lock, deadline, [&] { return s.stopping == 0; });
  return s.stopping == 0 && s.abandoned == 0;
}

bool running_at_exit() noexcept {
  exit_state& s = exits();
  std::lock_guard lock(s.m);
  return s.stopping != 0 || s.abandoned != 0;
}

}  // namespace mv::addon
