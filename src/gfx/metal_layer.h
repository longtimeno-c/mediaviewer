// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// CAMetalLayer hosted in an AppKit view. maxDrawables = 1, 8-bit sRGB (D6).
// The display link is created against this layer; the lab waits on it before
// encode (docs/design/15 PR 16).
#pragma once

#include <cstdint>

#include "core/result.h"
#include "gfx/device_mac.h"

namespace mv::gfx {

class metal_layer {
 public:
  metal_layer() = default;
  ~metal_layer();

  metal_layer(const metal_layer&) = delete;
  metal_layer& operator=(const metal_layer&) = delete;

  // `nsview` is an NSView whose backing layer is already a CAMetalLayer.
  [[nodiscard]] expected attach(void* nsview, metal_device& dev, std::uint32_t width,
                                std::uint32_t height, double contents_scale) noexcept;
  void destroy() noexcept;

  [[nodiscard]] bool valid() const noexcept { return layer_ != nullptr; }
  // Sets the drawable size and scale, only when the layer's own differ.
  [[nodiscard]] expected resize(std::uint32_t width, std::uint32_t height,
                                double contents_scale) noexcept;
  // The layer's drawable size or contents scale is not `width` x `height` at
  // `contents_scale`: something other than resize() changed them. Before
  // 2026-11-04 every chrome move re-set both and hid such a change; once
  // resize() skipped unchanged sizes, a layer that no longer matched the
  // canvas stayed so (2026-11-05: the photo drawn larger than the canvas and
  // cut off, Fit not fixing it, brush dabs off the pointer). Two property
  // reads; the lab checks every frame it draws.
  [[nodiscard]] bool drifted(std::uint32_t width, std::uint32_t height, double contents_scale) const noexcept;

  [[nodiscard]] void* native_layer() const noexcept { return layer_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] double refresh_interval_seconds() const noexcept { return refresh_seconds_; }
  void refresh_output_info() noexcept;
  // How many times resize() assigned the drawable size (a real change only):
  // --pane-drag-soak checks a column drag never does.
  [[nodiscard]] std::uint32_t drawable_sets() const noexcept { return drawable_sets_; }

 private:
  void* view_ = nullptr;
  void* layer_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  double scale_ = 0.0;
  double refresh_seconds_ = 0.0;
  std::uint32_t drawable_sets_ = 0;
};

}  // namespace mv::gfx
