// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image/tiles.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

#include "gfx/texture.h"
#include "image/upload.h"

namespace mv::image {

// ---------------------------------------------------------------------------
// tile_layout
// ---------------------------------------------------------------------------
tile_layout tile_layout::make(std::uint32_t width, std::uint32_t height) noexcept {
  tile_layout t;
  if (width == 0 || height == 0) return t;
  t.width_ = width;
  t.height_ = height;
  std::uint32_t w = width;
  std::uint32_t h = height;
  bool overview_found = false;
  std::uint32_t index = 0;
  for (std::uint32_t l = 0; l < kMaxLevels; ++l) {
    tile_level& lv = t.levels_[l];
    lv.width = w;
    lv.height = h;
    t.level_count_ = l + 1;
    if (!overview_found && std::max(w, h) <= k_overview_max_edge) {
      t.overview_level_ = l;
      overview_found = true;
    }
    if (!overview_found) {
      lv.tiles_x = (w + k_tile_size - 1) / k_tile_size;
      lv.tiles_y = (h + k_tile_size - 1) / k_tile_size;
      lv.first_index = index;
      index += lv.tiles_x * lv.tiles_y;
    }
    if (w == 1 && h == 1) break;
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
  }
  t.tile_count_ = index;
  return t;
}

float tile_layout::scale_x(std::uint32_t l) const noexcept {
  return static_cast<float>(width_) / static_cast<float>(levels_[l].width);
}

float tile_layout::scale_y(std::uint32_t l) const noexcept {
  return static_cast<float>(height_) / static_cast<float>(levels_[l].height);
}

std::uint32_t tile_layout::lod_for_zoom(float zoom) const noexcept {
  if (!(zoom > 0.0f) || level_count_ == 0) return overview_level_;
  std::uint32_t l = 0;
  // Step coarser while the next level would still be at least one texel per
  // screen pixel. The small slack keeps exactly 50 % on the coarser level.
  while (l < overview_level_) {
    const float next = zoom * std::min(scale_x(l + 1), scale_y(l + 1));
    if (next > 1.0001f) break;
    ++l;
  }
  return l;
}

tile_range tile_layout::visible(std::uint32_t level, float pan_x, float pan_y, float zoom,
                                float view_w, float view_h, std::uint32_t ring) const noexcept {
  tile_range r;
  r.level = level;
  if (level >= overview_level_ || !(zoom > 0.0f)) return r;
  const tile_level& lv = levels_[level];
  const float half_w = view_w * 0.5f / zoom;
  const float half_h = view_h * 0.5f / zoom;
  const float ts = static_cast<float>(k_tile_size);
  const float left = (pan_x - half_w) / scale_x(level) / ts;
  const float right = (pan_x + half_w) / scale_x(level) / ts;
  const float top = (pan_y - half_h) / scale_y(level) / ts;
  const float bottom = (pan_y + half_h) / scale_y(level) / ts;
  const auto clamp_to = [](float v, std::uint32_t n) -> std::uint32_t {
    if (!(v > 0.0f)) return 0;
    if (v >= static_cast<float>(n)) return n;
    return static_cast<std::uint32_t>(v);
  };
  const float g = static_cast<float>(ring);
  r.x0 = clamp_to(std::floor(left) - g, lv.tiles_x);
  r.y0 = clamp_to(std::floor(top) - g, lv.tiles_y);
  r.x1 = clamp_to(std::floor(right) + 1.0f + g, lv.tiles_x);
  r.y1 = clamp_to(std::floor(bottom) + 1.0f + g, lv.tiles_y);
  return r;
}

// ---------------------------------------------------------------------------
// tile_source
// ---------------------------------------------------------------------------
result<std::shared_ptr<const tile_source>> tile_source::build(
    std::shared_ptr<const display_image> full, const job_context* ctx) {
  if (!full || full->width == 0 || full->height == 0 ||
      full->rgba.size() != static_cast<std::size_t>(full->width) * full->height * 4) {
    return err(status::corrupt);
  }
  auto src = std::make_shared<tile_source>();
  src->layout_ = tile_layout::make(full->width, full->height);
  src->full_ = std::move(full);
  src->levels_.resize(src->layout_.level_count());
  for (std::uint32_t l = 1; l < src->layout_.level_count(); ++l) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    const tile_level& prev = src->layout_.level(l - 1);
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    if (!downsample_half(src->level_rgba(l - 1), prev.width, prev.height, src->levels_[l], w, h,
                         ctx)) {
      return err(status::cancelled);
    }
  }
  return std::shared_ptr<const tile_source>(std::move(src));
}

