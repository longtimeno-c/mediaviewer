// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 verify, the shared half (docs/design/10): colour adjusts in the linear FP16
// working space (D6), the export bake, the histogram / clipping reduction,
// the shader kernel's single source, and the edit session's slider ops.
//
// What the hosts add on top — a slider drag being shader-only within one
// refresh on a 45 MP RAW, the HLSL and MSL twins on real GPUs, the pane —
// is the manual part of the verify (README, "PR 11").
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "codec/decode.h"
#include "edit/adjust.h"
#include "gfx/adjust_kernel.h"
#include "edit/bake.h"
#include "edit/edit_stack.h"
#include "edit/encode.h"
#include "edit/export.h"
#include "edit/histogram.h"
#include "image/colour.h"
#include "image/half.h"
#include "image/linear.h"
#include "shell/adjust_pane.h"
#include "shell/edit_session.h"

namespace {

namespace edit = mv::edit;
namespace image = mv::image;
using edit::adjust_param;
using Catch::Approx;

double srgb_to_linear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double linear_to_srgb(double c) {
  c = std::clamp(c, 0.0, 1.0);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

// The specification of the chain, in double precision and written the long
// way round (not through the kernel), so a slip in the kernel's tokens shows.
void reference(const double in[3], const edit::colour& c, double out[3]) {
  const auto wb = edit::white_balance_gains(c.get(adjust_param::temperature),
                                            c.get(adjust_param::tint));
  const double gain = std::exp2(c.get(adjust_param::exposure));
  const double slope = std::exp2(c.get(adjust_param::contrast) / 100.0 * 0.585);
  const double sat = 1.0 + c.get(adjust_param::saturation) / 100.0;
  double v[3];
  for (int i = 0; i < 3; ++i) {
    v[i] = std::max(0.0, in[i] * wb[static_cast<std::size_t>(i)] * gain);
    v[i] = 0.18 * std::pow(v[i] / 0.18, slope);
  }
  const double y = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
  for (int i = 0; i < 3; ++i) out[i] = std::max(0.0, y + (v[i] - y) * sat);
}

mv::codec::raster gradient(std::uint32_t w, std::uint32_t h) {
  mv::codec::raster r;
  r.width = w;
  r.height = h;
  r.format = mv::codec::format_family::jpeg;
  r.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint8_t* p = r.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
      p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
      p[2] = static_cast<std::uint8_t>((x * 7 + y * 13) & 0xFF);
      p[3] = 255;
    }
  }
  return r;
}

edit::colour colour_of(std::initializer_list<std::pair<adjust_param, float>> values) {
  edit::colour c;
  for (const auto& [p, v] : values) c.v[static_cast<std::size_t>(p)] = v;
  return c;
}

}  // namespace

// ---- FP16 -----------------------------------------------------------------

