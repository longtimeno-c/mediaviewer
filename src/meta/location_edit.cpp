// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #352 (owner, 2026-10-12): the Metadata pane's Location editor. What a person
// types for a position, read the same way on both hosts (meta.h
// parse_location), and what the row may do for a record (write.h
// location_rights_of). Pure: no I/O, no Exiv2.
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "meta/meta.h"
#include "meta/write.h"

namespace mv::meta {
namespace {

// Unit marks after a number, as single bytes the normaliser below writes in
// place of their UTF-8 / ASCII spellings. Not letters: 's' is south.
constexpr char kDeg = '\x01';
constexpr char kMin = '\x02';
constexpr char kSec = '\x03';

constexpr const char* kErrShape = "Enter latitude and longitude, e.g. 38.7979, -9.3907";
constexpr const char* kErrLatitude = "Latitude must be between 90\xC2\xB0 S and 90\xC2\xB0 N (-90 to 90)";
constexpr const char* kErrLongitude = "Longitude must be between 180\xC2\xB0 W and 180\xC2\xB0 E (-180 to 180)";
constexpr const char* kErrSixty = "Minutes and seconds must be less than 60";
constexpr const char* kErrHemisphere = "N or S goes with latitude, E or W with longitude";
constexpr const char* kErrSignAndLetter = "Use a minus sign or S / W, not both";
constexpr const char* kErrTooLong = "That is too long for a position";

constexpr std::size_t kMaxInput = 256;

bool starts(std::string_view s, std::size_t at, std::string_view what) {
  return s.size() - at >= what.size() && s.substr(at, what.size()) == what;
}

// The text with every spelling of a unit, minus or space reduced to one byte.
// False when a byte is outside what a position can hold.
bool normalise(std::string_view in, std::string& out) {
  out.clear();
  for (std::size_t i = 0; i < in.size();) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c < 0x80) {
      if (c == '\'' && i + 1 < in.size() && in[i + 1] == '\'') {  // '' for seconds
        out += kSec;
        i += 2;
        continue;
      }
      switch (c) {
        case '\'': out += kMin; break;
        case '"': out += kSec; break;
        case '\t': case '\r': case '\n': out += ' '; break;
        default:
          if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == ';' || c == ' ' || c == '+' ||
              c == '-' || c == 'N' || c == 'S' || c == 'E' || c == 'W' || c == 'n' || c == 's' ||
              c == 'e' || c == 'w') {
            out += static_cast<char>(c);
          } else {
            return false;
          }
      }
      ++i;
      continue;
    }
    struct spelling {
      std::string_view utf8;
      char as;
    };
    static constexpr spelling kMap[] = {
        {"\xC2\xB0", kDeg},      // ° degree sign
        {"\xC2\xBA", kDeg},      // º masculine ordinal, typed for ° on many keyboards
        {"\xCB\x9A", kDeg},      // ˚ ring above
        {"\xE2\x80\xB2", kMin},  // ′ prime
        {"\xE2\x80\x99", kMin},  // ’ right single quote (smart quotes)
        {"\xC2\xB4", kMin},      // ´ acute accent
        {"\xE2\x80\xB3", kSec},  // ″ double prime
        {"\xE2\x80\x9D", kSec},  // ” right double quote (smart quotes)
        {"\xE2\x88\x92", '-'},   // − minus sign
        {"\xE2\x80\x93", '-'},   // – en dash
        {"\xC2\xA0", ' '},       // no-break space
        {"\xE2\x80\xAF", ' '},   // narrow no-break space
        {"\xE2\x80\x89", ' '},   // thin space
    };
    bool hit = false;
    for (const spelling& m : kMap) {
      if (starts(in, i, m.utf8)) {
        out += m.as;
        i += m.utf8.size();
        hit = true;
        break;
      }
    }
    if (!hit) return false;
  }
  return true;
}

bool is_hemi(char c) {
  return c == 'N' || c == 'S' || c == 'E' || c == 'W' || c == 'n' || c == 's' || c == 'e' || c == 'w';
}
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return s;
}

