// SPDX-License-Identifier: GPL-2.0-or-later
// Immutable 8-bit sRGB textures created on a worker (plan/02: CreateTexture2D
// with D3D11_SUBRESOURCE_DATA on the free-threaded device, never Map on the
// immediate context). One place for the desc, so the single-texture still and
// the tiled pyramid cannot drift apart.
#pragma once

#include <cstdint>
#include <span>

#include "core/status.h"
#include "gfx/device.h"

namespace mv::gfx {

// D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION at feature level 11_0.
inline constexpr std::uint32_t k_max_texture_dimension = 16384;

struct rgba8_level {
  const std::uint8_t* rgba = nullptr;  // width * height * 4, sRGB-encoded
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Levels must be a floor-half chain starting at the top level. Any thread.
// out_of_memory for E_OUTOFMEMORY, internal for any other failure.
[[nodiscard]] status create_srgb_texture(ID3D11Device* device,
                                         std::span<const rgba8_level> levels,
                                         com_ptr<ID3D11Texture2D>& texture,
                                         com_ptr<ID3D11ShaderResourceView>& srv) noexcept;

}  // namespace mv::gfx
