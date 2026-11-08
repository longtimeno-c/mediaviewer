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
  [[nodiscard]] float c() const noexcept { return d > 0.0f ? (lx - rx) / d : 1.0f; }
  [[nodiscard]] float s() const noexcept { return d > 0.0f ? (ly - ry) / d : 0.0f; }
};

// The pixel (x, y) steps from (cx, cy) in the eye line's frame: x along the
// line between the eyes (c, sn its direction), y across it. A tilted head's
// patch is read as an upright one's (a shut eye's lashes stay one row).
px3 along(const image::linear_image& img, float cx, float cy, float c, float sn, int x, int y) noexcept {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  return at(img, static_cast<int>(std::lround(cx + fx * c - fy * sn)),
            static_cast<int>(std::lround(cy + fx * sn + fy * c)));
}

eye_pts eyes_of(const image::linear_image& img, const face_landmarks& lm) noexcept {
  const float w = static_cast<float>(img.width), h = static_cast<float>(img.height);
  eye_pts e{lm[0] * w, lm[1] * h, lm[2] * w, lm[3] * h, 0};
  e.d = std::hypot(e.lx - e.rx, e.ly - e.ry);
  return e;
}

// One eye's patch about (cx, cy), sized by the eye distance `d`: the spread of
// lightness (10th to 90th percentile) and the share of its rows with a dark
// pixel in the middle third (the iris, or a line of lashes).
struct eye_read {
  float spread = 0.0f;
  float rows = 0.0f;
};

eye_read read_eye(const image::linear_image& img, float cx, float cy, float d, float c, float sn) noexcept {
  const int hw = std::max(2, static_cast<int>(0.28f * d)), hh = std::max(2, static_cast<int>(0.16f * d));
  std::vector<float> v;
  v.reserve(static_cast<std::size_t>((2 * hw + 1) * (2 * hh + 1)));
  for (int y = -hh; y <= hh; ++y) {
    for (int x = -hw; x <= hw; ++x) v.push_back(lightness(along(img, cx, cy, c, sn, x, y)));
  }
  std::vector<float> sorted = v;
  std::sort(sorted.begin(), sorted.end());
  const float lo = sorted[sorted.size() / 10], hi = sorted[sorted.size() * 9 / 10];
  const float spread = hi - lo;
  if (spread <= 1e-4f) return {};
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
  return {spread, static_cast<float>(rows) / static_cast<float>(2 * hh + 1)};
}

float one_eye(const image::linear_image& img, float cx, float cy, float d, float c, float sn) noexcept {
  const eye_read r = read_eye(img, cx, cy, d, c, sn);
  return r.spread * r.rows;
}

}  // namespace

float eye_openness(const image::linear_image& img, const face_landmarks& lm) noexcept {
  if (!img.valid()) return 0.0f;
  const eye_pts e = eyes_of(img, lm);
  if (e.d < 4.0f) return 0.0f;
  return std::min(one_eye(img, e.rx, e.ry, e.d, e.c(), e.s()), one_eye(img, e.lx, e.ly, e.d, e.c(), e.s()));
}

namespace {
// The share of an eye patch's rows (one_eye's patch) with a pixel in the
// middle third darker than 55 % of the patch's median lightness (its skin): the
// iris and lids of an open eye, a line of lashes of a shut one. -1 when the
// patch has nothing dark at all (nothing to read).
float dark_rows(const image::linear_image& img, float cx, float cy, float d, float c, float sn) {
  const int hw = std::max(2, static_cast<int>(0.28f * d)), hh = std::max(2, static_cast<int>(0.16f * d));
  std::vector<float> v;
  v.reserve(static_cast<std::size_t>((2 * hw + 1) * (2 * hh + 1)));
  for (int y = -hh; y <= hh; ++y) {
    for (int x = -hw; x <= hw; ++x) v.push_back(lightness(along(img, cx, cy, c, sn, x, y)));
  }
  std::vector<float> sorted = v;
  std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
  const float dark = 0.55f * sorted[sorted.size() / 2];
  int rows = 0;
  for (int y = 0; y <= 2 * hh; ++y) {
    for (int x = hw - hw / 3; x <= hw + hw / 3; ++x) {
      if (v[static_cast<std::size_t>(y * (2 * hw + 1) + x)] < dark) {
        ++rows;
        break;
      }
    }
  }
  if (rows == 0) return -1.0f;
  return static_cast<float>(rows) / static_cast<float>(2 * hh + 1);
}
}  // namespace

