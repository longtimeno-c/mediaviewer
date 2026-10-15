// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #352: mv_meta_parse_location, the Location editor's parser for the Windows
// chrome (the Mac host calls meta::parse_location through its bridge). Pure:
// no session. Separate TU so it needs nothing of abi.cpp's Win32 includes.
#include <cstddef>
#include <cstring>
#include <string>

#include "abi/guard.h"
#include "mediaviewer/mediaviewer.h"
#include "meta/meta.h"

static_assert(sizeof(mv_location_input) == 152, "mv_location_input layout is part of the ABI");
static_assert(offsetof(mv_location_input, lat) == 8 && offsetof(mv_location_input, text) == 24,
              "mv_location_input layout is part of the ABI");

namespace {

void put_text(char* dst, std::size_t cap, const std::string& s) {
  std::size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  // Never cut a UTF-8 sequence in half.
  while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  std::memcpy(dst, s.data(), n);
  dst[n] = '\0';
}

}  // namespace

extern "C" {

mv_status MV_CALL mv_meta_parse_location(const char* utf8, mv_location_input* out) {
  return static_cast<mv_status>(mv::abi::guard("mv_meta_parse_location", [&]() -> mv::status {
    MV_REQUIRE(utf8 != nullptr, "utf8 must not be null");
    MV_REQUIRE(out != nullptr && out->struct_size >= sizeof(mv_location_input),
               "out must be an mv_location_input with struct_size set");
    const mv::meta::location_input r = mv::meta::parse_location(utf8);
    out->lat = 0;
    out->lon = 0;
    switch (r.k) {
      case mv::meta::location_input::kind::point:
        out->kind = MV_LOCATION_POINT;
        out->lat = r.point.lat;
        out->lon = r.point.lon;
        put_text(out->text, sizeof(out->text), mv::meta::format_point(r.point));
        break;
      case mv::meta::location_input::kind::blank:
        out->kind = MV_LOCATION_BLANK;
        out->text[0] = '\0';
        break;
      case mv::meta::location_input::kind::invalid:
        out->kind = MV_LOCATION_INVALID;
        put_text(out->text, sizeof(out->text), r.error);
        break;
    }
    return mv::status::ok;
  }));
}

}  // extern "C"
