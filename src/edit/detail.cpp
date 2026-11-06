// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/detail.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

#include "core/parallel.h"
#include "image/half.h"

namespace mv::edit {
namespace {

using plane = std::vector<float>;

// One box blur of radius r along rows, edges clamped, a running sum per row.
void box_rows(const float* in, float* out, int w, int h, int r) noexcept {
  core::parallel_bands(static_cast<std::size_t>(h), 32, [&](std::size_t y0, std::size_t y1) {
    const float k = 1.0f / static_cast<float>(2 * r + 1);
    for (std::size_t y = y0; y < y1; ++y) {
      const float* row = in + y * static_cast<std::size_t>(w);
      float* dst = out + y * static_cast<std::size_t>(w);
      auto at = [&](int x) { return row[std::clamp(x, 0, w - 1)]; };
      float sum = 0.0f;
      for (int x = -r; x <= r; ++x) sum += at(x);
      for (int x = 0; x < w; ++x) {
        dst[x] = sum * k;
        sum += at(x + r + 1) - at(x - r);
      }
    }
  });
}

// The same down the columns: each band keeps one running sum per column, so
// memory is read row after row.
void box_cols(const float* in, float* out, int w, int h, int r) noexcept {
  core::parallel_bands(static_cast<std::size_t>(w), 64, [&](std::size_t x0, std::size_t x1) {
    const float k = 1.0f / static_cast<float>(2 * r + 1);
    const std::size_t n = x1 - x0;
    std::vector<float> sum(n, 0.0f);
    auto row = [&](int y) { return in + static_cast<std::size_t>(std::clamp(y, 0, h - 1)) * static_cast<std::size_t>(w); };
    for (int y = -r; y <= r; ++y) {
      const float* s = row(y);
      for (std::size_t i = 0; i < n; ++i) sum[i] += s[x0 + i];
    }
    for (int y = 0; y < h; ++y) {
      float* dst = out + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
      const float* add = row(y + r + 1);
      const float* sub = row(y - r);
      for (std::size_t i = 0; i < n; ++i) {
        dst[x0 + i] = sum[i] * k;
        sum[i] += add[x0 + i] - sub[x0 + i];
      }
    }
  });
}

// `passes` box blurs of radius r (three is close to a Gaussian), in place.
void blur(plane& p, plane& tmp, int w, int h, int r, int passes) noexcept {
  if (r <= 0) return;
  for (int i = 0; i < passes; ++i) {
    box_rows(p.data(), tmp.data(), w, h, r);
    box_cols(tmp.data(), p.data(), w, h, r);
  }
}

}  // namespace

detail_params detail_of(const colour& c) noexcept {
  detail_params d;
  d.definition = c.get(adjust_param::definition) / 100.0f;
  d.brilliance = c.get(adjust_param::brilliance) / 100.0f;
  d.sharpen = c.get(adjust_param::sharpen) / 100.0f;
  d.noise = c.get(adjust_param::noise) / 100.0f;
  return d;
}

result<image::linear_image> apply_detail(const image::linear_image& src, const detail_params& d,
                                         const job_context* ctx) {
  if (!src.valid()) return err(status::invalid_arg);
  image::linear_image out;
  try {
    out = src;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  if (d.identity()) return out;
  const int w = static_cast<int>(src.width), h = static_cast<int>(src.height);
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  const float long_edge = static_cast<float>(std::max(w, h));
  // Radii scale with the picture, so the preview (kWorkingPreviewEdge) and a
  // full-size export look alike.
  const float scale = long_edge / static_cast<float>(image::kWorkingPreviewEdge);
  const int r_base = std::max(2, static_cast<int>(std::lround(0.01f * long_edge)));
  const int r_small = std::max(1, static_cast<int>(std::lround(scale)));
  const int r_noise = std::clamp(static_cast<int>(std::lround(1.5f * scale)), 1, 4);

  plane Y, L, A, B;
  try {
    Y.resize(n);
    L.resize(n);
    A.resize(n);
    B.resize(n);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  auto rgb = [&](std::size_t i, int k) { return image::half_to_float(src.rgba[i * 4 + static_cast<std::size_t>(k)]); };
  core::parallel_bands(n, 1u << 16, [&](std::size_t i0, std::size_t i1) {
    for (std::size_t i = i0; i < i1; ++i) {
      const float y = std::max(0.0f, 0.2126f * rgb(i, 0) + 0.7152f * rgb(i, 1) + 0.0722f * rgb(i, 2));
      Y[i] = y;
      L[i] = std::pow(y, 1.0f / 2.2f);
    }
  });
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  // Noise, lightness: a range-weighted mean over (2r+1)², the weight
  // 1 / (1 + (ΔL / σ)²), so an edge (a large ΔL) keeps its own side.
  if (d.noise > 0.0f) {
    const float sigma = 0.012f + 0.05f * d.noise;
    const float inv = 1.0f / (sigma * sigma);
    const int r = r_noise;
    core::parallel_bands(static_cast<std::size_t>(h), 16, [&](std::size_t y0, std::size_t y1) {
      for (std::size_t yy = y0; yy < y1; ++yy) {
        const int y = static_cast<int>(yy);
        for (int x = 0; x < w; ++x) {
          const float c = L[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
          float sw = 0.0f, sv = 0.0f;
          for (int dy = -r; dy <= r; ++dy) {
            const std::size_t row = static_cast<std::size_t>(std::clamp(y + dy, 0, h - 1)) * static_cast<std::size_t>(w);
            for (int dx = -r; dx <= r; ++dx) {
              const float v = L[row + static_cast<std::size_t>(std::clamp(x + dx, 0, w - 1))];
              const float wt = 1.0f / (1.0f + (v - c) * (v - c) * inv);
              sw += wt;
              sv += wt * v;
            }
          }
          A[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = sv / sw;
        }
      }
    });
    for (std::size_t i = 0; i < n; ++i) L[i] += (A[i] - L[i]) * d.noise;
    if (ctx && ctx->cancelled()) return err(status::cancelled);
  }

  // Definition and brilliance against a wide base (B).
  if (d.definition != 0.0f || d.brilliance != 0.0f) {
    B = L;
    blur(B, A, w, h, r_base, 3);
    for (std::size_t i = 0; i < n; ++i) {
      const float l = L[i], base = B[i];
      const float mid = 4.0f * std::clamp(l, 0.0f, 1.0f) * (1.0f - std::clamp(l, 0.0f, 1.0f));
      float v = l + d.definition * 0.8f * (l - base) * mid;
      v += d.brilliance * (0.35f * (0.5f - base) * mid + 0.2f * (l - base));
      L[i] = v;
    }
    if (ctx && ctx->cancelled()) return err(status::cancelled);
  }

  // Sharpen: an unsharp mask on a small blur, each step capped.
  if (d.sharpen > 0.0f) {
    B = L;
    blur(B, A, w, h, r_small, 2);
    for (std::size_t i = 0; i < n; ++i) {
      L[i] += std::clamp((L[i] - B[i]) * 1.5f * d.sharpen, -0.08f, 0.08f);
    }
    if (ctx && ctx->cancelled()) return err(status::cancelled);
  }

  // Noise, colour: each channel's ratio to luma toward its wide box mean
  // (Y-weighted), so colour speckle goes and edges in lightness stay.
  std::array<plane*, 3> chroma{};
  plane C0, C1, C2;
  if (d.noise > 0.0f) {
    try {
      C0.resize(n);
      C1.resize(n);
      C2.resize(n);
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
    chroma = {&C0, &C1, &C2};
    B = Y;
    blur(B, A, w, h, 4 * r_noise, 2);
    for (int k = 0; k < 3; ++k) {
      plane& p = *chroma[static_cast<std::size_t>(k)];
      for (std::size_t i = 0; i < n; ++i) p[i] = rgb(i, k);
      blur(p, A, w, h, 4 * r_noise, 2);
      for (std::size_t i = 0; i < n; ++i) p[i] = B[i] > 1e-5f ? p[i] / B[i] : 1.0f;
    }
    if (ctx && ctx->cancelled()) return err(status::cancelled);
  }

  // Back to RGB as a ratio of the new luma to the old (hue kept); what a
  // ratio cannot lift at black is added as grey, as the kernel's tone does.
  core::parallel_bands(n, 1u << 16, [&](std::size_t i0, std::size_t i1) {
    for (std::size_t i = i0; i < i1; ++i) {
      const float yt = std::pow(std::max(L[i], 0.0f), 2.2f);
      const float y = Y[i];
      const float k = yt / std::max(y, 1e-4f);
      const float rest = yt - y * k;
      for (int c = 0; c < 3; ++c) {
        float ratio = y > 1e-4f ? rgb(i, c) / y : 1.0f;
        if (chroma[0] != nullptr) {
          ratio += ((*chroma[static_cast<std::size_t>(c)])[i] - ratio) * d.noise;
        }
        const float v = y > 1e-4f ? ratio * yt : rgb(i, c) * k + rest;
        out.rgba[i * 4 + static_cast<std::size_t>(c)] = image::float_to_half(std::max(v, 0.0f));
      }
    }
  });
  return out;
}

}  // namespace mv::edit
