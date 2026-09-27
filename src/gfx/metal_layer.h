// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// CAMetalLayer hosted in an AppKit view. maxDrawables = 1, 8-bit sRGB (D6).
// The display link is created against this layer; the lab waits on it before
// encode (plan/15 PR 16).
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
  [[nodiscard]] expected resize(std::uint32_t width, std::uint32_t height,
                                double contents_scale) noexcept;

  [[nodiscard]] void* native_layer() const noexcept { return layer_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] double refresh_interval_seconds() const noexcept { return refresh_seconds_; }
  void refresh_output_info() noexcept;

 private:
  void* view_ = nullptr;
  void* layer_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
  double refresh_seconds_ = 0.0;
};

}  // namespace mv::gfx
