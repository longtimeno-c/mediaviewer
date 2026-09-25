// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Immutable GPU texture produced on a decode worker
// (plan/02: CreateTexture2D + D3D11_SUBRESOURCE_DATA, never Map on the
// immediate context).
#pragma once

#include <cstdint>
#include <memory>

#include "codec/format.h"
#include "gfx/device.h"

namespace mv::image {

class tile_set;

// Ordered like canvas::image_quality (which the render thread converts to);
// image/ does not depend on canvas/.
enum class gpu_quality : std::uint8_t { preview = 0, full_top = 1, full = 2 };

struct gpu_image {
  gfx::com_ptr<ID3D11Texture2D> texture;
  gfx::com_ptr<ID3D11ShaderResourceView> srv;
  gfx::com_ptr<ID3D11Device> device;
  // The image this texture stands for, in full-resolution pixels. The camera
  // works in these. A preview, or a tiled image's overview, is smaller than
  // this; the blit stretches it over the same rect.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  // The texture's own top-level size.
  std::uint32_t texture_width = 0;
  std::uint32_t texture_height = 0;
  std::uint32_t mip_levels = 0;
  std::uint32_t generation = 0;
  codec::format_family format = codec::format_family::unknown;
  bool icc_tagged = false;

  // Identity for preview → full refinement (canvas/refinement.h). Stamped at
  // publish: `item_key` from the path the item was decoded for,
  // `view_generation` from the session's generation at the moment of publish.
  std::uint64_t item_key = 0;
  std::uint32_t view_generation = 0;
  gpu_quality quality = gpu_quality::full;
  // Mean sRGB luma (0..255) of a sparse sample, measured on the worker. A RAW's
  // embedded JPEG and LibRaw's render of the same frame differ by up to ~40
  // levels; the render thread lengthens the refinement fade when they do.
  std::uint8_t mean_luma = 0;

  // Images above ~64 MP or wider than the texture limit (plan/04 "Tiled
  // pyramid"): `texture` is the always-resident overview and the tiles are
  // created on demand. Held here only; the render thread never copies it.
  std::shared_ptr<tile_set> tiles;
};

}  // namespace mv::image
