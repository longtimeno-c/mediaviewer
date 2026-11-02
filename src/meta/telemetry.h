// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #606: a clip's GPS route from its telemetry (docs/design/06 "Telemetry
// routes"). GoPro cameras and DJI drones write no ISO 6709 tag: the position
// is in a telemetry stream, sampled many times a second.
//
//   GoPro   the "GoPro MET" data track (codec tag `gpmd`), one GPMF payload per
//           packet (about one a second): DEVC > STRM > GPS5 (HERO5-HERO10) or
//           GPS9 (HERO11 on), with SCAL, GPSF (fix) and GPSP (precision).
//   DJI     the text subtitle track drones write beside the picture (the same
//           text as their .SRT sidecar): "[latitude: 38.79] [longitude: -9.39]"
//           (or DJI's own "longtitude"), or "GPS(lon, lat, alt)" on older models.
//           The newer `djmd` protobuf track is not read (no published schema).
//
// Portable and pure, like meta/parse.h: bytes or text in, points out. No FFmpeg,
// no platform header (D9), so `tools/fuzz/fuzz_meta_gpmf.cpp` links it alone.
// Every read is bounds-checked against the span it was given; nesting, KLV
// count and points are capped. Never throws; nothing touches the network
// (rule 6). clip.cpp feeds it packets through libavformat.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "meta/meta.h"

namespace mv::meta {

// How a route is kept: points closer than `min_step_m` to the last one kept
// are dropped (a camera standing still writes the same fix 18 times a second),
// and once `max_points` are held every other point goes and the step doubles,
// so a long drive keeps its shape at a coarser spacing. Altitude rides along
// where the source has it.
struct route_limits {
  std::size_t max_points = 20000;
  double min_step_m = 1.0;
};

class route_builder {
 public:
  explicit route_builder(route_limits limits = {}) noexcept;
  // Adds a fix; refuses one that is not finite, out of range or exactly 0, 0
  // (what a receiver without lock reports). Never throws: an allocation
  // failure keeps what is held.
  void add(const geo_point& p) noexcept;
  [[nodiscard]] const std::vector<geo_point>& points() const noexcept { return points_; }
  // The route, ending at the last fix accepted even when thinning or the
  // minimum step skipped it (it replaces the final point when full).
  [[nodiscard]] std::vector<geo_point> take() noexcept;
  // Fixes offered and accepted (before thinning); true once thinning dropped any.
  [[nodiscard]] std::size_t accepted() const noexcept { return accepted_; }
  [[nodiscard]] bool thinned() const noexcept { return thinned_; }

 private:
  route_limits limits_;
  double step_m_;
  std::vector<geo_point> points_;
  std::size_t accepted_ = 0;
  bool thinned_ = false;
  geo_point last_{};  // the latest fix accepted (valid), kept or not
};

// Ground distance in metres between two points (haversine, mean Earth radius).
[[nodiscard]] double distance_m(const geo_point& a, const geo_point& b) noexcept;

// Caps on one GPMF payload: GoPro's own nesting is DEVC > STRM > value, so 4
// levels is generous; a real payload holds a few hundred KLVs.
inline constexpr std::size_t kGpmfMaxDepth = 4;
inline constexpr std::size_t kGpmfMaxKlvs = 1u << 16;
inline constexpr std::size_t kGpmfMaxPayload = 8u * 1024 * 1024;

// One GoPro `gpmd` packet (a GPMF payload). Every GPS5 / GPS9 sample with a 2D
// or 3D fix and a dilution of precision of 5 or better goes to `out`, in order.
// A stream with no GPSF is taken at its word (older firmware); one with no SCAL
// cannot be read and is skipped. Returns the fixes found (before `out` thins).
std::size_t read_gpmf(std::span<const std::uint8_t> payload, route_builder& out) noexcept;

// One DJI subtitle cue's text, as cue_text decodes it (one line or several).
// The fix it carries, or nullopt when the text is not DJI telemetry or has no
// fix (0, 0 before lock). Bounded: only the first 4 KB is looked at.
[[nodiscard]] std::optional<geo_point> parse_dji_text(std::string_view text) noexcept;

// The route as the search side keeps it: at most `max_points`, the first and
// the last fix always kept, the rest evenly spaced along the list.
[[nodiscard]] std::vector<geo_point> sample_route(std::span<const geo_point> route, std::size_t max_points);

}  // namespace mv::meta
