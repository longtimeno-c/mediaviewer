// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "infer/preprocess.h"

#include <algorithm>
#include <cmath>

namespace mv::infer {
namespace {

// PIL's bicubic (Resample.c): a = -0.5, support 2.
double bicubic(double x) noexcept {
  constexpr double a = -0.5;
  x = std::fabs(x);
  if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
  if (x < 2.0) return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
  return 0.0;
}

constexpr int kPrecision = 32 - 8 - 2;  // PIL's PRECISION_BITS for 8-bit images

struct taps {
  std::vector<int> first;             // per output pixel
  std::vector<int> count;
  std::vector<std::int32_t> k;        // count_max per output pixel, fixed point
  int stride = 0;
};

// PIL's precompute_coeffs + normalize_coeffs_8bpc.
taps coefficients(std::uint32_t in_size, std::uint32_t out_size) {
  const double scale = static_cast<double>(in_size) / out_size;
  const double filterscale = std::max(scale, 1.0);
  const double support = 2.0 * filterscale;
  taps t;
  t.stride = static_cast<int>(std::ceil(support)) * 2 + 1;
  t.first.resize(out_size);
  t.count.resize(out_size);
  t.k.assign(static_cast<std::size_t>(out_size) * t.stride, 0);
  std::vector<double> w(static_cast<std::size_t>(t.stride));
  for (std::uint32_t xx = 0; xx < out_size; ++xx) {
    const double center = (xx + 0.5) * scale;
    const double ss = 1.0 / filterscale;
    int xmin = static_cast<int>(center - support + 0.5);
    if (xmin < 0) xmin = 0;
    int xmax = static_cast<int>(center + support + 0.5);
    if (xmax > static_cast<int>(in_size)) xmax = static_cast<int>(in_size);
    xmax -= xmin;
    xmax = std::min(xmax, t.stride);
    double ww = 0.0;
    for (int x = 0; x < xmax; ++x) {
      const double v = bicubic((x + xmin - center + 0.5) * ss);
      w[static_cast<std::size_t>(x)] = v;
      ww += v;
    }
    for (int x = 0; x < xmax; ++x) {
      const double v = ww != 0.0 ? w[static_cast<std::size_t>(x)] / ww : 0.0;
      t.k[static_cast<std::size_t>(xx) * t.stride + x] =
          static_cast<std::int32_t>(v < 0 ? v * (1 << kPrecision) - 0.5 : v * (1 << kPrecision) + 0.5);
    }
    t.first[xx] = xmin;
    t.count[xx] = xmax;
  }
  return t;
}

std::uint8_t clip8(std::int64_t v) noexcept {
  v >>= kPrecision;
  return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

std::vector<std::uint8_t> resize_bicubic(const rgb_view& img, std::uint32_t w, std::uint32_t h) {
  const std::uint32_t iw = img.width, ih = img.height;
  std::vector<std::uint8_t> mid;
  const std::uint8_t* src = img.rgb;
  // Horizontal, then vertical, each rounding to 8 bits (PIL's order).
  if (w != iw) {
    const taps t = coefficients(iw, w);
    mid.resize(static_cast<std::size_t>(w) * ih * 3);
    for (std::uint32_t y = 0; y < ih; ++y) {
      const std::uint8_t* row = src + static_cast<std::size_t>(y) * iw * 3;
      std::uint8_t* out = mid.data() + static_cast<std::size_t>(y) * w * 3;
      for (std::uint32_t x = 0; x < w; ++x) {
        const std::int32_t* k = &t.k[static_cast<std::size_t>(x) * t.stride];
        for (int c = 0; c < 3; ++c) {
          std::int64_t ss = std::int64_t{1} << (kPrecision - 1);
          for (int i = 0; i < t.count[x]; ++i) {
            ss += static_cast<std::int64_t>(row[(t.first[x] + i) * 3 + c]) * k[i];
          }
          out[x * 3 + c] = clip8(ss);
        }
      }
    }
    src = mid.data();
  }
  if (h == ih) {
    if (src == img.rgb) return std::vector<std::uint8_t>(img.rgb, img.rgb + static_cast<std::size_t>(w) * h * 3);
    return mid;
  }
  const taps t = coefficients(ih, h);
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 3);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::int32_t* k = &t.k[static_cast<std::size_t>(y) * t.stride];
    std::uint8_t* o = out.data() + static_cast<std::size_t>(y) * w * 3;
    for (std::uint32_t x = 0; x < w * 3; ++x) {
      std::int64_t ss = std::int64_t{1} << (kPrecision - 1);
      for (int i = 0; i < t.count[y]; ++i) {
        ss += static_cast<std::int64_t>(src[static_cast<std::size_t>(t.first[y] + i) * w * 3 + x]) * k[i];
      }
      o[x] = clip8(ss);
    }
  }
  return out;
}

void clip_tensor(const rgb_view& img, const clip_norm& norm, std::vector<float>& out) {
  const std::uint32_t s = norm.size;
  // Shortest side to `s`, the other scaled and rounded half-to-even (Python's
  // round(), as the reference script computes it), never below `s`.
  std::uint32_t rw = s, rh = s;
  if (img.width <= img.height) {
    rh = std::max(s, static_cast<std::uint32_t>(std::nearbyint(static_cast<double>(img.height) * s / img.width)));
  } else {
    rw = std::max(s, static_cast<std::uint32_t>(std::nearbyint(static_cast<double>(img.width) * s / img.height)));
  }
  const std::vector<std::uint8_t> r = resize_bicubic(img, rw, rh);
  const std::uint32_t left = (rw - s) / 2, top = (rh - s) / 2;
  const std::size_t plane = static_cast<std::size_t>(s) * s;
  const std::size_t base = out.size();
  out.resize(base + plane * 3);
  for (std::uint32_t y = 0; y < s; ++y) {
    const std::uint8_t* row = r.data() + (static_cast<std::size_t>(y + top) * rw + left) * 3;
    for (std::uint32_t x = 0; x < s; ++x) {
      for (int c = 0; c < 3; ++c) {
        const float v = row[x * 3 + c] / 255.0f;
        out[base + c * plane + static_cast<std::size_t>(y) * s + x] = (v - norm.mean[c]) / norm.std[c];
      }
    }
  }
}

void yunet_tensor(const rgb_view& img, std::uint32_t side, std::vector<float>& out, float& scale) {
  const double f = std::min(static_cast<double>(side) / img.width, static_cast<double>(side) / img.height);
  const auto w = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(img.width * f));
  const auto h = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(img.height * f));
  const std::vector<std::uint8_t> r = resize_bicubic(img, w, h);
  scale = static_cast<float>(1.0 / f);
  const std::size_t plane = static_cast<std::size_t>(side) * side;
  out.assign(plane * 3, 0.0f);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint8_t* p = &r[(static_cast<std::size_t>(y) * w + x) * 3];
      const std::size_t at = static_cast<std::size_t>(y) * side + x;
      out[at] = p[2];              // B
      out[plane + at] = p[1];      // G
      out[2 * plane + at] = p[0];  // R
    }
  }
}

