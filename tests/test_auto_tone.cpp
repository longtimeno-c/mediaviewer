// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #289 — auto tone (edit/auto_tone.h): exposure and contrast from the
// histogram, the one-op colour set it lands as, and the pane's token.
//
// Synthetic histograms are neutral (R = G = B = luma) unless a case says
// otherwise; "where a code lands" runs a neutral grey through the real kernel
// (gfx/adjust_kernel.h) and the sRGB encode, as the canvas does.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>

#include "edit/adjust.h"
#include "edit/auto_adjust.h"
#include "edit/auto_tone.h"
#include "edit/edit_stack.h"
#include "edit/histogram.h"
#include "gfx/adjust_kernel.h"
#include "image/half.h"
#include "image/linear.h"
#include "shell/adjust_pane.h"

namespace {

namespace edit = mv::edit;
namespace image = mv::image;
using edit::adjust_param;

double srgb_to_linear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
double linear_to_srgb(double c) {
  c = std::clamp(c, 0.0, 1.0);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

// A neutral histogram: `weight(code)` samples at each code.
edit::histogram neutral(const std::function<std::uint32_t(int)>& weight) {
  edit::histogram h;
  for (int i = 0; i < 256; ++i) {
    const std::uint32_t n = weight(i);
    const auto k = static_cast<std::size_t>(i);
    h.r[k] = h.g[k] = h.b[k] = h.luma[k] = n;
    h.samples += n;
  }
  return h;
}

// Even counts across [lo, hi].
edit::histogram band(int lo, int hi, std::uint32_t n = 1000) {
  return neutral([=](int i) { return i >= lo && i <= hi ? n : 0u; });
}

// Where a displayed grey at `code` (with exposure and contrast at 0) lands
// under `c`, as an sRGB code.
double lands(double code, const edit::colour& c) {
  namespace k = mv::gfx::kernel;
  const float v = static_cast<float>(srgb_to_linear(code / 255.0));
  const k::float3 out = mv::gfx::adjust(k::float3{v, v, v}, k::float4{}, edit::uniforms_of(c));
  return linear_to_srgb(out.y) * 255.0;
}

float ev(const edit::colour& c) { return c.get(adjust_param::exposure); }
float con(const edit::colour& c) { return c.get(adjust_param::contrast); }

void check_within_caps(const edit::auto_tone_result& r) {
  CHECK(std::abs(ev(r.value)) <= edit::kAutoToneMaxExposure);
  CHECK(con(r.value) >= 0.0f);
  CHECK(con(r.value) <= edit::kAutoToneMaxContrast);
}

// A small scene in linear FP16: a smooth ramp between two sRGB codes with a
// colour cast, so the histogram's channels differ and the luma is not a
// copy of any of them.
image::linear_image scene(int lo, int hi, std::uint32_t w = 256, std::uint32_t h = 64) {
  image::linear_image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const double t = (static_cast<double>(x) + static_cast<double>(y) / h) / w;
      const double code = lo + (hi - lo) * t;
      const double v = srgb_to_linear(code / 255.0);
      std::uint16_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      p[0] = image::float_to_half(static_cast<float>(v * 0.92));
      p[1] = image::float_to_half(static_cast<float>(v));
      p[2] = image::float_to_half(static_cast<float>(v * 0.85));
      p[3] = image::float_to_half(1.0f);
    }
  }
  return img;
}

// A w × h picture whose pixel (x, y) is f(x, y) in linear RGB.
template <class F>
image::linear_image picture(std::uint32_t w, std::uint32_t h, F&& f) {
  image::linear_image img;
  img.width = w;
  img.height = h;
  img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::array<float, 3> c = f(x, y);
      std::uint16_t* p = img.rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
      for (int i = 0; i < 3; ++i) p[i] = image::float_to_half(c[static_cast<std::size_t>(i)]);
      p[3] = image::float_to_half(1.0f);
    }
  }
  return img;
}

}  // namespace

// ---- degenerate input ---------------------------------------------------------------

