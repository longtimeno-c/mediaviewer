// SPDX-License-Identifier: GPL-2.0-or-later
// Tiled pyramid for large images (plan/04, "Tiled pyramid for large images").
//
// Above ~64 MP — or wider/taller than the 16384 texture limit — a still is not
// one texture:
//
//   * CPU: a floor-half Mitchell pyramid (tile_source), built once on the
//     decode worker. Level 0 is the decoded image itself, not a copy.
//   * An **overview** texture — the first level whose long edge is <= 2048,
//     with its full mip chain — is uploaded with the publish and stays
//     resident. It is every level coarser than the tiles, so "the coarsest
//     2-3 levels resident always" holds and a zoom-out is blurry, never blank.
//   * Every finer level is split into 256x256 tiles. A tile texture is 260x260
//     (a 2-texel clamped border for bilinear and Catmull-Rom across seams) with
//     one extra mip, so a level is minified by at most 2x.
//   * The render thread says which tiles it wants (visible at the current LOD
//     plus a one-tile ring) through lock-free per-tile states; the session's
//     tile_service thread builds and creates them, a few per refresh interval
//     (plan/03 upload budget). The render thread draws ready tiles coarse to
//     fine over the overview and evicts least-recently-used tiles above a VRAM
//     budget. It never waits on the service and never creates a texture.
//
// CPU memory retained for a tiled image: the decoded full-resolution RGBA
// (which the session already kept for device-rebuild re-upload) plus the
// pyramid levels below it, one third again — 533 MB for a 100 MP still, held
// only while that image is current, inside plan/02's min(25 % RAM, 4 GB).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/job_system.h"
#include "core/result.h"
#include "gfx/blit.h"
#include "gfx/device.h"
#include "image/colour.h"
#include "image/gpu_image.h"

namespace mv::image {

inline constexpr std::uint32_t k_tile_size = 256;
inline constexpr std::uint32_t k_tile_border = 2;
inline constexpr std::uint32_t k_tile_texture = k_tile_size + 2 * k_tile_border;  // 260
inline constexpr std::uint32_t k_overview_max_edge = 2048;
inline constexpr std::uint64_t k_tiled_pixel_threshold = 64ull * 1000ull * 1000ull;
// Resident tile VRAM. ~750 tiles: a 4K canvas at 100 % with its ring and the
// two fallback levels is ~260. Separate from (and far below) the 512 MB viewer
// LRU a single-texture 100 MP still used to take on its own.
inline constexpr std::uint64_t k_tile_vram_budget = 256ull * 1024 * 1024;
inline constexpr std::uint64_t k_tile_bytes =
    static_cast<std::uint64_t>(k_tile_texture) * k_tile_texture * 4 +
    static_cast<std::uint64_t>(k_tile_texture / 2) * (k_tile_texture / 2) * 4;
// Tiles created per refresh-sized tick, and requests outstanding at once.
inline constexpr std::uint32_t k_tiles_per_tick = 4;
inline constexpr std::uint32_t k_tile_tick_ms = 16;
inline constexpr std::uint32_t k_max_inflight_tiles = 24;

[[nodiscard]] constexpr bool needs_tiles(std::uint32_t width, std::uint32_t height) noexcept {
  return static_cast<std::uint64_t>(width) * height > k_tiled_pixel_threshold ||
         width > 16384u || height > 16384u;
}

// ---------------------------------------------------------------------------
// Pure geometry. No GPU, no allocation after make().
// ---------------------------------------------------------------------------
struct tile_level {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t tiles_x = 0;
  std::uint32_t tiles_y = 0;
  std::uint32_t first_index = 0;  // into the flat tile array (tile levels only)
};

struct tile_range {
  std::uint32_t level = 0;
  std::uint32_t x0 = 0, y0 = 0;  // inclusive
  std::uint32_t x1 = 0, y1 = 0;  // exclusive; empty when x0 == x1 or y0 == y1
  [[nodiscard]] bool empty() const noexcept { return x0 >= x1 || y0 >= y1; }
};

class tile_layout {
 public:
  [[nodiscard]] static tile_layout make(std::uint32_t width, std::uint32_t height) noexcept;

  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  // All levels down to 1x1 (the D3D11 chain).
  [[nodiscard]] std::uint32_t level_count() const noexcept { return level_count_; }
  // First level whose long edge is <= k_overview_max_edge. Levels below it are
  // tiled; it and everything coarser live in the overview texture.
  [[nodiscard]] std::uint32_t overview_level() const noexcept { return overview_level_; }
  [[nodiscard]] std::uint32_t tile_count() const noexcept { return tile_count_; }
  [[nodiscard]] const tile_level& level(std::uint32_t l) const noexcept { return levels_[l]; }

