// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #606: GoPro GPMF and DJI subtitle telemetry → GPS fixes (meta/telemetry.h).
//
// GPMF (github.com/gopro/gpmf-parser, "GPMF Introduction"): a payload is a run
// of KLVs. Each is an 8-byte header (a FourCC key; one type character; a
// one-byte struct size; a 16-bit big-endian repeat count) and struct size ×
// repeat bytes of big-endian data, padded to a multiple of 4. Type 0 is a
// nest of KLVs. A STRM's properties (SCAL, GPSF, GPSP, TYPE) come before its
// samples and hold for the rest of that STRM ("sticky").
#include "meta/telemetry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>

#include "meta/parse_util.h"

namespace mv::meta {
namespace {

using parse::util::be16;
using parse::util::be32;
using parse::util::be64;
using parse::util::bytes;

constexpr double kEarthRadiusM = 6371008.8;
constexpr double kPi = 3.14159265358979323846;

bool valid_fix(const geo_point& p) noexcept {
  if (!std::isfinite(p.lat) || !std::isfinite(p.lon)) return false;
  if (std::fabs(p.lat) > 90.0 || std::fabs(p.lon) > 180.0) return false;
  // A receiver with no lock reports exactly 0, 0 (DJI writes it every frame).
  return !(std::fabs(p.lat) < 1e-9 && std::fabs(p.lon) < 1e-9);
}

// ---- GPMF ---------------------------------------------------------------------

std::size_t type_size(char t) noexcept {
  switch (t) {
    case 'b': case 'B': case 'c': return 1;
    case 's': case 'S': return 2;
    case 'l': case 'L': case 'f': case 'F': case 'q': return 4;
    case 'd': case 'j': case 'J': case 'Q': return 8;
    default: return 0;  // G (128-bit), U (date), ? (complex), 0 (nest): not numbers
  }
}

bool numeric(char t) noexcept { return t != 'c' && t != 'F' && type_size(t) != 0; }

// One big-endian number of type `t` at `at` (the caller checked the bounds).
double number_at(bytes b, std::size_t at, char t) noexcept {
  switch (t) {
    case 'b': return static_cast<std::int8_t>(b[at]);
    case 'B': return b[at];
    case 's': return static_cast<std::int16_t>(be16(b, at));
    case 'S': return be16(b, at);
    case 'l': return static_cast<std::int32_t>(be32(b, at));
    case 'L': return be32(b, at);
    case 'q': return static_cast<std::int32_t>(be32(b, at)) / 65536.0;
    case 'f': {
      const std::uint32_t u = be32(b, at);
      float f;
      std::memcpy(&f, &u, sizeof f);
      return f;
    }
    case 'd': {
      const std::uint64_t u = be64(b, at);
      double d;
      std::memcpy(&d, &u, sizeof d);
      return d;
    }
    case 'j': return static_cast<double>(static_cast<std::int64_t>(be64(b, at)));
    case 'J': return static_cast<double>(be64(b, at));
    case 'Q': return static_cast<double>(static_cast<std::int64_t>(be64(b, at))) / 4294967296.0;
    default: return 0;
  }
}

constexpr std::size_t kMaxElements = 16;

// The sticky properties of one STRM.
struct stream_state {
  std::array<double, kMaxElements> scal{};
  std::size_t n_scal = 0;
  int fix = -1;     // GPSF: 0 none, 2 2D, 3 3D; -1 not stated
  double dop = -1;  // GPSP: dilution of precision x 100; -1 not stated
  std::array<char, kMaxElements> type{};  // TYPE: a complex sample's element types
  std::size_t n_type = 0;

  // The divisor for element i: one SCAL for all, or one per element.
  [[nodiscard]] double scale(std::size_t i) const noexcept {
    if (n_scal == 0) return 0;
    if (n_scal == 1) return scal[0];
    return i < n_scal ? scal[i] : 0;
  }
};

struct gpmf_walk {
  route_builder& out;
  std::size_t klvs = 0;
  std::size_t found = 0;

  void offer(double lat, double lon, double alt, bool has_alt) noexcept {
    geo_point p;
    p.lat = lat;
    p.lon = lon;
    if (has_alt && std::isfinite(alt) && std::fabs(alt) < 100000.0) p.altitude = alt;
    if (!valid_fix(p)) return;
    ++found;
    out.add(p);
  }

