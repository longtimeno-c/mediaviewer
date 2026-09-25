// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

namespace mv::edit {
namespace {

float srgb_to_linear(float c) noexcept {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c) noexcept {
  c = std::clamp(c, 0.0f, 1.0f);
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

struct transfer_tables {
  std::array<float, 256> to_linear{};
  std::array<std::uint8_t, 65536> to_srgb{};  // indexed by linear * 65535
  transfer_tables() noexcept {
    for (int i = 0; i < 256; ++i) to_linear[static_cast<std::size_t>(i)] = srgb_to_linear(static_cast<float>(i) / 255.0f);
    for (int i = 0; i < 65536; ++i) {
      to_srgb[static_cast<std::size_t>(i)] =
          static_cast<std::uint8_t>(std::lround(linear_to_srgb(static_cast<float>(i) / 65535.0f) * 255.0f));
    }
  }
};

const transfer_tables& tables() noexcept {
  static const transfer_tables t;
  return t;
}

// Premultiplied linear RGBA.
struct px4 {
  float r = 0, g = 0, b = 0, a = 0;
};

px4 fetch(const codec::raster& src, int x, int y, const transfer_tables& t) noexcept {
  x = std::clamp(x, 0, static_cast<int>(src.width) - 1);
  y = std::clamp(y, 0, static_cast<int>(src.height) - 1);
  const std::uint8_t* p = src.rgba.data() + (static_cast<std::size_t>(y) * src.width + static_cast<std::size_t>(x)) * 4;
  const float a = p[3] / 255.0f;
  return px4{t.to_linear[p[0]] * a, t.to_linear[p[1]] * a, t.to_linear[p[2]] * a, a};
}

px4 bilinear(const codec::raster& src, double sx, double sy, const transfer_tables& t) noexcept {
  const double fx = std::floor(sx), fy = std::floor(sy);
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  const float ax = static_cast<float>(sx - fx), ay = static_cast<float>(sy - fy);
  const px4 a = fetch(src, x0, y0, t), b = fetch(src, x0 + 1, y0, t);
  const px4 c = fetch(src, x0, y0 + 1, t), d = fetch(src, x0 + 1, y0 + 1, t);
  auto lerp = [](float p, float q, float k) { return p + (q - p) * k; };
  return px4{lerp(lerp(a.r, b.r, ax), lerp(c.r, d.r, ax), ay),
             lerp(lerp(a.g, b.g, ax), lerp(c.g, d.g, ax), ay),
             lerp(lerp(a.b, b.b, ax), lerp(c.b, d.b, ax), ay),
             lerp(lerp(a.a, b.a, ax), lerp(c.a, d.a, ax), ay)};
}

void copy_exact(const codec::raster& src, const placement& p, codec::raster& out) noexcept {
  const bool t = p.total.transposes();
  const bool fx = p.total.flip_x();
  const bool fy = p.total.flip_y();
  const std::uint32_t ow = p.oriented.w, oh = p.oriented.h;
  const auto* s = reinterpret_cast<const std::uint32_t*>(src.rgba.data());
  auto* d = reinterpret_cast<std::uint32_t*>(out.rgba.data());
  for (std::uint32_t y = 0; y < out.height; ++y) {
    const std::uint32_t Y = y + p.crop_y;
    const std::uint32_t py = fy ? oh - 1 - Y : Y;
    std::uint32_t* drow = d + static_cast<std::size_t>(y) * out.width;
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::uint32_t X = x + p.crop_x;
      const std::uint32_t px = fx ? ow - 1 - X : X;
      const std::uint32_t sx = t ? py : px;
      const std::uint32_t sy = t ? px : py;
      drow[x] = s[static_cast<std::size_t>(sy) * src.width + sx];
    }
  }
}

}  // namespace

result<codec::raster> render(const codec::raster& src, const placement& p, const job_context* ctx) {
  if (src.width == 0 || src.height == 0 ||
      src.rgba.size() < static_cast<std::size_t>(src.width) * src.height * 4) {
    return err(status::invalid_arg);
  }
  const size2 expect = p.total.transposes() ? size2{src.height, src.width} : size2{src.width, src.height};
  if (!(expect == p.oriented) || p.output.w == 0 || p.output.h == 0) return err(status::invalid_arg);
  constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
  if (static_cast<std::uint64_t>(p.output.w) * p.output.h > kMaxPixels) {
    return err(status::unsupported_format);
  }

  codec::raster out;
  out.format = src.format;
  out.intent = src.intent;
  out.tagged_srgb = src.tagged_srgb;
  out.width = p.output.w;
  out.height = p.output.h;
  try {
    out.icc = src.icc;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }

  if (p.exact_copy) {
    copy_exact(src, p, out);
    return out;
  }

  const transfer_tables& t = tables();
  const double W = src.width, H = src.height;
  // Supersample when the output is smaller than the region it covers, so a
  // downscale averages instead of aliasing. Capped: an 8x8 box is plenty
  // ahead of an encoder.
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
          const double u = (x + (i + 0.5) / nx) / out.width;
          const double su = m[0] * u + m[1] * v + m[2];
          const double sv = m[3] * u + m[4] * v + m[5];
          const px4 s = bilinear(src, su * W - 0.5, sv * H - 0.5, t);
          acc.r += s.r;
          acc.g += s.g;
          acc.b += s.b;
          acc.a += s.a;
        }
      }
      const float a = acc.a * inv_n;
      std::uint8_t* o = row + static_cast<std::size_t>(x) * 4;
      if (a <= 0.0f) {
        o[0] = o[1] = o[2] = o[3] = 0;
        continue;
      }
      const float k = inv_n / a;  // average, then un-premultiply
      auto enc = [&](float lin) {
        const int idx = static_cast<int>(std::lround(std::clamp(lin, 0.0f, 1.0f) * 65535.0f));
        return t.to_srgb[static_cast<std::size_t>(idx)];
      };
      o[0] = enc(acc.r * k);
      o[1] = enc(acc.g * k);
      o[2] = enc(acc.b * k);
      o[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
    }
  }
  return out;
}

}  // namespace mv::edit