bool eyes_look_shut(const image::linear_image& img, const face_landmarks& lm) noexcept {
  if (!img.valid()) return false;
  const eye_pts e = eyes_of(img, lm);
  if (e.d < 8.0f) return false;  // too small to tell
  try {
    const float r = dark_rows(img, e.rx, e.ry, e.d, e.c(), e.s()), l = dark_rows(img, e.lx, e.ly, e.d, e.c(), e.s());
    // A patch with nothing dark is not read as shut (no eye there to read).
    auto shut = [](float rows) { return rows >= 0.0f && rows < kShutRows; };
    return shut(r) || shut(l);
  } catch (...) {
    return false;  // out of memory: no guess
  }
}

std::array<float, 4> face_region(const std::array<float, 4>& box, const face_landmarks& lm, float w,
                                 float h) noexcept {
  float x0 = box[0] - kFaceRegionGrow * box[2], y0 = box[1] - kFaceRegionGrow * box[3];
  float x1 = box[0] + box[2] * (1.0f + kFaceRegionGrow), y1 = box[1] + box[3] * (1.0f + kFaceRegionGrow);
  if (w > 0.0f && h > 0.0f) {
    // transfer_face's ellipse: about the nose nudged toward the eyes, half
    // axes kFaceHalfWidth and kFaceHalfHeight eye distances, turned with the
    // eye line.
    const float rx = lm[0] * w, ry = lm[1] * h, lx = lm[2] * w, ly = lm[3] * h;
    const float vx = lx - rx, vy = ly - ry, d = std::sqrt(vx * vx + vy * vy);
    if (d > 0.0f && std::isfinite(d)) {
      const float c = vx / d, s = vy / d, a = kFaceHalfWidth * d, b = kFaceHalfHeight * d;
      const float ex = (rx + lx) * 0.5f, ey = (ry + ly) * 0.5f;
      const float cx = lm[4] * w + (ex - lm[4] * w) * 0.1f, cy = lm[5] * h + (ey - lm[5] * h) * 0.1f;
      const float hx = std::sqrt(a * a * c * c + b * b * s * s), hy = std::sqrt(a * a * s * s + b * b * c * c);
      x0 = std::min(x0, (cx - hx) / w), x1 = std::max(x1, (cx + hx) / w);
      y0 = std::min(y0, (cy - hy) / h), y1 = std::max(y1, (cy + hy) / h);
    }
  }
  x0 = std::clamp(x0, 0.0f, 1.0f), y0 = std::clamp(y0, 0.0f, 1.0f);
  x1 = std::clamp(x1, 0.0f, 1.0f), y1 = std::clamp(y1, 0.0f, 1.0f);
  return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

namespace {
// Box blur, `radius` each side, in place over a w x h plane (outside = 0).
void box_blur(std::vector<float>& v, int w, int h, int radius) {
  std::vector<float> line(static_cast<std::size_t>(std::max(w, h)));
  for (int y = 0; y < h; ++y) {
    float sum = 0.0f;
    float* row = v.data() + static_cast<std::size_t>(y) * w;
    for (int x = -radius; x <= radius; ++x) sum += (x >= 0 && x < w) ? row[x] : 0.0f;
    for (int x = 0; x < w; ++x) {
      line[static_cast<std::size_t>(x)] = sum;
      const int out = x - radius, in = x + radius + 1;
      if (out >= 0) sum -= row[out];
      if (in < w) sum += row[in];
    }
    std::copy(line.begin(), line.begin() + w, row);
  }
  for (int x = 0; x < w; ++x) {
    float sum = 0.0f;
    for (int y = -radius; y <= radius; ++y) sum += (y >= 0 && y < h) ? v[static_cast<std::size_t>(y) * w + x] : 0.0f;
    for (int y = 0; y < h; ++y) {
      line[static_cast<std::size_t>(y)] = sum;
      const int out = y - radius, in = y + radius + 1;
      if (out >= 0) sum -= v[static_cast<std::size_t>(out) * w + x];
      if (in < h) sum += v[static_cast<std::size_t>(in) * w + x];
    }
    for (int y = 0; y < h; ++y) v[static_cast<std::size_t>(y) * w + x] = line[static_cast<std::size_t>(y)];
  }
}

// One region of the target replaced by the donor's, through `to_donor`
// (target pixel -> donor pixel, `scale` donor pixels a target pixel): an
// ellipse at (cx, cy) with half axes a (along cosr, sinr) and b, feathered over
// its outer 35 %. The donor's look is matched to the target's:
//   eyes (`local` false)  on the ring of skin between 1.1 and 1.6 of the
//                         ellipse, one gain and offset per channel;
//   a face (`local` true) area by area: both faces blurred inside the ellipse
//                         alone (about a third of an eye distance), the donor
//                         times this photo's blur over its own. Its features
//                         stay, this photo's light and colour come with them,
//                         and a ring of hair, collar and background is never
//                         read. A donor whose colour differs from this photo's
//                         in patches (face paint, a coloured spot) is refused:
//                         false, nothing written.
template <class ToDonor>
bool paste_region(image::linear_image& target, const image::linear_image& donor, ToDonor to_donor, float scale,
                  float cx, float cy, float a, float b, float cosr, float sinr, bool local = false) {
  // The donor's look matched to the target's, measured on the ring of skin
  // between 1.1 and 1.6 of the ellipse (owner 2026-11-04: the light, the
  // glints and the glare follow the photo): per channel mean and spread;
  // how sharp and how grainy (mean |Laplacian|); and how bright the
  // highlights on the skin are against it (the 98th percentile of
  // luminance over the mean), which scales the donor's catchlights.
  struct look {
    double m[3] = {0, 0, 0}, sq[3] = {0, 0, 0}, lap = 0;
    std::vector<float> lum;
    int n = 0;
  } tl, dl;
  const auto luma = [](const px3& q) { return 0.2126f * q.r + 0.7152f * q.g + 0.0722f * q.b; };
  // Measured on the ring for eyes, inside the face for a face. The square
  // walked is the ellipse's own (its long half axis may be b).
  const float ring_in = local ? 0.0f : 1.1f, ring_out = local ? 1.0f : 1.6f;
  const int rx = static_cast<int>(ring_out * std::max(a, b)) + 2;
  for (int y = static_cast<int>(cy) - rx; y <= static_cast<int>(cy) + rx; ++y) {
    for (int x = static_cast<int>(cx) - rx; x <= static_cast<int>(cx) + rx; ++x) {
      const float ux = (static_cast<float>(x) - cx) * cosr + (static_cast<float>(y) - cy) * sinr;
      const float uy = -(static_cast<float>(x) - cx) * sinr + (static_cast<float>(y) - cy) * cosr;
      const float q = std::sqrt((ux / a) * (ux / a) + (uy / b) * (uy / b));
      if (q < ring_in || q > ring_out) continue;
      float dx = 0, dy = 0;
      to_donor(static_cast<float>(x), static_cast<float>(y), dx, dy);
      const px3 tp = at(target, x, y), dp = bilinear(donor, dx, dy);
      // Both sampled on the target's grid, so their detail compares.
      const auto lap = [&](auto&& sample, float sx, float sy, float step) {
        const float c = luma(sample(sx, sy));
        return std::abs(4.0f * c - luma(sample(sx - step, sy)) - luma(sample(sx + step, sy)) -
                        luma(sample(sx, sy - step)) - luma(sample(sx, sy + step)));
      };
      tl.lap += lap([&](float u, float v) { return at(target, static_cast<int>(u), static_cast<int>(v)); },
                    static_cast<float>(x), static_cast<float>(y), 1.0f);
      dl.lap += lap([&](float u, float v) { return bilinear(donor, u, v); }, dx, dy, scale);
      const float tv[3] = {tp.r, tp.g, tp.b}, dv[3] = {dp.r, dp.g, dp.b};
      for (int k = 0; k < 3; ++k) {
        tl.m[k] += tv[k], tl.sq[k] += tv[k] * tv[k];
        dl.m[k] += dv[k], dl.sq[k] += dv[k] * dv[k];
      }
      tl.lum.push_back(luma(tp));
      dl.lum.push_back(luma(dp));
      ++tl.n, ++dl.n;
    }
  }
  float gain[3] = {1, 1, 1}, offset[3] = {0, 0, 0};
  float glint = 1.0f;     // how much of a catchlight's excess over the skin stays
  float soften = 0.0f;    // 0..1 of a 3x3 blur over the donor patch
  float grain = 0.0f;     // the noise added, in linear units
  float skin_mean = 0.0f;
  if (tl.n > 8) {
    const double n = tl.n;
    for (int k = 0; k < 3; ++k) {
      const double tm = tl.m[k] / n, dm = dl.m[k] / n;
      // The skin's colour, as a ratio: src' = src * tm / dm. A spread match
      // (src - dm) * ts / ds + tm washed a donor's eyes out against dim,
      // warm skin (2026-11-07, IMG_2113): its whites and lids came out pale
      // and cold. The ratio keeps the donor's contrast in this light.
      gain[k] = dm > 1e-6 ? std::clamp(static_cast<float>(tm / dm), 0.25f, 4.0f) : 1.0f;
      offset[k] = 0.0f;
    }
    const auto peak = [](std::vector<float>& v) {
      const std::size_t i = v.size() * 98 / 100;
      std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
      return v[i];
    };
    const double tmean = (tl.m[0] * 0.2126 + tl.m[1] * 0.7152 + tl.m[2] * 0.0722) / n;
    const double dmean = (dl.m[0] * 0.2126 + dl.m[1] * 0.7152 + dl.m[2] * 0.0722) / n;
    skin_mean = static_cast<float>(tmean);
    if (tmean > 1e-6 && dmean > 1e-6) {
      const float tr = peak(tl.lum) / static_cast<float>(tmean), dr = peak(dl.lum) / static_cast<float>(dmean);
      glint = dr > 1e-3f ? std::clamp(tr / dr, 0.35f, 1.5f) : 1.0f;
    }
    const double tlap = tl.lap / n, dlap = dl.lap / n;
    if (dlap > tlap * 1.15 && dlap > 1e-6) soften = std::clamp(static_cast<float>(1.0 - tlap / dlap), 0.0f, 0.8f);
    // Grain: what the target's detail has beyond the donor's, as noise.
    if (tlap > dlap * 1.15) grain = static_cast<float>((tlap - dlap) * 0.25);
  }
  // The ellipse's square (its long half axis each way), feathered over its
  // outer 35 %.
  const int r = static_cast<int>(std::max(a, b)) + 2;
  const int x0 = static_cast<int>(cx) - r, y0 = static_cast<int>(cy) - r, side = 2 * r + 1;
  const auto q_at = [&](int x, int y) {
    const float ux = (static_cast<float>(x) - cx) * cosr + (static_cast<float>(y) - cy) * sinr;
    const float uy = -(static_cast<float>(x) - cx) * sinr + (static_cast<float>(y) - cy) * cosr;
    return std::sqrt((ux / a) * (ux / a) + (uy / b) * (uy / b));
  };
  // This photo's light over the donor's, per channel and pixel, on a square
  // around the ellipse (fx0, fy0, fside):
  //   a face  both blurred inside the ellipse alone (about a third of an eye
  //           distance): its features stay, this photo's light comes with them;
  //   eyes    both blurred over the ring of skin around the ellipse alone, wide
  //           enough to reach the middle: the light inside follows the skin
  //           next to it all the way round. One gain for the whole eye left a
  //           lighter or darker patch wherever the donor's light fell
  //           differently over the brow or the cheek (2026-11-07, IMG_2113).
  std::vector<float> ratio;
  int fx0 = x0, fy0 = y0, fside = side;
  {
    // Eyes read the ring's skin only: the inside's (a shut eye's lid, even
    // only the ellipse's outer rim) laid over the donor's whites and iris
    // took their contrast (2026-11-07).
    const float in = local ? 0.0f : 1.0f, out = local ? 1.0f : ring_out;
    if (!local) {
      const int fr = static_cast<int>(out * std::max(a, b)) + 2;
      fx0 = static_cast<int>(cx) - fr, fy0 = static_cast<int>(cy) - fr, fside = 2 * fr + 1;
    }
    // Eyes: skin only. Brows, lashes and hair on the ring are not this photo's
    // light against the donor's (a brow over the donor's skin read as a lid
    // to lighten, 2026-11-07): a pixel darker than 60 % of either ring's
    // median, or brighter than twice it (glare), is left out.
    float t_med = 0.0f, d_med = 0.0f;
    if (!local && !tl.lum.empty()) {
      auto median = [](std::vector<float> v) {
        std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
        return v[v.size() / 2];
      };
      t_med = median(tl.lum), d_med = median(dl.lum);
    }
    const std::size_t n = static_cast<std::size_t>(fside) * static_cast<std::size_t>(fside);
    std::vector<float> m(n, 0.0f), tb[3], db[3];
    for (int k = 0; k < 3; ++k) tb[k].assign(n, 0.0f), db[k].assign(n, 0.0f);
    for (int j = 0; j < fside; ++j) {
      const int y = fy0 + j;
      if (y < 0 || y >= static_cast<int>(target.height)) continue;
      for (int i = 0; i < fside; ++i) {
        const int x = fx0 + i;
        if (x < 0 || x >= static_cast<int>(target.width)) continue;
        const float q = q_at(x, y);
        if (q < in || q >= out) continue;
        float dx = 0, dy = 0;
        to_donor(static_cast<float>(x), static_cast<float>(y), dx, dy);
        const px3 tp = at(target, x, y), dp = bilinear(donor, dx, dy);
        if (t_med > 0.0f && d_med > 0.0f) {
          const float lt = luma(tp), ld = luma(dp);
          if (lt < 0.6f * t_med || ld < 0.6f * d_med || lt > 2.0f * t_med || ld > 2.0f * d_med) continue;
        }
        const std::size_t o = static_cast<std::size_t>(j) * fside + i;
        m[o] = 1.0f;
        tb[0][o] = tp.r, tb[1][o] = tp.g, tb[2][o] = tp.b;
        db[0][o] = dp.r, db[1][o] = dp.g, db[2][o] = dp.b;
      }
    }
    // Eyes: the ring's own ratio, lightly smoothed (a tenth of the long half
    // axis), before the wide blur below takes over.
    std::vector<float> ms, tbs[3], dbs[3];
    if (!local) {
      ms = m;
      for (int k = 0; k < 3; ++k) tbs[k] = tb[k], dbs[k] = db[k];
      const int small = std::max(1, static_cast<int>(0.1f * std::max(a, b)));
      for (int pass = 0; pass < 2; ++pass) {
        box_blur(ms, fside, fside, small);
        for (int k = 0; k < 3; ++k) box_blur(tbs[k], fside, fside, small), box_blur(dbs[k], fside, fside, small);
      }
    }
    // Three box passes, near a Gaussian: a third of an eye distance for a
    // face; for eyes, about the long half axis, so the ring reaches the middle.
    const int box = local ? std::max(1, static_cast<int>(0.33f * std::min(a, b) * 0.58f))
                          : std::max(1, static_cast<int>(0.6f * std::max(a, b)));
    for (int pass = 0; pass < 3; ++pass) {
      box_blur(m, fside, fside, box);
      for (int k = 0; k < 3; ++k) box_blur(tb[k], fside, fside, box), box_blur(db[k], fside, fside, box);
    }
    ratio.assign(n * 3, 1.0f);
    // How evenly the donor's colour sits under this photo's (a face): the
    // spread of the red/green and blue/green ratios over the face's middle.
    double sum[2] = {0, 0}, sq[2] = {0, 0};
    int count = 0;
    for (int j = 0; j < fside; ++j) {
      for (int i = 0; i < fside; ++i) {
        const std::size_t o = static_cast<std::size_t>(j) * fside + i;
        float rk[3];
        if (m[o] <= 1e-4f) {
          for (int k = 0; k < 3; ++k) ratio[o * 3 + static_cast<std::size_t>(k)] = local ? 1.0f : gain[k];
          continue;
        }
        for (int k = 0; k < 3; ++k) {
          const float t = tb[k][o] / m[o], d = db[k][o] / m[o];
          rk[k] = std::clamp((t + 1e-4f) / (d + 1e-4f), 0.25f, 4.0f);
          ratio[o * 3 + static_cast<std::size_t>(k)] = rk[k];
        }
        if (local && q_at(fx0 + i, fy0 + j) < 0.85f) {
          const double cr = std::log(rk[0] / rk[1]), cb = std::log(rk[2] / rk[1]);
          sum[0] += cr, sum[1] += cb, sq[0] += cr * cr, sq[1] += cb * cb;
          ++count;
        }
      }
    }
    if (local && count > 16) {
      double spread = 0.0;
      for (int k = 0; k < 2; ++k) {
        const double mean = sum[k] / count;
        spread = std::max(spread, std::sqrt(std::max(0.0, sq[k] / count - mean * mean)));
      }
      if (spread > kLookPatchy) return false;
    }
    if (!local) {
      // A membrane: the ring's skin keeps its own ratio, every other pixel
      // inside the ring's edge relaxes to the mean of its neighbours, from the
      // wide blur's field. The eye then meets the skin round it exactly, all
      // the way round (the wide blur alone left the eyes a shade light where
      // the cheek below outweighed the lid above, 2026-11-07).
      std::vector<std::uint8_t> fixed_px(n, 0);
      for (int j = 0; j < fside; ++j) {
        for (int i = 0; i < fside; ++i) {
          const std::size_t o = static_cast<std::size_t>(j) * fside + i;
          const float q = q_at(fx0 + i, fy0 + j);
          if (q >= ring_out) {
            fixed_px[o] = 1;  // outside: the wide field, held
          } else if (q >= 1.0f && ms[o] > 0.5f) {
            fixed_px[o] = 1;
            for (int k = 0; k < 3; ++k) {
              ratio[o * 3 + static_cast<std::size_t>(k)] =
                  std::clamp((tbs[k][o] / ms[o] + 1e-4f) / (dbs[k][o] / ms[o] + 1e-4f), 0.25f, 4.0f);
            }
          }
        }
      }
      std::vector<float> next = ratio;
      for (int it = 0; it < 240; ++it) {
        for (int j = 1; j + 1 < fside; ++j) {
          for (int i = 1; i + 1 < fside; ++i) {
            const std::size_t o = static_cast<std::size_t>(j) * fside + i;
            if (fixed_px[o]) continue;
            for (std::size_t k = 0; k < 3; ++k) {
              next[o * 3 + k] = 0.25f * (ratio[(o - 1) * 3 + k] + ratio[(o + 1) * 3 + k] +
                                         ratio[(o - static_cast<std::size_t>(fside)) * 3 + k] +
                                         ratio[(o + static_cast<std::size_t>(fside)) * 3 + k]);
            }
          }
        }
        ratio.swap(next);
        for (std::size_t o = 0; o < n; ++o) {
          if (fixed_px[o]) {
            for (std::size_t k = 0; k < 3; ++k) next[o * 3 + k] = ratio[o * 3 + k];
          }
        }
      }
    }
    gain[0] = gain[1] = gain[2] = 1.0f;
    offset[0] = offset[1] = offset[2] = 0.0f;
  }
  for (int y = y0; y < y0 + side; ++y) {
    if (y < 0 || y >= static_cast<int>(target.height)) continue;
    for (int x = x0; x < x0 + side; ++x) {
      if (x < 0 || x >= static_cast<int>(target.width)) continue;
      const float q = q_at(x, y);
      if (q >= 1.0f) continue;
      float alpha = std::clamp((1.0f - q) / (local ? 0.35f : 0.3f), 0.0f, 1.0f);
      alpha = alpha * alpha * (3.0f - 2.0f * alpha);
      float dx = 0, dy = 0;
      to_donor(static_cast<float>(x), static_cast<float>(y), dx, dy);
      px3 dp = bilinear(donor, dx, dy);
      if (!ratio.empty()) {
        const float* rk = ratio.data() + (static_cast<std::size_t>(y - fy0) * fside + static_cast<std::size_t>(x - fx0)) * 3;
        dp.r *= rk[0], dp.g *= rk[1], dp.b *= rk[2];
      }
      if (soften > 0.0f) {
        px3 sum{0, 0, 0};
        for (int oy = -1; oy <= 1; ++oy) {
          for (int ox = -1; ox <= 1; ++ox) {
            const px3 nb = bilinear(donor, dx + ox * scale, dy + oy * scale);
            sum.r += nb.r, sum.g += nb.g, sum.b += nb.b;
          }
        }
        if (!ratio.empty()) {
          const float* rk =
              ratio.data() + (static_cast<std::size_t>(y - fy0) * fside + static_cast<std::size_t>(x - fx0)) * 3;
          sum.r *= rk[0], sum.g *= rk[1], sum.b *= rk[2];
        }
        dp.r += (sum.r / 9.0f - dp.r) * soften;
        dp.g += (sum.g / 9.0f - dp.g) * soften;
        dp.b += (sum.b / 9.0f - dp.b) * soften;
      }
      std::uint16_t* p =
          target.rgba.data() + (static_cast<std::size_t>(y) * target.width + static_cast<std::size_t>(x)) * 4;
      float src[3] = {dp.r * gain[0] + offset[0], dp.g * gain[1] + offset[1], dp.b * gain[2] + offset[2]};
      // A catchlight (well over the skin around it) keeps glint x its excess.
      const float l = 0.2126f * src[0] + 0.7152f * src[1] + 0.0722f * src[2];
      const float knee = 1.5f * skin_mean;
      if (skin_mean > 0.0f && l > knee && glint != 1.0f) {
        const float to = knee + (l - knee) * glint;
        for (float& c : src) c *= to / l;
      }
      if (grain > 0.0f) {
        // Deterministic per pixel: the same edit gives the same file.
        std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        const float u = (static_cast<float>(h & 0xffffu) / 65535.0f - 0.5f) * 2.0f;
        for (float& c : src) c += u * grain;
      }
      for (int k = 0; k < 3; ++k) {
        const float o = image::half_to_float(p[k]);
        p[k] = image::float_to_half(std::max(0.0f, o + (src[k] - o) * alpha));
      }
    }
  }
  return true;
}
}  // namespace

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
  // The ellipse's half axes: wide and tall enough to take a shut eye's whole
  // lash line and lids (0.34 x 0.21 left the ends of a tilted shut eye's
  // lashes under the new one, 2026-11-07).
  const float a = 0.44f * t.d, b = 0.27f * t.d;
  const float cosr = tvx / t.d, sinr = tvy / t.d;   // along the eye line
  for (const auto& [cx, cy] : {std::pair{t.rx, t.ry}, std::pair{t.lx, t.ly}}) {
    (void)paste_region(target, donor, to_donor, scale, cx, cy, a, b, cosr, sinr);
  }
  return {};
}