TEST_CASE("auto tone: an empty or flat histogram changes nothing", "[auto_tone]") {
  edit::colour current;
  current.v[static_cast<std::size_t>(adjust_param::exposure)] = 0.7f;
  current.v[static_cast<std::size_t>(adjust_param::saturation)] = 25.0f;

  const auto empty = edit::auto_tone(edit::histogram{}, current);
  CHECK(empty.flat);
  CHECK_FALSE(empty.changed);
  CHECK(empty.value == current);

  const auto one_bin = edit::auto_tone(band(120, 120), current);
  CHECK(one_bin.flat);
  CHECK_FALSE(one_bin.changed);

  // A grey card with a little noise: narrower than kAutoToneMinSpan.
  const auto card = edit::auto_tone(band(117, 122), current);
  CHECK(card.flat);
  CHECK(card.value == current);

  // A black frame.
  const auto black = edit::auto_tone(band(0, 3), current);
  CHECK(black.flat);

  // Luma with no colour channels (not something compute_histogram makes).
  edit::histogram luma_only;
  luma_only.luma[100] = 50;
  luma_only.samples = 50;
  CHECK(edit::auto_tone(luma_only, current).flat);
}

TEST_CASE("auto tone: a photo that already spans black to white is left alone", "[auto_tone]") {
  // Even across the range, and a mid-heavy camera-like spread from 3 to 252.
  const auto uniform = edit::auto_tone(band(0, 255), edit::colour{});
  CHECK_FALSE(uniform.flat);
  CHECK_FALSE(uniform.changed);
  CHECK(uniform.value == edit::colour{});

  const auto camera = edit::auto_tone(
      neutral([](int i) -> std::uint32_t {
        if (i < 3 || i > 252) return 0;
        const double d = (i - 110) / 55.0;
        return static_cast<std::uint32_t>(300 + 4000 * std::exp(-d * d));
      }),
      edit::colour{});
  CHECK_FALSE(camera.changed);
  CHECK(camera.value == edit::colour{});
}

// ---- what it does -------------------------------------------------------------------

TEST_CASE("auto tone: an underexposed photo is brightened to the white target", "[auto_tone]") {
  // 1.65 EV under: inside the exposure cap.
  const auto r = edit::auto_tone(band(0, 150), edit::colour{});
  REQUIRE(r.changed);
  check_within_caps(r);
  CHECK(ev(r.value) > 1.4f);
  CHECK(ev(r.value) < 1.8f);
  // Black is already at 0: the stretch is (nearly) a pure gain.
  CHECK(con(r.value) <= 10.0f);
  CHECK(std::abs(lands(r.white, r.value) - edit::kAutoToneWhiteCode) < 1.5);
}

TEST_CASE("auto tone: very dark input is capped, not blown up", "[auto_tone]") {
  const auto r = edit::auto_tone(band(0, 30), edit::colour{});
  REQUIRE(r.changed);
  check_within_caps(r);
  CHECK(ev(r.value) == edit::kAutoToneMaxExposure);
  CHECK(lands(r.white, r.value) < edit::kAutoToneWhiteCode);  // short of white: the cap held
}

TEST_CASE("auto tone: a low-contrast (hazy) photo gets contrast, capped", "[auto_tone]") {
  // A veil over the frame: black at 60, white at 190. Levels would take them
  // to 8 and 250; a power about 18 % grey capped at +40 cannot do both, so
  // the black is held where it was and white gets as far as the cap allows.
  const auto r = edit::auto_tone(band(60, 190), edit::colour{});
  REQUIRE(r.changed);
  check_within_caps(r);
  CHECK(con(r.value) == edit::kAutoToneMaxContrast);
  CHECK(lands(r.black, r.value) <= r.black + 0.5);  // never lifted
  CHECK(lands(r.white, r.value) > r.white + 10.0);
  CHECK(lands(r.white, r.value) <= edit::kAutoToneWhiteCode + 0.5);
}