struct coordinate {
  double value = 0;  // signed degrees
  char hemi = 0;     // 'N' 'S' 'E' 'W', or 0 when the text gave none
};

// One coordinate: [hemi] [sign] D[.d] [unit] [M[.m] [unit] [S[.s] [unit]]] [hemi].
// Only the last number given may have a fraction.
const char* parse_coordinate(std::string_view s, coordinate& out) {
  s = trim(s);
  if (s.empty()) return kErrShape;
  char hemi = 0;
  if (is_hemi(s.front())) {
    hemi = upper(s.front());
    s = trim(s.substr(1));
  }
  if (!s.empty() && is_hemi(s.back())) {
    if (hemi != 0) return kErrShape;
    hemi = upper(s.back());
    s = trim(s.substr(0, s.size() - 1));
  }
  bool neg = false;
  if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
    neg = s.front() == '-';
    s = trim(s.substr(1));
  }
  if (neg && hemi != 0) return kErrSignAndLetter;

  double parts[3] = {0, 0, 0};
  int n = 0;
  bool fraction_seen = false;
  std::size_t i = 0;
  while (i < s.size()) {
    if (n == 3) return kErrShape;
    if (fraction_seen) return kErrShape;  // a number after a fractional one
    // A number: digits, optionally a point and more digits.
    const std::size_t start = i;
    double v = 0;
    std::size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      if (++digits > 9) return kErrShape;
      v = v * 10 + (s[i++] - '0');
    }
    if (i < s.size() && s[i] == '.') {
      ++i;
      double scale = 0.1;
      std::size_t frac = 0;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        if (frac++ < 15) v += (s[i] - '0') * scale;
        scale *= 0.1;
        ++i;
      }
      if (frac == 0 && digits == 0) return kErrShape;
      fraction_seen = true;
    }
    if (i == start) return kErrShape;
    // An optional unit mark, which must name this number's place.
    while (i < s.size() && s[i] == ' ') ++i;
    if (i < s.size() && (s[i] == kDeg || s[i] == kMin || s[i] == kSec)) {
      const int place = s[i] == kDeg ? 0 : s[i] == kMin ? 1 : 2;
      if (place != n) return kErrShape;
      ++i;
      while (i < s.size() && s[i] == ' ') ++i;
    } else if (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) {
      return kErrShape;
    }
    parts[n++] = v;
  }
  if (n == 0) return kErrShape;
  if ((n >= 2 && parts[1] >= 60.0) || (n >= 3 && parts[2] >= 60.0)) return kErrSixty;
  double v = parts[0] + parts[1] / 60.0 + parts[2] / 3600.0;
  if (hemi == 'S' || hemi == 'W' || neg) v = -v;
  out.value = v;
  out.hemi = hemi;
  return nullptr;
}

// Two plain decimals: "38.7979 -9.3907", split at the one space between them.
// Units are not allowed here, so a DMS pair without a separator or letters is
// refused rather than guessed. npos when there is no such split.
std::size_t plain_split(std::string_view s) {
  for (char c : s) {
    if (c == kDeg || c == kMin || c == kSec) return std::string_view::npos;
  }
  const std::string_view t = trim(s);
  const std::size_t space = t.find(' ');
  if (space == std::string_view::npos) return std::string_view::npos;
  if (trim(t.substr(space)).find(' ') != std::string_view::npos) return std::string_view::npos;
  return static_cast<std::size_t>(t.data() - s.data()) + space;
}

