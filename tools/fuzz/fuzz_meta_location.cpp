// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness (#352): meta::parse_location, what a person types into the
// Metadata pane's Location editor (both hosts; Windows through the C ABI). It
// is pure, so it builds from src/meta/location_edit.cpp alone. Any input may
// be refused, but an accepted one is a finite point in range with no "-0", a
// refusal carries a one-line UTF-8 reason, and the editor's starting text for
// the point reads back to it within the 6 decimals it shows.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fuzz_meta.h"
#include "meta/meta.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using mv::meta::location_input;
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  const location_input r = mv::meta::parse_location(text);
  if (r.error == nullptr) mv::fuzz::meta_fail("parse_location: null error");
  switch (r.k) {
    case location_input::kind::point: {
      const double lat = r.point.lat, lon = r.point.lon;
      if (!std::isfinite(lat) || !std::isfinite(lon)) mv::fuzz::meta_fail("parse_location: not finite");
      if (std::fabs(lat) > 90.0 || std::fabs(lon) > 180.0) mv::fuzz::meta_fail("parse_location: out of range");
      if ((lat == 0.0 && std::signbit(lat)) || (lon == 0.0 && std::signbit(lon))) {
        mv::fuzz::meta_fail("parse_location: -0");
      }
      if (r.point.altitude) mv::fuzz::meta_fail("parse_location: altitude");
      if (*r.error != '\0') mv::fuzz::meta_fail("parse_location: point with an error");
      const location_input back = mv::meta::parse_location(mv::meta::format_location_input(r.point));
      if (back.k != location_input::kind::point || std::fabs(back.point.lat - lat) > 5.1e-7 ||
          std::fabs(back.point.lon - lon) > 5.1e-7) {
        mv::fuzz::meta_fail("parse_location: starting text does not read back");
      }
      break;
    }
    case location_input::kind::blank:
      for (const char c : text) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && static_cast<unsigned char>(c) < 0x80) {
          mv::fuzz::meta_fail("parse_location: blank with text");
        }
      }
      break;
    case location_input::kind::invalid: {
      const std::string why(r.error);
      if (why.empty() || why.size() > 127 || !mv::fuzz::utf8_ok(why) || why.find('\n') != std::string::npos) {
        mv::fuzz::meta_fail("parse_location: bad reason");
      }
      break;
    }
    default:
      mv::fuzz::meta_fail("parse_location: unknown kind");
  }
  return 0;
}
