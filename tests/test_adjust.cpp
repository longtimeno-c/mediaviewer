// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 verify, the shared half (plan/10): colour adjusts in the linear FP16
// working space (D6), the export bake, the histogram / clipping reduction,
// the shader kernel's single source, and the edit session's slider ops.
//
// What the hosts add on top — a slider drag being shader-only within one
// refresh on a 45 MP RAW, the HLSL and MSL twins on real GPUs, the pane —
// is the manual part of the verify (README, "PR 11").
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
  // plan/07 "preview is the export", and D6: the working space round-trips
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
    const edit::adjust_uniforms u = edit::uniforms_of(c);
    const mv::gfx::kernel::float3 v = mv::gfx::kernel::mv_adjust(
        {r, g, b}, {u.a0[0], u.a0[1], u.a0[2], u.a0[3]}, {u.a1[0], u.a1[1], u.a1[2], u.a1[3]});
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
  CHECK(text.find("float3 mv_adjust(float3 c, float4 a0, float4 a1)") != std::string_view::npos);
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