TEST_CASE("half floats round-trip every finite value and round to nearest even", "[adjust][half]") {
  for (std::uint32_t h = 0; h < 0x10000; ++h) {
    const auto bits = static_cast<std::uint16_t>(h);
    const float f = image::half_to_float(bits);
    if (std::isnan(f)) {
      CHECK(std::isnan(image::half_to_float(image::float_to_half(f))));
      continue;
    }
    REQUIRE(image::float_to_half(f) == bits);
  }
  CHECK(image::float_to_half(1.0f) == 0x3C00);
  CHECK(image::float_to_half(0.5f) == 0x3800);
  CHECK(image::float_to_half(65504.0f) == 0x7BFF);
  CHECK(image::float_to_half(1e9f) == 0x7C00);
  CHECK(image::float_to_half(-0.0f) == 0x8000);
  CHECK(image::float_to_half(1e-9f) == 0x0000);
  // 1 + 2^-11 is exactly halfway between 1 and the next half: ties to even.
  CHECK(image::float_to_half(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);
  CHECK(image::float_to_half(1.0f + 3 * std::ldexp(1.0f, -11)) == 0x3C02);
  // Smallest subnormal.
  CHECK(image::half_to_float(0x0001) == std::ldexp(1.0f, -24));
}

// ---- the working image ------------------------------------------------------

TEST_CASE("with every slider at zero the working image bakes to the viewer's pixels",
          "[adjust][working]") {
  // docs/design/07 "preview is the export", and D6: the working space round-trips
  // the 8-bit display image exactly, so opening the pane changes nothing.
  const auto src = gradient(64, 48);
  auto working = image::linear_from_srgb8(src.rgba, src.width, src.height, src.format);
  REQUIRE(working);
  const edit::placement p = edit::place(edit::geometry{}, edit::size2{64, 48});
  auto baked = edit::bake(*working, p, edit::uniforms_of(edit::colour{}));
  REQUIRE(baked);
  REQUIRE(baked->rgba == src.rgba);
  CHECK(baked->tagged_srgb);
  CHECK(baked->icc.empty());
}

TEST_CASE("downsample averages in linear light and keeps the aspect", "[adjust][working]") {
  // A 1-pixel black / white checkerboard halves to 0.5 linear (sRGB 188),
  // not to sRGB 128 — averaging the codes would be the classic mistake.
  mv::codec::raster r;
  r.width = 64;
  r.height = 32;
  r.rgba.resize(64 * 32 * 4);
  for (std::uint32_t i = 0; i < 64 * 32; ++i) {
    const bool on = ((i % 64) + (i / 64)) % 2 == 0;
    std::memset(r.rgba.data() + i * 4, on ? 255 : 0, 3);
    r.rgba[i * 4 + 3] = 255;
  }
  auto working = image::linear_from_srgb8(r.rgba, 64, 32, r.format);
  REQUIRE(working);
  auto small = image::downsample(*working, 16);
  REQUIRE(small);
  CHECK(small->width == 16);
  CHECK(small->height == 8);
  for (std::size_t i = 0; i < small->rgba.size(); i += 4) {
    REQUIRE(image::half_to_float(small->rgba[i]) == Approx(0.5).margin(1e-3));
    REQUIRE(image::half_to_float(small->rgba[i + 3]) == Approx(1.0).margin(1e-3));
  }
  // Already small enough: a copy.
  auto same = image::downsample(*small, 4096);
  REQUIRE(same);
  CHECK(same->rgba == small->rgba);
}

TEST_CASE("decode_linear of a JPEG equals its display image, linearised", "[adjust][working]") {
  const auto src = gradient(40, 30);
  auto jpeg = edit::encode(src, edit::encode_options{}, {});
  REQUIRE(jpeg);
  auto working = image::decode_linear(*jpeg);
  REQUIRE(working);
  auto decoded = mv::codec::decode(*jpeg);
  REQUIRE(decoded);
  auto shown = image::to_display(std::move(decoded).value());
  REQUIRE(shown);
  REQUIRE(working->width == shown->width);
  CHECK_FALSE(working->from_raw);
  for (std::size_t i = 0; i < shown->rgba.size(); ++i) {
    const double want = (i % 4 == 3) ? shown->rgba[i] / 255.0 : srgb_to_linear(shown->rgba[i] / 255.0);
    REQUIRE(image::half_to_float(working->rgba[i]) == Approx(want).margin(1e-3));
  }
}

// ---- the kernel -------------------------------------------------------------

TEST_CASE("the kernel matches the written-out chain within 8-bit rounding", "[adjust][kernel]") {
  const edit::colour cases[] = {
      colour_of({}),
      colour_of({{adjust_param::exposure, 1.0f}}),
      colour_of({{adjust_param::exposure, -2.5f}}),
      colour_of({{adjust_param::contrast, 60.0f}}),
      colour_of({{adjust_param::contrast, -100.0f}}),
      colour_of({{adjust_param::saturation, -100.0f}}),
      colour_of({{adjust_param::saturation, 80.0f}}),
      colour_of({{adjust_param::temperature, 70.0f}, {adjust_param::tint, -40.0f}}),
      colour_of({{adjust_param::exposure, 0.7f},
                 {adjust_param::contrast, 25.0f},
                 {adjust_param::saturation, 30.0f},
                 {adjust_param::temperature, -55.0f},
                 {adjust_param::tint, 20.0f}}),
  };
  for (const edit::colour& c : cases) {
    const edit::adjust_uniforms u = edit::uniforms_of(c);
    for (int r = 0; r < 256; r += 15) {
      for (int g = 0; g < 256; g += 17) {
        for (int b = 0; b < 256; b += 51) {
          const double in[3] = {srgb_to_linear(r / 255.0), srgb_to_linear(g / 255.0),
                                srgb_to_linear(b / 255.0)};
          double want[3];
          reference(in, c, want);
          const float lin[3] = {static_cast<float>(in[0]), static_cast<float>(in[1]),
                                static_cast<float>(in[2])};
          std::uint8_t got[3];
          edit::bake_pixel(lin, u, got);
          for (int i = 0; i < 3; ++i) {
            const double code = linear_to_srgb(want[i]) * 255.0;
            REQUIRE(std::abs(got[i] - code) <= 1.0);
          }
        }
      }
    }
  }
}

TEST_CASE("each slider does what its name says", "[adjust][kernel]") {
  auto out = [](const edit::colour& c, float r, float g, float b) {
    const mv::gfx::kernel::float3 v = mv::gfx::adjust({r, g, b}, {}, edit::uniforms_of(c));
    return std::array<float, 3>{v.x, v.y, v.z};
  };
  // Identity is the input to float rounding (the pivot divide and multiply).
  const auto id = out({}, 0.3f, 0.02f, 0.9f);
  CHECK(id[0] == Approx(0.3f).epsilon(1e-6));
  CHECK(id[1] == Approx(0.02f).epsilon(1e-6));
  CHECK(id[2] == Approx(0.9f).epsilon(1e-6));
  // +1 EV doubles linear light.
  const auto ev = out(colour_of({{adjust_param::exposure, 1.0f}}), 0.1f, 0.2f, 0.3f);
  CHECK(ev[0] == Approx(0.2f));
  CHECK(ev[2] == Approx(0.6f));
  // Contrast pivots on 18 % grey and spreads either side of it.
  const auto grey = out(colour_of({{adjust_param::contrast, 80.0f}}), 0.18f, 0.18f, 0.18f);
  CHECK(grey[0] == Approx(0.18f).epsilon(1e-5));
  const auto dark = out(colour_of({{adjust_param::contrast, 80.0f}}), 0.05f, 0.05f, 0.05f);
  const auto light = out(colour_of({{adjust_param::contrast, 80.0f}}), 0.6f, 0.6f, 0.6f);
  CHECK(dark[0] < 0.05f);
  CHECK(light[0] > 0.6f);
  // Saturation −100 is Rec.709 luma on every channel.
  const auto mono = out(colour_of({{adjust_param::saturation, -100.0f}}), 0.8f, 0.2f, 0.1f);
  CHECK(mono[0] == Approx(mono[1]));
  CHECK(mono[1] == Approx(mono[2]));
  CHECK(mono[0] == Approx(0.2126f * 0.8f + 0.7152f * 0.2f + 0.0722f * 0.1f));
  // Warmer: red up, blue down; a neutral grey keeps its luminance.
  const auto warm = out(colour_of({{adjust_param::temperature, 60.0f}}), 0.4f, 0.4f, 0.4f);
  CHECK(warm[0] > 0.4f);
  CHECK(warm[2] < 0.4f);
  CHECK(0.2126f * warm[0] + 0.7152f * warm[1] + 0.0722f * warm[2] == Approx(0.4f).epsilon(1e-4));
  const auto cool = out(colour_of({{adjust_param::temperature, -60.0f}}), 0.4f, 0.4f, 0.4f);
  CHECK(cool[0] < 0.4f);
  CHECK(cool[2] > 0.4f);
  // Tint +: magenta (green down relative to red and blue).
  const auto magenta = out(colour_of({{adjust_param::tint, 50.0f}}), 0.4f, 0.4f, 0.4f);
  CHECK(magenta[1] < magenta[0]);
  CHECK(magenta[1] < magenta[2]);
  // Negative input never leaves the kernel.
  const auto neg = out(colour_of({{adjust_param::saturation, 100.0f}}), 1.0f, 0.0f, 0.0f);
  CHECK(neg[1] >= 0.0f);
  CHECK(neg[2] >= 0.0f);
}

namespace {

// The full Adjust list's stages, written out in double precision from
// docs/design/07's description (not through the kernel's tokens).
void reference_full(const double in[3], const edit::colour& c, double px, double py,
                    double out[3]) {
  const auto get = [&](adjust_param p) { return static_cast<double>(c.get(p)); };
  const auto wb = edit::white_balance_gains(c.get(adjust_param::temperature),
                                            c.get(adjust_param::tint));
  const double gain = std::exp2(get(adjust_param::exposure));
  double v[3];
  for (int i = 0; i < 3; ++i) v[i] = std::max(0.0, in[i] * wb[static_cast<std::size_t>(i)] * gain);
  const double hl = get(adjust_param::highlights) / 100, sh = get(adjust_param::shadows) / 100,
               wh = get(adjust_param::whites) / 100 * edit::kWhitesReach,
               bl = get(adjust_param::blacks) / 100 * edit::kBlacksReach;
  if (hl != 0 || sh != 0 || wh != 0 || bl != 0) {
    const double l = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
    const double p = std::pow(l, 1 / 2.2), q = std::min(p, 1.0), r = 1 - q;
    const double t = p + hl * q * q * q * r + sh * q * r * r * r + wh * q * q * q + bl * r * r * r;
    const double lt = std::pow(std::max(t, 0.0), 2.2);
    const double k = lt / std::max(l, 1e-4);
    for (double& x : v) x = std::max(0.0, x * k + (lt - l * k));
  }
  const double slope = std::exp2(get(adjust_param::contrast) / 100.0 * 0.585);
  for (double& x : v) x = 0.18 * std::pow(x / 0.18, slope);
  const double y = 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
  double sat = 1.0 + get(adjust_param::saturation) / 100.0;
  if (get(adjust_param::vibrance) != 0) {
    const double hi = std::max({v[0], v[1], v[2]}), lo = std::min({v[0], v[1], v[2]});
    const double s = (hi - lo) / std::max(hi, 1e-4);
    sat *= std::max(0.0, 1 + get(adjust_param::vibrance) / 100 * (1 - s));
  }
  for (double& x : v) x = std::max(0.0, y + (x - y) * sat);
  if (get(adjust_param::mono) > 0) {
    const auto w = edit::mono_weights(c.get(adjust_param::mono_filter));
    double m = std::max(0.0, w[0] * v[0] + w[1] * v[1] + w[2] * v[2]);
    m = 0.18 * std::pow(m / 0.18, std::exp2(get(adjust_param::mono_tone) / 100 * 0.585));
    for (double& x : v) x += (m - x) * get(adjust_param::mono) / 100;
  }
  if (get(adjust_param::vignette) != 0) {
    const double inner = 0.35 + 0.3 * get(adjust_param::vignette_radius) / 100;
    const double outer = inner + 0.6 + 0.4 * get(adjust_param::vignette_softness) / 100;
    double f = std::clamp((std::hypot(px, py) - inner) / (outer - inner), 0.0, 1.0);
    f = f * f * (3 - 2 * f);
    for (double& x : v) x *= std::exp2(-get(adjust_param::vignette) / 100 * 1.5 * f);
  }
  for (int i = 0; i < 3; ++i) out[i] = v[i];
}

}  // namespace

TEST_CASE("the full Adjust list: the kernel matches the written-out chain", "[adjust][kernel]") {
  const edit::colour cases[] = {
      colour_of({{adjust_param::highlights, -100.0f}}),
      colour_of({{adjust_param::highlights, 70.0f}}),
      colour_of({{adjust_param::shadows, 100.0f}}),
      colour_of({{adjust_param::shadows, -60.0f}}),
      colour_of({{adjust_param::whites, 80.0f}}),
      colour_of({{adjust_param::blacks, -100.0f}}),
      colour_of({{adjust_param::blacks, 50.0f}}),
      colour_of({{adjust_param::vibrance, 100.0f}}),
      colour_of({{adjust_param::vibrance, -70.0f}, {adjust_param::saturation, 20.0f}}),
      colour_of({{adjust_param::mono, 100.0f}}),
      colour_of({{adjust_param::mono, 60.0f}, {adjust_param::mono_filter, 80.0f},
                 {adjust_param::mono_tone, 40.0f}}),
      colour_of({{adjust_param::mono, 100.0f}, {adjust_param::mono_filter, -100.0f}}),
      colour_of({{adjust_param::vignette, 100.0f}}),
      colour_of({{adjust_param::vignette, -50.0f}, {adjust_param::vignette_radius, 60.0f},
                 {adjust_param::vignette_softness, -80.0f}}),
      colour_of({{adjust_param::exposure, 0.4f}, {adjust_param::contrast, 30.0f},
                 {adjust_param::highlights, -50.0f}, {adjust_param::shadows, 40.0f},
                 {adjust_param::whites, 20.0f}, {adjust_param::blacks, -20.0f},
                 {adjust_param::vibrance, 30.0f}, {adjust_param::temperature, 25.0f},
                 {adjust_param::vignette, 40.0f}}),
  };
  const double places[][2] = {{0.0, 0.0}, {0.3, -0.2}, {0.55, 0.5}, {-0.8, 0.6}};
  for (const edit::colour& c : cases) {
    const edit::adjust_uniforms u = edit::uniforms_of(c);
    for (const auto& at : places) {
      const mv::gfx::kernel::float4 pos{static_cast<float>(at[0]), static_cast<float>(at[1]), 0, 0};
      for (int r = 0; r < 256; r += 15) {
        for (int g = 0; g < 256; g += 17) {
          for (int b = 0; b < 256; b += 51) {
            const double in[3] = {srgb_to_linear(r / 255.0), srgb_to_linear(g / 255.0),
                                  srgb_to_linear(b / 255.0)};
            double want[3];
            reference_full(in, c, at[0], at[1], want);
            const mv::gfx::kernel::float3 got = mv::gfx::adjust(
                {static_cast<float>(in[0]), static_cast<float>(in[1]), static_cast<float>(in[2])},
                pos, u);
            const float g3[3] = {got.x, got.y, got.z};
            for (int i = 0; i < 3; ++i) {
              INFO("rgb " << r << "," << g << "," << b << " channel " << i);
              REQUIRE(std::abs(linear_to_srgb(g3[i]) * 255.0 - linear_to_srgb(want[i]) * 255.0) <=
                      1.0);
            }
          }
        }
      }
    }
  }
}

TEST_CASE("the full Adjust list: each new slider does what its name says", "[adjust][kernel]") {
  namespace k = mv::gfx::kernel;
  auto grey = [](const edit::colour& c, float v, k::float4 pos = {}) {
    return mv::gfx::adjust({v, v, v}, pos, edit::uniforms_of(c)).y;
  };
  auto at = [](const edit::colour& c, float r, float g, float b, k::float4 pos = {}) {
    return mv::gfx::adjust({r, g, b}, pos, edit::uniforms_of(c));
  };
  // Highlights down pull bright greys down, barely move dark ones; shadows up
  // lift dark greys, barely move bright ones. Black stays black for both.
  const auto hl = colour_of({{adjust_param::highlights, -100.0f}});
  const auto sh = colour_of({{adjust_param::shadows, 100.0f}});
  CHECK(grey(hl, 0.5f) < 0.5f * 0.8f);
  CHECK(grey(hl, 0.01f) == Approx(0.01f).epsilon(0.05));
  CHECK(grey(sh, 0.02f) > 0.02f * 1.5f);
  CHECK(grey(sh, 0.9f) == Approx(0.9f).epsilon(0.03));
  CHECK(grey(hl, 0.0f) == 0.0f);
  CHECK(grey(sh, 0.0f) == 0.0f);
  // Whites move the top, blacks the bottom: +blacks lifts pure black.
  CHECK(grey(colour_of({{adjust_param::whites, 100.0f}}), 0.8f) > 0.8f);
  CHECK(grey(colour_of({{adjust_param::blacks, 100.0f}}), 0.0f) > 0.0f);
  CHECK(grey(colour_of({{adjust_param::blacks, -100.0f}}), 0.003f) < 0.003f);
  // Every tone slider at either end keeps the curve monotone.
  for (adjust_param p : {adjust_param::highlights, adjust_param::shadows, adjust_param::whites,
                         adjust_param::blacks}) {
    for (float v : {-100.0f, 100.0f}) {
      const auto c = colour_of({{p, v}});
      float last = -1.0f;
      for (int i = 0; i <= 200; ++i) {
        const float y = grey(c, static_cast<float>(i) / 200.0f * 1.2f);
        INFO(edit::range_of(p).name << " " << v << " at " << i);
        CHECK(y >= last - 1e-6f);
        last = y;
      }
    }
  }
  // Tone keeps hue: a colour's channel ratios survive highlights / shadows.
  const auto tinted = at(sh, 0.04f, 0.02f, 0.01f);
  CHECK(tinted.x / tinted.y == Approx(2.0f).epsilon(0.01));
  // Vibrance lifts a muted colour more than a strong one.
  const auto vib = colour_of({{adjust_param::vibrance, 100.0f}});
  const auto sat = colour_of({{adjust_param::saturation, 50.0f}});
  const auto muted_v = at(vib, 0.30f, 0.25f, 0.22f);
  const auto muted_s = at(sat, 0.30f, 0.25f, 0.22f);
  const auto strong_v = at(vib, 0.60f, 0.05f, 0.02f);
  CHECK(muted_v.x - muted_v.z > muted_s.x - muted_s.z);
  CHECK(strong_v.x == Approx(at({}, 0.60f, 0.05f, 0.02f).x).epsilon(0.05));
  // Black & white: equal channels; a red filter makes red brighter than a
  // blue filter does, and a neutral grey keeps its value under any filter.
  const auto bw = at(colour_of({{adjust_param::mono, 100.0f}}), 0.6f, 0.1f, 0.05f);
  CHECK(bw.x == Approx(bw.y));
  CHECK(bw.y == Approx(bw.z));
  const auto red = colour_of({{adjust_param::mono, 100.0f}, {adjust_param::mono_filter, 100.0f}});
  const auto blue = colour_of({{adjust_param::mono, 100.0f}, {adjust_param::mono_filter, -100.0f}});
  CHECK(at(red, 0.6f, 0.1f, 0.05f).x > at(blue, 0.6f, 0.1f, 0.05f).x);
  CHECK(grey(red, 0.3f) == Approx(0.3f).epsilon(1e-4));
  CHECK(grey(blue, 0.3f) == Approx(0.3f).epsilon(1e-4));
  // Half intensity is half way.
  const auto half = at(colour_of({{adjust_param::mono, 50.0f}}), 0.6f, 0.1f, 0.05f);
  CHECK(half.x == Approx((0.6f + bw.x) / 2).epsilon(1e-4));
  // Vignette: the centre untouched, the corner 1.5 stops down at +100, up at −100.
  const auto vig = colour_of({{adjust_param::vignette, 100.0f}});
  CHECK(grey(vig, 0.4f, {0, 0, 0, 0}) == 0.4f);
  const k::float4 corner{0.8f, 0.6f, 0, 0};  // distance 1
  CHECK(grey(vig, 0.4f, corner) == Approx(0.4f * std::exp2(-1.5f)).epsilon(1e-4));
  CHECK(grey(colour_of({{adjust_param::vignette, -100.0f}}), 0.2f, corner) > 0.2f);
  // A wider radius leaves more of the frame alone.
  const k::float4 mid{0.36f, 0.27f, 0, 0};  // distance 0.45
  const auto wide = colour_of({{adjust_param::vignette, 100.0f}, {adjust_param::vignette_radius, 100.0f}});
  CHECK(grey(wide, 0.4f, mid) > grey(vig, 0.4f, mid));
  // The map puts a w × h frame's corners at distance 1 and its centre at 0.
  const mv::gfx::vignette_map m(400.0f, 300.0f);
  const k::float4 c0 = m.at(0, 0);
  CHECK(std::hypot(c0.x, c0.y) == Approx(1.0f).epsilon(0.01));
  const k::float4 cc = m.at(199, 149);
  CHECK(std::hypot(cc.x, cc.y) < 0.01f);
}

TEST_CASE("sections: every parameter belongs to one, and a section resets alone", "[adjust][stack]") {
  for (int i = 0; i < edit::kAdjustParamCount; ++i) {
    const auto p = static_cast<adjust_param>(i);
    INFO(edit::range_of(p).name);
    CHECK(edit::section_of(p) != edit::adjust_section::count);
    CHECK(std::string_view(edit::range_of(p).name).size() > 0);
    CHECK(edit::clamp_param(p, 0.0f) == 0.0f);  // 0 is in every range: the identity
  }
  edit::colour c = colour_of({{adjust_param::exposure, 1.0f}, {adjust_param::shadows, 30.0f},
                              {adjust_param::vibrance, 20.0f}, {adjust_param::vignette, 50.0f}});
  const edit::colour no_light = edit::without_section(c, edit::adjust_section::light);
  CHECK(no_light.get(adjust_param::exposure) == 0.0f);
  CHECK(no_light.get(adjust_param::shadows) == 0.0f);
  CHECK(no_light.get(adjust_param::vibrance) == 20.0f);
  CHECK(no_light.get(adjust_param::vignette) == 50.0f);
  // Intensity cannot go negative.
  CHECK(edit::clamp_param(adjust_param::mono, -10.0f) == 0.0f);
}

TEST_CASE("export with a vignette: corners darker than the centre, in the bake", "[adjust][export]") {
  image::linear_image flat;
  flat.width = 64;
  flat.height = 48;
  flat.rgba.resize(64 * 48 * 4);
  for (std::size_t i = 0; i < flat.rgba.size(); i += 4) {
    flat.rgba[i] = flat.rgba[i + 1] = flat.rgba[i + 2] = image::float_to_half(0.2f);
    flat.rgba[i + 3] = image::float_to_half(1.0f);
  }
  const edit::placement p = edit::place(edit::geometry{}, {64, 48});
  auto out = edit::bake(flat, p, edit::uniforms_of(colour_of({{adjust_param::vignette, 80.0f}})));
  REQUIRE(out);
  auto px = [&](std::uint32_t x, std::uint32_t y) { return out->rgba[(y * 64 + x) * 4]; };
  CHECK(px(0, 0) < px(32, 24));
  CHECK(px(63, 47) < px(32, 24));
  CHECK(px(32, 24) == px(31, 23));  // the middle is untouched
}

TEST_CASE("white balance (0, 0) is exactly unity", "[adjust][kernel]") {
  const auto g = edit::white_balance_gains(0.0f, 0.0f);
  CHECK(g[0] == 1.0f);
  CHECK(g[1] == 1.0f);
  CHECK(g[2] == 1.0f);
  const edit::adjust_uniforms u = edit::uniforms_of(edit::colour{});
  CHECK(u == edit::adjust_uniforms{});
}

TEST_CASE("the shader text is the kernel's own tokens, in the HLSL / MSL / C++ subset",
          "[adjust][kernel]") {
  const std::string_view text = mv::gfx::kernel::kAdjustKernelText;
  CHECK(text.find("float3 mv_adjust(float3 c, float4 pos, float4 a0, float4 a1, float4 a2, "
                  "float4 a3, float4 a4, float4 a5)") != std::string_view::npos);
  CHECK(text.find("pow(") != std::string_view::npos);
  CHECK(text.find("dot(") != std::string_view::npos);
  // Nothing that exists in only one of the three languages.
  for (const char* banned : {"lerp", "mix(", "saturate", "clamp(", "half", "//", "/*", "#",
                             ".xyz", ".rgb", ".xy", "static", "constexpr", "inline",
                             "std::", "thread", "device", "constant"}) {
    INFO(banned);
    CHECK(text.find(banned) == std::string_view::npos);
  }
  // Every float3 constructor spells out three components (HLSL has no splat).
  for (std::size_t at = text.find("float3("); at != std::string_view::npos;
       at = text.find("float3(", at + 1)) {
    const std::size_t close = text.find(')', at);
    const std::string_view args = text.substr(at + 7, close - at - 7);
    CHECK(std::count(args.begin(), args.end(), ',') == 2);
  }
}

// ---- the stack ---------------------------------------------------------------

TEST_CASE("fold_colour: last set wins, reset clears, values clamp", "[adjust][stack]") {
  edit::edit_stack s;
  edit::op o{edit::op_kind::adjust};
  o.param = adjust_param::exposure;
  o.value = 1.0f;
  s.push(o);
  o.value = 9.0f;  // out of range
  s.push(o);
  o.param = adjust_param::tint;
  o.value = std::nanf("");
  s.push(o);
  s.push({edit::op_kind::rotate_cw});
  edit::colour c = edit::fold_colour(s.ops);
  CHECK(c.get(adjust_param::exposure) == 5.0f);
  CHECK(c.get(adjust_param::tint) == 0.0f);
  // Colour ops do not move the geometry.
  CHECK(edit::fold(s).orient == mv::codec::kRotateCw);
  edit::op reset{edit::op_kind::adjust};
  reset.param = adjust_param::count;
  s.push(reset);
  CHECK(edit::fold_colour(s.ops).identity());
  s.undo();
  CHECK(edit::fold_colour(s.ops).get(adjust_param::exposure) == 5.0f);
}

TEST_CASE("edit session: a slider drag is one undo step, reset is one more", "[adjust][session]") {
  mv::shell::edit_session es;
  es.set_item({"/p/a.jpg", 100, 5, 640, 480, true});
  CHECK(es.set_adjust(adjust_param::exposure, 0.1f) == mv::shell::edit_effect::redraw);
  CHECK(es.set_adjust(adjust_param::exposure, 0.4f) == mv::shell::edit_effect::redraw);
  CHECK(es.set_adjust(adjust_param::exposure, 0.8f) == mv::shell::edit_effect::redraw);
  CHECK(es.set_adjust(adjust_param::exposure, 0.8f) == mv::shell::edit_effect::none);
  REQUIRE(es.stack()->ops.size() == 1);
  CHECK(es.set_adjust(adjust_param::contrast, 20.0f) == mv::shell::edit_effect::redraw);
  REQUIRE(es.stack()->ops.size() == 2);
  CHECK(es.reset_adjust() == mv::shell::edit_effect::redraw);
  CHECK(es.colour().identity());
  CHECK(es.reset_adjust() == mv::shell::edit_effect::none);
  CHECK(es.run(mv::shell::command_id::undo_edit) == mv::shell::edit_effect::redraw);
  CHECK(es.colour().get(adjust_param::contrast) == 20.0f);
  CHECK(es.colour().get(adjust_param::exposure) == 0.8f);
  CHECK(es.run(mv::shell::command_id::undo_edit) == mv::shell::edit_effect::redraw);
  CHECK(es.run(mv::shell::command_id::undo_edit) == mv::shell::edit_effect::redraw);
  CHECK(es.colour().identity());
  CHECK(es.stack()->empty());
  // A drag back to where it started leaves nothing behind.
  CHECK(es.set_adjust(adjust_param::saturation, 30.0f) == mv::shell::edit_effect::redraw);
  CHECK(es.set_adjust(adjust_param::saturation, 0.0f) == mv::shell::edit_effect::redraw);
  CHECK(es.stack()->empty());
}

TEST_CASE("edit session: set_colour (auto tone) is one op and one undo", "[adjust][session]") {
  mv::shell::edit_session es;
  CHECK(es.set_colour(edit::colour{}) == mv::shell::edit_effect::refused);  // no item
  es.set_item({"/p/a.jpg", 100, 5, 640, 480, true});
  CHECK(es.set_adjust(adjust_param::saturation, 25.0f) == mv::shell::edit_effect::redraw);
  edit::colour c = es.colour();
  c.v[static_cast<std::size_t>(adjust_param::exposure)] = 0.6f;
  c.v[static_cast<std::size_t>(adjust_param::contrast)] = 18.0f;
  CHECK(es.set_colour(c) == mv::shell::edit_effect::redraw);
  CHECK(es.stack()->ops.size() == 2);
  CHECK(es.colour() == c);
  CHECK(es.set_colour(c) == mv::shell::edit_effect::none);  // nothing changes, nothing pushed
  CHECK(es.stack()->ops.size() == 2);
  // A slider after it is its own op; undoing that and then the auto tone
  // leaves the saturation the user set before.
  CHECK(es.set_adjust(adjust_param::exposure, 0.2f) == mv::shell::edit_effect::redraw);
  CHECK(es.stack()->ops.size() == 3);
  CHECK(es.run(mv::shell::command_id::undo_edit) == mv::shell::edit_effect::redraw);
  CHECK(es.colour() == c);
  CHECK(es.run(mv::shell::command_id::undo_edit) == mv::shell::edit_effect::redraw);
  CHECK(es.colour().get(adjust_param::exposure) == 0.0f);
  CHECK(es.colour().get(adjust_param::contrast) == 0.0f);
  CHECK(es.colour().get(adjust_param::saturation) == 25.0f);
}

TEST_CASE("edit session: colour makes `[` `]` a stack op, never a file rewrite", "[adjust][session]") {
  // Rule 5 via PR 10: only a stack of pure rotate / flip rewrites a JPEG.
  mv::shell::edit_session es;
  es.set_item({"/p/a.jpg", 100, 5, 640, 480, true});
  CHECK(es.run(mv::shell::command_id::rotate_cw) == mv::shell::edit_effect::write_rotation);
  CHECK(es.set_adjust(adjust_param::exposure, 1.0f) == mv::shell::edit_effect::redraw);
  // The debounce fires after the colour op: nothing is written.
  CHECK_FALSE(es.take_pending_write().has_value());
  CHECK(es.run(mv::shell::command_id::rotate_cw) == mv::shell::edit_effect::redraw);
  CHECK(es.export_geometry().orient == mv::codec::compose(mv::codec::kRotateCw, mv::codec::kRotateCw));
}

TEST_CASE("edit session: colour set while a rotation is being written survives the landing",
          "[adjust][session]") {
  mv::shell::edit_session es;
  es.set_item({"/p/a.jpg", 100, 5, 640, 480, true});
  CHECK(es.run(mv::shell::command_id::rotate_cw) == mv::shell::edit_effect::write_rotation);
  REQUIRE(es.take_pending_write().has_value());
  CHECK(es.set_adjust(adjust_param::saturation, -40.0f) == mv::shell::edit_effect::redraw);
  es.write_finished(true);
  es.set_item({"/p/a.jpg", 101, 6, 480, 640, true});  // the rewritten file
  CHECK(es.export_geometry().orient.identity());      // the turn is in the file now
  CHECK(es.colour().get(adjust_param::saturation) == -40.0f);
}

// ---- export -------------------------------------------------------------------

TEST_CASE("export with colour: a re-encoded, upright JPEG of the adjusted pixels",
          "[adjust][export]") {
  const auto src = gradient(96, 64);
  auto jpeg = edit::encode(src, edit::encode_options{edit::image_format::jpeg, 95}, {});
  REQUIRE(jpeg);
  edit::geometry g;
  g.orient = mv::codec::kRotateCw;
  const edit::colour c = colour_of({{adjust_param::exposure, 1.0f}});
  edit::export_options opt;
  opt.encode.format = edit::image_format::png;  // lossless output, so pixels compare exactly
  auto out = edit::export_image(*jpeg, g, c, opt);
  REQUIRE(out);
  CHECK_FALSE(out->lossless);
  CHECK(out->width == 64);
  CHECK(out->height == 96);

  // Export matches the preview: the preview is the kernel over the working
  // image through the same placement.
  auto working = image::decode_linear(*jpeg);
  REQUIRE(working);
  const edit::placement p = edit::place(g, edit::size2{working->width, working->height});
  auto preview = edit::bake(*working, p, edit::uniforms_of(c));
  REQUIRE(preview);
  auto written = mv::codec::decode(out->bytes);
  REQUIRE(written);
  REQUIRE(written->width == preview->width);
  int worst = 0;
  for (std::size_t i = 0; i < preview->rgba.size(); ++i) {
    worst = std::max(worst, std::abs(int(preview->rgba[i]) - int(written->rgba[i])));
  }
  CHECK(worst <= 1);

  // …and is brighter than the unadjusted export.
  auto plain = edit::export_image(*jpeg, g, edit::colour{}, opt);
  REQUIRE(plain);
  auto plain_px = mv::codec::decode(plain->bytes);
  REQUIRE(plain_px);
  long sum_adj = 0, sum_plain = 0;
  for (std::size_t i = 0; i < written->rgba.size(); i += 4) {
    sum_adj += written->rgba[i + 1];
    sum_plain += plain_px->rgba[i + 1];
  }
  CHECK(sum_adj > sum_plain);
}

TEST_CASE("export with identity colour is PR 10's export, lossless path included",
          "[adjust][export]") {
  const auto src = gradient(64, 48);  // MCU-aligned for 4:2:0
  auto jpeg = edit::encode(src, edit::encode_options{}, {});
  REQUIRE(jpeg);
  edit::geometry g;
  g.orient = mv::codec::kRotateCw;
  auto a = edit::export_image(*jpeg, g, edit::export_options{});
  auto b = edit::export_image(*jpeg, g, edit::colour{}, edit::export_options{});
  REQUIRE(a);
  REQUIRE(b);
  CHECK(a->lossless);
  CHECK(a->bytes == b->bytes);
  // The same colour exported twice is byte-identical (the cross-platform
  // half of the verify: nothing time- or thread-dependent reaches the bytes).
  const edit::colour c = colour_of({{adjust_param::contrast, 30.0f}, {adjust_param::tint, 10.0f}});
  auto x = edit::export_image(*jpeg, g, c, edit::export_options{});
  auto y = edit::export_image(*jpeg, g, c, edit::export_options{});
  REQUIRE(x);
  REQUIRE(y);
  CHECK(x->bytes == y->bytes);
}

// ---- histogram ------------------------------------------------------------------

TEST_CASE("histogram counts what is shown, and clipping matches the blinkies", "[adjust][histogram]") {
  const auto src = gradient(128, 128);
  auto working = image::linear_from_srgb8(src.rgba, 128, 128, src.format);
  REQUIRE(working);
  auto h = edit::compute_histogram(*working, edit::uniforms_of(edit::colour{}));
  REQUIRE(h);
  CHECK(h->samples == 128u * 128u);
  // Red is the x ramp: every code 0..255 appears, each about 128 / 2 times.
  std::uint64_t total = 0;
  for (std::uint32_t v : h->r) total += v;
  CHECK(total == h->samples);
  CHECK(h->r[0] > 0);
  CHECK(h->r[255] > 0);

  // +5 EV blows most of the frame out; the high-clip fraction follows.
  auto hot = edit::compute_histogram(
      *working, edit::uniforms_of(colour_of({{adjust_param::exposure, 5.0f}})));
  REQUIRE(hot);
  CHECK(hot->high_fraction() > h->high_fraction());
  CHECK(hot->high_fraction() > 0.9f);
  auto cold = edit::compute_histogram(
      *working, edit::uniforms_of(colour_of({{adjust_param::exposure, -5.0f}})));
  REQUIRE(cold);
  CHECK(cold->low_fraction() > h->low_fraction());

  const auto packed = edit::pack_histogram(*h);
  const auto peak = *std::max_element(packed.begin(), packed.end());
  CHECK(peak == 1000);
}

TEST_CASE("histogram samples a big image within its budget", "[adjust][histogram]") {
  image::linear_image big;
  big.width = 3000;
  big.height = 2000;
  big.rgba.assign(3000u * 2000u * 4u, image::float_to_half(0.5f));
  auto h = edit::compute_histogram(big, edit::uniforms_of(edit::colour{}));
  REQUIRE(h);
  CHECK(h->samples <= edit::kHistogramSampleBudget);
  CHECK(h->samples > edit::kHistogramSampleBudget / 2);
}

// ---- the pane ---------------------------------------------------------------------

TEST_CASE("adjust pane: sliders wait for the working image, stale results are dropped",
          "[adjust][pane]") {
  mv::shell::adjust_pane pane;
  // Nothing on the canvas: nothing to build.
  CHECK_FALSE(pane.toggle(false).has_value());
  CHECK(pane.visible());
  CHECK(pane.readiness() == mv::shell::adjust_readiness::none);

  const auto t1 = pane.set_item(11, false);
  REQUIRE(t1.has_value());
  CHECK(pane.readiness() == mv::shell::adjust_readiness::preparing);
  CHECK(pane.view({}).readiness == static_cast<int>(mv::shell::adjust_readiness::preparing));
  CHECK_FALSE(pane.take_histogram_request().has_value());  // not before the data exists

  // The user walks on before it lands: the old result is dropped.
  const auto t2 = pane.set_item(12, false);
  REQUIRE(t2.has_value());
  CHECK_FALSE(pane.working_landed(*t1, true, true));
  CHECK(pane.readiness() == mv::shell::adjust_readiness::preparing);
  CHECK(pane.working_landed(*t2, true, true));
  CHECK(pane.readiness() == mv::shell::adjust_readiness::ready);
  CHECK(pane.view({}).from_raw == 1);

  const auto hist = pane.take_histogram_request();
  REQUIRE(hist.has_value());
  CHECK_FALSE(pane.take_histogram_request().has_value());  // not stale any more
  edit::histogram h;
  h.samples = 10;
  h.clipped_high = 5;
  CHECK(pane.histogram_landed(*hist, h));
  CHECK(pane.view({}).clip_high == Approx(0.5f));
  pane.histogram_dirty();
  CHECK(pane.take_histogram_request().has_value());

  // Closed pane, no colour: a new item builds nothing. With colour it does:
  // the canvas needs the working texture to draw the adjusted pixels.
  CHECK_FALSE(pane.toggle(false).has_value());
  CHECK_FALSE(pane.set_item(13, false).has_value());
  CHECK(pane.set_item(14, true).has_value());
  // A failure is reported, not retried on every event.
  const auto t3 = pane.set_item(15, true);
  REQUIRE(t3.has_value());
  CHECK_FALSE(pane.working_landed(*t3, false, false));
  CHECK(pane.readiness() == mv::shell::adjust_readiness::failed);
  CHECK_FALSE(pane.colour_changed(true).has_value());
}

// Hidden (docs/plans/edit-adjust.md slice 1): the export bake's time at
// 24 MP, PR 11's sliders only, base and new alike.
TEST_CASE("bake: 24 MP with PR 11's sliders", "[.perf-bench][adjust-bench]") {
  image::linear_image src;
  src.width = 6000;
  src.height = 4000;
  src.rgba.resize(static_cast<std::size_t>(src.width) * src.height * 4);
  for (std::size_t i = 0; i < src.rgba.size(); i += 4) {
    const float v = static_cast<float>(i % 4093) / 4093.0f;
    src.rgba[i] = image::float_to_half(v);
    src.rgba[i + 1] = image::float_to_half(v * 0.8f);
    src.rgba[i + 2] = image::float_to_half(v * 0.6f);
    src.rgba[i + 3] = image::float_to_half(1.0f);
  }
  const edit::placement p = edit::place(edit::geometry{}, {src.width, src.height});
  const edit::adjust_uniforms u = edit::uniforms_of(colour_of(
      {{adjust_param::exposure, 0.5f}, {adjust_param::contrast, 20.0f},
       {adjust_param::saturation, 10.0f}, {adjust_param::temperature, 15.0f}}));
  double best = 1e9;
  for (int run = 0; run < 5; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    auto out = edit::bake(src, p, u);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(out);
    best = std::min(best, ms);
  }
  std::printf("{\"bench\":\"bake24mp_pr11\",\"best_ms\":%.1f}\n", best);
}

// New only: every stage on, the bake's worst case.
TEST_CASE("bake: 24 MP with every stage", "[.perf-bench][adjust-bench]") {
  image::linear_image src;
  src.width = 6000;
  src.height = 4000;
  src.rgba.resize(static_cast<std::size_t>(src.width) * src.height * 4);
  for (std::size_t i = 0; i < src.rgba.size(); i += 4) {
    const float v = static_cast<float>(i % 4093) / 4093.0f;
    src.rgba[i] = image::float_to_half(v);
    src.rgba[i + 1] = image::float_to_half(v * 0.8f);
    src.rgba[i + 2] = image::float_to_half(v * 0.6f);
    src.rgba[i + 3] = image::float_to_half(1.0f);
  }
  const edit::placement p = edit::place(edit::geometry{}, {src.width, src.height});
  const edit::adjust_uniforms u = edit::uniforms_of(colour_of(
      {{adjust_param::exposure, 0.5f}, {adjust_param::contrast, 20.0f},
       {adjust_param::highlights, -30.0f}, {adjust_param::shadows, 30.0f},
       {adjust_param::vibrance, 20.0f}, {adjust_param::mono, 100.0f},
       {adjust_param::mono_tone, 20.0f}, {adjust_param::vignette, 40.0f}}));
  double best = 1e9;
  for (int run = 0; run < 5; ++run) {
    const auto t0 = std::chrono::steady_clock::now();
    auto out = edit::bake(src, p, u);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE(out);
    best = std::min(best, ms);
  }
  std::printf("{\"bench\":\"bake24mp_all\",\"best_ms\":%.1f}\n", best);
}
