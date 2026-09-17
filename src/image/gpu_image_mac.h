// SPDX-License-Identifier: GPL-2.0-or-later
// Immutable Metal texture produced on a decode worker (plan/15 PR 17 — the
// Metal twin of image/gpu_image.h, which is D3D11-only and stays that way).
#pragma once

#include <cstdint>

#include "codec/format.h"

namespace mv::image {

// Same field set and semantics as image::gpu_image (image/gpu_image.h);
// no tile_set/quality/item_key/generation fields yet — PR 17 has no filmstrip
// or preview→full refinement to track, only a single opened still.
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

  [[nodiscard]] bool valid() const noexcept { return texture != nullptr; }
};

}  // namespace mv::image