namespace {
// Nose and mouth corners in the face's own frame (see pose_distance).
std::array<float, 6> face_frame(const face_landmarks& lm, float w, float h) noexcept {
  const float rx = lm[0] * w, ry = lm[1] * h, lx = lm[2] * w, ly = lm[3] * h;
  const float cx = (rx + lx) * 0.5f, cy = (ry + ly) * 0.5f;
  const float dx = lx - rx, dy = ly - ry;
  const float d = std::sqrt(dx * dx + dy * dy);
  std::array<float, 6> out{};
  if (d <= 0.0f) return out;
  const float c = dx / d, s = dy / d;  // rotate the eye line onto x
  for (int i = 0; i < 3; ++i) {
    const float px = lm[4 + 2 * i] * w - cx, py = lm[5 + 2 * i] * h - cy;
    out[2 * i] = (c * px + s * py) / d;
    out[2 * i + 1] = (-s * px + c * py) / d;
  }
  return out;
}
}  // namespace

float pose_distance(const face_landmarks& a, float aw, float ah, const face_landmarks& b, float bw,
                    float bh) noexcept {
  const auto fa = face_frame(a, aw, ah), fb = face_frame(b, bw, bh);
  float sum = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float dx = fa[2 * i] - fb[2 * i], dy = fa[2 * i + 1] - fb[2 * i + 1];
    sum += std::sqrt(dx * dx + dy * dy);
  }
  return sum / 3.0f;
}

