// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/picture_job.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

namespace mv::enhance {
namespace {

std::uint32_t core_of(std::uint32_t tile, std::uint32_t overlap) noexcept {
  return tile > 2 * overlap ? tile - 2 * overlap : 0;
}

}  // namespace

std::uint64_t tiles_for(std::uint32_t w, std::uint32_t h, std::uint32_t tile, std::uint32_t overlap) noexcept {
  const std::uint32_t core = core_of(tile, overlap);
  if (core == 0) return 0;
  return std::uint64_t{(w + core - 1) / core} * ((h + core - 1) / core);
}

expected enhance_picture(std::span<const std::uint8_t> rgb, std::uint32_t w, std::uint32_t h, tile_model& model,
                         const picture_options& o, std::vector<std::uint8_t>& out) {
  const std::uint32_t t = model.tile();
  const std::uint32_t ms = model.scale();
  const std::uint32_t s = o.out_scale;
  const std::uint32_t core = core_of(t, o.overlap);
  if (w == 0 || h == 0 || rgb.size() < std::size_t{w} * h * 3 || core == 0 || ms == 0 || s == 0 || s > ms ||
      ms % s != 0) {
    return err(status::invalid_arg);
  }
  const std::uint32_t k = ms / s;  // model pixels averaged into one output pixel, per side
  const std::uint32_t ow = w * s, oh = h * s;
  out.assign(std::size_t{ow} * oh * 3, 0);
  const float wet = static_cast<float>(std::clamp(o.strength, 0.0, 1.0));
  const float dry = 1.0f - wet;
  const std::size_t plane = std::size_t{t} * t;
  const std::uint32_t mt = t * ms;  // model output side
  const std::size_t mplane = std::size_t{mt} * mt;
  std::vector<float> in(plane * 3);
  std::vector<float> res;
  const auto px = [&](std::int64_t x, std::int64_t y, int c) {
    x = std::clamp<std::int64_t>(x, 0, w - 1);
    y = std::clamp<std::int64_t>(y, 0, h - 1);
    return rgb[(static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)];
  };
  // The original, enlarged bilinearly to the output grid (the dry side of the mix).
  const auto base = [&](std::uint32_t X, std::uint32_t Y, int c) -> float {
    if (s == 1) return px(X, Y, c) / 255.0f;
    const double fx = (X + 0.5) / s - 0.5, fy = (Y + 0.5) / s - 0.5;
    const auto x0 = static_cast<std::int64_t>(std::floor(fx)), y0 = static_cast<std::int64_t>(std::floor(fy));
    const double ax = fx - static_cast<double>(x0), ay = fy - static_cast<double>(y0);
    const double top = px(x0, y0, c) * (1 - ax) + px(x0 + 1, y0, c) * ax;
    const double bot = px(x0, y0 + 1, c) * (1 - ax) + px(x0 + 1, y0 + 1, c) * ax;
    return static_cast<float>((top * (1 - ay) + bot * ay) / 255.0);
  };
  const std::uint64_t total = tiles_for(w, h, t, o.overlap);
  std::uint64_t done = 0;
  for (std::uint32_t y0 = 0; y0 < h; y0 += core) {
    for (std::uint32_t x0 = 0; x0 < w; x0 += core) {
      if (o.cancel && o.cancel->load()) return err(status::cancelled);
      if (o.progress) o.progress(static_cast<double>(done++) / static_cast<double>(total));
      // The tile: this centre and `overlap` around it, edges replicated.
      const std::int64_t sx = static_cast<std::int64_t>(x0) - o.overlap;
      const std::int64_t sy = static_cast<std::int64_t>(y0) - o.overlap;
      for (std::uint32_t y = 0; y < t; ++y) {
        for (std::uint32_t x = 0; x < t; ++x) {
          for (int c = 0; c < 3; ++c) {
            in[static_cast<std::size_t>(c) * plane + std::size_t{y} * t + x] = px(sx + x, sy + y, c) / 255.0f;
          }
        }
      }
      MV_TRY_VOID(model.run(in, res));
      if (res.size() != mplane * 3) return err(status::corrupt);
      // Its centre, averaged k x k into the output.
      const std::uint32_t cw = std::min(core, w - x0), ch = std::min(core, h - y0);
      const std::uint32_t ox0 = x0 * s, oy0 = y0 * s;
      const std::uint32_t mx0 = o.overlap * ms, my0 = o.overlap * ms;
      const float inv = 1.0f / static_cast<float>(k * k);
      for (std::uint32_t Y = 0; Y < ch * s; ++Y) {
        for (std::uint32_t X = 0; X < cw * s; ++X) {
          for (int c = 0; c < 3; ++c) {
            const float* p = res.data() + static_cast<std::size_t>(c) * mplane;
            float sum = 0;
            for (std::uint32_t dy = 0; dy < k; ++dy) {
              const float* row = p + std::size_t{my0 + Y * k + dy} * mt + mx0 + X * k;
              for (std::uint32_t dx = 0; dx < k; ++dx) sum += row[dx];
            }
            float v = sum * inv;
            if (dry > 0) v = wet * v + dry * base(ox0 + X, oy0 + Y, c);
            out[(std::size_t{oy0 + Y} * ow + ox0 + X) * 3 + static_cast<std::size_t>(c)] =
                static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
          }
        }
      }
    }
  }
  return {};
}

// ---- restore_picture ----

namespace {

// One axis of a resample from `in` to `out` samples (out <= in): per output
// sample, the first input index and its normalised Lanczos-3 weights, the
// kernel widened by the ratio so a downscale never aliases. Built once a run.
struct resample_axis {
  std::uint32_t taps = 0;
  std::vector<std::int32_t> first;  // out
  std::vector<float> w;             // out * taps