const std::uint8_t* tile_source::level_rgba(std::uint32_t l) const noexcept {
  return l == 0 ? full_->rgba.data() : levels_[l].data();
}

void tile_source::extract_tile(std::uint32_t level, std::uint32_t tx, std::uint32_t ty,
                               std::vector<std::uint8_t>& mip0,
                               std::vector<std::uint8_t>& mip1) const {
  const tile_level& lv = layout_.level(level);
  const std::uint8_t* src = level_rgba(level);
  const std::int64_t n = k_tile_texture;
  mip0.resize(static_cast<std::size_t>(n * n * 4));
  const std::int64_t ox = static_cast<std::int64_t>(tx) * k_tile_size - k_tile_border;
  const std::int64_t oy = static_cast<std::int64_t>(ty) * k_tile_size - k_tile_border;
  const std::int64_t lw = lv.width;
  const std::int64_t lh = lv.height;
  // Columns inside the level copy as one run; the clamped border either side
  // repeats the edge pixel.
  const std::int64_t run_x0 = std::clamp<std::int64_t>(-ox, 0, n);
  const std::int64_t run_x1 = std::clamp<std::int64_t>(lw - ox, run_x0, n);
  for (std::int64_t j = 0; j < n; ++j) {
    const std::int64_t sy = std::clamp<std::int64_t>(oy + j, 0, lh - 1);
    const std::uint8_t* row = src + static_cast<std::size_t>(sy * lw * 4);
    std::uint8_t* out = mip0.data() + static_cast<std::size_t>(j * n * 4);
    for (std::int64_t i = 0; i < run_x0; ++i) std::memcpy(out + i * 4, row, 4);
    if (run_x1 > run_x0) {
      std::memcpy(out + run_x0 * 4, row + (ox + run_x0) * 4,
                  static_cast<std::size_t>((run_x1 - run_x0) * 4));
    }
    for (std::int64_t i = run_x1; i < n; ++i) std::memcpy(out + i * 4, row + (lw - 1) * 4, 4);
  }
  std::uint32_t w = 0;
  std::uint32_t h = 0;
  (void)downsample_half(mip0.data(), k_tile_texture, k_tile_texture, mip1, w, h, nullptr);
}

// ---------------------------------------------------------------------------
// tile_set
// ---------------------------------------------------------------------------
tile_set::tile_set(ID3D11Device* device, std::shared_ptr<const tile_source> source,
                   generation gen, tile_service* service)
    : device_(device), source_(std::move(source)), gen_(gen) {
  service_.store(service, std::memory_order_release);
  const std::uint32_t n = source_->layout().tile_count();
  slots_ = std::make_unique<slot[]>(n == 0 ? 1 : n);
  requested_.reserve(k_max_inflight_tiles * 2);
  evict_scratch_.reserve(n);
  mip0_.reserve(static_cast<std::size_t>(k_tile_texture) * k_tile_texture * 4);
}

tile_set::~tile_set() {
  const std::uint32_t n = source_ ? source_->layout().tile_count() : 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (ID3D11ShaderResourceView* srv = slots_[i].srv.exchange(nullptr)) srv->Release();
  }
}

void tile_set::request(std::uint32_t index) noexcept {
  slot& s = slots_[index];
  s.last_used = frame_;
  if (s.state.load(std::memory_order_acquire) != k_empty) return;
  if (requested_.size() >= k_max_inflight_tiles) return;
  s.order.store(++order_seq_, std::memory_order_release);
  std::uint8_t expected = k_empty;
  if (s.state.compare_exchange_strong(expected, k_requested, std::memory_order_acq_rel)) {
    requested_.push_back(index);
    new_requests_ = true;
  }
}

void tile_set::push_draws(std::uint32_t level, const tile_range& r,
                          std::vector<gfx::tile_quad>& draws) noexcept {
  if (r.empty()) return;
  const tile_layout& L = layout();
  const tile_level& lv = L.level(level);
  const float sx = L.scale_x(level);
  const float sy = L.scale_y(level);
  for (std::uint32_t ty = r.y0; ty < r.y1; ++ty) {
    for (std::uint32_t tx = r.x0; tx < r.x1; ++tx) {
      slot& s = slots_[L.index(level, tx, ty)];
      if (s.state.load(std::memory_order_acquire) != k_ready) continue;
      ID3D11ShaderResourceView* srv = s.srv.load(std::memory_order_acquire);
      if (!srv) continue;
      s.last_used = frame_;
      gfx::tile_quad q;
      q.srv = srv;
      q.origin_x = static_cast<float>(tx * k_tile_size);
      q.origin_y = static_cast<float>(ty * k_tile_size);
      q.scale_x = sx;
      q.scale_y = sy;
      q.content_w = static_cast<float>(std::min(k_tile_size, lv.width - tx * k_tile_size));
      q.content_h = static_cast<float>(std::min(k_tile_size, lv.height - ty * k_tile_size));
      // Grows only in the first frames of an image; the caller reuses it.
      draws.push_back(q);
    }
  }
}