  // GPS5: lat, lon, alt, 2D speed, 3D speed as int32, each over its SCAL.
  void gps5(bytes data, std::size_t struct_size, std::size_t repeat, const stream_state& s) noexcept {
    if (struct_size != 20) return;
    if (s.fix >= 0 && s.fix < 2) return;   // no lock
    if (s.dop > 500) return;               // GPSP over 500 (DOP 5): not a usable fix
    const double sl = s.scale(0), so = s.scale(1), sa = s.scale(2);
    if (!(sl != 0) || !(so != 0)) return;  // no SCAL: the numbers mean nothing
    for (std::size_t i = 0; i < repeat; ++i) {
      const std::size_t at = i * 20;
      const double lat = static_cast<std::int32_t>(be32(data, at)) / sl;
      const double lon = static_cast<std::int32_t>(be32(data, at + 4)) / so;
      const double alt = sa != 0 ? static_cast<std::int32_t>(be32(data, at + 8)) / sa : 0;
      offer(lat, lon, alt, sa != 0);
    }
  }

  // GPS9 (HERO11 on): a complex sample its TYPE spells, normally "lllllllSS":
  // lat, lon, alt, 2D speed, 3D speed, days since 2000, seconds since
  // midnight, DOP, fix, each over its SCAL. Fix and DOP travel per sample.
  void gps9(bytes data, std::size_t struct_size, std::size_t repeat, const stream_state& s) noexcept {
    if (s.n_type < 9) return;
    std::array<std::size_t, kMaxElements> offset{};
    std::size_t total = 0;
    for (std::size_t e = 0; e < s.n_type; ++e) {
      const std::size_t n = type_size(s.type[e]);
      if (n == 0 || !numeric(s.type[e])) return;
      offset[e] = total;
      total += n;
    }
    if (total != struct_size) return;
    const double sl = s.scale(0), so = s.scale(1), sa = s.scale(2), sd = s.scale(7), sf = s.scale(8);
    if (!(sl != 0) || !(so != 0)) return;
    for (std::size_t i = 0; i < repeat; ++i) {
      const std::size_t at = i * struct_size;
      const double fix = number_at(data, at + offset[8], s.type[8]) / (sf != 0 ? sf : 1);
      const double dop = number_at(data, at + offset[7], s.type[7]) / (sd != 0 ? sd : 1);
      if (!(fix >= 2) || !(dop <= 5)) continue;
      const double lat = number_at(data, at + offset[0], s.type[0]) / sl;
      const double lon = number_at(data, at + offset[1], s.type[1]) / so;
      const double alt = sa != 0 ? number_at(data, at + offset[2], s.type[2]) / sa : 0;
      offer(lat, lon, alt, sa != 0);
    }
  }