TEST_CASE("auto tone: a washed-out bright photo darkens its mid-tones, keeps its white",
          "[auto_tone]") {
  const auto r = edit::auto_tone(band(110, 255), edit::colour{});
  REQUIRE(r.changed);
  check_within_caps(r);
  CHECK(con(r.value) > 10.0f);
  CHECK(ev(r.value) < 0.0f);
  CHECK(lands(r.median, r.value) < r.median - 5.0);
  CHECK(lands(255.0, r.value) > 253.5);  // white is never lowered
}

TEST_CASE("auto tone: a clipped highlight is not pulled down to grey", "[auto_tone]") {
  // A sky blown in the camera JPEG (3 % at 255) over a normal frame.
  auto h = band(2, 200);
  h.r[255] = h.g[255] = h.b[255] = h.luma[255] = 6000;
  h.samples += 6000;
  const auto r = edit::auto_tone(h, edit::colour{});
  CHECK(r.white == 255.0f);
  CHECK(ev(r.value) <= 0.0f);
  CHECK(lands(255.0, r.value) > 253.5);
}

TEST_CASE("auto tone: white comes from the brightest channel", "[auto_tone]") {
  // Luma tops out at 180, but red reaches 240: stretching luma to 250 would
  // clip red. White is read from red.
  auto h = band(0, 180);
  h.r = {};
  for (int i = 0; i <= 240; ++i) h.r[static_cast<std::size_t>(i)] = 1000 * 181 / 241;
  const auto r = edit::auto_tone(h, edit::colour{});
  CHECK(r.white > 239.0f);
  const auto neutral_r = edit::auto_tone(band(0, 180), edit::colour{});
  CHECK(ev(r.value) < ev(neutral_r.value));
}

TEST_CASE("auto tone: caps hold across many shapes", "[auto_tone]") {
  std::uint32_t seed = 12345;
  auto next = [&seed] {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
  };
  for (int trial = 0; trial < 500; ++trial) {
    const int lo = static_cast<int>(next() % 256);
    const int hi = std::min(255, lo + static_cast<int>(next() % 256));
    const int peak = lo + static_cast<int>(next() % static_cast<std::uint32_t>(hi - lo + 1));
    const auto h = neutral([&](int i) -> std::uint32_t {
      if (i < lo || i > hi) return 0;
      return 1 + static_cast<std::uint32_t>(5000 / (1 + std::abs(i - peak)));
    });
    const auto r = edit::auto_tone(h, edit::colour{});
    check_within_caps(r);
    // Never darkens the white percentile, never lifts the black one.
    if (!r.flat) {
      CHECK(lands(r.white, r.value) >= std::min<double>(r.white, edit::kAutoToneWhiteCode) - 1.0);
      CHECK(lands(r.black, r.value) <= std::max<double>(r.black, edit::kAutoToneBlackCode) + 1.0);
    }
  }
}

// ---- absolute, idempotent, deterministic ----------------------------------------------

TEST_CASE("auto tone sets exposure and contrast absolutely and keeps the rest", "[auto_tone]") {
  const auto h = band(30, 170);
  edit::colour user;
  user.v[static_cast<std::size_t>(adjust_param::exposure)] = 1.3f;
  user.v[static_cast<std::size_t>(adjust_param::contrast)] = -60.0f;
  user.v[static_cast<std::size_t>(adjust_param::saturation)] = 20.0f;
  user.v[static_cast<std::size_t>(adjust_param::temperature)] = -15.0f;
  user.v[static_cast<std::size_t>(adjust_param::tint)] = 5.0f;
  const auto a = edit::auto_tone(h, user);
  const auto b = edit::auto_tone(h, edit::colour{});
  CHECK(ev(a.value) == ev(b.value));
  CHECK(con(a.value) == con(b.value));
  CHECK(a.value.get(adjust_param::saturation) == 20.0f);
  CHECK(a.value.get(adjust_param::temperature) == -15.0f);
  CHECK(a.value.get(adjust_param::tint) == 5.0f);

  const edit::colour basis = edit::auto_tone_basis(user);
  CHECK(ev(basis) == 0.0f);
  CHECK(con(basis) == 0.0f);
  CHECK(basis.get(adjust_param::saturation) == 20.0f);

  // The rest of the Light section is part of what Auto sets: back to 0.
  user.v[static_cast<std::size_t>(adjust_param::highlights)] = -40.0f;
  user.v[static_cast<std::size_t>(adjust_param::blacks)] = 25.0f;
  user.v[static_cast<std::size_t>(adjust_param::vignette)] = 30.0f;
  const auto c = edit::auto_tone(h, user);
  CHECK(c.value.get(adjust_param::highlights) == 0.0f);
  CHECK(c.value.get(adjust_param::blacks) == 0.0f);
  CHECK(c.value.get(adjust_param::vignette) == 30.0f);
  CHECK(ev(c.value) == ev(b.value));
}

