// SPDX-License-Identifier: GPL-2.0-or-later
#include "edit/histogram.h"

#include <algorithm>
#include <cmath>

#include "gfx/adjust_kernel.h"
#include "image/half.h"

namespace mv::edit {
namespace {

namespace kernel = gfx::kernel;

float linear_to_srgb(float c) noexcept {
  c = std::clamp(c, 0.0f, 1.0f);
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

std::size_t code(float lin) noexcept {
  if (!(lin > 0.0f)) return 0;
  return static_cast<std::size_t>(std::lround(linear_to_srgb(lin) * 255.0f));
}

}  // namespace

result<histogram> compute_histogram(const image::linear_image& working, const adjust_uniforms& u,
                                    const job_context* ctx) {
  if (!working.valid()) return err(status::invalid_arg);
  const std::uint64_t pixels = static_cast<std::uint64_t>(working.width) * working.height;
  std::uint32_t step = 1;
  while (pixels / (static_cast<std::uint64_t>(step) * step) > kHistogramSampleBudget) ++step;

  const kernel::float4 a0{u.a0[0], u.a0[1], u.a0[2], u.a0[3]};
  const kernel::float4 a1{u.a1[0], u.a1[1], u.a1[2], u.a1[3]};
  histogram h;
  for (std::uint32_t y = step / 2; y < working.height; y += step) {
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    const std::uint16_t* row = working.rgba.data() + static_cast<std::size_t>(y) * working.width * 4;
    for (std::uint32_t x = step / 2; x < working.width; x += step) {
      const std::uint16_t* p = row + static_cast<std::size_t>(x) * 4;
      if (!(image::half_to_float(p[3]) > 0.0f)) continue;  // transparent: nothing is shown
      const kernel::float3 c = kernel::mv_adjust(
          kernel::float3{image::half_to_float(p[0]), image::half_to_float(p[1]),
                         image::half_to_float(p[2])},
          a0, a1);
      ++h.r[code(c.x)];
      ++h.g[code(c.y)];
      ++h.b[code(c.z)];
      ++h.luma[code(kernel::dot(c, kernel::float3{0.2126f, 0.7152f, 0.0722f}))];
      const float hi = std::max(c.x, std::max(c.y, c.z));
      if (hi >= kClipHighLinear) ++h.clipped_high;
      else if (hi <= kClipLowLinear) ++h.clipped_low;
      ++h.samples;
    }
  }
  return h;
}

std::array<std::uint16_t, 4 * kHistogramBins> pack_histogram(const histogram& h) noexcept {
  std::array<std::uint16_t, 4 * kHistogramBins> out{};
  const std::array<std::uint32_t, 256>* channels[4] = {&h.r, &h.g, &h.b, &h.luma};
  constexpr int kPer = 256 / kHistogramBins;
  std::uint64_t binned[4][kHistogramBins] = {};
  std::uint64_t peak = 0;
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i < 256; ++i) binned[c][i / kPer] += (*channels[c])[static_cast<std::size_t>(i)];
    for (int i = 1; i + 1 < kHistogramBins; ++i) peak = std::max(peak, binned[c][i]);
  }
  if (peak == 0) {
    for (int c = 0; c < 4; ++c) {
      peak = std::max({peak, binned[c][0], binned[c][kHistogramBins - 1]});
    }
  }
  if (peak == 0) return out;
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i < kHistogramBins; ++i) {
      const std::uint64_t v = std::min<std::uint64_t>(binned[c][i] * 1000 / peak, 1000);
      out[static_cast<std::size_t>(c * kHistogramBins + i)] = static_cast<std::uint16_t>(v);
    }
  }
  return out;
}

}  // namespace mv::edit