void sface_tensor(const rgb_view& img, const std::array<float, 10>& lm, std::vector<float>& out) {
  // ArcFace's 112x112 template (OpenCV FaceRecognizerSF::alignCrop).
  static constexpr float dst[10] = {38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f,
                                    71.7366f, 41.5493f, 92.3655f, 70.7299f, 92.2041f};
  // Umeyama without reflection: dst ~ s R src + t, then invert for sampling.
  double mx = 0, my = 0, dx = 0, dy = 0;
  for (int i = 0; i < 5; ++i) {
    mx += lm[i * 2];
    my += lm[i * 2 + 1];
    dx += dst[i * 2];
    dy += dst[i * 2 + 1];
  }
  mx /= 5; my /= 5; dx /= 5; dy /= 5;
  double a = 0, b = 0, var = 0;
  for (int i = 0; i < 5; ++i) {
    const double sx = lm[i * 2] - mx, sy = lm[i * 2 + 1] - my;
    const double tx = dst[i * 2] - dx, ty = dst[i * 2 + 1] - dy;
    a += sx * tx + sy * ty;
    b += sx * ty - sy * tx;
    var += sx * sx + sy * sy;
  }
  if (var <= 1e-9) var = 1e-9;
  const double c = a / var, s = b / var;  // [c -s; s c] = scale * rotation
  const double tx = dx - (c * mx - s * my), ty = dy - (s * mx + c * my);
  // Inverse: src = M^-1 (dst - t).
  const double det = c * c + s * s;
  const double ic = c / det, is = -s / det;
  constexpr std::uint32_t kSide = 112;
  const std::size_t plane = kSide * kSide;
  out.assign(plane * 3, 0.0f);
  for (std::uint32_t y = 0; y < kSide; ++y) {
    for (std::uint32_t x = 0; x < kSide; ++x) {
      const double px = x - tx, py = y - ty;
      const double sx = ic * px - is * py;
      const double sy = is * px + ic * py;
      const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
      const double fx = sx - x0, fy = sy - y0;
      for (int ch = 0; ch < 3; ++ch) {
        double acc = 0;
        for (int j = 0; j < 2; ++j) {
          for (int i = 0; i < 2; ++i) {
            const int xi = x0 + i, yi = y0 + j;
            if (xi < 0 || yi < 0 || xi >= static_cast<int>(img.width) || yi >= static_cast<int>(img.height)) continue;
            const double wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            acc += wgt * img.rgb[(static_cast<std::size_t>(yi) * img.width + xi) * 3 + ch];
          }
        }
        out[ch * plane + static_cast<std::size_t>(y) * kSide + x] = static_cast<float>(acc);
      }
    }
  }
}

}  // namespace mv::infer