TEST_CASE("auto tone is deterministic", "[auto_tone]") {
  const auto h = band(25, 205);
  const auto a = edit::auto_tone(h, edit::colour{});
  const auto b = edit::auto_tone(h, edit::colour{});
  CHECK(a.value == b.value);  // colour has padding now (curves, spots): compare, never memcmp
  CHECK(a.black == b.black);
  CHECK(a.median == b.median);
  CHECK(a.white == b.white);
}

TEST_CASE("auto tone on the working image: a second click changes nothing, and the result "
          "re-histograms as balanced",
          "[auto_tone]") {
  // Dark, with a real black: the fit is not capped, so the shown result
  // should itself read as balanced.
  const image::linear_image working = scene(2, 150);
  auto first = edit::compute_auto_tone(working, edit::colour{});
  REQUIRE(first);
  REQUIRE(first->changed);
  check_within_caps(*first);

  // Clicked again with the result applied: the same values (the basis
  // histogram does not depend on exposure or contrast).
  auto second = edit::compute_auto_tone(working, first->value);
  REQUIRE(second);
  CHECK_FALSE(second->changed);
  CHECK(second->value == first->value);

  // What the canvas now shows, read as if it were a fresh photo: auto tone
  // would barely move it.
  auto shown = edit::compute_histogram(working, edit::uniforms_of(first->value));
  REQUIRE(shown);
  const auto again = edit::auto_tone(*shown, edit::colour{});
  INFO("first " << ev(first->value) << " EV, " << con(first->value) << " contrast; shown reads "
                << again.black << " / " << again.median << " / " << again.white);
  CHECK(std::abs(ev(again.value)) <= 0.1f);
  CHECK(con(again.value) <= 5.0f);
  CHECK(std::abs(again.white - edit::kAutoToneWhiteCode) <= 2.0f);

  // Where the caps bind (a veiled frame), the shown result would ask for more
  // if read as a fresh photo, which is why auto tone reads the basis: clicking
  // again does not ratchet the contrast up.
  const image::linear_image hazy = scene(60, 190);
  auto h1 = edit::compute_auto_tone(hazy, edit::colour{});
  REQUIRE(h1);
  CHECK(con(h1->value) == edit::kAutoToneMaxContrast);
  auto h2 = edit::compute_auto_tone(hazy, h1->value);
  REQUIRE(h2);
  CHECK(h2->value == h1->value);

  // An invalid working image is an error, not a guess.
  CHECK_FALSE(edit::compute_auto_tone(image::linear_image{}, edit::colour{}));
}

// ---- landing as one op -----------------------------------------------------------------

