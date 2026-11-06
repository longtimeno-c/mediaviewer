// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/eyes.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "image/half.h"

namespace mv::edit {
namespace {

struct px3 {
  float r = 0, g = 0, b = 0;
};

px3 at(const image::linear_image& img, int x, int y) noexcept {
  x = std::clamp(x, 0, static_cast<int>(img.width) - 1);
  y = std::clamp(y, 0, static_cast<int>(img.height) - 1);
  const std::uint16_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4;
  return {image::half_to_float(p[0]), image::half_to_float(p[1]), image::half_to_float(p[2])};
}

px3 bilinear(const image::linear_image& img, float x, float y) noexcept {
  const float fx = std::floor(x), fy = std::floor(y);
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  const float ax = x - fx, ay = y - fy;
  const px3 a = at(img, x0, y0), b = at(img, x0 + 1, y0), c = at(img, x0, y0 + 1), d = at(img, x0 + 1, y0 + 1);
  auto mix = [&](float p, float q, float s, float t) { return (p + (q - p) * ax) + ((s + (t - s) * ax) - (p + (q - p) * ax)) * ay; };
  return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b)};
}

float lightness(const px3& p) noexcept {
  return std::pow(std::max(0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b, 0.0f), 1.0f / 2.2f);
}

struct eye_pts {
  float rx, ry, lx, ly, d;  // pixels; d = distance between the eyes
};

eye_pts eyes_of(const image::linear_image& img, const face_landmarks& lm) noexcept {
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  eye_pts e{lm[0] * w, lm[1] * h, lm[2] * w, lm[3] * h, 0};
  e.d = std::hypot(e.lx - e.rx, e.ly - e.ry);
  return e;
}

float one_eye(const image::linear_image& img, float cx, float cy, float d) noexcept {
  const int hw = std::max(2, static_cast<int>(0.28f * d)), hh = std::max(2, static_cast<int>(0.16f * d));
  std::vector<float> v;
  v.reserve(static_cast<std::size_t>((2 * hw + 1) * (2 * hh + 1)));
  for (int y = -hh; y <= hh; ++y) {
    for (int x = -hw; x <= hw; ++x) {
      v.push_back(lightness(at(img, static_cast<int>(cx) + x, static_cast<int>(cy) + y)));
    }
  }
  std::vector<float> sorted = v;
  std::sort(sorted.begin(), sorted.end());
  const float lo = sorted[sorted.size() / 10], hi = sorted[sorted.size() * 9 / 10];
  const float spread = hi - lo;
  if (spread <= 1e-4f) return 0.0f;
  const float dark = lo + 0.3f * spread;
  // How many rows have a dark pixel near the middle third (the iris), of all.
  int rows = 0;
  for (int y = 0; y <= 2 * hh; ++y) {
    for (int x = hw - hw / 3; x <= hw + hw / 3; ++x) {
      if (v[static_cast<std::size_t>(y * (2 * hw + 1) + x)] < dark) {
        ++rows;
        break;
      }
    }
  }
  return spread * static_cast<float>(rows) / static_cast<float>(2 * hh + 1);
}

}  // namespace

float eye_openness(const image::linear_image& img, const face_landmarks& lm) noexcept {
  if (!img.valid()) return 0.0f;
  const eye_pts e = eyes_of(img, lm);
  if (e.d < 4.0f) return 0.0f;
  return std::min(one_eye(img, e.rx, e.ry, e.d), one_eye(img, e.lx, e.ly, e.d));
}

