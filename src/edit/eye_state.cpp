// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/eye_state.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "image/half.h"

namespace mv::edit {
namespace {

#include "edit/eye_state_weights.inc"

// conv `k`x`k`, valid padding, stride 1: in (cin, h, w) -> out (cout, h-k+1, w-k+1).
template <int Cin, int Cout, int K>
void conv(const float* in, int h, int w, const float* weight, const float* bias, float* out) noexcept {
  const int oh = h - K + 1, ow = w - K + 1;
  for (int o = 0; o < Cout; ++o) {
    float* dst = out + static_cast<std::ptrdiff_t>(o) * oh * ow;
    const float b = bias ? bias[o] : 0.0f;
    for (int i = 0; i < oh * ow; ++i) dst[i] = b;
    for (int c = 0; c < Cin; ++c) {
      const float* src = in + static_cast<std::ptrdiff_t>(c) * h * w;
      const float* wk = weight + (static_cast<std::ptrdiff_t>(o) * Cin + c) * K * K;
      for (int ky = 0; ky < K; ++ky) {
        for (int kx = 0; kx < K; ++kx) {
          const float wv = wk[ky * K + kx];
          for (int y = 0; y < oh; ++y) {
            const float* row = src + static_cast<std::ptrdiff_t>(y + ky) * w + kx;
            float* d = dst + static_cast<std::ptrdiff_t>(y) * ow;
            for (int x = 0; x < ow; ++x) d[x] += wv * row[x];
          }
        }
      }
    }
  }
}

// max-pool `p`x`p`, stride p (floor), then ReLU when asked: (c, h, w) -> (c, h/p, w/p).
void pool(const float* in, int c, int h, int w, int p, bool relu, float* out) noexcept {
  const int oh = h / p, ow = w / p;
  for (int k = 0; k < c; ++k) {
    for (int y = 0; y < oh; ++y) {
      for (int x = 0; x < ow; ++x) {
        float m = -1e30f;
        for (int dy = 0; dy < p; ++dy) {
          for (int dx = 0; dx < p; ++dx) {
            m = std::max(m, in[(static_cast<std::ptrdiff_t>(k) * h + y * p + dy) * w + x * p + dx]);
          }
        }
        out[(static_cast<std::ptrdiff_t>(k) * oh + y) * ow + x] = relu ? std::max(m, 0.0f) : m;
      }
    }
  }
}

float srgb(float c) noexcept {
  c = std::clamp(std::isfinite(c) ? c : 0.0f, 0.0f, 1.0f);
  return 255.0f * (c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
}

// The picture's grey (sRGB, 0..255) at pixel (x, y), its centres on whole
// numbers, edges held.
float grey_at(const image::linear_image& img, int x, int y) noexcept {
  x = std::clamp(x, 0, static_cast<int>(img.width) - 1);
  y = std::clamp(y, 0, static_cast<int>(img.height) - 1);
  const std::uint16_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * img.width + x) * 4;
  return 0.299f * srgb(image::half_to_float(p[0])) + 0.587f * srgb(image::half_to_float(p[1])) +
         0.114f * srgb(image::half_to_float(p[2]));
}

float grey_bilinear(const image::linear_image& img, float x, float y) noexcept {
  const float fx = std::floor(x), fy = std::floor(y);
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  const float ax = x - fx, ay = y - fy;
  const float top = grey_at(img, x0, y0) * (1 - ax) + grey_at(img, x0 + 1, y0) * ax;
  const float bot = grey_at(img, x0, y0 + 1) * (1 - ax) + grey_at(img, x0 + 1, y0 + 1) * ax;
  return top * (1 - ay) + bot * ay;
}

// One eye's 32x32 grey square, `side` pixels of the picture across, centred
// on (ex, ey) and turned with the eye line (c, s): four samples a cell.
void eye_square(const image::linear_image& img, float ex, float ey, float c, float s, float side,
                std::array<float, 32 * 32>& out) noexcept {
  const float st = side / 32.0f;
  for (int v = 0; v < 32; ++v) {
    for (int u = 0; u < 32; ++u) {
      float sum = 0.0f;
      for (const float oy : {-0.25f, 0.25f}) {
        for (const float ox : {-0.25f, 0.25f}) {
          const float a = (static_cast<float>(u) - 16.0f + ox) * st, b = (static_cast<float>(v) - 16.0f + oy) * st;
          sum += grey_bilinear(img, ex + c * a - s * b, ey + s * a + c * b);
        }
      }
      out[static_cast<std::size_t>(v) * 32 + u] = sum * 0.25f;
    }
  }
}

}  // namespace

float eye_open_probability(const std::array<float, 32 * 32>& grey) noexcept {
  // 3 equal channels in, as the model takes colour: (v - 127) / 255.
  static thread_local float in[3 * 32 * 32], a[10 * 30 * 30], b[10 * 15 * 15], c2[20 * 13 * 13], d[20 * 6 * 6],
      e[50 * 4 * 4], f[2 * 4 * 4];
  for (int k = 0; k < 3; ++k) {
    for (int i = 0; i < 32 * 32; ++i) in[k * 32 * 32 + i] = (grey[static_cast<std::size_t>(i)] - 127.0f) / 255.0f;
  }
  conv<3, 10, 3>(in, 32, 32, kConv1Weight, kConv1Bias, a);
  pool(a, 10, 30, 30, 2, true, b);
  conv<10, 20, 3>(b, 15, 15, kConv2Weight, kConv2Bias, c2);
  pool(c2, 20, 13, 13, 2, true, d);
  conv<20, 50, 3>(d, 6, 6, kConv3Weight, kConv3Bias, e);
  conv<50, 2, 1>(e, 4, 4, kConv4Weight, nullptr, f);
  float z[2];
  for (int k = 0; k < 2; ++k) z[k] = *std::max_element(f + k * 16, f + k * 16 + 16);
  const float m = std::max(z[0], z[1]);
  const float e0 = std::exp(z[0] - m), e1 = std::exp(z[1] - m);
  return e1 / (e0 + e1);
}

namespace {
// Linear luminance at a pixel, edges held.
float lum_at(const image::linear_image& img, float x, float y) noexcept {
  const int xi = std::clamp(static_cast<int>(x), 0, static_cast<int>(img.width) - 1);
  const int yi = std::clamp(static_cast<int>(y), 0, static_cast<int>(img.height) - 1);
  const std::uint16_t* p = img.rgba.data() + (static_cast<std::size_t>(yi) * img.width + xi) * 4;
  return 0.2126f * image::half_to_float(p[0]) + 0.7152f * image::half_to_float(p[1]) +
         0.0722f * image::half_to_float(p[2]);
}
// The mean of a box turned with the eye line: centre (cx, cy), half sides hw x hh.
float box_mean(const image::linear_image& img, float cx, float cy, float c, float s, float hw, float hh) noexcept {
  float sum = 0.0f;
  int n = 0;
  for (int j = -3; j <= 3; ++j) {
    for (int i = -5; i <= 5; ++i) {
      const float a = static_cast<float>(i) / 5.0f * hw, b = static_cast<float>(j) / 3.0f * hh;
      sum += lum_at(img, cx + c * a - s * b, cy + s * a + c * b);
      ++n;
    }
  }
  return sum / static_cast<float>(n);
}
}  // namespace

bool eyes_readable(const image::linear_image& img, const face_landmarks& lm) noexcept {
  if (!img.valid()) return false;
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  const float rx = lm[0] * w, ry = lm[1] * h, lx = lm[2] * w, ly = lm[3] * h;
  const float d = std::hypot(lx - rx, ly - ry);
  if (!(d >= kEyeReadMinPx)) return false;
  // Both eyes inside the picture.
  for (const float v : {rx, lx}) {
    if (v < 0.0f || v >= w) return false;
  }
  for (const float v : {ry, ly}) {
    if (v < 0.0f || v >= h) return false;
  }
  const float c = (lx - rx) / d, s = (ly - ry) / d;
  const float mx = (rx + lx) * 0.5f, my = (ry + ly) * 0.5f;
  // Turned: the nose off the eyes' middle, or far below them (a face seen
  // from below, or points gone astray).
  const float nx = lm[4] * w - mx, ny = lm[5] * h - my;
  const float nu = (nx * c + ny * s) / d, nv = (-nx * s + ny * c) / d;
  if (std::abs(nu) > 0.3f || nv > 0.85f || nv < 0.2f) return false;
  // Dark glasses: the lids above both eyes as dark as a lens, against the
  // cheeks under them (a shut eye's lid is skin).
  const float cheek = box_mean(img, mx - s * 0.75f * d, my + c * 0.75f * d, c, s, 0.5f * d, 0.12f * d);
  if (cheek > 1e-5f) {
    float lid = 0.0f;
    for (const auto& [ex, ey] : {std::pair{rx, ry}, std::pair{lx, ly}}) {
      lid = std::max(lid, box_mean(img, ex + s * 0.17f * d, ey - c * 0.17f * d, c, s, 0.18f * d, 0.05f * d) / cheek);
    }
    if (lid < 0.5f) return false;
  }
  // The points on eyes, not brows: a brow, a band darker than the lid, a
  // fifth to two fifths of an eye distance above at least one of them (YuNet
  // can put a face's eye points on its brows, looking down; then there is
  // only forehead above).
  float brow_ratio = 1e9f;
  for (const auto& [ex, ey] : {std::pair{rx, ry}, std::pair{lx, ly}}) {
    auto band = [&](float up) { return box_mean(img, ex + s * up * d, ey - c * up * d, c, s, 0.2f * d, 0.03f * d); };
    const float lid = band(0.12f);
    float brow = 1e9f;
    for (int i = 0; i < 8; ++i) brow = std::min(brow, band(0.2f + 0.22f * static_cast<float>(i) / 7.0f));
    if (lid > 1e-6f) brow_ratio = std::min(brow_ratio, brow / lid);
  }
  if (!(brow_ratio < 0.72f)) return false;
  // Sharp enough to read: a band over both eyes, levelled and scaled so the
  // eyes are 64 px apart, its mean |Laplacian| over its mean grey (a blurred
  // face read 0.016; every blink of the owner's 0.034 or more).
  constexpr int kBw = 112, kBh = 20;
  std::array<float, kBw * kBh> band{};
  const float step = d / 64.0f;
  for (int j = 0; j < kBh; ++j) {
    for (int i = 0; i < kBw; ++i) {
      const float a = (static_cast<float>(i) - kBw / 2) * step, b = (static_cast<float>(j) - kBh / 2) * step;
      band[static_cast<std::size_t>(j) * kBw + i] = grey_bilinear(img, mx + c * a - s * b, my + s * a + c * b);
    }
  }
  double lap = 0.0, mean = 0.0;
  int n = 0;
  for (int j = 1; j + 1 < kBh; ++j) {
    for (int i = 1; i + 1 < kBw; ++i) {
      const auto at = [&](int x, int y) { return band[static_cast<std::size_t>(y) * kBw + x]; };
      lap += std::abs(at(i - 1, j) + at(i + 1, j) + at(i, j - 1) + at(i, j + 1) - 4.0f * at(i, j));
      mean += at(i, j);
      ++n;
    }
  }
  return n > 0 && mean > 1.0 && (lap / n) / (mean / n) >= 0.025;
}

eye_state read_eye_state(const image::linear_image& img, const face_landmarks& lm) noexcept {
  eye_state out;
  if (!img.valid() || !eyes_readable(img, lm)) return out;
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  const float rx = lm[0] * w, ry = lm[1] * h, lx = lm[2] * w, ly = lm[3] * h;
  const float dist = std::hypot(lx - rx, ly - ry);
  const float c = (lx - rx) / dist, s = (ly - ry) / dist;
  // Three sizes x four contrasts x mirrored: 24 reads an eye (2026-11-07:
  // one size and one contrast alone left shut and open faces overlapping).
  constexpr float kSizes[] = {0.40f, 0.45f, 0.50f};
  constexpr float kSpreads[] = {35.0f, 40.0f, 45.0f, 50.0f};
  std::array<float, 32 * 32> raw{}, std_grey{}, flip{};
  float* eyes[2] = {&out.right, &out.left};
  const float centre[2][2] = {{rx, ry}, {lx, ly}};
  for (int e = 0; e < 2; ++e) {
    const float ex = centre[e][0], ey = centre[e][1];
    // An eye whose square leaves the picture by more than a quarter is not read.
    const float half = 0.25f * dist;
    if (ex < -half || ey < -half || ex > w + half || ey > h + half) continue;
    double sum = 0.0;
    int n = 0;
    for (const float z : kSizes) {
      eye_square(img, ex, ey, c, s, z * dist, raw);
      double mean = 0.0, sq = 0.0;
      for (const float v : raw) mean += v, sq += static_cast<double>(v) * v;
      mean /= raw.size();
      const float sd = std::max(1.0f, static_cast<float>(std::sqrt(std::max(0.0, sq / raw.size() - mean * mean))));
      for (const float spread : kSpreads) {
        for (std::size_t i = 0; i < raw.size(); ++i) {
          std_grey[i] = std::clamp((raw[i] - static_cast<float>(mean)) / sd * spread + 127.0f, 0.0f, 255.0f);
        }
        for (int y = 0; y < 32; ++y) {
          for (int x = 0; x < 32; ++x) flip[static_cast<std::size_t>(y) * 32 + x] = std_grey[static_cast<std::size_t>(y) * 32 + 31 - x];
        }
        sum += eye_open_probability(std_grey) + eye_open_probability(flip);
        n += 2;
      }
    }
    *eyes[e] = static_cast<float>(sum / n);
  }
  return out;
}

}  // namespace mv::edit
