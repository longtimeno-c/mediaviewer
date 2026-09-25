// SPDX-License-Identifier: GPL-2.0-or-later
// Pure helpers behind the metadata read model: no Exiv2, no FFmpeg, no I/O.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "meta/meta.h"
#include "meta/write.h"

namespace mv::meta {
namespace {

// Howard Hinnant's days_from_civil: days since 1970-01-01, proleptic Gregorian.
constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
  y -= m <= 2;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Reads exactly `n` ASCII digits at `s[pos]`, advancing pos. -1 on failure.
int digits(std::string_view s, std::size_t& pos, std::size_t n) noexcept {
  if (pos + n > s.size()) return -1;
  int v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = s[pos + i];
    if (c < '0' || c > '9') return -1;
    v = v * 10 + (c - '0');
  }
  pos += n;
  return v;
}

}  // namespace

std::optional<std::int64_t> parse_date_key(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  std::size_t pos = 0;
  const int year = digits(s, pos, 4);
  if (year < 1) return std::nullopt;
  if (pos >= s.size() || (s[pos] != ':' && s[pos] != '-')) return std::nullopt;
  ++pos;
  const int month = digits(s, pos, 2);
  if (month < 1 || month > 12) return std::nullopt;
  if (pos >= s.size() || (s[pos] != ':' && s[pos] != '-')) return std::nullopt;
  ++pos;
  const int day = digits(s, pos, 2);
  if (day < 1 || day > 31) return std::nullopt;

  int hour = 0, minute = 0, second = 0;
  if (pos < s.size() && (s[pos] == ' ' || s[pos] == 'T')) {
    ++pos;
    hour = digits(s, pos, 2);
    if (hour < 0 || hour > 23 || pos >= s.size() || s[pos] != ':') return std::nullopt;
    ++pos;
    minute = digits(s, pos, 2);
    if (minute < 0 || minute > 59) return std::nullopt;
    if (pos < s.size() && s[pos] == ':') {
      ++pos;
      second = digits(s, pos, 2);
      if (second < 0 || second > 60) return std::nullopt;
    }
  }
  // A camera that never had its clock set writes 0000:00:00; rejected above
  // (year < 1, month < 1). A non-zero date is taken at its word.
  const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month),
                                            static_cast<unsigned>(day));
  return days * 86400 + hour * 3600 + minute * 60 + second;
}

std::string format_coordinate(double degrees, char hemisphere) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.5f\xC2\xB0 %c", std::fabs(degrees), hemisphere);
  return buf;
}

void orient_point(std::uint8_t orientation, float& x, float& y) noexcept {
  const float ox = x, oy = y;
  switch (orientation) {
    case 2: x = 1.0f - ox; break;
    case 3: x = 1.0f - ox; y = 1.0f - oy; break;
    case 4: y = 1.0f - oy; break;
    case 5: x = oy; y = ox; break;
    case 6: x = 1.0f - oy; y = ox; break;
    case 7: x = 1.0f - oy; y = 1.0f - ox; break;
    case 8: x = oy; y = 1.0f - ox; break;
    default: break;
  }
}

std::string format_rating(int rating) {
  if (rating < 0) return "Rejected";
  if (rating == 0) return {};
  if (rating > kMaxRating) rating = kMaxRating;
  std::string out;
  for (int i = 0; i < kMaxRating; ++i) out += i < rating ? "\xE2\x98\x85" : "\xE2\x98\x86";  // ★ ☆
  return out;
}

std::string format_dimensions(std::uint32_t w, std::uint32_t h) {
  if (w == 0 || h == 0) return {};
  return std::to_string(w) + " \xC3\x97 " + std::to_string(h);
}

std::string format_size(std::uint64_t bytes) {
  if (bytes == 0) return {};
  char buf[32];
  if (bytes >= (1ull << 30)) {
    std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / static_cast<double>(1ull << 30));
  } else if (bytes >= (1ull << 20)) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / static_cast<double>(1ull << 20));
  } else if (bytes >= 1024) {
    std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  }
  return buf;
}

std::vector<field> summary_rows(const metadata& m) {
  const summary& s = m.s;
  std::vector<field> rows;
  const auto add = [&](const char* label, const std::string& value) { rows.push_back({label, value}); };
  add("Dimensions", format_dimensions(s.width, s.height));
  add("File size", format_size(s.file_size));
  add("Format", s.format);
  add("Camera", s.camera);
  if (m.is_clip) {
    add("Duration", s.duration);
    add("Video codec", s.codec);
    add("Bit rate", s.bitrate);
  } else {
    add("Lens", s.lens);
    add("Exposure", s.exposure);
    add("Aperture", s.aperture);
    add("ISO", s.iso);
    add("Focal length", s.focal_length);
    add("Colour space", s.colour_space);
  }
  add("Date taken", s.date_taken);
  add("Location", s.gps);
  add("Rating", format_rating(s.rating));
  add("Comment", s.comment);
  return rows;
}

namespace {
std::string join(std::initializer_list<const std::string*> parts, const char* sep) {
  std::string out;
  for (const std::string* p : parts) {
    if (p->empty()) continue;
    if (!out.empty()) out += sep;
    out += *p;
  }
  return out;
}
}  // namespace

std::string overlay_camera_line(const metadata& m) {
  return m.is_clip ? m.s.camera : join({&m.s.camera, &m.s.lens}, "  |  ");
}

std::string overlay_exposure_line(const metadata& m) {
  if (m.is_clip) return join({&m.s.duration, &m.s.codec, &m.s.bitrate}, "   ");
  return join({&m.s.exposure, &m.s.aperture, &m.s.iso, &m.s.focal_length}, "   ");
}

std::string overlay_date_line(const metadata& m) {
  return join({&m.s.date_taken, &m.s.gps}, "   ");
}

std::vector<af_point> displayed_af_points(const metadata& m) {
  std::vector<af_point> out;
  out.reserve(m.af_points.size());
  for (const af_point& p : m.af_points) {
    float ax = p.x, ay = p.y, bx = p.x + p.w, by = p.y + p.h;
    orient_point(m.display_orientation, ax, ay);
    orient_point(m.display_orientation, bx, by);
    af_point q;
    q.x = std::min(ax, bx);
    q.y = std::min(ay, by);
    q.w = std::fabs(bx - ax);
    q.h = std::fabs(by - ay);
    q.in_focus = p.in_focus;
    out.push_back(q);
  }
  return out;
}

}  // namespace mv::meta