TEST_CASE("adjust_all: every colour parameter in one op, one undo", "[auto_tone][stack]") {
  edit::edit_stack s;
  edit::op sat{edit::op_kind::adjust};
  sat.param = adjust_param::saturation;
  sat.value = 30.0f;
  s.push(sat);

  edit::op all{edit::op_kind::adjust_all};
  all.values.v[static_cast<std::size_t>(adjust_param::exposure)] = 1.25f;
  all.values.v[static_cast<std::size_t>(adjust_param::contrast)] = 400.0f;  // clamps to 100
  all.values.v[static_cast<std::size_t>(adjust_param::saturation)] = 30.0f;
  s.push(all);
  edit::colour c = edit::fold_colour(s.ops);
  CHECK(ev(c) == 1.25f);
  CHECK(con(c) == 100.0f);
  CHECK(c.get(adjust_param::saturation) == 30.0f);
  CHECK(edit::fold(s).identity());  // colour only: no geometry

  // A slider after it still wins for its own parameter.
  edit::op e{edit::op_kind::adjust};
  e.param = adjust_param::exposure;
  e.value = -0.5f;
  s.push(e);
  CHECK(ev(edit::fold_colour(s.ops)) == -0.5f);
  CHECK(con(edit::fold_colour(s.ops)) == 100.0f);

  CHECK(s.undo());
  CHECK(s.undo());  // the whole auto tone, in one step
  c = edit::fold_colour(s.ops);
  CHECK(ev(c) == 0.0f);
  CHECK(con(c) == 0.0f);
  CHECK(c.get(adjust_param::saturation) == 30.0f);
}

TEST_CASE("adjust pane: an auto-tone result lands only on the colour it was read from",
          "[auto_tone][pane]") {
  mv::shell::adjust_pane pane;
  CHECK_FALSE(pane.request_auto_tone().has_value());  // nothing on the canvas
  (void)pane.show(true, false);
  const auto build = pane.set_item(21, false);
  REQUIRE(build.has_value());
  CHECK_FALSE(pane.request_auto_tone().has_value());  // the working image is still building
  REQUIRE(pane.working_landed(*build, true, false));

  const auto t1 = pane.request_auto_tone();
  REQUIRE(t1.has_value());
  CHECK(pane.auto_tone_landed(*t1));
  CHECK_FALSE(pane.auto_tone_landed(*t1));  // once

  // A slider moved while the job ran: the result is stale.
  const auto t2 = pane.request_auto_tone();
  REQUIRE(t2.has_value());
  (void)pane.colour_changed(true);
  CHECK_FALSE(pane.auto_tone_landed(*t2));

  // A second click supersedes the first.
  const auto t3 = pane.request_auto_tone();
  const auto t4 = pane.request_auto_tone();
  REQUIRE(t3.has_value());
  REQUIRE(t4.has_value());
  CHECK_FALSE(pane.auto_tone_landed(*t3));
  CHECK(pane.auto_tone_landed(*t4));

  // The user walked on.
  const auto t5 = pane.request_auto_tone();
  REQUIRE(t5.has_value());
  (void)pane.set_item(22, false);
  CHECK_FALSE(pane.auto_tone_landed(*t5));

  CHECK(pane.auto_tone_landed(0) == false);
}

// ---- the other sections' Auto (edit/auto_adjust.h) -------------------------------------

TEST_CASE("auto colour: a neutral grey world is left alone, a cast is mostly taken out",
          "[auto_tone][auto_colour]") {
  CHECK(edit::grey_world({0.2, 0.2, 0.2}) == std::array<float, 2>{0.0f, 0.0f});
  CHECK(edit::grey_world({0.0, 0.2, 0.2}) == std::array<float, 2>{0.0f, 0.0f});  // degenerate

  // Lit by a warm light: the sliders go cooler, and the result is neutral to
  // within the damping (a sunset keeps some warmth).
  const std::array<double, 3> warm{0.30, 0.20, 0.12};
  const auto wb = edit::grey_world(warm);
  CHECK(wb[0] < -edit::kAutoColourMin);
  const auto full = edit::white_balance_gains(wb[0] / edit::kAutoColourDamping,
                                              wb[1] / edit::kAutoColourDamping);
  const double r = full[0] * warm[0] / (full[1] * warm[1]);
  const double b = full[2] * warm[2] / (full[1] * warm[1]);
  // The search's 1-unit grid plus the rounding after damping.
  CHECK(std::abs(std::log(r)) < 0.08);
  CHECK(std::abs(std::log(b)) < 0.08);
  // Strength scales the move; confidence fades with the neutrals' colour.
  CHECK(edit::grey_world(warm, 0.0f) == std::array<float, 2>{0.0f, 0.0f});
  CHECK(std::abs(edit::grey_world(warm, 0.5f)[0]) < std::abs(wb[0]));
  CHECK(edit::neutral_confidence(0.05f) == 1.0f);
  CHECK(edit::neutral_confidence(0.25f) == Catch::Approx(0.5f));
  CHECK(edit::neutral_confidence(0.6f) == 0.0f);
  // Never past the cap.
  const auto extreme = edit::grey_world({0.9, 0.05, 0.01});
  CHECK(std::abs(extreme[0]) <= edit::kAutoColourMax);
  CHECK(std::abs(extreme[1]) <= edit::kAutoColourMax);
}

