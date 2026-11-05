// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/edit-adjust.md slice 5b: typed requests to the Adjust sliders.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "edit/prompt.h"
#include "shell/adjust_pane.h"

namespace {

namespace edit = mv::edit;
using edit::adjust_param;
using Catch::Approx;

float get(const edit::prompt_result& r, adjust_param p) { return r.value.get(p); }

}  // namespace

TEST_CASE("prompt: plain requests move the slider they name, the way they say", "[prompt]") {
  const edit::colour none;
  CHECK(get(edit::parse_prompt("brighter", none), adjust_param::exposure) == Approx(0.3f));
  CHECK(get(edit::parse_prompt("Make it darker please", none), adjust_param::exposure) == Approx(-0.3f));
  CHECK(get(edit::parse_prompt("too bright", none), adjust_param::exposure) == Approx(-0.3f));
  CHECK(get(edit::parse_prompt("too dark", none), adjust_param::exposure) == Approx(0.3f));
  CHECK(get(edit::parse_prompt("more contrast", none), adjust_param::contrast) == 20.0f);
  CHECK(get(edit::parse_prompt("less contrast", none), adjust_param::contrast) == -20.0f);
  CHECK(get(edit::parse_prompt("warmer", none), adjust_param::temperature) == 15.0f);
  CHECK(get(edit::parse_prompt("it's too blue", none), adjust_param::temperature) == 15.0f);
  CHECK(get(edit::parse_prompt("too warm", none), adjust_param::temperature) == -15.0f);
  CHECK(get(edit::parse_prompt("less noisy", none), adjust_param::noise) == 30.0f);
  CHECK(get(edit::parse_prompt("reduce the noise", none), adjust_param::noise) == 30.0f);
  CHECK(get(edit::parse_prompt("sharper", none), adjust_param::sharpen) == 30.0f);
  CHECK(get(edit::parse_prompt("black and white", none), adjust_param::mono) == 100.0f);
  CHECK(get(edit::parse_prompt("B&W", none), adjust_param::mono) == 100.0f);
}

TEST_CASE("prompt: regions take the adjective before them", "[prompt]") {
  const edit::colour none;
  const auto a = edit::parse_prompt("brighter shadows", none);
  CHECK(get(a, adjust_param::shadows) == 30.0f);
  CHECK(get(a, adjust_param::exposure) == 0.0f);
  CHECK(get(edit::parse_prompt("darker sky", none), adjust_param::highlights) == -30.0f);
  CHECK(get(edit::parse_prompt("recover the highlights", none), adjust_param::highlights) == -30.0f);
  CHECK(get(edit::parse_prompt("lift the shadows", none), adjust_param::shadows) == 30.0f);
  CHECK(get(edit::parse_prompt("deeper blacks", none), adjust_param::blacks) == -20.0f);
  CHECK(get(edit::parse_prompt("darker corners", none), adjust_param::vignette) == 30.0f);
}

TEST_CASE("prompt: a bit and much scale, moves add to where the slider is and clamp", "[prompt]") {
  edit::colour c;
  c.v[static_cast<std::size_t>(adjust_param::temperature)] = 90.0f;
  const auto r = edit::parse_prompt("a bit brighter and much warmer", c);
  CHECK(get(r, adjust_param::exposure) == Approx(0.2f));  // 0.15 onto the slider's 0.1 steps
  CHECK(get(r, adjust_param::temperature) == 100.0f);  // 90 + 30, clamped
  REQUIRE(r.applied.size() == 2);
  CHECK(r.applied[0] == "Exposure +0.2 EV");  // the value and the note agree
  CHECK(r.applied[1] == "Temperature +10");
  CHECK(edit::prompt_summary(r) == "Exposure +0.2 EV, Temperature +10");
}

TEST_CASE("prompt: looks move several sliders; autos and unknown words are reported", "[prompt]") {
  const edit::colour none;
  const auto film = edit::parse_prompt("like an old film", none);
  CHECK(get(film, adjust_param::blacks) == 20.0f);
  CHECK(get(film, adjust_param::saturation) == -15.0f);
  CHECK(film.unknown.empty());
  const auto fix = edit::parse_prompt("fix the white balance", none);
  CHECK_FALSE(fix.changed(none));
  REQUIRE(fix.autos.size() == 1);
  CHECK(fix.autos[0] == edit::adjust_section::colour);
  const auto whatever = edit::parse_prompt("remove the person behind her", none);
  CHECK_FALSE(whatever.changed(none));
  CHECK(edit::prompt_summary(whatever).find("not understood: person, behind, her") != std::string::npos);
  CHECK(edit::prompt_summary(edit::parse_prompt("", none)) == "Nothing to change");
  // "keep the sky" leaves the sky alone while the rest moves.
  const auto keep = edit::parse_prompt("brighter but keep the sky", none);
  CHECK(get(keep, adjust_param::exposure) == Approx(0.3f));
  CHECK(get(keep, adjust_param::highlights) == 0.0f);
}

