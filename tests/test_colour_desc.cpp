// SPDX-License-Identifier: GPL-2.0-or-later
// plan/05: "Do not assume BT.709 limited range." Every clip found on the dev
// box is untagged, so this resolution path is what stands between us and the
// classic washed-out-video bug.
#include <catch2/catch_test_macros.hpp>

#include "gfx/colour_desc.h"

using namespace mv::gfx;

TEST_CASE("an explicit tag always beats the resolution heuristic", "[colour]") {
  colour_desc d;
  d.matrix = colour_matrix::bt2020_ncl;
  d.primaries = colour_primaries::bt2020;
  d.transfer = colour_transfer::arib_std_b67;
  d.range = colour_range::full;

  // 1080p would otherwise resolve to BT.709 limited.
  const auto r = resolve_unspecified(d, 1920, 1080);
  REQUIRE(r.matrix == colour_matrix::bt2020_ncl);
  REQUIRE(r.primaries == colour_primaries::bt2020);
  REQUIRE(r.transfer == colour_transfer::arib_std_b67);
  REQUIRE(r.range == colour_range::full);
}

TEST_CASE("untagged HD resolves to BT.709", "[colour]") {
  const auto r = resolve_unspecified(colour_desc{}, 1920, 1080);
  REQUIRE(r.matrix == colour_matrix::bt709);
  REQUIRE(r.primaries == colour_primaries::bt709);
}

TEST_CASE("untagged SD resolves to BT.601, not BT.709", "[colour]") {
  const auto r = resolve_unspecified(colour_desc{}, 720, 576);
  REQUIRE(r.matrix == colour_matrix::bt601);
}

TEST_CASE("untagged UHD resolves to BT.2020", "[colour]") {
  const auto r = resolve_unspecified(colour_desc{}, 3840, 2160);
  REQUIRE(r.matrix == colour_matrix::bt2020_ncl);
  REQUIRE(r.primaries == colour_primaries::bt2020);
}

TEST_CASE("an untagged clip is never guessed into HDR", "[colour]") {
  // Guessing PQ or HLG on untagged 4K would tone-map ordinary video and darken
  // it — worse than the bug the heuristic exists to prevent.
  const auto r = resolve_unspecified(colour_desc{}, 3840, 2160);
  REQUIRE(r.transfer == colour_transfer::bt709);
  REQUIRE_FALSE(needs_tone_map(r));
}

TEST_CASE("HDR transfers are the ones flagged for tone mapping", "[colour]") {
  colour_desc pq;
  pq.transfer = colour_transfer::smpte2084;
  REQUIRE(needs_tone_map(pq));

  colour_desc hlg;
  hlg.transfer = colour_transfer::arib_std_b67;
  REQUIRE(needs_tone_map(hlg));

  colour_desc sdr;
  sdr.transfer = colour_transfer::bt709;
  REQUIRE_FALSE(needs_tone_map(sdr));
}

TEST_CASE("untagged YUV defaults to limited range", "[colour]") {
  const auto r = resolve_unspecified(colour_desc{}, 1920, 1080);
  REQUIRE(r.range == colour_range::limited);
}