expected transfer_eyes(image::linear_image& target, const face_landmarks& target_lm,
                       const image::linear_image& donor, const face_landmarks& donor_lm) noexcept {
  if (!target.valid() || !donor.valid()) return err(status::invalid_arg);
  const eye_pts t = eyes_of(target, target_lm), s = eyes_of(donor, donor_lm);
  if (t.d < 12.0f || s.d < 12.0f) return err(status::invalid_arg);
  // Target pixel -> donor pixel: the similarity that puts the target's eye
  // line on the donor's (inverse of donor -> target).
  const float tvx = t.lx - t.rx, tvy = t.ly - t.ry, svx = s.lx - s.rx, svy = s.ly - s.ry;
  const float scale = s.d / t.d;
  const float ang = std::atan2(svy, svx) - std::atan2(tvy, tvx);
  const float ca = std::cos(ang) * scale, sa = std::sin(ang) * scale;
  auto to_donor = [&](float x, float y, float& dx, float& dy) {
    const float ox = x - t.rx, oy = y - t.ry;
    dx = s.rx + ca * ox - sa * oy;
    dy = s.ry + sa * ox + ca * oy;
  };
  const float a = 0.34f * t.d, b = 0.21f * t.d;  // the ellipse's half axes
  const float cosr = tvx / t.d, sinr = tvy / t.d;   // along the eye line
  for (const auto& [cx, cy] : {std::pair{t.rx, t.ry}, std::pair{t.lx, t.ly}}) {
    // Colour match on the ring of skin between 1.1 and 1.6 of the ellipse.
    double tm[3] = {0, 0, 0}, dm[3] = {0, 0, 0};
    int n = 0;
    const int rx = static_cast<int>(1.7f * a) + 2;
    for (int y = static_cast<int>(cy) - rx; y <= static_cast<int>(cy) + rx; ++y) {
      for (int x = static_cast<int>(cx) - rx; x <= static_cast<int>(cx) + rx; ++x) {
        const float ux = (static_cast<float>(x) - cx) * cosr + (static_cast<float>(y) - cy) * sinr;
        const float uy = -(static_cast<float>(x) - cx) * sinr + (static_cast<float>(y) - cy) * cosr;
        const float q = std::sqrt((ux / a) * (ux / a) + (uy / b) * (uy / b));
        if (q < 1.1f || q > 1.6f) continue;
        const px3 tp = at(target, x, y);
        float dx = 0, dy = 0;
        to_donor(static_cast<float>(x), static_cast<float>(y), dx, dy);
        const px3 dp = bilinear(donor, dx, dy);
        tm[0] += tp.r, tm[1] += tp.g, tm[2] += tp.b;
        dm[0] += dp.r, dm[1] += dp.g, dm[2] += dp.b;
        ++n;
      }
    }
    float gain[3] = {1, 1, 1};
    if (n > 0) {
      for (int k = 0; k < 3; ++k) {
        gain[k] = dm[k] > 1e-6 ? std::clamp(static_cast<float>(tm[k] / dm[k]), 0.6f, 1.6f) : 1.0f;
      }
    }
    // The ellipse, feathered over its outer 35 %.
    const int r = static_cast<int>(a) + 2;
    for (int y = static_cast<int>(cy) - r; y <= static_cast<int>(cy) + r; ++y) {
      if (y < 0 || y >= static_cast<int>(target.height)) continue;
      for (int x = static_cast<int>(cx) - r; x <= static_cast<int>(cx) + r; ++x) {
        if (x < 0 || x >= static_cast<int>(target.width)) continue;
        const float ux = (static_cast<float>(x) - cx) * cosr + (static_cast<float>(y) - cy) * sinr;
        const float uy = -(static_cast<float>(x) - cx) * sinr + (static_cast<float>(y) - cy) * cosr;
        const float q = std::sqrt((ux / a) * (ux / a) + (uy / b) * (uy / b));
        if (q >= 1.0f) continue;
        float alpha = std::clamp((1.0f - q) / 0.35f, 0.0f, 1.0f);
        alpha = alpha * alpha * (3.0f - 2.0f * alpha);
        float dx = 0, dy = 0;
        to_donor(static_cast<float>(x), static_cast<float>(y), dx, dy);
        const px3 dp = bilinear(donor, dx, dy);
        std::uint16_t* p =
            target.rgba.data() + (static_cast<std::size_t>(y) * target.width + static_cast<std::size_t>(x)) * 4;
        const float src[3] = {dp.r * gain[0], dp.g * gain[1], dp.b * gain[2]};
        for (int k = 0; k < 3; ++k) {
          const float o = image::half_to_float(p[k]);
          p[k] = image::float_to_half(std::max(0.0f, o + (src[k] - o) * alpha));
        }
      }
    }
  }
  return {};
}

}  // namespace mv::edit