TEST_CASE("prompt: the owner's sentence and other common phrasings", "[prompt]") {
  const edit::colour none;
  // 2026-11-05: typed into Describe a change, nothing moved (the field sent
  // "", not the parser: this sentence always parsed).
  const auto owner = edit::parse_prompt("warmer and brighter, reduce noise", none);
  CHECK(get(owner, adjust_param::temperature) == 15.0f);
  CHECK(get(owner, adjust_param::exposure) == Approx(0.3f));
  CHECK(get(owner, adjust_param::noise) == 30.0f);
  CHECK(owner.unknown.empty());
  CHECK(edit::prompt_summary(owner) == "Exposure +0.3 EV, Temperature +15, Noise reduction +30");

  CHECK(get(edit::parse_prompt("more noise reduction", none), adjust_param::noise) == 30.0f);
  CHECK(get(edit::parse_prompt("less noise reduction", edit::parse_prompt("denoise", none).value),
            adjust_param::noise) == 0.0f);
  CHECK(get(edit::parse_prompt("smooth", none), adjust_param::noise) == 30.0f);
  CHECK(get(edit::parse_prompt("add some sharpness", none), adjust_param::sharpen) == 30.0f);
  CHECK(get(edit::parse_prompt("desaturate", none), adjust_param::saturation) == -20.0f);
  CHECK(get(edit::parse_prompt("enhance the vibrancy", none), adjust_param::vibrance) == 25.0f);
  CHECK(get(edit::parse_prompt("boost the highlight", none), adjust_param::highlights) == 30.0f);
  // Apostrophes split words; their tails are not "not understood".
  const auto its = edit::parse_prompt("it's a bit dark", none);
  CHECK(get(its, adjust_param::exposure) == Approx(-0.2f));  // -0.15 onto the 0.1 steps
  CHECK(its.unknown.empty());
}

TEST_CASE("prompt: up and down", "[prompt]") {
  const edit::colour none;
  // Before a mover, "down" says which way it goes.
  CHECK(get(edit::parse_prompt("turn down the contrast", none), adjust_param::contrast) == -20.0f);
  CHECK(get(edit::parse_prompt("bring down the highlights", none), adjust_param::highlights) == -30.0f);
  CHECK(get(edit::parse_prompt("tone down the saturation", none), adjust_param::saturation) == -20.0f);
  CHECK(get(edit::parse_prompt("turn up the contrast", none), adjust_param::contrast) == 20.0f);
  // Right after a slider's name, it turns that slider round.
  CHECK(get(edit::parse_prompt("exposure down", none), adjust_param::exposure) == Approx(-0.3f));
  CHECK(get(edit::parse_prompt("noise down", none), adjust_param::noise) == 30.0f);
  const auto two = edit::parse_prompt("contrast down, warmth up", none);
  CHECK(get(two, adjust_param::contrast) == -20.0f);
  CHECK(get(two, adjust_param::temperature) == 15.0f);
  REQUIRE(two.applied.size() == 2);
  CHECK(two.applied[0] == "Contrast -20");
  // After an adjective it is only an echo, and turns nothing that follows.
  const auto cool = edit::parse_prompt("cool it down and add contrast", none);
  CHECK(get(cool, adjust_param::temperature) == -15.0f);
  CHECK(get(cool, adjust_param::contrast) == 20.0f);
  CHECK(get(edit::parse_prompt("warm it up", none), adjust_param::temperature) == 15.0f);
}

TEST_CASE("adjust pane: the request's note fits its field, cut between characters", "[prompt][pane]") {
  mv::shell::adjust_pane pane;
  pane.set_prompt_note("Exposure +0.3 EV");
  CHECK(std::string(pane.view(edit::colour{}).prompt_note) == "Exposure +0.3 EV");
  std::string long_note;
  while (long_note.size() < 200) long_note += "Température ";  // two-byte é
  pane.set_prompt_note(long_note);
  const auto v = pane.view(edit::colour{});
  const std::string shown(v.prompt_note);
  CHECK(shown.size() <= 127);
  CHECK(long_note.compare(0, shown.size(), shown) == 0);
  CHECK((static_cast<unsigned char>(long_note[shown.size()]) & 0xC0) != 0x80);  // not mid-character
  (void)pane.set_item(9, false);
  CHECK(std::string(pane.view(edit::colour{}).prompt_note).empty());
}