  // Full-resolution pixels per level pixel on each axis (exact, not 2^l: a
  // floor-half level of an odd image covers the same rect).
  [[nodiscard]] float scale_x(std::uint32_t l) const noexcept;
  [[nodiscard]] float scale_y(std::uint32_t l) const noexcept;

  // The level to draw at `zoom` (1.0 = 100 %): the coarsest level still at or
  // above one texel per screen pixel, so it is minified by less than 2x.
  // overview_level() means "no tiles, the overview is enough".
  [[nodiscard]] std::uint32_t lod_for_zoom(float zoom) const noexcept;

  // Tiles of `level` intersecting the view (camera pan = image pixel at the
  // view centre), grown by `ring` tiles and clamped to the level.
  [[nodiscard]] tile_range visible(std::uint32_t level, float pan_x, float pan_y, float zoom,
                                   float view_w, float view_h, std::uint32_t ring) const noexcept;

  [[nodiscard]] std::uint32_t index(std::uint32_t level, std::uint32_t tx,
                                    std::uint32_t ty) const noexcept {
    return levels_[level].first_index + ty * levels_[level].tiles_x + tx;
  }

 private:
  static constexpr std::uint32_t kMaxLevels = 32;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  std::uint32_t level_count_ = 0;
  std::uint32_t overview_level_ = 0;
  std::uint32_t tile_count_ = 0;
  tile_level levels_[kMaxLevels]{};
};

// ---------------------------------------------------------------------------
// CPU pyramid. Immutable after build; shared by the tile service and a device
// rebuild.
// ---------------------------------------------------------------------------
class tile_source {
 public:
  // Worker only. Builds levels 1..N from `full` (checks `ctx` per row block).
  [[nodiscard]] static result<std::shared_ptr<const tile_source>> build(
      std::shared_ptr<const display_image> full, const job_context* ctx = nullptr);

  [[nodiscard]] const tile_layout& layout() const noexcept { return layout_; }
  [[nodiscard]] const display_image& full() const noexcept { return *full_; }
  [[nodiscard]] const std::uint8_t* level_rgba(std::uint32_t l) const noexcept;

  // 260x260 padded level pixels for one tile (edge-clamped past the level) and
  // its 130x130 mip.
  void extract_tile(std::uint32_t level, std::uint32_t tx, std::uint32_t ty,
                    std::vector<std::uint8_t>& mip0, std::vector<std::uint8_t>& mip1) const;

 private:
  tile_layout layout_{};
  std::shared_ptr<const display_image> full_;
  std::vector<std::vector<std::uint8_t>> levels_;  // [0] unused (level 0 is full_)
};

class tile_service;

struct tile_stats {
  std::uint32_t lod = 0;
  std::uint32_t overview_level = 0;
  std::uint32_t resident = 0;
  std::uint32_t requested = 0;
  std::uint32_t drawn = 0;
  std::uint64_t created = 0;
  std::uint64_t evicted = 0;
  std::uint64_t vram_bytes = 0;
  std::uint64_t vram_peak_bytes = 0;
  std::uint32_t last_create_us = 0;   // CreateTexture2D alone, last tile
  std::uint32_t max_create_us = 0;
  std::uint32_t last_build_us = 0;    // CPU extract + mip, last tile
};

// What the render thread sees this frame, in canvas pixels.
struct tile_view {
  float pan_x = 0.0f;
  float pan_y = 0.0f;
  float zoom = 1.0f;
  float view_w = 1.0f;
  float view_h = 1.0f;
};

// GPU tiles of one image on one device.
class tile_set {
 public:
  tile_set(ID3D11Device* device, std::shared_ptr<const tile_source> source,
           generation gen, tile_service* service);
  ~tile_set();

  tile_set(const tile_set&) = delete;
  tile_set& operator=(const tile_set&) = delete;

  [[nodiscard]] const tile_layout& layout() const noexcept { return source_->layout(); }
  [[nodiscard]] generation gen() const noexcept { return gen_; }
  [[nodiscard]] const std::shared_ptr<const tile_source>& source() const noexcept {
    return source_;
  }

  // [render thread] Marks what the view needs, requests missing tiles
  // (lock-free), cancels requests the view no longer needs, evicts over
  // budget, and fills `draws` coarse to fine with the ready tiles to draw.
  // Never blocks. `draws` is reused frame to frame.
  void frame(const tile_view& view, std::vector<gfx::tile_quad>& draws) noexcept;