TEST_CASE("auto colour on the working image: absolute, idempotent, keeps the rest",
          "[auto_tone][auto_colour]") {
  // A greenish-blue cast over a grey ramp, a clipped highlight strip on top
  // that must not vote.
  const image::linear_image img = picture(128, 64, [](std::uint32_t x, std::uint32_t y) {
    if (y < 4) return std::array<float, 3>{1.0f, 0.95f, 0.2f};
    const float v = 0.03f + 0.5f * static_cast<float>(x) / 127.0f;
    return std::array<float, 3>{v * 0.9f, v * 1.0f, v * 1.12f};
  });
  edit::colour user;
  user.v[static_cast<std::size_t>(adjust_param::saturation)] = 15.0f;
  user.v[static_cast<std::size_t>(adjust_param::temperature)] = -70.0f;
  auto first = edit::compute_auto(img, user, edit::adjust_section::colour);
  REQUIRE(first);
  CHECK(first->changed);
  CHECK(first->value.get(adjust_param::temperature) > 0.0f);  // warmer than as shot
  CHECK(first->value.get(adjust_param::saturation) == 15.0f);
  auto second = edit::compute_auto(img, first->value, edit::adjust_section::colour);
  REQUIRE(second);
  CHECK_FALSE(second->changed);

  // A frame mostly of blue sea under neutral light, with grey stone in it:
  // the stone sets the light, the sea is not a cast. And a frame of sea
  // alone, nothing grey in it, is left nearly as it is.
  const image::linear_image sea = picture(128, 64, [](std::uint32_t x, std::uint32_t y) {
    if (x < 24) {
      const float v = 0.05f + 0.3f * static_cast<float>(y) / 63.0f;
      return std::array<float, 3>{v, v, v};
    }
    const float v = 0.04f + 0.1f * static_cast<float>(x % 17) / 16.0f;
    return std::array<float, 3>{v * 0.3f, v * 0.7f, v * 1.4f};
  });
  auto calm = edit::compute_auto(sea, edit::colour{}, edit::adjust_section::colour);
  REQUIRE(calm);
  CHECK(std::abs(calm->value.get(adjust_param::temperature)) < 10.0f);
  CHECK(std::abs(calm->value.get(adjust_param::tint)) < 10.0f);
  const image::linear_image open_sea = picture(64, 64, [](std::uint32_t x, std::uint32_t) {
    const float v = 0.04f + 0.1f * static_cast<float>(x % 17) / 16.0f;
    return std::array<float, 3>{v * 0.3f, v * 0.7f, v * 1.4f};
  });
  auto open = edit::compute_auto(open_sea, edit::colour{}, edit::adjust_section::colour);
  REQUIRE(open);
  CHECK_FALSE(open->changed);
}