void tile_set::frame(const tile_view& view, std::vector<gfx::tile_quad>& draws) noexcept {
  ++frame_;
  draws.clear();
  new_requests_ = false;
  const tile_layout& L = layout();
  const std::uint32_t ov = L.overview_level();
  const std::uint32_t lod = L.lod_for_zoom(view.zoom);
  lod_.store(lod, std::memory_order_relaxed);

  if (lod < ov) {
    const tile_range inner =
        L.visible(lod, view.pan_x, view.pan_y, view.zoom, view.view_w, view.view_h, 0);
    const tile_range outer =
        L.visible(lod, view.pan_x, view.pan_y, view.zoom, view.view_w, view.view_h, 1);
    // What is on screen first, then the ring.
    for (std::uint32_t ty = inner.y0; ty < inner.y1; ++ty) {
      for (std::uint32_t tx = inner.x0; tx < inner.x1; ++tx) request(L.index(lod, tx, ty));
    }
    for (std::uint32_t ty = outer.y0; ty < outer.y1; ++ty) {
      for (std::uint32_t tx = outer.x0; tx < outer.x1; ++tx) {
        if (tx >= inner.x0 && tx < inner.x1 && ty >= inner.y0 && ty < inner.y1) continue;
        request(L.index(lod, tx, ty));
      }
    }
    // Coarse to fine: a finer ready tile overdraws its parent, a missing one
    // leaves the parent (or the overview beneath everything) showing.
    const std::uint32_t coarsest = std::min(ov - 1, lod + 2);
    std::size_t before_finest = 0;
    for (std::uint32_t l = coarsest + 1; l-- > lod;) {
      if (l == lod) before_finest = draws.size();
      push_draws(l, L.visible(l, view.pan_x, view.pan_y, view.zoom, view.view_w, view.view_h, 0),
                 draws);
    }
    const std::size_t inner_count =
        static_cast<std::size_t>(inner.x1 - inner.x0) * (inner.y1 - inner.y0);
    if (draws.size() - before_finest < inner_count) {
      incomplete_frames_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  drawn_.store(static_cast<std::uint32_t>(draws.size()), std::memory_order_relaxed);

  // Cancel requests the view has left; count what is still coming.
  std::size_t keep = 0;
  std::int32_t outstanding = 0;
  for (std::uint32_t index : requested_) {
    slot& s = slots_[index];
    std::uint8_t st = s.state.load(std::memory_order_acquire);
    if (st == k_requested && s.last_used != frame_) {
      std::uint8_t expected = k_requested;
      if (s.state.compare_exchange_strong(expected, k_empty, std::memory_order_acq_rel)) continue;
      st = expected;
    }
    if (st == k_requested || st == k_creating) {
      requested_[keep++] = index;
      ++outstanding;
    }
  }
  requested_.resize(keep);
  outstanding_.store(outstanding, std::memory_order_relaxed);

  evict_over_budget();

  if (new_requests_) {
    if (tile_service* svc = service_.load(std::memory_order_acquire)) svc->poke();
  }
}

void tile_set::evict_over_budget() noexcept {
  if (resident_bytes_.load(std::memory_order_relaxed) <= k_tile_vram_budget) return;
  const std::uint32_t n = layout().tile_count();
  evict_scratch_.clear();
  for (std::uint32_t i = 0; i < n; ++i) {
    const slot& s = slots_[i];
    if (s.last_used < frame_ && s.state.load(std::memory_order_acquire) == k_ready) {
      evict_scratch_.push_back(i);
    }
  }
  std::sort(evict_scratch_.begin(), evict_scratch_.end(), [this](std::uint32_t a, std::uint32_t b) {
    return slots_[a].last_used < slots_[b].last_used;
  });
  for (std::uint32_t i : evict_scratch_) {
    if (resident_bytes_.load(std::memory_order_relaxed) <= k_tile_vram_budget) break;
    slot& s = slots_[i];
    std::uint8_t expected = k_ready;
    if (!s.state.compare_exchange_strong(expected, k_empty, std::memory_order_acq_rel)) continue;
    if (ID3D11ShaderResourceView* srv = s.srv.exchange(nullptr, std::memory_order_acq_rel)) {
      srv->Release();
      resident_bytes_.fetch_sub(k_tile_bytes, std::memory_order_relaxed);
    }
    evicted_.fetch_add(1, std::memory_order_relaxed);
  }
}

std::uint32_t tile_set::service(std::uint32_t max_tiles) noexcept {
  const tile_layout& L = layout();
  const std::uint32_t n = L.tile_count();
  std::uint32_t made = 0;
  while (made < max_tiles) {
    std::uint32_t best = n;
    std::uint64_t best_order = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t i = 0; i < n; ++i) {
      const slot& s = slots_[i];
      if (s.state.load(std::memory_order_relaxed) != k_requested) continue;
      const std::uint64_t o = s.order.load(std::memory_order_relaxed);
      if (o < best_order) {
        best_order = o;
        best = i;
      }
    }
    if (best == n) break;
    slot& s = slots_[best];
    std::uint8_t expected = k_requested;
    if (!s.state.compare_exchange_strong(expected, k_creating, std::memory_order_acq_rel)) continue;

    std::uint32_t level = 0;
    while (level + 1 < L.overview_level() && best >= L.level(level + 1).first_index) ++level;
    const tile_level& lv = L.level(level);
    const std::uint32_t local = best - lv.first_index;
    const std::uint32_t tx = local % lv.tiles_x;
    const std::uint32_t ty = local / lv.tiles_x;

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    source_->extract_tile(level, tx, ty, mip0_, mip1_);
    const auto t1 = clock::now();
    const gfx::rgba8_level views[2] = {{mip0_.data(), k_tile_texture, k_tile_texture},
                                       {mip1_.data(), k_tile_texture / 2, k_tile_texture / 2}};
    gfx::com_ptr<ID3D11Texture2D> texture;
    gfx::com_ptr<ID3D11ShaderResourceView> srv;
    const status made_status = gfx::create_srgb_texture(device_.Get(), views, texture, srv);
    const auto t2 = clock::now();

    const auto us = [](clock::duration d) {
      return static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(d).count());
    };
    last_build_us_.store(us(t1 - t0), std::memory_order_relaxed);
    const std::uint32_t create_us = us(t2 - t1);
    last_create_us_.store(create_us, std::memory_order_relaxed);
    if (create_us > max_create_us_.load(std::memory_order_relaxed)) {
      max_create_us_.store(create_us, std::memory_order_relaxed);
    }

    if (made_status == status::ok) {
      s.srv.store(srv.Detach(), std::memory_order_release);
      const std::uint64_t bytes =
          resident_bytes_.fetch_add(k_tile_bytes, std::memory_order_relaxed) + k_tile_bytes;
      std::uint64_t peak = peak_bytes_.load(std::memory_order_relaxed);
      while (bytes > peak &&
             !peak_bytes_.compare_exchange_weak(peak, bytes, std::memory_order_relaxed)) {
      }
      created_.fetch_add(1, std::memory_order_relaxed);
    }
    // A failed create lands as "ready, nothing to draw" so it is not retried
    // every frame; eviction clears it like any other tile.
    s.state.store(k_ready, std::memory_order_release);
    ready_seq_.fetch_add(1, std::memory_order_relaxed);
    ++made;
  }
  return made;
}