  // [any thread] Bumped every time a tile lands; the render thread redraws
  // when it changes.
  [[nodiscard]] std::uint64_t ready_sequence() const noexcept {
    return ready_seq_.load(std::memory_order_relaxed);
  }
  // [any thread] Requested tiles not yet landed.
  [[nodiscard]] bool pending() const noexcept {
    return outstanding_.load(std::memory_order_relaxed) > 0;
  }
  [[nodiscard]] tile_stats stats() const noexcept;

  // [service thread] Creates up to `max_tiles` requested tiles, oldest request
  // first. Returns how many it created.
  std::uint32_t service(std::uint32_t max_tiles) noexcept;

  // [service thread] The service is going away.
  void detach_service() noexcept { service_.store(nullptr, std::memory_order_release); }

  // [any thread] The service feeding this set, or null once it has gone.
  [[nodiscard]] tile_service* service() const noexcept {
    return service_.load(std::memory_order_acquire);
  }

 private:
  enum : std::uint8_t { k_empty = 0, k_requested = 1, k_creating = 2, k_ready = 3 };
  struct slot {
    std::atomic<std::uint8_t> state{k_empty};
    std::atomic<ID3D11ShaderResourceView*> srv{nullptr};
    std::atomic<std::uint64_t> order{0};
    std::uint64_t last_used = 0;  // render thread only
  };

  void request(std::uint32_t index) noexcept;
  void evict_over_budget() noexcept;
  void push_draws(std::uint32_t level, const tile_range& r, std::vector<gfx::tile_quad>& draws) noexcept;

  gfx::com_ptr<ID3D11Device> device_;
  std::shared_ptr<const tile_source> source_;
  generation gen_ = 0;
  std::atomic<tile_service*> service_{nullptr};
  std::unique_ptr<slot[]> slots_;

  // Render-thread only.
  std::uint64_t frame_ = 0;
  std::uint64_t order_seq_ = 0;
  std::vector<std::uint32_t> requested_;
  std::vector<std::uint32_t> evict_scratch_;
  bool new_requests_ = false;

  std::atomic<std::uint64_t> ready_seq_{0};
  std::atomic<std::int32_t> outstanding_{0};
  std::atomic<std::uint64_t> resident_bytes_{0};
  std::atomic<std::uint64_t> peak_bytes_{0};
  std::atomic<std::uint64_t> created_{0};
  std::atomic<std::uint64_t> evicted_{0};
  std::atomic<std::uint32_t> last_create_us_{0};
  std::atomic<std::uint32_t> max_create_us_{0};
  std::atomic<std::uint32_t> last_build_us_{0};
  std::atomic<std::uint32_t> lod_{0};
  std::atomic<std::uint32_t> drawn_{0};

  // Service thread only.
  std::vector<std::uint8_t> mip0_;
  std::vector<std::uint8_t> mip1_;
};

// One thread per session, started with the first tiled image. It creates the
// tiles the current image's render thread asked for, a few per tick, and frees
// retired sets off the render thread (a tiled image's CPU pyramid is hundreds
// of MB; freeing it inside a frame is a hitch).
class tile_service {
 public:
  using ready_fn = void (*)(void* user) noexcept;

  // `current_generation` is read to skip sets whose view has moved on.
  // `on_ready` wakes the render thread (it may be idle) after tiles land.
  tile_service(const job_system* jobs, ready_fn on_ready, void* user) noexcept;
  ~tile_service();

  tile_service(const tile_service&) = delete;
  tile_service& operator=(const tile_service&) = delete;

  // [worker] Makes `set` the one served; older sets are kept only as long as
  // someone else still holds them. Starts the thread on first use.
  void serve(std::shared_ptr<tile_set> set);

  // [any thread, lock-free, never blocks] Something changed: requests, or a
  // gpu_image holding a set was released.
  void poke() noexcept;

 private:
  void run() noexcept;

  const job_system* jobs_;
  ready_fn on_ready_;
  void* user_;

  std::mutex mutex_;  // never taken by the render thread
  std::condition_variable cv_;
  std::vector<std::shared_ptr<tile_set>> sets_;
  std::atomic<std::uint32_t> pokes_{0};
  bool stop_ = false;
  std::thread thread_;
};

// [worker] Whole tiled publish: CPU pyramid, overview texture, tile set served
// by `service`. The returned gpu_image has width/height = the full image and
// `tiles` set.
[[nodiscard]] result<gpu_image> upload_tiled(ID3D11Device* device,
                                             std::shared_ptr<const display_image> full,
                                             generation gen, tile_service& service,
                                             const job_context* ctx = nullptr,
                                             std::shared_ptr<const tile_source> reuse = nullptr);

}  // namespace mv::image
