// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#606): a clip's telemetry as libavformat hands it over,
// one packet at a time. The input is one GoPro gpmd packet (meta::read_gpmf)
// and, as text, one DJI subtitle cue (meta::parse_dji_text). Pure: it builds
// from src/meta/gpmf.cpp alone (mv_meta_parse). Any input may give no fix, but
// every fix is finite and on the globe, never exactly 0, 0, its altitude
// finite; the route never holds more than its cap; and sampling it keeps its
// ends.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "fuzz_meta.h"
#include "meta/telemetry.h"

namespace {

void check_point(const mv::meta::geo_point& p, const char* who) {
  if (!std::isfinite(p.lat) || !std::isfinite(p.lon) || std::fabs(p.lat) > 90.0 || std::fabs(p.lon) > 180.0) {
    mv::fuzz::meta_fail(who);
  }
  if (p.lat == 0.0 && p.lon == 0.0) mv::fuzz::meta_fail(who);
  if (p.altitude && !std::isfinite(*p.altitude)) mv::fuzz::meta_fail(who);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> bytes(data, size);
  // A small cap so the thinning path runs too.
  mv::meta::route_builder route(mv::meta::route_limits{.max_points = 64, .min_step_m = 0.5});
  const std::size_t found = mv::meta::read_gpmf(bytes, route);
  // A second packet onto the same route, as the clip reader feeds them.
  (void)mv::meta::read_gpmf(bytes.subspan(size / 2), route);
  if (route.points().size() > 64) mv::fuzz::meta_fail("read_gpmf: past the route cap");
  if (route.accepted() < found) mv::fuzz::meta_fail("read_gpmf: fixes lost before the route");
  for (const auto& p : route.points()) check_point(p, "read_gpmf: bad fix");
  const auto sampled = mv::meta::sample_route(route.points(), 8);
  if (sampled.size() > 8) mv::fuzz::meta_fail("sample_route: past its cap");
  if (!route.points().empty()) {
    if (sampled.empty() || sampled.front().lat != route.points().front().lat ||
        sampled.back().lat != route.points().back().lat) {
      mv::fuzz::meta_fail("sample_route: ends not kept");
    }
  }

  const std::string_view text(reinterpret_cast<const char*>(data), size);
  if (const auto p = mv::meta::parse_dji_text(text)) check_point(*p, "parse_dji_text: bad fix");
  return 0;
}
