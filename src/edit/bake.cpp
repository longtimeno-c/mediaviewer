// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/bake.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

#include "gfx/adjust_kernel.h"
#include "image/half.h"

namespace mv::edit {
namespace {

namespace kernel = gfx::kernel;

float linear_to_srgb(float c) noexcept {
  c = std::clamp(c, 0.0f, 1.0f);
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Linear 0..1 in 1/65535 steps -> sRGB code, as the _SRGB render target and
// Metal's sRGB drawable encode (saturate, then the sRGB OETF, then round).
struct encode_table {
  std::array<std::uint8_t, 65536> to_srgb{};
  encode_table() noexcept {
    for (int i = 0; i < 65536; ++i) {
      to_srgb[static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>(std::lround(linear_to_srgb(i / 65535.0f) * 255.0f));
    }
  }
};

const encode_table& table() noexcept {
  static const encode_table t;
  return t;
}

std::uint8_t encode(float lin, const encode_table& t) noexcept {
  if (!(lin > 0.0f)) return 0;  // also NaN
  if (lin >= 1.0f) return 255;
  return t.to_srgb[static_cast<std::size_t>(std::lround(lin * 65535.0f))];
}

// Straight-alpha linear RGBA of one texel, premultiplied for filtering.
struct px4 {
  float r = 0, g = 0, b = 0, a = 0;
};

px4 fetch(const image::linear_image& src, int x, int y) noexcept {
  x = std::clamp(x, 0, static_cast<int>(src.width) - 1);
  y = std::clamp(y, 0, static_cast<int>(src.height) - 1);
  const std::uint16_t* p =
      src.rgba.data() + (static_cast<std::size_t>(y) * src.width + static_cast<std::size_t>(x)) * 4;
  const float a = std::clamp(image::half_to_float(p[3]), 0.0f, 1.0f);
  return px4{image::half_to_float(p[0]) * a, image::half_to_float(p[1]) * a,
             image::half_to_float(p[2]) * a, a};
}

px4 bilinear(const image::linear_image& src, double sx, double sy) noexcept {
  const double fx = std::floor(sx), fy = std::floor(sy);
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  const float ax = static_cast<float>(sx - fx), ay = static_cast<float>(sy - fy);
  const px4 a = fetch(src, x0, y0), b = fetch(src, x0 + 1, y0);
  const px4 c = fetch(src, x0, y0 + 1), d = fetch(src, x0 + 1, y0 + 1);
  auto lerp = [](float p, float q, float k) { return p + (q - p) * k; };
  return px4{lerp(lerp(a.r, b.r, ax), lerp(c.r, d.r, ax), ay),
             lerp(lerp(a.g, b.g, ax), lerp(c.g, d.g, ax), ay),
             lerp(lerp(a.b, b.b, ax), lerp(c.b, d.b, ax), ay),
             lerp(lerp(a.a, b.a, ax), lerp(c.a, d.a, ax), ay)};
}

void write_pixel(float r, float g, float b, float a, const adjust_uniforms& u,
                 const encode_table& t, std::uint8_t* o) noexcept {
  if (a <= 0.0f) {
    o[0] = o[1] = o[2] = o[3] = 0;
    return;
  }
  const kernel::float4 a0{u.a0[0], u.a0[1], u.a0[2], u.a0[3]};
  const kernel::float4 a1{u.a1[0], u.a1[1], u.a1[2], u.a1[3]};
  const kernel::float3 c = kernel::mv_adjust(kernel::float3{r, g, b}, a0, a1);
  o[0] = encode(c.x, t);
  o[1] = encode(c.y, t);
  o[2] = encode(c.z, t);
  o[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

}  // namespace

void bake_pixel(const float rgb_linear[3], const adjust_uniforms& u, std::uint8_t out[3]) noexcept {
  std::uint8_t o[4];
  write_pixel(rgb_linear[0], rgb_linear[1], rgb_linear[2], 1.0f, u, table(), o);
  out[0] = o[0];
  out[1] = o[1];
  out[2] = o[2];
}

result<codec::raster> bake(const image::linear_image& src, const placement& p,
                           const adjust_uniforms& u, const job_context* ctx) {
  if (!src.valid()) return err(status::invalid_arg);
  const size2 expect =
      p.total.transposes() ? size2{src.height, src.width} : size2{src.width, src.height};
  if (!(expect == p.oriented) || p.output.w == 0 || p.output.h == 0) return err(status::invalid_arg);
  constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
  if (static_cast<std::uint64_t>(p.output.w) * p.output.h > kMaxPixels) {
    return err(status::unsupported_format);
  }

  codec::raster out;
  out.format = src.format;
  out.intent = codec::transfer_intent::display_referred;
  out.tagged_srgb = true;
  out.width = p.output.w;
  out.height = p.output.h;
  try {
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  const encode_table& t = table();

  if (p.exact_copy) {
    // Rotate / flip / unscaled crop: each output pixel is one source texel,
    // exactly as geometry.cpp's copy_exact picks it.
    const bool tr = p.total.transposes();
    const bool fx = p.total.flip_x();
    const bool fy = p.total.flip_y();
    const std::uint32_t ow = p.oriented.w, oh = p.oriented.h;
    for (std::uint32_t y = 0; y < out.height; ++y) {
      if (ctx && (y & 63) == 0 && ctx->cancelled()) return err(status::cancelled);
      const std::uint32_t Y = y + p.crop_y;
      const std::uint32_t py = fy ? oh - 1 - Y : Y;
      std::uint8_t* row = out.rgba.data() + static_cast<std::size_t>(y) * out.width * 4;
      for (std::uint32_t x = 0; x < out.width; ++x) {
        const std::uint32_t X = x + p.crop_x;
        const std::uint32_t px = fx ? ow - 1 - X : X;
        const std::uint32_t sx = tr ? py : px;
        const std::uint32_t sy = tr ? px : py;
        const std::uint16_t* s = src.rgba.data() + (static_cast<std::size_t>(sy) * src.width + sx) * 4;
        write_pixel(image::half_to_float(s[0]), image::half_to_float(s[1]),
                    image::half_to_float(s[2]), image::half_to_float(s[3]), u, t,
                    row + static_cast<std::size_t>(x) * 4);
      }
    }
    return out;
  }

  // Straighten / resize: PR 10's resampler (supersampled bilinear in linear
  // light, premultiplied), then the kernel on the un-premultiplied average.
  const double W = src.width, H = src.height;
  const int nx = std::clamp(static_cast<int>(std::ceil(double(p.cropped.w) / out.width)), 1, 8);
  const int ny = std::clamp(static_cast<int>(std::ceil(double(p.cropped.h) / out.height)), 1, 8);
  const float inv_n = 1.0f / static_cast<float>(nx * ny);
  const float* m = p.map.m;
  for (std::uint32_t y = 0; y < out.height; ++y) {
    if (ctx && (y & 63) == 0 && ctx->cancelled()) return err(status::cancelled);
    std::uint8_t* row = out.rgba.data() + static_cast<std::size_t>(y) * out.width * 4;
    for (std::uint32_t x = 0; x < out.width; ++x) {
      px4 acc;
      for (int j = 0; j < ny; ++j) {
        const double v = (y + (j + 0.5) / ny) / out.height;
        for (int i = 0; i < nx; ++i) {
          const double uu = (x + (i + 0.5) / nx) / out.width;
          const double su = m[0] * uu + m[1] * v + m[2];
          const double sv = m[3] * uu + m[4] * v + m[5];
          const px4 s = bilinear(src, su * W - 0.5, sv * H - 0.5);
          acc.r += s.r;
          acc.g += s.g;
          acc.b += s.b;
          acc.a += s.a;
        }
      }
      const float a = acc.a * inv_n;
      const float k = a > 0.0f ? inv_n / a : 0.0f;
      write_pixel(acc.r * k, acc.g * k, acc.b * k, a, u, t, row + static_cast<std::size_t>(x) * 4);
    }
  }
  return out;
}

}  // namespace mv::edit
