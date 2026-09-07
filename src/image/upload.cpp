// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/upload.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace mv::image {
namespace {

float mitchell(float x) noexcept {
  x = std::fabs(x);
  constexpr float B = 1.0f / 3.0f;
  constexpr float C = 1.0f / 3.0f;
  if (x < 1.0f) {
    return ((12.0f - 9.0f * B - 6.0f * C) * x * x * x +
            (-18.0f + 12.0f * B + 6.0f * C) * x * x + (6.0f - 2.0f * B)) /
           6.0f;
  }
  if (x < 2.0f) {
    return ((-B - 6.0f * C) * x * x * x + (6.0f * B + 30.0f * C) * x * x +
            (-12.0f * B - 48.0f * C) * x + (8.0f * B + 24.0f * C)) /
           6.0f;
  }
  return 0.0f;
}

float srgb_decode_u8(std::uint8_t c) noexcept {
  const float x = static_cast<float>(c) * (1.0f / 255.0f);
  return x <= 0.04045f ? x * (1.0f / 12.92f) : std::pow((x + 0.055f) * (1.0f / 1.055f), 2.4f);
}

std::uint8_t srgb_encode_u8(float linear) noexcept {
  linear = std::clamp(linear, 0.0f, 1.0f);
  const float s = (linear <= 0.0031308f) ? 12.92f * linear
                                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
  return static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

void downsample_2x(const std::uint8_t* src, std::uint32_t sw, std::uint32_t sh,
                   std::vector<std::uint8_t>& dst, std::uint32_t& dw, std::uint32_t& dh) {
  // D3D11 mip extents are floor-half, not ceil. (w+1)/2 is the wrong pitch on
  // odd camera JPEGs (plan/04).
  dw = std::max(1u, sw / 2u);
  dh = std::max(1u, sh / 2u);
  dst.assign(static_cast<std::size_t>(dw) * dh * 4, 0);

  // Separable Mitchell in linear light, then sRGB-encode the level.
  std::vector<float> tmp(static_cast<std::size_t>(dw) * sh * 4);

  for (std::uint32_t y = 0; y < sh; ++y) {
    for (std::uint32_t x = 0; x < dw; ++x) {
      const float src_x = (x + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f;
      float acc[4] = {0, 0, 0, 0};
      float wsum = 0;
      const int ix = static_cast<int>(std::floor(src_x));
      for (int t = ix - 1; t <= ix + 2; ++t) {
        const int s = std::clamp(t, 0, static_cast<int>(sw) - 1);
        const float w = mitchell(src_x - static_cast<float>(t));
        const std::uint8_t* p = src + (static_cast<std::size_t>(y) * sw + static_cast<std::size_t>(s)) * 4;
        acc[0] += srgb_decode_u8(p[0]) * w;
        acc[1] += srgb_decode_u8(p[1]) * w;
        acc[2] += srgb_decode_u8(p[2]) * w;
        acc[3] += static_cast<float>(p[3]) * (1.0f / 255.0f) * w;
        wsum += w;
      }
      float* o = tmp.data() + (static_cast<std::size_t>(y) * dw + x) * 4;
      const float inv = (wsum > 0.0f) ? 1.0f / wsum : 0.0f;
      for (int c = 0; c < 4; ++c) o[c] = acc[c] * inv;
    }
  }

  for (std::uint32_t y = 0; y < dh; ++y) {
    for (std::uint32_t x = 0; x < dw; ++x) {
      const float src_y = (y + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f;
      float acc[4] = {0, 0, 0, 0};
      float wsum = 0;
      const int iy = static_cast<int>(std::floor(src_y));
      for (int t = iy - 1; t <= iy + 2; ++t) {
        const int s = std::clamp(t, 0, static_cast<int>(sh) - 1);
        const float w = mitchell(src_y - static_cast<float>(t));
        const float* p = tmp.data() + (static_cast<std::size_t>(s) * dw + x) * 4;
        for (int c = 0; c < 4; ++c) acc[c] += p[c] * w;
        wsum += w;
      }
      std::uint8_t* o = dst.data() + (static_cast<std::size_t>(y) * dw + x) * 4;
      const float inv = (wsum > 0.0f) ? 1.0f / wsum : 0.0f;
      o[0] = srgb_encode_u8(acc[0] * inv);
      o[1] = srgb_encode_u8(acc[1] * inv);
      o[2] = srgb_encode_u8(acc[2] * inv);
      o[3] = static_cast<std::uint8_t>(std::clamp(acc[3] * inv * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  }
}

std::uint32_t mip_count_for(std::uint32_t w, std::uint32_t h) noexcept {
  std::uint32_t n = 1;
  while (w > 1 || h > 1) {
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
    ++n;
  }
  return n;
}

}  // namespace

result<gpu_image> upload(ID3D11Device* device, const display_image& src, std::uint32_t generation,
                         const job_context* ctx, std::uint32_t mip_limit) {
  if (!device) return err(status::invalid_arg);
  if (src.width == 0 || src.height == 0 ||
      src.rgba.size() != static_cast<std::size_t>(src.width) * src.height * 4) {
    return err(status::corrupt);
  }

  struct level {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<std::uint8_t> rgba;
  };
  std::vector<level> mips;
  const std::uint32_t levels = mip_count_for(src.width, src.height);
  mips.resize(mip_limit == 1 ? 1u : levels);
  mips[0].w = src.width;
  mips[0].h = src.height;
  mips[0].rgba = src.rgba;

  for (std::size_t i = 1; i < mips.size(); ++i) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    downsample_2x(mips[i - 1].rgba.data(), mips[i - 1].w, mips[i - 1].h, mips[i].rgba, mips[i].w,
                  mips[i].h);
  }

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = src.width;
  desc.Height = src.height;
  desc.MipLevels = static_cast<UINT>(mips.size());
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  desc.SampleDesc = {1, 0};
  desc.Usage = D3D11_USAGE_IMMUTABLE;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  std::vector<D3D11_SUBRESOURCE_DATA> subs(mips.size());
  for (std::size_t i = 0; i < mips.size(); ++i) {
    subs[i].pSysMem = mips[i].rgba.data();
    subs[i].SysMemPitch = mips[i].w * 4;
    subs[i].SysMemSlicePitch = 0;
  }

  gpu_image out;
  HRESULT hr = device->CreateTexture2D(&desc, subs.data(), out.texture.GetAddressOf());
  if (FAILED(hr)) {
    return err(hr == E_OUTOFMEMORY ? status::out_of_memory : status::internal);
  }
  hr = device->CreateShaderResourceView(out.texture.Get(), nullptr, out.srv.GetAddressOf());
  if (FAILED(hr)) return err(status::internal);

  out.device = device;
  out.width = src.width;
  out.height = src.height;
  out.mip_levels = static_cast<std::uint32_t>(mips.size());
  out.generation = generation;
  out.format = src.format;
  out.icc_tagged = src.icc_tagged;
  return out;
}

}  // namespace mv::image