TEST_CASE("auto black & white: on at full intensity, the filter that spreads the grey",
          "[auto_tone][auto_mono]") {
  // Red flowers against green leaves of the same luma: a red filter splits
  // them, Rec.709 does not.
  const image::linear_image img = picture(64, 64, [](std::uint32_t x, std::uint32_t) {
    return x < 32 ? std::array<float, 3>{0.6f, 0.08f, 0.05f}
                  : std::array<float, 3>{0.05f, 0.21f, 0.05f};
  });
  auto r = edit::compute_auto(img, edit::colour{}, edit::adjust_section::mono);
  REQUIRE(r);
  CHECK(r->value.get(adjust_param::mono) == 100.0f);
  CHECK(r->value.get(adjust_param::mono_filter) != 0.0f);

  // A grey picture gains nothing from a filter: the tie goes to 0.
  const image::linear_image grey = picture(32, 32, [](std::uint32_t x, std::uint32_t) {
    const float v = 0.02f + 0.02f * static_cast<float>(x);
    return std::array<float, 3>{v, v, v};
  });
  auto g = edit::compute_auto(grey, edit::colour{}, edit::adjust_section::mono);
  REQUIRE(g);
  CHECK(g->value.get(adjust_param::mono_filter) == 0.0f);

  // An intensity the user chose is kept.
  edit::colour half;
  half.v[static_cast<std::size_t>(adjust_param::mono)] = 40.0f;
  auto k = edit::compute_auto(img, half, edit::adjust_section::mono);
  REQUIRE(k);
  CHECK(k->value.get(adjust_param::mono) == 40.0f);

  // The vignette has no Auto: nothing changes.
  auto v = edit::compute_auto(img, half, edit::adjust_section::vignette);
  REQUIRE(v);
  CHECK_FALSE(v->changed);
}

TEST_CASE("adjust pane: the view says which section's Auto is running", "[auto_tone][pane]") {
  mv::shell::adjust_pane pane;
  (void)pane.show(true, false);
  const auto build = pane.set_item(31, false);
  REQUIRE(build.has_value());
  REQUIRE(pane.working_landed(*build, true, false));
  CHECK(pane.view(edit::colour{}).auto_section == -1);
  const auto t = pane.request_auto_tone(edit::adjust_section::mono);
  REQUIRE(t.has_value());
  CHECK(pane.view(edit::colour{}).auto_section == static_cast<int>(edit::adjust_section::mono));
  CHECK(pane.auto_tone_landed(*t));
  CHECK(pane.view(edit::colour{}).auto_section == -1);
}

TEST_CASE("auto levels: each channel stretched to its ends, within reach", "[auto_tone][auto_levels]") {
  // A flat, faintly blue frame: every channel short of black and white.
  const image::linear_image img = picture(128, 32, [](std::uint32_t x, std::uint32_t) {
    const float t = static_cast<float>(x) / 127.0f;
    const float p = 0.15f + 0.65f * t;  // display-encoded 0.15 .. 0.8
    const float v = std::pow(p, 2.2f);
    return std::array<float, 3>{v * 0.9f, v, std::min(v * 1.1f, 0.95f)};
  });
  auto r = edit::compute_auto(img, edit::colour{}, edit::adjust_section::levels);
  REQUIRE(r);
  REQUIRE(r->changed);
  for (adjust_param p : {adjust_param::levels_red_black, adjust_param::levels_green_black,
                         adjust_param::levels_blue_black}) {
    CHECK(r->value.get(p) > 0.0f);
    CHECK(r->value.get(p) <= edit::kAutoLevelsReach);
  }
  CHECK(r->value.get(adjust_param::levels_green_white) < 0.0f);
  CHECK(r->value.get(adjust_param::levels_black) == 0.0f);  // the image's own row untouched
  // Shown, green now spans nearly black to nearly white.
  const auto t = edit::table_of(r->value);
  const auto u = edit::uniforms_of(r->value);
  const auto lo = mv::gfx::adjust({std::pow(0.16f, 2.2f), std::pow(0.16f, 2.2f), std::pow(0.16f, 2.2f)},
                                  {}, u, &t);
  CHECK(std::pow(lo.y, 1.0f / 2.2f) < 0.05f);
  // Idempotent, and the vignette and curves have no Auto.
  auto again = edit::compute_auto(img, r->value, edit::adjust_section::levels);
  REQUIRE(again);
  CHECK_FALSE(again->changed);
  CHECK_FALSE(edit::has_auto(edit::adjust_section::vignette));
  CHECK_FALSE(edit::has_auto(edit::adjust_section::curves));
  CHECK(edit::has_auto(edit::adjust_section::levels));
}

// ---- docs/plans/edit-refresh.md S2: the header's one-click ✦ Auto ------------------------