  resample_axis(std::uint32_t in, std::uint32_t out) {
    const double k = static_cast<double>(in) / out;
    const double radius = 3.0 * k;
    taps = static_cast<std::uint32_t>(std::ceil(2 * radius)) + 1;
    first.resize(out);
    w.assign(std::size_t{out} * taps, 0.0f);
    const auto lanczos = [](double x) {
      x = std::fabs(x);
      if (x < 1e-9) return 1.0;
      if (x >= 3.0) return 0.0;
      const double px = std::numbers::pi * x;
      return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
    };
    for (std::uint32_t i = 0; i < out; ++i) {
      const double centre = (i + 0.5) * k - 0.5;
      const auto j0 = static_cast<std::int32_t>(std::floor(centre - radius)) + 1;
      first[i] = j0;
      double sum = 0;
      for (std::uint32_t t = 0; t < taps; ++t) sum += lanczos((j0 + static_cast<std::int32_t>(t) - centre) / k);
      for (std::uint32_t t = 0; t < taps; ++t) {
        w[std::size_t{i} * taps + t] =
            static_cast<float>(lanczos((j0 + static_cast<std::int32_t>(t) - centre) / k) / (sum != 0 ? sum : 1));
      }
    }
  }
};

// `src`: 3 planes of side `in`; `dst`: 3 planes of side `out` (axis built for in -> out).
void downscale_planes(const std::vector<float>& src, std::uint32_t in, std::uint32_t out, const resample_axis& ax,
                      std::vector<float>& tmp, std::vector<float>& dst) {
  tmp.assign(std::size_t{3} * in * out, 0.0f);  // rows of `in`, columns of `out`
  dst.assign(std::size_t{3} * out * out, 0.0f);
  const auto clampi = [in](std::int32_t j) {
    return static_cast<std::uint32_t>(std::clamp<std::int32_t>(j, 0, static_cast<std::int32_t>(in) - 1));
  };
  for (int c = 0; c < 3; ++c) {
    const float* p = src.data() + std::size_t(c) * in * in;
    float* q = tmp.data() + std::size_t(c) * in * out;
    for (std::uint32_t y = 0; y < in; ++y) {
      const float* row = p + std::size_t{y} * in;
      for (std::uint32_t x = 0; x < out; ++x) {
        const float* wt = ax.w.data() + std::size_t{x} * ax.taps;
        float acc = 0;
        for (std::uint32_t t = 0; t < ax.taps; ++t) acc += wt[t] * row[clampi(ax.first[x] + static_cast<std::int32_t>(t))];
        q[std::size_t{y} * out + x] = acc;
      }
    }
    float* d = dst.data() + std::size_t(c) * out * out;
    for (std::uint32_t y = 0; y < out; ++y) {
      const float* wt = ax.w.data() + std::size_t{y} * ax.taps;
      float* drow = d + std::size_t{y} * out;
      for (std::uint32_t t = 0; t < ax.taps; ++t) {
        const float* srow = q + std::size_t{clampi(ax.first[y] + static_cast<std::int32_t>(t))} * out;
        const float f = wt[t];
        for (std::uint32_t x = 0; x < out; ++x) drow[x] += f * srow[x];
      }
    }
  }
}

// A tile's result a picture can be made of: finite, never far off 0..1, and
// hardly ever past it. A sound restorer overshoots a little at edges: on 40
// 192-px tiles (20 COCO photos, centre and corner) Real-HAT-GAN reached
// -0.18..1.22 with at most 0.004 % of its samples past -0.15..1.15,
// NAFNet-REDS (when sound) 0.01 %, SCUNet and RealWebPhoto none. A
// half-broken tile stays in range but speckles: NAFNet-GoPro gave -0.34..1.38
// with 0.3 % of samples past 0.1 of the range and 1 % of pixels moved by more
// than 0.2 on a sharp crop; REDS 1.9 % past. Past 0.1 % is not a picture
// (GoPro: 17 of the 40 tiles; REDS: 5).
bool tile_sane(const std::vector<float>& v) {
  std::size_t off = 0;
  for (const float f : v) {
    if (!(f >= -1.0f && f <= 2.0f)) return false;  // also NaN
    if (f < -0.15f || f > 1.15f) ++off;
  }
  return off * 1000 <= v.size();
}

// 3 square planes of side `n` seen another way: 1 mirrored left to right,
// 2 upside down, 3 transposed. Each is its own inverse.
void orient_planes(const std::vector<float>& src, std::uint32_t n, int view, std::vector<float>& dst) {
  const std::size_t plane = std::size_t{n} * n;
  dst.resize(plane * 3);
  for (std::size_t c = 0; c < 3; ++c) {
    const float* p = src.data() + c * plane;
    float* q = dst.data() + c * plane;
    for (std::uint32_t y = 0; y < n; ++y) {
      for (std::uint32_t x = 0; x < n; ++x) {
        const std::uint32_t sx = view == 1 ? n - 1 - x : view == 3 ? y : x;
        const std::uint32_t sy = view == 2 ? n - 1 - y : view == 3 ? x : y;
        q[std::size_t{y} * n + x] = p[std::size_t{sy} * n + sx];
      }
    }
  }
}

// Reflection into [0, n) (a photo smaller than a tile).
std::int64_t reflect(std::int64_t i, std::int64_t n) {
  if (n == 1) return 0;
  const std::int64_t period = 2 * (n - 1);
  i %= period;
  if (i < 0) i += period;
  return i < n ? i : period - i;
}

}  // namespace

std::vector<std::uint32_t> tile_starts(std::uint32_t n, std::uint32_t tile, std::uint32_t overlap) {
  std::vector<std::uint32_t> out;
  if (n == 0 || tile == 0) return out;
  if (n <= tile) return {0};
  const std::uint32_t stride = tile > overlap ? tile - overlap : 1;
  std::uint32_t p = 0;
  for (;;) {
    out.push_back(p);
    if (p + tile >= n) break;
    p = std::min(p + stride, n - tile);
  }
  return out;
}

std::uint64_t restore_tiles(std::uint32_t w, std::uint32_t h, std::uint32_t tile, std::uint32_t overlap) {
  return std::uint64_t{tile_starts(w, tile, overlap).size()} * tile_starts(h, tile, overlap).size();
}

std::vector<float> tent_weights(std::uint32_t side, std::uint32_t ramp, bool first, bool last) {
  std::vector<float> w(side, 1.0f);
  if (ramp == 0) return w;
  for (std::uint32_t i = 0; i < side; ++i) {
    float v = 1.0f;
    if (!first) v = std::min(v, (static_cast<float>(i) + 0.5f) / static_cast<float>(ramp));
    if (!last) v = std::min(v, (static_cast<float>(side - i) - 0.5f) / static_cast<float>(ramp));
    w[i] = std::max(v, 1e-4f);
  }
  return w;
}

expected restore_picture(std::span<const std::uint8_t> rgb, std::uint32_t w, std::uint32_t h, tile_model& model,
                         const restore_options& o, std::vector<std::uint8_t>& out) {
  const std::uint32_t t = model.tile();
  const std::uint32_t ms = model.scale();
  const std::uint32_t s = o.out_scale;
  if (w == 0 || h == 0 || rgb.size() < std::size_t{w} * h * 3 || t == 0 || ms == 0 || s == 0 || s > ms ||
      ms % s != 0 || o.overlap >= t) {
    return err(status::invalid_arg);
  }
  const std::uint32_t mt = t * ms;  // the model's output side
  const std::uint32_t T = t * s;    // a tile's side in the output
  const std::uint32_t ramp = o.overlap * s;
  const std::uint64_t ow64 = std::uint64_t{w} * s, oh64 = std::uint64_t{h} * s;
  const auto ow = static_cast<std::uint32_t>(ow64), oh = static_cast<std::uint32_t>(oh64);
  out.assign(std::size_t{ow} * oh * 3, 0);
  const std::vector<std::uint32_t> xs = tile_starts(w, t, o.overlap);
  const std::vector<std::uint32_t> ys = tile_starts(h, t, o.overlap);
  const std::size_t total = xs.size() * ys.size();
  const float wet = static_cast<float>(std::clamp(o.strength, 0.0, 1.0));
  const float dry = 1.0f - wet;
  std::optional<resample_axis> axis;
  if (mt != T) axis.emplace(mt, T);
  // Weights per column / row of tiles (first and last are the photo's edges).
  std::vector<std::vector<float>> wx(xs.size()), wy(ys.size());
  for (std::size_t i = 0; i < xs.size(); ++i) wx[i] = tent_weights(T, ramp, i == 0, i + 1 == xs.size());
  for (std::size_t i = 0; i < ys.size(); ++i) wy[i] = tent_weights(T, ramp, i == 0, i + 1 == ys.size());

  const auto px = [&](std::int64_t x, std::int64_t y, int c) {
    x = reflect(x, w);
    y = reflect(y, h);
    return rgb[(static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)];
  };
  const auto clamped = [&](std::int64_t x, std::int64_t y, int c) {
    x = std::clamp<std::int64_t>(x, 0, w - 1);
    y = std::clamp<std::int64_t>(y, 0, h - 1);
    return rgb[(static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)];
  };
  // The photo enlarged bilinearly (the dry side of Strength).
  const auto base = [&](std::uint32_t X, std::uint32_t Y, int c) -> float {
    if (s == 1) return clamped(X, Y, c) / 255.0f;
    const double fx = (X + 0.5) / s - 0.5, fy = (Y + 0.5) / s - 0.5;
    const auto x0 = static_cast<std::int64_t>(std::floor(fx)), y0 = static_cast<std::int64_t>(std::floor(fy));
    const double ax = fx - static_cast<double>(x0), ay = fy - static_cast<double>(y0);
    const double top = clamped(x0, y0, c) * (1 - ax) + clamped(x0 + 1, y0, c) * ax;
    const double bot = clamped(x0, y0 + 1, c) * (1 - ax) + clamped(x0 + 1, y0 + 1, c) * ax;
    return static_cast<float>((top * (1 - ay) + bot * ay) / 255.0);
  };

  // The band: output rows [band_y0, band_y0 + T), float sums and weights.
  const std::uint32_t band_rows = std::min(T, oh);
  std::vector<float> sum(std::size_t{band_rows} * ow * 3, 0.0f), wsum(std::size_t{band_rows} * ow, 0.0f);
  std::uint32_t band_y0 = 0;
  // Writes rows [band_y0, upto) and moves the band down to start at `upto`.
  const auto flush = [&](std::uint32_t upto) {
    upto = std::min(upto, oh);
    if (upto <= band_y0) return;
    const std::uint32_t n = upto - band_y0;
    for (std::uint32_t r = 0; r < n; ++r) {
      const std::uint32_t Y = band_y0 + r;
      for (std::uint32_t X = 0; X < ow; ++X) {
        const std::size_t b = std::size_t{r} * ow + X;
        const float wt = wsum[b];
        for (int c = 0; c < 3; ++c) {
          float v = wt > 0 ? sum[b * 3 + static_cast<std::size_t>(c)] / wt : base(X, Y, c);
          if (dry > 0) v = wet * v + dry * base(X, Y, c);
          out[(std::size_t{Y} * ow + X) * 3 + static_cast<std::size_t>(c)] =
              static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        }
      }
    }
    const std::size_t keep = band_rows - std::min(n, band_rows);
    std::copy(sum.begin() + static_cast<std::ptrdiff_t>(std::size_t{n} * ow * 3), sum.end(), sum.begin());
    std::fill(sum.begin() + static_cast<std::ptrdiff_t>(keep * ow * 3), sum.end(), 0.0f);
    std::copy(wsum.begin() + static_cast<std::ptrdiff_t>(std::size_t{n} * ow), wsum.end(), wsum.begin());
    std::fill(wsum.begin() + static_cast<std::ptrdiff_t>(keep * ow), wsum.end(), 0.0f);
    band_y0 = upto;
  };

  const std::size_t plane = std::size_t{t} * t;
  std::vector<float> in(plane * 3), res, tmp, small, alt, back;
  std::size_t done = 0;
  if (o.unstable) *o.unstable = 0;
  for (std::size_t r = 0; r < ys.size(); ++r) {
    flush(ys[r] * s);  // nothing from here on reaches above this tile row
    for (std::size_t q = 0; q < xs.size(); ++q) {
      if (o.progress) o.progress(done, total);
      if (o.cancel && o.cancel->load()) return err(status::cancelled);
      const std::uint32_t x0 = xs[q], y0 = ys[r];
      for (int c = 0; c < 3; ++c) {
        float* dst = in.data() + static_cast<std::size_t>(c) * plane;
        for (std::uint32_t y = 0; y < t; ++y) {
          for (std::uint32_t x = 0; x < t; ++x) dst[std::size_t{y} * t + x] = px(x0 + x, y0 + y, c) / 255.0f;
        }
      }
      MV_TRY_VOID(model.run(in, res));
      if (res.size() != std::size_t{mt} * mt * 3) return err(status::corrupt);
      // A restorer that blows up on this tile (seen with NAFNet's GoPro and
      // REDS exports on some photos: values in the thousands) is asked again
      // with the tile mirrored or turned, and its answer turned back; if
      // every view blows up, the tile is the photo itself (cross-faded like
      // any other tile), never the model's wild pixels.
      bool sound = tile_sane(res);
      for (int view = 1; view <= 3 && !sound; ++view) {
        if (o.cancel && o.cancel->load()) return err(status::cancelled);
        orient_planes(in, t, view, alt);
        MV_TRY_VOID(model.run(alt, res));
        if (res.size() != std::size_t{mt} * mt * 3) return err(status::corrupt);
        orient_planes(res, mt, view, back);
        res.swap(back);
        sound = tile_sane(res);
      }
      const std::size_t tplane = std::size_t{T} * T;
      const std::uint32_t ox0 = x0 * s, oy0 = y0 * s;
      const std::uint32_t cw = std::min(T, ow - ox0), ch = std::min(T, oh - oy0);
      const std::vector<float>* tile_out = &res;
      if (!sound) {
        if (o.unstable) ++*o.unstable;
        small.assign(tplane * 3, 0.0f);
        for (int c = 0; c < 3; ++c) {
          for (std::uint32_t y = 0; y < ch; ++y) {
            for (std::uint32_t x = 0; x < cw; ++x) {
              small[static_cast<std::size_t>(c) * tplane + std::size_t{y} * T + x] = base(ox0 + x, oy0 + y, c);
            }
          }
        }
        tile_out = &small;
      } else if (axis) {
        downscale_planes(res, mt, T, *axis, tmp, small);
        tile_out = &small;
      }
      const std::vector<float>& fx = wx[q];
      const std::vector<float>& fy = wy[r];
      for (std::uint32_t y = 0; y < ch; ++y) {
        const std::uint32_t br = oy0 + y - band_y0;
        for (std::uint32_t x = 0; x < cw; ++x) {
          const float wt = fx[x] * fy[y];
          const std::size_t b = std::size_t{br} * ow + ox0 + x;
          for (int c = 0; c < 3; ++c) {
            sum[b * 3 + static_cast<std::size_t>(c)] +=
                wt * (*tile_out)[static_cast<std::size_t>(c) * tplane + std::size_t{y} * T + x];
          }
          wsum[b] += wt;
        }
      }
      ++done;
    }
  }
  flush(oh);
  if (o.progress) o.progress(total, total);
  return {};
}

}  // namespace mv::enhance
