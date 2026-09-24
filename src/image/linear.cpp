// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/linear.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>

#include "codec/decode.h"
#include "image/colour.h"
#include "image/half.h"

namespace mv::image {
namespace {

constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;

// sRGB 8-bit code -> FP16 linear, and 16-bit linear code -> FP16. The same
// table is what the D3D11 _SRGB view and Metal's sRGB format compute for the
// 8-bit viewer texture, so a working image made from it matches the viewer.
struct tables {
  std::array<std::uint16_t, 256> srgb8{};
  std::array<std::uint16_t, 256> alpha8{};
  tables() noexcept {
    for (int i = 0; i < 256; ++i) {
      const float c = static_cast<float>(i) / 255.0f;
      const float lin = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
      srgb8[static_cast<std::size_t>(i)] = float_to_half(lin);
      alpha8[static_cast<std::size_t>(i)] = float_to_half(c);
    }
  }
};

const tables& lut() noexcept {
  static const tables t;
  return t;
}

bool allocate(linear_image& out, std::uint32_t w, std::uint32_t h) noexcept {
  if (w == 0 || h == 0 || static_cast<std::uint64_t>(w) * h > kMaxPixels) return false;
  try {
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  } catch (const std::bad_alloc&) {
    return false;
  }
  out.width = w;
  out.height = h;
  return true;
}

// Source span [a, b) of output cell `i` when `n_in` samples cover `n_out`.
struct span1 {
  std::uint32_t first = 0;
  std::uint32_t last = 0;  // inclusive
  float w_first = 1.0f;    // coverage of the first and last source samples
  float w_last = 1.0f;
};

span1 footprint(std::uint32_t i, std::uint32_t n_in, std::uint32_t n_out) noexcept {
  const double scale = static_cast<double>(n_in) / n_out;
  const double a = i * scale;
  const double b = std::min(static_cast<double>(n_in), (i + 1) * scale);
  span1 s;
  s.first = static_cast<std::uint32_t>(std::floor(a));
  s.last = std::min(n_in - 1, static_cast<std::uint32_t>(std::ceil(b)) - 1);
  if (s.first == s.last) {
    s.w_first = s.w_last = static_cast<float>(b - a);
  } else {
    s.w_first = static_cast<float>((s.first + 1) - a);
    s.w_last = static_cast<float>(b - s.last);
  }
  return s;
}

}  // namespace

result<linear_image> linear_from_srgb8(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                       std::uint32_t height, codec::format_family format,
                                       const job_context* ctx) {
  if (rgba.size() != static_cast<std::size_t>(width) * height * 4) return err(status::invalid_arg);
  linear_image out;
  if (!allocate(out, width, height)) return err(status::out_of_memory);
  out.format = format;
  const tables& t = lut();
  const std::size_t pixels = static_cast<std::size_t>(width) * height;
  for (std::size_t i = 0; i < pixels; ++i) {
    if ((i & 0xfffff) == 0 && ctx && ctx->cancelled()) return err(status::cancelled);
    const std::uint8_t* s = rgba.data() + i * 4;
    std::uint16_t* d = out.rgba.data() + i * 4;
    d[0] = t.srgb8[s[0]];
    d[1] = t.srgb8[s[1]];
    d[2] = t.srgb8[s[2]];
    d[3] = t.alpha8[s[3]];
  }
  return out;
}

result<linear_image> linear_from_raster16(codec::raster16&& src, const job_context* ctx) {
  if (src.width == 0 || src.height == 0 ||
      static_cast<std::uint64_t>(src.width) * src.height > kMaxPixels ||
      src.rgba.size() != static_cast<std::size_t>(src.width) * src.height * 4) {
    return err(status::invalid_arg);
  }
  linear_image out;
  out.width = src.width;
  out.height = src.height;
  out.format = src.format;
  out.from_raw = true;
  out.rgba = std::move(src.rgba);
  const std::size_t n = out.rgba.size();
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 0x3fffff) == 0 && ctx && ctx->cancelled()) return err(status::cancelled);
    out.rgba[i] = float_to_half(static_cast<float>(out.rgba[i]) / 65535.0f);
  }
  return out;
}

result<linear_image> decode_linear(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (codec::looks_like_raw(bytes)) {
    MV_TRY(codec::raster16 developed, codec::decode_raw_linear(bytes, ctx));
    return linear_from_raster16(std::move(developed), ctx);
  }
  MV_TRY(codec::raster decoded, codec::decode(bytes, ctx));
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  const codec::format_family format = decoded.format;
  MV_TRY(display_image shown, to_display(std::move(decoded), ctx));
  return linear_from_srgb8(shown.rgba, shown.width, shown.height, format, ctx);
}

result<linear_image> downsample(const linear_image& src, std::uint32_t max_edge,
                                const job_context* ctx) {
  if (!src.valid() || max_edge == 0) return err(status::invalid_arg);
  const std::uint32_t long_edge = std::max(src.width, src.height);
  if (long_edge <= max_edge) {
    try {
      return linear_image(src);
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
  }
  const double k = static_cast<double>(max_edge) / long_edge;
  const auto ow = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(src.width * k)));
  const auto oh = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(src.height * k)));

  linear_image out;
  if (!allocate(out, ow, oh)) return err(status::out_of_memory);
  out.format = src.format;
  out.from_raw = src.from_raw;

  std::vector<span1> xs;
  try {
    xs.resize(ow);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  for (std::uint32_t x = 0; x < ow; ++x) xs[x] = footprint(x, src.width, ow);

  for (std::uint32_t y = 0; y < oh; ++y) {
    if ((y & 31) == 0 && ctx && ctx->cancelled()) return err(status::cancelled);
    const span1 sy = footprint(y, src.height, oh);
    for (std::uint32_t x = 0; x < ow; ++x) {
      const span1& sx = xs[x];
      // Premultiplied accumulation, so a transparent pixel adds no colour.
      double r = 0, g = 0, b = 0, a = 0, area = 0;
      for (std::uint32_t j = sy.first; j <= sy.last; ++j) {
        const float wy = j == sy.first ? sy.w_first : (j == sy.last ? sy.w_last : 1.0f);
        const std::uint16_t* row = src.rgba.data() + static_cast<std::size_t>(j) * src.width * 4;
        for (std::uint32_t i = sx.first; i <= sx.last; ++i) {
          const float wx = i == sx.first ? sx.w_first : (i == sx.last ? sx.w_last : 1.0f);
          const double w = static_cast<double>(wx) * wy;
          const std::uint16_t* p = row + static_cast<std::size_t>(i) * 4;
          const double pa = half_to_float(p[3]);
          r += w * pa * half_to_float(p[0]);
          g += w * pa * half_to_float(p[1]);
          b += w * pa * half_to_float(p[2]);
          a += w * pa;
          area += w;
        }
      }
      std::uint16_t* d = out.rgba.data() + (static_cast<std::size_t>(y) * ow + x) * 4;
      if (a <= 0.0 || area <= 0.0) {
        d[0] = d[1] = d[2] = d[3] = 0;
        continue;
      }
      d[0] = float_to_half(static_cast<float>(r / a));
      d[1] = float_to_half(static_cast<float>(g / a));
      d[2] = float_to_half(static_cast<float>(b / a));
      d[3] = float_to_half(static_cast<float>(a / area));
    }
  }
  return out;
}

}  // namespace mv::image
