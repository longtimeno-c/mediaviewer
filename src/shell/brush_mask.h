// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-refresh.md S4a: the Remove brush's committed strokes as one
// tint texture over the photo, instead of hundreds of ImGui circles a frame.
//
//   * brush_mask_sync (UI thread) knows which dabs the canvas's texture holds,
//     which build is in flight, and which dabs still travel live in the
//     snapshot (edit_view::brush, at most kBrushDabs).
//   * brush_mask_builder (one pool job at a time) rasterises the dabs into a
//     coverage buffer at kBrushMaskLongSide on the long side, adding only the
//     new dabs when a job extends the last one, then tints it: the theme's
//     accent at 40 %, a 1 px darker edge, erase writes 0. Straight alpha,
//     RGBA8 UNORM (as ImGui's own textures are).
//   * the worker hands the pixels to the present lab, which creates one
//     immutable texture (D3D11_SUBRESOURCE_DATA / MTLTexture at creation) and
//     hands it to the render thread by an atomic exchange, like the working
//     texture. At 1024 on the long side it is at most 4 MB, well inside the
//     ~2 ms upload budget (image/upload.h k_frame_budget_create_bytes).
//
// A build runs after a stroke ends, an undo, a redo or a clear, and during a
// long stroke every kBrushMaskFlushDabs dabs (kBrushMaskFlushEraseDabs while
// erasing, so the erase shows), so the live tail stays short. Portable: no
// gfx, no ImGui, no platform header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "shell/remove_brush.h"

namespace mv::shell {

inline constexpr std::uint32_t kBrushMaskLongSide = 1024;
inline constexpr std::size_t kBrushMaskFlushDabs = 256;
inline constexpr std::size_t kBrushMaskFlushEraseDabs = 32;
// The tint: the accent at 40 % inside, its edge pixel darker and stronger.
inline constexpr std::uint8_t kBrushMaskFillAlpha = 102;
inline constexpr std::uint8_t kBrushMaskEdgeAlpha = 200;

// What a build holds: the first `count` dabs of the brush at `gen`, for the
// host's `item` and mask `epoch` (shell::brush_mask_sync::epoch), over a shown
// picture of frame_w x frame_h, tinted with `accent` (0xRRGGBBAA).
struct brush_mask_tag {
  std::uint64_t item = 0;
  std::uint32_t epoch = 0;
  std::uint32_t gen = 0;
  std::uint32_t count = 0;
  std::uint32_t frame_w = 0;
  std::uint32_t frame_h = 0;
  std::uint32_t accent = 0;
  friend bool operator==(const brush_mask_tag&, const brush_mask_tag&) = default;
};

struct brush_mask_job {
  brush_mask_tag tag;
  std::vector<brush_dab> dabs;  // the first tag.count dabs
};

struct brush_mask_pixels {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;  // width * height * 4, straight alpha
};

// The raster's size for a shown picture: kBrushMaskLongSide on the long side
// (never larger than the picture), the picture's aspect. {0, 0} for none.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> brush_mask_size(std::uint32_t frame_w,
                                                                      std::uint32_t frame_h) noexcept;

// Stamps `dabs` in order into `cover` (w x h, 255 painted, 0 not; erase dabs
// write 0). A dab smaller than a pixel still marks the pixel its centre is in.
void stamp_brush_dabs(std::span<const brush_dab> dabs, std::uint32_t w, std::uint32_t h,
                      std::vector<std::uint8_t>& cover) noexcept;

// The tint of `cover` (w x h) into `rgba`: painted pixels the accent at
// kBrushMaskFillAlpha, a painted pixel with an unpainted 4-neighbour the
// accent at 60 % brightness and kBrushMaskEdgeAlpha, the rest transparent
// (their colour the edge's, so filtering draws no dark fringe).
void tint_brush_mask(std::span<const std::uint8_t> cover, std::uint32_t w, std::uint32_t h, std::uint32_t accent,
                     std::vector<std::uint8_t>& rgba);

// Worker side. One job at a time (the host never has two in flight); keeps the
// coverage of the last job so the next one that extends it stamps only the new
// dabs.
class brush_mask_builder {
 public:
  // The tint of the job's dabs into `out`. False for nothing to draw (no
  // dabs, no frame).
  [[nodiscard]] bool build(const brush_mask_job& job, brush_mask_pixels& out);
  // Dabs the last build stamped (all, or only the new ones).
  [[nodiscard]] std::size_t last_stamped() const noexcept { return last_stamped_; }

 private:
  std::vector<std::uint8_t> cover_;
  brush_mask_tag held_{};
  std::uint32_t w_ = 0, h_ = 0;
  bool valid_ = false;
  std::size_t last_stamped_ = 0;
};

// UI thread: what the canvas's mask holds and what to build next.
class brush_mask_sync {
 public:
  // After any change to the brush: the build to start now, or nullopt (one is
  // in flight, the mask is up to date, or a stroke has not run far enough to
  // flush). An empty brush asks for nothing: the host drops the texture.
  [[nodiscard]] std::optional<brush_mask_job> next(const remove_brush& b, std::uint64_t item,
                                                   std::uint32_t frame_w, std::uint32_t frame_h,
                                                   std::uint32_t accent);
  // The build `tag` finished: `ok` when the lab took its texture.
  void landed(const brush_mask_tag& tag, bool ok) noexcept;
  // The lab's texture was dropped (the brush was cleared).
  void dropped() noexcept { landed_.reset(); }
  // Another item or a new geometry: whatever the lab holds or is building is
  // stale. Bumps the epoch the canvas draws a texture for.
  void reset() noexcept;

  // The first dab the canvas draws live: the count a landed build of this
  // epoch and generation holds (never one still in flight), else 0.
  [[nodiscard]] std::size_t tail_from(const remove_brush& b) const noexcept;
  // Whether the canvas draws its texture: something is brushed and a build of
  // this epoch landed (after an undo it may still show the undone stroke for
  // the few milliseconds until the next build lands).
  [[nodiscard]] bool mask_on(const remove_brush& b) const noexcept {
    return !b.empty() && landed_ && landed_->epoch == epoch_;
  }
  [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] bool in_flight() const noexcept { return in_flight_.has_value(); }

 private:
  [[nodiscard]] bool holds_prefix(const std::optional<brush_mask_tag>& t, const remove_brush& b) const noexcept {
    return t && t->epoch == epoch_ && t->gen == b.generation() && t->count <= b.dabs().size();
  }

  std::optional<brush_mask_tag> landed_;
  std::optional<brush_mask_tag> in_flight_;
  std::optional<brush_mask_tag> failed_;
  std::uint32_t epoch_ = 1;
};

}  // namespace mv::shell