expected transfer_face(image::linear_image& target, const face_landmarks& target_lm,
                       const image::linear_image& donor, const face_landmarks& donor_lm) {
  if (!target.valid() || !donor.valid()) return err(status::invalid_arg);
  const float tw = static_cast<float>(target.width), th = static_cast<float>(target.height);
  const float dw = static_cast<float>(donor.width), dh = static_cast<float>(donor.height);
  if (pose_distance(target_lm, tw, th, donor_lm, dw, dh) > kPoseMatch) return err(status::invalid_arg);
  // The five points in pixels, and the similarity target -> donor that fits
  // them best (least squares: donor = md + [p -q; q p] (target - mt)).
  float tx[5], ty[5], sx[5], sy[5], mtx = 0, mty = 0, msx = 0, msy = 0;
  for (int i = 0; i < 5; ++i) {
    tx[i] = target_lm[2 * i] * tw, ty[i] = target_lm[2 * i + 1] * th;
    sx[i] = donor_lm[2 * i] * dw, sy[i] = donor_lm[2 * i + 1] * dh;
    mtx += tx[i] / 5, mty += ty[i] / 5, msx += sx[i] / 5, msy += sy[i] / 5;
  }
  float num_p = 0, num_q = 0, den = 0;
  for (int i = 0; i < 5; ++i) {
    const float ax = tx[i] - mtx, ay = ty[i] - mty, bx = sx[i] - msx, by = sy[i] - msy;
    num_p += ax * bx + ay * by;
    num_q += ax * by - ay * bx;
    den += ax * ax + ay * ay;
  }
  if (den <= 0.0f) return err(status::invalid_arg);
  const float pp = num_p / den, qq = num_q / den, scale = std::sqrt(pp * pp + qq * qq);
  auto to_donor = [&](float x, float y, float& dx, float& dy) {
    const float ox = x - mtx, oy = y - mty;
    dx = msx + pp * ox - qq * oy;
    dy = msy + qq * ox + pp * oy;
  };
  const float vx = tx[1] - tx[0], vy = ty[1] - ty[0], d = std::sqrt(vx * vx + vy * vy);
  if (d < 24.0f) return err(status::invalid_arg);
  const float cosr = vx / d, sinr = vy / d;
  // The face's middle: the nose, nudged toward the eyes; half width
  // kFaceHalfWidth eye distances, half height kFaceHalfHeight (brows to chin).
  const float ex = (tx[0] + tx[1]) * 0.5f, ey = (ty[0] + ty[1]) * 0.5f;
  const float cx = tx[2] + (ex - tx[2]) * 0.1f, cy = ty[2] + (ey - ty[2]) * 0.1f;
  if (!paste_region(target, donor, to_donor, scale, cx, cy, kFaceHalfWidth * d, kFaceHalfHeight * d, cosr, sinr,
                    true)) {
    return err(status::invalid_arg);  // its colour differs from this photo's in patches
  }
  return {};
}

}  // namespace mv::edit
