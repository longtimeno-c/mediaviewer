// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/import/naming.h"

#include <cstdio>
#include <ctime>

namespace mv::import {
namespace {

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) noexcept {
  return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

std::string two(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%02d", v);
  return b;
}

std::string four(int v) {
  char b[8];
  std::snprintf(b, sizeof b, "%04d", v);
  return b;
}

std::string type_folder(const layout_input& in) {
  if (in.has_raw) return "RAW";
  switch (in.primary_type) {
    case file_type::jpeg: return "JPEG";
    case file_type::heic: return "HEIC";
    case file_type::video: return "Video";
    default: return "Other";
  }
}

}  // namespace

civil civil_from_seconds(std::int64_t s) noexcept {
  // Howard Hinnant's days_from_civil inverse; exact for the proleptic
  // Gregorian calendar, no zone, no locale.
  std::int64_t days = s / 86400;
  std::int64_t rem = s % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<unsigned>(days - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  civil c;
  c.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  c.month = static_cast<int>(m);
  c.day = static_cast<int>(d);
  c.hour = static_cast<int>(rem / 3600);
  c.minute = static_cast<int>((rem / 60) % 60);
  c.second = static_cast<int>(rem % 60);
  return c;
}

std::int64_t local_wall_from_utc(std::int64_t utc) noexcept {
  const auto t = static_cast<std::time_t>(utc);
  std::tm local{};
#if defined(_WIN32)
  if (localtime_s(&local, &t) != 0) return utc;
#else
  if (!localtime_r(&t, &local)) return utc;
#endif
  std::int64_t days = 0;
  char buf[48];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1,
                local.tm_mday);
  if (!parse_day(buf, days)) return utc;
  return days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
}

std::string day_key(std::int64_t s) {
  const civil c = civil_from_seconds(s);
  return four(c.year) + "-" + two(c.month) + "-" + two(c.day);
}

bool parse_day(std::string_view t, std::int64_t& days) noexcept {
  if (t.size() != 10 || t[4] != '-' || t[7] != '-') return false;
  int v[3] = {0, 0, 0};
  const std::size_t start[3] = {0, 5, 8};
  const std::size_t len[3] = {4, 2, 2};
  for (int k = 0; k < 3; ++k) {
    for (std::size_t i = 0; i < len[k]; ++i) {
      const char c = t[start[k] + i];
      if (c < '0' || c > '9') return false;
      v[k] = v[k] * 10 + (c - '0');
    }
  }
  const int y0 = v[0];
  const unsigned m = static_cast<unsigned>(v[1]);
  const unsigned d = static_cast<unsigned>(v[2]);
  if (m < 1 || m > 12 || d < 1 || d > 31) return false;
  const int y = y0 - (m <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
  return true;
}

std::string sanitize_component(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 || c == 0x7F || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' ||
        ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
      out.push_back('_');
    } else {
      out.push_back(ch);
    }
  }
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.empty()) return "_";
  // CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without an extension.
  const std::string_view base = std::string_view(out).substr(0, out.find('.'));
  static constexpr const char* kReserved[] = {"con", "prn", "aux", "nul"};
  bool reserved = false;
  for (const char* r : kReserved) reserved = reserved || iequals(base, r);
  if (base.size() == 4 && (istarts_with(base, "com") || istarts_with(base, "lpt")) &&
      base[3] >= '1' && base[3] <= '9') {
    reserved = true;
  }
  if (reserved) out.insert(base.size(), "_");
  return out;
}

std::string_view stem_of(std::string_view name) noexcept {
  auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0) return name;
  std::string_view stem = name.substr(0, dot);
  // A sidecar named after the whole file ("IMG_0001.CR3.xmp") pairs by the
  // inner stem.
  const std::string_view ext = name.substr(dot);
  if (iequals(ext, ".xmp")) {
    const auto inner = stem.find_last_of('.');
    if (inner != std::string_view::npos && inner > 0 &&
        classify(stem) != file_type::none && classify(stem) != file_type::sidecar) {
      stem = stem.substr(0, inner);
    }
  }
  return stem;
}

std::string layout_folder(const preset& p, const layout_input& in) {
  const civil c = civil_from_seconds(in.taken);
  std::string out;
  switch (p.layout) {
    case layout_kind::year_day:
      out = four(c.year) + "/" + four(c.year) + "-" + two(c.month) + "-" + two(c.day);
      break;
    case layout_kind::year_month_day:
      out = four(c.year) + "/" + two(c.month) + "/" + two(c.day);
      break;
    case layout_kind::day:
      out = four(c.year) + "-" + two(c.month) + "-" + two(c.day);
      break;
    case layout_kind::card: {
      // Keep the card's own folders, each component sanitised.
      std::string_view rest = in.source_rel_dir;
      while (!rest.empty()) {
        const auto slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (!part.empty()) {
          if (!out.empty()) out.push_back('/');
          out += sanitize_component(part);
        }
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
      }
      break;
    }
    case layout_kind::flat:
      break;
  }
  const bool dated = p.layout != layout_kind::card && p.layout != layout_kind::flat;
  if (dated && p.layout_camera) {
    out += "/";
    out += in.camera.empty() ? std::string("Unknown camera") : sanitize_component(in.camera);
  }
  if (p.layout_type) {
    if (!out.empty()) out.push_back('/');
    out += type_folder(in);
  }
  return out;
}

bool template_uses_seq(std::string_view tmpl) noexcept {
  return tmpl.find("{seq}") != std::string_view::npos;
}

std::string render_stem(std::string_view tmpl, const rename_input& in) {
  const civil c = civil_from_seconds(in.taken);
  std::string out;
  for (std::size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == '{') {
      const auto close = tmpl.find('}', i);
      if (close != std::string_view::npos) {
        const std::string_view token = tmpl.substr(i + 1, close - i - 1);
        bool known = true;
        if (token == "date") {
          out += four(c.year) + "-" + two(c.month) + "-" + two(c.day);
        } else if (token == "time") {
          out += two(c.hour) + two(c.minute) + two(c.second);
        } else if (token == "camera") {
          out += in.camera.empty() ? std::string("Unknown") : std::string(in.camera);
        } else if (token == "seq") {
          out += four(static_cast<int>(in.seq % 100000));
        } else if (token == "original") {
          out += in.original_stem;
        } else {
          known = false;
        }
        if (known) {
          i = close + 1;
          continue;
        }
      }
    }
    out.push_back(tmpl[i]);
    ++i;
  }
  return sanitize_component(out);
}

std::string with_stem(std::string_view name, std::string_view old_stem, std::string_view new_stem) {
  if (istarts_with(name, old_stem)) {
    return std::string(new_stem) + std::string(name.substr(old_stem.size()));
  }
  return std::string(new_stem) + "_" + std::string(name);
}

std::string clash_stem(std::string_view stem, int n) {
  return std::string(stem) + " (" + std::to_string(n) + ")";
}

}  // namespace mv::import