  // Walks one level of KLVs. `s` is the enclosing STRM's state, or null
  // outside one.
  void walk(bytes b, std::size_t depth, stream_state* s) noexcept {
    std::size_t at = 0;
    while (parse::util::has(b, at, 8)) {
      if (++klvs > kGpmfMaxKlvs) return;
      const char key[4] = {static_cast<char>(b[at]), static_cast<char>(b[at + 1]), static_cast<char>(b[at + 2]),
                           static_cast<char>(b[at + 3])};
      const char type = static_cast<char>(b[at + 4]);
      const std::size_t struct_size = b[at + 5];
      const std::size_t repeat = be16(b, at + 6);
      const std::size_t len = struct_size * repeat;  // at most 255 * 65535: no overflow
      if (!parse::util::has(b, at + 8, len)) return;  // truncated: what came before stands
      const bytes data = b.subspan(at + 8, len);
      const auto is = [&](const char* k) { return std::memcmp(key, k, 4) == 0; };
      if (type == 0) {
        if (depth + 1 < kGpmfMaxDepth) {
          if (is("STRM")) {
            stream_state inner;  // a new STRM starts with nothing sticky
            walk(data, depth + 1, &inner);
          } else {
            walk(data, depth + 1, s);
          }
        }
      } else if (s) {
        const std::size_t tsize = type_size(type);
        if (is("SCAL") && numeric(type) && tsize != 0 && len >= tsize) {
          s->n_scal = std::min(len / tsize, kMaxElements);
          for (std::size_t i = 0; i < s->n_scal; ++i) s->scal[i] = number_at(data, i * tsize, type);
        } else if (is("GPSF") && numeric(type) && tsize != 0 && len >= tsize) {
          const double f = number_at(data, 0, type);
          s->fix = f >= 0 && f <= 3 ? static_cast<int>(f) : 0;
        } else if (is("GPSP") && numeric(type) && tsize != 0 && len >= tsize) {
          const double p = number_at(data, 0, type);
          s->dop = p >= 0 ? p : 0;
        } else if (is("TYPE") && type == 'c') {
          s->n_type = std::min(len, kMaxElements);
          for (std::size_t i = 0; i < s->n_type; ++i) s->type[i] = static_cast<char>(data[i]);
          // A NUL-padded TYPE ends at its first NUL.
          for (std::size_t i = 0; i < s->n_type; ++i) {
            if (s->type[i] == 0) {
              s->n_type = i;
              break;
            }
          }
        } else if (is("GPS5") && type == 'l') {
          gps5(data, struct_size, repeat, *s);
        } else if (is("GPS9") && type == '?') {
          gps9(data, struct_size, repeat, *s);
        }
      }
      // Padded to 4 bytes; the padding of the last KLV may be missing.
      const std::size_t padded = (len + 3) & ~static_cast<std::size_t>(3);
      if (!parse::util::has(b, at + 8, padded)) return;
      at += 8 + padded;
    }
  }
};

// ---- DJI ----------------------------------------------------------------------

char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Case-insensitive find of an ASCII word.
std::size_t find_word(std::string_view s, std::string_view word, std::size_t from = 0) noexcept {
  if (word.empty() || s.size() < word.size()) return std::string_view::npos;
  for (std::size_t i = from; i + word.size() <= s.size(); ++i) {
    std::size_t k = 0;
    while (k < word.size() && lower(s[i + k]) == word[k]) ++k;
    if (k == word.size()) return i;
  }
  return std::string_view::npos;
}

void skip_spaces(std::string_view s, std::size_t& i) noexcept {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}

// A signed decimal at `i` ("-9.3907", "+38.79", "15"), by hand: from_chars for
// doubles is not on every toolchain this builds with. Advances `i`.
std::optional<double> decimal(std::string_view s, std::size_t& i) noexcept {
  skip_spaces(s, i);
  bool neg = false;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    neg = s[i] == '-';
    ++i;
  }
  double v = 0, scale = 0;
  std::size_t digits = 0;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '.') {
      if (scale != 0) break;
      scale = 1;
    } else if (c >= '0' && c <= '9') {
      if (++digits > 20) return std::nullopt;
      if (scale != 0) {
        scale *= 10;
        v += (c - '0') / scale;
      } else {
        v = v * 10 + (c - '0');
      }
    } else {
      break;
    }
  }
  if (digits == 0) return std::nullopt;
  return neg ? -v : v;
}

// "<key> : <number>" after the key's position.
std::optional<double> value_after(std::string_view s, std::size_t key_at, std::size_t key_len) noexcept {
  std::size_t i = key_at + key_len;
  skip_spaces(s, i);
  if (i >= s.size() || s[i] != ':') return std::nullopt;
  ++i;
  return decimal(s, i);
}

}  // namespace

route_builder::route_builder(route_limits limits) noexcept
    : limits_(limits), step_m_(limits.min_step_m > 0 ? limits.min_step_m : 0) {
  if (limits_.max_points < 2) limits_.max_points = 2;
}

void route_builder::add(const geo_point& p) noexcept {
  if (!valid_fix(p)) return;
  ++accepted_;
  last_ = p;
  if (!points_.empty() && step_m_ > 0 && distance_m(points_.back(), p) < step_m_) return;
  try {
    if (points_.size() >= limits_.max_points) {
      // Full: keep every other point (the first stays) and space the rest wider.
      std::size_t w = 0;
      for (std::size_t r = 0; r < points_.size(); r += 2) points_[w++] = points_[r];
      points_.resize(w);
      step_m_ = step_m_ > 0 ? step_m_ * 2 : 1.0;
      thinned_ = true;
      if (distance_m(points_.back(), p) < step_m_) return;
    }
    points_.push_back(p);
  } catch (...) {
    // Out of memory: what is held stands.
  }
}

