// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/eta.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "addons/enhance/picture_job.h"

namespace mv::enhance {
namespace {

std::uint32_t io_uint(const model_spec* spec, const char* key, std::uint32_t fallback) {
  if (!spec) return fallback;
  const json::value* v = spec->io.find(key);
  if (!v || v->k != json::kind::number) return fallback;
  const double d = v->is_integer ? static_cast<double>(v->i) : v->d;
  return d >= 1 && d <= 1 << 16 ? static_cast<std::uint32_t>(d) : fallback;
}

}  // namespace

double seconds_per_tile(model_kind k) noexcept {
  switch (k) {
    case model_kind::still: return 5.4;     // Real-HAT-GAN, a 192-px tile
    case model_kind::natural: return 10.0;  // RealWebPhoto (DAT-2), 256
    case model_kind::denoise: return 4.0;   // SCUNet, 512
    case model_kind::deblur:
    case model_kind::motion: return 1.25;   // NAFNet, 512
    default: return -1.0;
  }
}

double seconds_per_step(const std::string& quality) noexcept {
  if (quality == "best") return 5.4;  // not measured: High's per-step time times the area
  if (quality == "high") return 2.15;
  return 0.8;
}

double seconds_per_face(const std::string& model) noexcept { return model == "gfpgan" ? 1.0 : 1.6; }

double thread_factor(int threads) noexcept {
  if (threads <= 0) return 1.0;
  return std::clamp(8.0 / static_cast<double>(threads), 0.5, 4.0);
}

double estimate_seconds(const eta_in& in) {
  const double factor = thread_factor(in.threads);
  if (in.kind == model_kind::restyle) {
    // The steps, and a few seconds to read, encode, decode and carry back.
    return std::max(1, in.steps) * seconds_per_step(in.quality) * factor + 6.0;
  }
  const double per_tile = seconds_per_tile(in.kind);
  if (per_tile < 0 || in.photo.width == 0 || in.photo.height == 0) return -1.0;
  const std::uint32_t tile = io_uint(in.spec, "tile", 512);
  std::uint32_t overlap = io_uint(in.spec, "overlap", tile / 4);
  if (overlap >= tile) overlap = tile / 4;
  const std::uint32_t scale = io_uint(in.spec, "scale", 1);
  std::uint32_t w = in.photo.width, h = in.photo.height;
  // The upscalers run on the photo shrunk to its detail (doc 29 "Shrunk to the
  // detail"): 1/d² of the pixels.
  if (scale > 1 && in.photo.detail >= 1.25) {
    const shrink_plan p = plan_shrink(w, h, scale, std::max<std::uint32_t>(1, in.out_scale), in.photo.detail);
    if (p.in_w > 0 && p.in_h > 0) {
      w = p.in_w;
      h = p.in_h;
    }
  }
  const auto tiles = static_cast<double>(restore_tiles(w, h, tile, overlap));
  // Reading the photo and writing the result: about a second and a half for
  // 12 MP each way, by the photo's own size.
  const double io = 1.0 + 1.5 * static_cast<double>(in.photo.width) * in.photo.height / 12e6 *
                              std::max<std::uint32_t>(1, in.out_scale);
  return tiles * per_tile * factor + io;
}

std::string eta_words(double s) {
  if (!(s >= 0)) return {};
  if (s < 10) return "A few seconds";
  char buf[48];
  if (s < 55) {
    std::snprintf(buf, sizeof buf, "About %d seconds", static_cast<int>(std::lround(s / 5.0) * 5));
    return buf;
  }
  const auto minutes = static_cast<long long>(std::lround(s / 60.0));
  if (minutes < 60) {
    std::snprintf(buf, sizeof buf, "About %lld min", std::max(1ll, minutes));
    return buf;
  }
  const long long h = minutes / 60, m = (minutes % 60) / 5 * 5;
  if (m == 0) std::snprintf(buf, sizeof buf, "About %lld h", h);
  else std::snprintf(buf, sizeof buf, "About %lld h %lld min", h, m);
  return buf;
}

}  // namespace mv::enhance
