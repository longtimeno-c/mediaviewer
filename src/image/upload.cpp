// SPDX-License-Identifier: GPL-2.0-or-later
#include "image/upload.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "gfx/texture.h"

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

std::uint8_t srgb_encode_exact(float linear) noexcept {
  linear = std::clamp(linear, 0.0f, 1.0f);
  const float s = (linear <= 0.0031308f) ? 12.92f * linear
                                         : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
  return static_cast<std::uint8_t>(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
}

// std::pow per tap was most of the cost of a large still's pyramid. Decode is
// an exact 256-entry table; encode is a 16-bit table, which is within 0.05 of
// an sRGB code value even on the steep toe of the curve.
const std::array<float, 256>& decode_lut() noexcept {
  static const std::array<float, 256> table = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i) {
      const float x = static_cast<float>(i) / 255.0f;
      t[static_cast<std::size_t>(i)] =
          x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    }
    return t;
  }();
  return table;
}

constexpr std::size_t kEncodeLutSize = 65536;

const std::vector<std::uint8_t>& encode_lut() {
  static const std::vector<std::uint8_t> table = [] {
    std::vector<std::uint8_t> t(kEncodeLutSize);
    for (std::size_t i = 0; i < kEncodeLutSize; ++i) {
      t[i] = srgb_encode_exact(static_cast<float>(i) / static_cast<float>(kEncodeLutSize - 1));
    }
    return t;
  }();
  return table;
}

inline std::uint8_t encode(const std::uint8_t* lut, float linear) noexcept {
  if (!(linear > 0.0f)) return lut[0];
  if (linear >= 1.0f) return lut[kEncodeLutSize - 1];
  return lut[static_cast<std::size_t>(linear * static_cast<float>(kEncodeLutSize - 1) + 0.5f)];
}

// Four-tap Mitchell kernel for every destination column (or row): clamped
// source indices and normalised weights. Same sample positions as before:
// centre-aligned, `(d + 0.5) * s / d_count - 0.5`.
struct taps {
  std::vector<std::uint32_t> index;  // count * 4
  std::vector<float> weight;         // count * 4
};