tile_stats tile_set::stats() const noexcept {
  tile_stats st;
  st.lod = lod_.load(std::memory_order_relaxed);
  st.overview_level = layout().overview_level();
  st.vram_bytes = resident_bytes_.load(std::memory_order_relaxed);
  st.vram_peak_bytes = peak_bytes_.load(std::memory_order_relaxed);
  st.resident = static_cast<std::uint32_t>(st.vram_bytes / k_tile_bytes);
  st.requested = static_cast<std::uint32_t>(std::max(0, outstanding_.load(std::memory_order_relaxed)));
  st.drawn = drawn_.load(std::memory_order_relaxed);
  st.created = created_.load(std::memory_order_relaxed);
  st.evicted = evicted_.load(std::memory_order_relaxed);
  st.last_create_us = last_create_us_.load(std::memory_order_relaxed);
  st.max_create_us = max_create_us_.load(std::memory_order_relaxed);
  st.last_build_us = last_build_us_.load(std::memory_order_relaxed);
  st.incomplete_frames = incomplete_frames_.load(std::memory_order_relaxed);
  return st;
}

// ---------------------------------------------------------------------------
// tile_service
// ---------------------------------------------------------------------------
tile_service::tile_service(const job_system* jobs, ready_fn on_ready, void* user) noexcept
    : jobs_(jobs), on_ready_(on_ready), user_(user) {}

