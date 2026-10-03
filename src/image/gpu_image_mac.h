// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Immutable Metal texture produced on a decode worker (docs/design/15 PR 17 — the
// Metal twin of image/gpu_image.h, which is D3D11-only and stays that way).
#pragma once

#include <cstdint>

#include "codec/format.h"

namespace mv::image {

// Same field set and semantics as image::gpu_image (image/gpu_image.h), minus
// tiling. `item_id` is the identity for preview → full refinement: the lab
// stamps every open with a fresh id, so a later image carrying the same id is
// the same picture arriving at a better resolution (camera_.refine), not a
// new item (camera_.fit).
struct gpu_image_mac {
  gpu_image_mac() noexcept = default;
  ~gpu_image_mac();

  gpu_image_mac(gpu_image_mac&& other) noexcept;
  gpu_image_mac& operator=(gpu_image_mac&& other) noexcept;

  gpu_image_mac(const gpu_image_mac&) = delete;
  gpu_image_mac& operator=(const gpu_image_mac&) = delete;

  // id<MTLTexture>, __bridge_retained. Released by the destructor (upload_mac.mm).
  void* texture = nullptr;

  // The image this texture stands for, in full-resolution pixels.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // The texture's own top-level size (== width/height in PR 17: no tiling yet).
  std::uint32_t texture_width = 0;
  std::uint32_t texture_height = 0;
  std::uint32_t mip_levels = 0;
  codec::format_family format = codec::format_family::unknown;
  bool icc_tagged = false;
  std::uint8_t mean_luma = 0;
  std::uint64_t item_id = 0;
  bool preview = false;

  [[nodiscard]] bool valid() const noexcept { return texture != nullptr; }

  // A second owner of the same immutable texture (one more retain), for the
  // lab's still cache: the render thread takes one, the cache keeps the other.
  [[nodiscard]] gpu_image_mac share() const noexcept;

  // Bytes the texture occupies, mips included (an estimate for cache budgets).
  [[nodiscard]] std::uint64_t approx_bytes() const noexcept {
    const std::uint64_t top = static_cast<std::uint64_t>(texture_width) * texture_height * 4;
    return mip_levels > 1 ? top + top / 3 : top;
  }
};

}  // namespace mv::image
