// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/texture.h"

#include <array>

namespace mv::gfx {

status create_srgb_texture(ID3D11Device* device, std::span<const rgba8_level> levels,
                           com_ptr<ID3D11Texture2D>& texture,
                           com_ptr<ID3D11ShaderResourceView>& srv) noexcept {
  if (!device || levels.empty() || levels.size() > 16) return status::invalid_arg;
  const rgba8_level& top = levels[0];
  if (!top.rgba || top.width == 0 || top.height == 0 || top.width > k_max_texture_dimension ||
      top.height > k_max_texture_dimension) {
    return status::invalid_arg;
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = top.width;
  desc.Height = top.height;
  desc.MipLevels = static_cast<UINT>(levels.size());
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  desc.SampleDesc = {1, 0};
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  std::array<D3D11_SUBRESOURCE_DATA, 16> subs{};
  for (std::size_t i = 0; i < levels.size(); ++i) {
    if (!levels[i].rgba) return status::invalid_arg;
    subs[i].pSysMem = levels[i].rgba;
    subs[i].SysMemPitch = levels[i].width * 4;
    subs[i].SysMemSlicePitch = 0;
  }

  texture.Reset();
  srv.Reset();
  HRESULT hr = device->CreateTexture2D(&desc, subs.data(), texture.GetAddressOf());
  if (FAILED(hr)) return hr == E_OUTOFMEMORY ? status::out_of_memory : status::internal;
  hr = device->CreateShaderResourceView(texture.Get(), nullptr, srv.GetAddressOf());
  if (FAILED(hr)) {
    texture.Reset();
    return status::internal;
  }
  return status::ok;
}

}  // namespace mv::gfx