tile_service::~tile_service() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lock(mutex_);
  for (auto& set : sets_) set->detach_service();
  sets_.clear();
}

void tile_service::serve(std::shared_ptr<tile_set> set) {
  if (!set) return;
  {
    std::lock_guard lock(mutex_);
    if (stop_) return;
    sets_.insert(sets_.begin(), std::move(set));
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
  }
  poke();
}

void tile_service::poke() noexcept {
  pokes_.fetch_add(1, std::memory_order_relaxed);
  cv_.notify_one();
}

void tile_service::run() noexcept {
  using clock = std::chrono::steady_clock;
  std::vector<std::shared_ptr<tile_set>> work;
  std::vector<std::shared_ptr<tile_set>> retired;
  std::uint32_t seen = 0;
  bool more = false;
  while (true) {
    {
      std::unique_lock lock(mutex_);
      if (!more) {
        // A poke racing the wait is caught by the timeout; nothing here is
        // the render thread's latency, only tile arrival.
        cv_.wait_for(lock, std::chrono::milliseconds(100), [&] {
          return stop_ || pokes_.load(std::memory_order_relaxed) != seen;
        });
      }
      if (stop_) break;
      // Read the generation here, awake, and not before the wait. That wait
      // is up to 100 ms long and navigation is exactly what ends it, so a
      // value sampled before it is the view the user has just left: the poke
      // that follows a bump would service it, creating a tick's worth of
      // tiles for an image nobody is looking at.
      const generation current = jobs_->current_generation();
      seen = pokes_.load(std::memory_order_relaxed);
      // A set nobody else holds and whose view has moved on is done. Its CPU
      // pyramid is freed here, on this thread, not in a render-thread release.
      for (auto it = sets_.begin(); it != sets_.end();) {
        if (it->use_count() == 1 && (*it)->gen() != current) {
          retired.push_back(std::move(*it));
          it = sets_.erase(it);
        } else {
          ++it;
        }
      }
      work = sets_;
    }
    retired.clear();

    const auto tick_start = clock::now();
    std::uint32_t made = 0;
    for (auto& set : work) {
      // Fresh each set, not the value from the top of the tick: a bump part
      // way through servicing stops the next set rather than the next tick.
      if (set->gen() != jobs_->current_generation()) continue;
      made += set->service(k_tiles_per_tick - made);
      if (made >= k_tiles_per_tick) break;
    }
    more = made >= k_tiles_per_tick;
    work.clear();
    if (made > 0 && on_ready_) on_ready_(user_);
    if (more) {
      // plan/03 rule 3: a few tile creates per refresh, the rest waits. This
      // thread may wait; the render thread never waits on it.
      std::this_thread::sleep_until(tick_start + std::chrono::milliseconds(k_tile_tick_ms));
    }
  }
}

// ---------------------------------------------------------------------------
// upload_tiled
// ---------------------------------------------------------------------------
result<gpu_image> upload_tiled(ID3D11Device* device, std::shared_ptr<const display_image> full,
                               generation gen, tile_service& service, const job_context* ctx,
                               std::shared_ptr<const tile_source> reuse) {
  if (!device) return err(status::invalid_arg);
  if (!full) return err(status::corrupt);
  std::shared_ptr<const tile_source> source;
  if (reuse && &reuse->full() == full.get()) {
    source = std::move(reuse);
  } else {
    auto built = tile_source::build(full, ctx);
    if (!built) return err(built.error());
    source = std::move(built).value();
  }
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  const tile_layout& L = source->layout();
  const std::uint32_t ov = L.overview_level();
  std::vector<gfx::rgba8_level> views;
  for (std::uint32_t l = ov; l < L.level_count() && views.size() < 16; ++l) {
    views.push_back({source->level_rgba(l), L.level(l).width, L.level(l).height});
  }

  gpu_image out;
  const status made = gfx::create_srgb_texture(device, views, out.texture, out.srv);
  if (made != status::ok) return err(made);
  out.device = device;
  out.width = full->width;
  out.height = full->height;
  out.texture_width = L.level(ov).width;
  out.texture_height = L.level(ov).height;
  out.mip_levels = static_cast<std::uint32_t>(views.size());
  out.generation = gen;
  out.format = full->format;
  out.icc_tagged = full->icc_tagged;
  out.quality = gpu_quality::full;
  out.mean_luma = mean_luma(*full);
  out.tiles = std::make_shared<tile_set>(device, std::move(source), gen, &service);
  service.serve(out.tiles);
  return out;
}

}  // namespace mv::image
