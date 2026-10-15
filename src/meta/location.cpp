// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/location.h"

#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <span>
#include <string>

#include "codec/decode.h"
#include "codec/format.h"
#include "io/file.h"
#include "meta/internal.h"
#include "meta/meta.h"

namespace mv::meta {
namespace {

constexpr std::size_t kHeadBytes = 256u * 1024;
constexpr std::size_t kTiffBytes = 8u * 1024 * 1024;

std::optional<double> number(std::string_view s) noexcept {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  if (s.empty()) return std::nullopt;
  // from_chars for doubles is not on every toolchain this builds with: digits
  // and one dot by hand.
  double v = 0;
  double scale = 0;
  bool any = false;
  for (char c : s) {
    if (c == '.') {
      if (scale != 0) return std::nullopt;
      scale = 1;
    } else if (c >= '0' && c <= '9') {
      any = true;
      if (scale != 0) {
        scale *= 10;
        v += (c - '0') / scale;
      } else {
        v = v * 10 + (c - '0');
      }
    } else {
      return std::nullopt;
    }
  }
  if (!any) return std::nullopt;
  return v;
}

const Exiv2::Exifdatum* find_exif(const Exiv2::ExifData& e, const char* key) {
  try {
    const auto it = e.findKey(Exiv2::ExifKey(key));
    return it == e.end() ? nullptr : &*it;
  } catch (...) {
    return nullptr;
  }
}

const Exiv2::Xmpdatum* find_xmp(const Exiv2::XmpData& x, const char* key) {
  try {
    const auto it = x.findKey(Exiv2::XmpKey(key));
    return it == x.end() ? nullptr : &*it;
  } catch (...) {
    return nullptr;
  }
}

std::optional<double> dms(const Exiv2::Exifdatum& d) {
  if (d.count() < 3) return std::nullopt;
  double parts[3];
  for (std::size_t i = 0; i < 3; ++i) {
    const Exiv2::Rational r = d.toRational(static_cast<long>(i));
    if (r.second == 0) return std::nullopt;
    parts[i] = static_cast<double>(r.first) / static_cast<double>(r.second);
  }
  return parts[0] + parts[1] / 60 + parts[2] / 3600;
}

std::optional<gps_fix> from_exif(const Exiv2::ExifData& e) {
  const auto* lat = find_exif(e, "Exif.GPSInfo.GPSLatitude");
  const auto* lon = find_exif(e, "Exif.GPSInfo.GPSLongitude");
  if (!lat || !lon) return std::nullopt;
  auto la = dms(*lat);
  auto lo = dms(*lon);
  if (!la || !lo || *la > 90.0 || *lo > 180.0) return std::nullopt;
  gps_fix f;
  const auto* lar = find_exif(e, "Exif.GPSInfo.GPSLatitudeRef");
  const auto* lor = find_exif(e, "Exif.GPSInfo.GPSLongitudeRef");
  f.lat = lar && lar->toString().rfind('S', 0) == 0 ? -*la : *la;
  f.lon = lor && lor->toString().rfind('W', 0) == 0 ? -*lo : *lo;
  if (const auto* alt = find_exif(e, "Exif.GPSInfo.GPSAltitude"); alt && alt->count() >= 1) {
    const Exiv2::Rational r = alt->toRational(0);
    if (r.second != 0) {
      double a = static_cast<double>(r.first) / static_cast<double>(r.second);
      const auto* ref = find_exif(e, "Exif.GPSInfo.GPSAltitudeRef");
      if (ref && ref->count() >= 1 && ref->toInt64(0) == 1) a = -a;
      f.altitude = a;
    }
  }
  return f;
}

std::optional<gps_fix> from_xmp(const Exiv2::XmpData& x) {
  const auto* lat = find_xmp(x, "Xmp.exif.GPSLatitude");
  const auto* lon = find_xmp(x, "Xmp.exif.GPSLongitude");
  if (!lat || !lon) return std::nullopt;
  auto la = xmp_coordinate_degrees(lat->toString());
  auto lo = xmp_coordinate_degrees(lon->toString());
  if (!la || !lo || std::fabs(*la) > 90.0 || std::fabs(*lo) > 180.0) return std::nullopt;
  gps_fix f;
  f.lat = *la;
  f.lon = *lo;
  return f;
}

std::optional<gps_fix> from_still(std::span<const std::uint8_t> bytes) noexcept {
  try {
    auto image = detail::open_image(bytes);
    if (!image) return std::nullopt;
    if (auto f = from_exif(image->exifData())) return f;
    return from_xmp(image->xmpData());
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

std::optional<double> xmp_coordinate_degrees(std::string_view s) noexcept {
  // "DDD,MM.mmmmk" or "DDD,MM,SSk", k one of N S E W (XMP spec, exif:GPSLatitude).
  if (s.size() < 3) return std::nullopt;
  const char ref = s.back();
  if (ref != 'N' && ref != 'S' && ref != 'E' && ref != 'W') return std::nullopt;
  s.remove_suffix(1);
  const auto c1 = s.find(',');
  if (c1 == std::string_view::npos) return std::nullopt;
  const auto deg = number(s.substr(0, c1));
  std::string_view rest = s.substr(c1 + 1);
  const auto c2 = rest.find(',');
  std::optional<double> minutes;
  std::optional<double> seconds = 0.0;
  if (c2 == std::string_view::npos) {
    minutes = number(rest);
  } else {
    minutes = number(rest.substr(0, c2));
    seconds = number(rest.substr(c2 + 1));
  }
  if (!deg || !minutes || !seconds || *minutes >= 60 || *seconds >= 60) return std::nullopt;
  const double v = *deg + *minutes / 60 + *seconds / 3600;
  return ref == 'S' || ref == 'W' ? -v : v;
}

std::optional<gps_fix> parse_coordinate_pair(std::string_view text) noexcept {
  auto one = [](std::string_view& s, char pos, char neg, double limit, double& out) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    const auto deg = s.find("\xC2\xB0");
    if (deg == std::string_view::npos) return false;
    const auto v = number(s.substr(0, deg));
    s.remove_prefix(deg + 2);
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    if (!v || s.empty() || *v > limit) return false;
    if (s.front() != pos && s.front() != neg) return false;
    out = s.front() == neg ? -*v : *v;
    s.remove_prefix(1);
    return true;
  };
  gps_fix f;
  std::string_view s = text;
  if (!one(s, 'N', 'S', 90.0, f.lat)) return std::nullopt;
  if (s.empty() || s.front() != ',') return std::nullopt;
  s.remove_prefix(1);
  if (!one(s, 'E', 'W', 180.0, f.lon) || !s.empty()) return std::nullopt;
  return f;
}

std::optional<geo_point> read_location(std::string_view utf8_path) {
  auto prefix = io::read_prefix(utf8_path, kHeadBytes);
  if (!prefix) return std::nullopt;
  const std::span<const std::uint8_t> head(prefix->data(), std::min<std::size_t>(prefix->size(), 1024));
  const codec::format_family family = codec::probe(head);
  if (family == codec::format_family::unknown) {
    // A clip: the container's location tag (meta/clip.cpp), through the same
    // reader the pane uses; container metadata only, no frames decoded.
    metadata m;
    if (!detail::read_clip(utf8_path, m) || m.s.gps.empty()) return std::nullopt;
    return parse_coordinate_pair(m.s.gps);
  }
  if (auto f = from_still(*prefix)) return f;
  if ((family == codec::format_family::tiff || family == codec::format_family::raw ||
       codec::looks_like_raw(*prefix)) &&
      prefix->size() >= kHeadBytes) {
    if (auto more = io::read_prefix(utf8_path, kTiffBytes)) return from_still(*more);
  }
  return std::nullopt;
}

}  // namespace mv::meta