// Where the text splits into its two coordinates: a comma or semicolon; else
// after a trailing hemisphere letter, or before a leading one; else between
// two plain numbers. npos when there is no split. `skip` is the separator's
// width (1 for a comma, 0 for a letter boundary).
std::size_t split_point(std::string_view s, std::size_t& skip) {
  skip = 1;
  std::size_t sep = std::string_view::npos;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == ',' || s[i] == ';') {
      if (sep != std::string_view::npos) return std::string_view::npos - 1;  // two separators
      sep = i;
    }
  }
  if (sep != std::string_view::npos) return sep;
  skip = 0;
  // Letters: "38 47 52 N 9 23 27 W" (suffixes) or "N 38 47 52 W 9 23 27" (prefixes).
  std::size_t letters[4] = {0, 0, 0, 0};
  std::size_t count = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (is_hemi(s[i])) {
      if (count == 4) return std::string_view::npos;
      letters[count++] = i;
    }
  }
  if (count == 2) {
    const bool first_leads = trim(s.substr(0, letters[0])).empty();
    return first_leads ? letters[1] : letters[0] + 1;
  }
  if (count == 1) {
    // One letter on one half only: "38.79N -9.39" splits after it, "-9.39 W"... is one half.
    const bool leads = trim(s.substr(0, letters[0])).empty();
    if (leads) return std::string_view::npos;
    // "-38.79 9.39 W": the letter ends the second half; split the numbers before it.
    if (trim(s.substr(letters[0] + 1)).empty()) return plain_split(s.substr(0, letters[0]));
    return letters[0] + 1;
  }
  if (count != 0) return std::string_view::npos;
  return plain_split(s);
}

void trim_zeros(std::string& s) {
  if (s.find('.') == std::string::npos) return;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
}

}  // namespace

location_input parse_location(std::string_view utf8) noexcept {
  location_input r;
  try {
    if (utf8.size() > kMaxInput) {
      r.error = kErrTooLong;
      return r;
    }
    std::string s;
    if (!normalise(utf8, s)) {
      r.error = kErrShape;
      return r;
    }
    const std::string_view text = trim(s);
    if (text.empty()) {
      r.k = location_input::kind::blank;
      return r;
    }
    std::size_t skip = 0;
    const std::size_t at = split_point(text, skip);
    if (at >= text.size()) {
      r.error = kErrShape;
      return r;
    }
    coordinate a, b;
    if (const char* e = parse_coordinate(text.substr(0, at), a)) {
      r.error = e;
      return r;
    }
    if (const char* e = parse_coordinate(text.substr(at + skip), b)) {
      r.error = e;
      return r;
    }
    const auto is_lat = [](char h) { return h == 'N' || h == 'S'; };
    const auto is_lon = [](char h) { return h == 'E' || h == 'W'; };
    // Latitude first unless the letters say otherwise ("9.39 W, 38.79 N").
    if (is_lon(a.hemi) || is_lat(b.hemi)) std::swap(a, b);
    if ((a.hemi != 0 && !is_lat(a.hemi)) || (b.hemi != 0 && !is_lon(b.hemi))) {
      r.error = kErrHemisphere;
      return r;
    }
    if (!(std::fabs(a.value) <= 90.0)) {
      r.error = kErrLatitude;
      return r;
    }
    if (!(std::fabs(b.value) <= 180.0)) {
      r.error = kErrLongitude;
      return r;
    }
    r.k = location_input::kind::point;
    r.point.lat = a.value == 0.0 ? 0.0 : a.value;  // no "-0"
    r.point.lon = b.value == 0.0 ? 0.0 : b.value;
    r.error = "";
    return r;
  } catch (...) {
    r = location_input{};
    r.error = kErrShape;
    return r;
  }
}

std::string format_location_input(const geo_point& p) {
  char lat[32], lon[32];
  std::snprintf(lat, sizeof(lat), "%.6f", p.lat);
  std::snprintf(lon, sizeof(lon), "%.6f", p.lon);
  std::string a(lat), b(lon);
  trim_zeros(a);
  trim_zeros(b);
  if (a == "-0") a = "0";
  if (b == "-0") b = "0";
  return a + ", " + b;
}

location_rights location_rights_of(const metadata& m) noexcept {
  location_rights r;
  // Every file takes a position: a plain JPEG in place, anything else in its
  // XMP sidecar, which the read model puts over the file (write.h).
  r.can_set = true;
  r.in_original = m.where_in_file && !m.writes_in_file;
  // Removing must reach every copy the viewer reads. A JPEG written in place
  // loses its EXIF and XMP GPS; a sidecar can only drop its own, so a position
  // the original holds stays (rule 5: the original is never rewritten).
  r.can_clear = m.s.where.has_value() && !r.in_original;
  return r;
}

}  // namespace mv::meta