std::vector<geo_point> route_builder::take() noexcept {
  // A route ends where the clip did: the last fix may have been inside the
  // step of the point before it, or dropped while the route was full.
  if (accepted_ > 0 && !points_.empty() &&
      (points_.back().lat != last_.lat || points_.back().lon != last_.lon)) {
    if (points_.size() >= limits_.max_points) {
      points_.back() = last_;
    } else {
      try {
        points_.push_back(last_);
      } catch (...) {
        points_.back() = last_;  // out of memory: the end still wins
      }
    }
  }
  return std::move(points_);
}

double distance_m(const geo_point& a, const geo_point& b) noexcept {
  const double rad = kPi / 180.0;
  const double dlat = (b.lat - a.lat) * rad;
  const double dlon = (b.lon - a.lon) * rad;
  const double s1 = std::sin(dlat / 2), s2 = std::sin(dlon / 2);
  const double h = s1 * s1 + std::cos(a.lat * rad) * std::cos(b.lat * rad) * s2 * s2;
  return 2 * kEarthRadiusM * std::asin(std::sqrt(std::min(1.0, std::max(0.0, h))));
}

std::size_t read_gpmf(std::span<const std::uint8_t> payload, route_builder& out) noexcept {
  if (payload.size() > kGpmfMaxPayload) payload = payload.first(kGpmfMaxPayload);
  gpmf_walk w{out};
  w.walk(payload, 0, nullptr);
  return w.found;
}

std::optional<geo_point> parse_dji_text(std::string_view text) noexcept {
  if (text.size() > 4096) text = text.substr(0, 4096);
  geo_point p;
  bool have = false;
  // Mavic 2 / Air 2S / Mini: "[latitude: 38.797] [longitude: -9.390] [rel_alt: 1.2 abs_alt: 85.9]".
  // Some firmware spells it "longtitude"; some puts a space before the colon.
  if (const std::size_t la = find_word(text, "latitude"); la != std::string_view::npos) {
    std::size_t lo = find_word(text, "longitude");
    std::size_t lo_len = 9;
    if (lo == std::string_view::npos) {
      lo = find_word(text, "longtitude");
      lo_len = 10;
    }
    if (lo != std::string_view::npos) {
      const auto lat = value_after(text, la, 8);
      const auto lon = value_after(text, lo, lo_len);
      if (lat && lon) {
        p.lat = *lat;
        p.lon = *lon;
        have = true;
        if (const std::size_t aa = find_word(text, "abs_alt"); aa != std::string_view::npos) {
          if (const auto a = value_after(text, aa, 7)) p.altitude = *a;
        } else if (const std::size_t al = find_word(text, "altitude"); al != std::string_view::npos) {
          if (const auto a = value_after(text, al, 8)) p.altitude = *a;
        }
      }
    }
  }
  // Phantom 4 / Mavic Pro / Mavic Air: "GPS(lon,lat,alt)" or "GPS (lon, lat, alt)",
  // longitude first.
  if (!have) {
    for (std::size_t g = find_word(text, "gps"); g != std::string_view::npos; g = find_word(text, "gps", g + 3)) {
      std::size_t i = g + 3;
      skip_spaces(text, i);
      if (i >= text.size() || text[i] != '(') continue;
      ++i;
      const auto lon = decimal(text, i);
      skip_spaces(text, i);
      if (!lon || i >= text.size() || text[i] != ',') continue;
      ++i;
      const auto lat = decimal(text, i);
      if (!lat) continue;
      p.lat = *lat;
      p.lon = *lon;
      have = true;
      skip_spaces(text, i);
      if (i < text.size() && text[i] == ',') {
        ++i;
        if (const auto a = decimal(text, i)) p.altitude = *a;
      }
      break;
    }
  }
  if (!have || !valid_fix(p)) return std::nullopt;
  if (p.altitude && (!std::isfinite(*p.altitude) || std::fabs(*p.altitude) >= 100000.0)) p.altitude.reset();
  return p;
}

std::vector<geo_point> sample_route(std::span<const geo_point> route, std::size_t max_points) {
  std::vector<geo_point> out;
  if (route.empty() || max_points == 0) return out;
  if (route.size() <= max_points) return {route.begin(), route.end()};
  if (max_points == 1) return {route.front()};
  out.reserve(max_points);
  const std::size_t last = route.size() - 1;
  for (std::size_t k = 0; k < max_points; ++k) {
    // k * last / (max_points - 1), exact at both ends.
    out.push_back(route[k * last / (max_points - 1)]);
  }
  return out;
}

}  // namespace mv::meta