TEST_CASE("auto enhance: tone then white balance in one result", "[auto_tone][auto_enhance]") {
  // Dark and warm: a one-click Auto lifts it and cools it, as the two
  // sections' Autos one after the other would.
  const image::linear_image img = picture(128, 64, [](std::uint32_t x, std::uint32_t) {
    const float v = 0.01f + 0.12f * static_cast<float>(x) / 127.0f;
    return std::array<float, 3>{v * 1.12f, v * 1.0f, v * 0.86f};
  });
  edit::colour user;
  user.v[static_cast<std::size_t>(adjust_param::vignette)] = -30.0f;  // not an Auto's: kept
  auto both = edit::compute_auto_sections(img, user, edit::kAutoEnhanceSections);
  REQUIRE(both);
  CHECK(both->changed);
  CHECK(both->value.get(adjust_param::exposure) > 0.0f);
  CHECK(both->value.get(adjust_param::temperature) < 0.0f);
  CHECK(both->value.get(adjust_param::vignette) == -30.0f);
  CHECK(both->value.get(adjust_param::mono) == 0.0f);  // never turned grey
  // The same as light, then colour on light's result.
  auto light = edit::compute_auto(img, user, edit::adjust_section::light);
  REQUIRE(light);
  auto colour = edit::compute_auto(img, light->value, edit::adjust_section::colour);
  REQUIRE(colour);
  CHECK(colour->value == both->value);
  // Sections without an Auto are skipped; no section is no change.
  auto none = edit::compute_auto_sections(img, user, edit::section_bit(edit::adjust_section::vignette));
  REQUIRE(none);
  CHECK_FALSE(none->changed);
  CHECK_FALSE(edit::compute_auto_sections(image::linear_image{}, user, edit::kAutoEnhanceSections));
}

TEST_CASE("adjust pane: ✦ Auto is one token, no section spinner, and a second click takes it back",
          "[auto_tone][auto_enhance][pane]") {
  mv::shell::adjust_pane pane;
  CHECK_FALSE(pane.request_auto(edit::kAutoEnhanceSections).has_value());  // nothing on the canvas
  (void)pane.show(true, false);
  const auto build = pane.set_item(41, false);
  REQUIRE(build.has_value());
  // The host's waiting ✦ Auto keys on the item (auto_enhance_settle).
  CHECK(pane.item() == 41u);
  CHECK(pane.readiness() == mv::shell::adjust_readiness::preparing);
  REQUIRE(pane.working_landed(*build, true, false));

  CHECK_FALSE(pane.request_auto(0).has_value());
  const auto t = pane.request_auto(edit::kAutoEnhanceSections);
  REQUIRE(t.has_value());
  CHECK(pane.auto_busy());
  CHECK(pane.auto_sections() == edit::kAutoEnhanceSections);
  CHECK_FALSE(pane.auto_running().has_value());  // not one section's Auto
  CHECK(pane.view(edit::colour{}).auto_section == -1);
  CHECK(pane.auto_tone_landed(*t));
  CHECK_FALSE(pane.auto_busy());
  CHECK(pane.auto_sections() == 0u);

  // Landed as op 3 with this colour: on while the stack is exactly that.
  edit::colour landed;
  landed.v[static_cast<std::size_t>(adjust_param::exposure)] = 0.6f;
  CHECK_FALSE(pane.auto_enhance_on(3, landed));
  pane.auto_enhance_landed(3, landed);
  CHECK(pane.auto_enhance_on(3, landed));
  CHECK_FALSE(pane.auto_enhance_on(4, landed));     // another op since
  CHECK_FALSE(pane.auto_enhance_on(2, landed));     // undone
  edit::colour moved = landed;
  moved.v[static_cast<std::size_t>(adjust_param::contrast)] = 5.0f;
  CHECK_FALSE(pane.auto_enhance_on(3, moved));      // a slider moved it
  // A new item forgets it.
  (void)pane.set_item(42, false);
  CHECK_FALSE(pane.auto_enhance_on(3, landed));
  CHECK(pane.item() == 42u);
}