taps make_taps(std::uint32_t src_count, std::uint32_t dst_count) {
  taps t;
  t.index.resize(static_cast<std::size_t>(dst_count) * 4);
  t.weight.resize(static_cast<std::size_t>(dst_count) * 4);
  for (std::uint32_t d = 0; d < dst_count; ++d) {
    const float pos = (static_cast<float>(d) + 0.5f) * static_cast<float>(src_count) /
                          static_cast<float>(dst_count) -
                      0.5f;
    const int base = static_cast<int>(std::floor(pos));
    float sum = 0.0f;
    for (int k = 0; k < 4; ++k) {
      const int tap = base - 1 + k;
      const float w = mitchell(pos - static_cast<float>(tap));
      t.index[static_cast<std::size_t>(d) * 4 + static_cast<std::size_t>(k)] =
          static_cast<std::uint32_t>(std::clamp(tap, 0, static_cast<int>(src_count) - 1));
      t.weight[static_cast<std::size_t>(d) * 4 + static_cast<std::size_t>(k)] = w;
      sum += w;
    }
    const float inv = sum != 0.0f ? 1.0f / sum : 0.0f;
    for (int k = 0; k < 4; ++k) {
      t.weight[static_cast<std::size_t>(d) * 4 + static_cast<std::size_t>(k)] *= inv;
    }
  }
  return t;
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

bool downsample_half(const std::uint8_t* src, std::uint32_t sw, std::uint32_t sh,
                     std::vector<std::uint8_t>& dst, std::uint32_t& dw, std::uint32_t& dh,
                     const job_context* ctx) {
  // D3D11 mip extents are floor-half, not ceil. (w+1)/2 is the wrong pitch on
  // odd camera JPEGs (plan/04).
  dw = std::max(1u, sw / 2u);
  dh = std::max(1u, sh / 2u);
  dst.assign(static_cast<std::size_t>(dw) * dh * 4, 0);

  const taps tx = make_taps(sw, dw);
  const taps ty = make_taps(sh, dh);
  const auto& dec = decode_lut();
  const std::uint8_t* enc = encode_lut().data();

  // Horizontally filtered source rows, linear RGBA. The vertical taps of
  // consecutive destination rows overlap and only move forward, so four slots
  // hold every row the next destination row can ask for. The old code kept the
  // whole horizontally filtered level as floats: 800 MB for a 100 MP still.
  struct row_slot {
    std::int64_t source_row = -1;
    std::vector<float> rgba;
  };
  std::array<row_slot, 4> rows;
  for (auto& r : rows) r.rgba.resize(static_cast<std::size_t>(dw) * 4);

  const auto filtered_row = [&](std::uint32_t y) -> const float* {
    std::size_t victim = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].source_row == static_cast<std::int64_t>(y)) return rows[i].rgba.data();
      if (rows[i].source_row < rows[victim].source_row) victim = i;
    }
    row_slot& slot = rows[victim];
    slot.source_row = y;
    const std::uint8_t* line = src + static_cast<std::size_t>(y) * sw * 4;
    float* out = slot.rgba.data();
    for (std::uint32_t x = 0; x < dw; ++x) {
      float acc[4] = {0, 0, 0, 0};
      for (std::size_t k = 0; k < 4; ++k) {
        const std::size_t at = static_cast<std::size_t>(x) * 4 + k;
        const float w = tx.weight[at];
        const std::uint8_t* p = line + static_cast<std::size_t>(tx.index[at]) * 4;
        acc[0] += dec[p[0]] * w;
        acc[1] += dec[p[1]] * w;
        acc[2] += dec[p[2]] * w;
        acc[3] += static_cast<float>(p[3]) * (1.0f / 255.0f) * w;
      }
      std::copy(acc, acc + 4, out + static_cast<std::size_t>(x) * 4);
    }
    return out;
  };

  for (std::uint32_t y = 0; y < dh; ++y) {
    if (ctx && (y & 31u) == 0 && ctx->cancelled()) return false;
    const float* r[4];
    const float* w = ty.weight.data() + static_cast<std::size_t>(y) * 4;
    for (std::size_t k = 0; k < 4; ++k) {
      r[k] = filtered_row(ty.index[static_cast<std::size_t>(y) * 4 + k]);
    }
    std::uint8_t* o = dst.data() + static_cast<std::size_t>(y) * dw * 4;
    for (std::uint32_t x = 0; x < dw; ++x) {
      const std::size_t at = static_cast<std::size_t>(x) * 4;
      float acc[4];
      for (std::size_t c = 0; c < 4; ++c) {
        acc[c] = r[0][at + c] * w[0] + r[1][at + c] * w[1] + r[2][at + c] * w[2] +
                 r[3][at + c] * w[3];
      }
      o[at + 0] = encode(enc, acc[0]);
      o[at + 1] = encode(enc, acc[1]);
      o[at + 2] = encode(enc, acc[2]);
      o[at + 3] = static_cast<std::uint8_t>(std::clamp(acc[3] * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  }
  return true;
}

std::uint8_t mean_luma(const display_image& src) noexcept {
  if (src.width == 0 || src.height == 0 ||
      src.rgba.size() < static_cast<std::size_t>(src.width) * src.height * 4) {
    return 0;
  }
  const std::uint32_t step_x = std::max(1u, src.width / 64u);
  const std::uint32_t step_y = std::max(1u, src.height / 64u);
  double sum = 0.0;
  std::uint64_t n = 0;
  for (std::uint32_t y = step_y / 2; y < src.height; y += step_y) {
    const std::uint8_t* row = src.rgba.data() + static_cast<std::size_t>(y) * src.width * 4;
    for (std::uint32_t x = step_x / 2; x < src.width; x += step_x) {
      const std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
      sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
      ++n;
    }
  }
  if (n == 0) return 0;
  return static_cast<std::uint8_t>(std::clamp(sum / static_cast<double>(n) + 0.5, 0.0, 255.0));
}

result<gpu_image> upload(ID3D11Device* device, const display_image& src, std::uint32_t generation,
                         const job_context* ctx, std::uint32_t mip_limit) {
  if (!device) return err(status::invalid_arg);
  if (src.width == 0 || src.height == 0 ||
      src.rgba.size() != static_cast<std::size_t>(src.width) * src.height * 4) {
    return err(status::corrupt);
  }
  // Above this the single texture cannot exist; image/tiles.h is the path.
  if (src.width > gfx::k_max_texture_dimension || src.height > gfx::k_max_texture_dimension) {
    return err(status::unsupported_format);
  }

  // Level 0 is the decoded image itself; only the smaller levels are new
  // memory (a copy of a 60 MP top level was 240 MB of nothing).
  struct level {
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    std::vector<std::uint8_t> rgba;
  };
  const std::uint32_t levels = mip_limit == 1 ? 1u : mip_count_for(src.width, src.height);
  std::vector<level> mips(levels);
  std::vector<gfx::rgba8_level> views(levels);
  views[0] = {src.rgba.data(), src.width, src.height};
  mips[0].w = src.width;
  mips[0].h = src.height;

  for (std::size_t i = 1; i < mips.size(); ++i) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    const std::uint8_t* from = i == 1 ? src.rgba.data() : mips[i - 1].rgba.data();
    if (!downsample_half(from, mips[i - 1].w, mips[i - 1].h, mips[i].rgba, mips[i].w, mips[i].h,
                         ctx)) {
      return err(status::cancelled);
    }
    views[i] = {mips[i].rgba.data(), mips[i].w, mips[i].h};
  }

  gpu_image out;
  const status made = gfx::create_srgb_texture(device, views, out.texture, out.srv);
  if (made != status::ok) return err(made);

  out.device = device;
  out.width = src.width;
  out.height = src.height;
  out.texture_width = src.width;
  out.texture_height = src.height;
  out.mip_levels = levels;
  out.generation = generation;
  out.format = src.format;
  out.icc_tagged = src.icc_tagged;
  out.mean_luma = mean_luma(src);
  return out;
}

}  // namespace mv::image
