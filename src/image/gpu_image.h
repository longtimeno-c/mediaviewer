// SPDX-License-Identifier: GPL-2.0-or-later
// Immutable GPU texture produced on a decode worker
// (plan/02: CreateTexture2D + D3D11_SUBRESOURCE_DATA, never Map on the
// immediate context).
#pragma once

#include <cstdint>

#include "codec/format.h"
#include "gfx/device.h"

namespace mv::image {

struct gpu_image {
  gfx::com_ptr<ID3D11Texture2D> texture;
  gfx::com_ptr<ID3D11ShaderResourceView> srv;
  gfx::com_ptr<ID3D11Device> device;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t mip_levels = 0;
  std::uint32_t generation = 0;
  codec::format_family format = codec::format_family::unknown;
  bool icc_tagged = false;
};

}  // namespace mv::image
