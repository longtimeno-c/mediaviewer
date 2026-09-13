// SPDX-License-Identifier: GPL-2.0-or-later
// Scan-time pairing and companion filtering (plan/04, PR 7).
//
// RAW+JPEG: same basename, one filmstrip stop, JPEG/HEIC is the primary.
// Live Photo: HEIC+MOV same basename, still is primary, motion is the pair.
// Ambiguous groups (three files, mixed stems) stay separate — never hide a
// file. Portable; no windows.h (D9).
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "io/dir.h"

namespace mv::io {

enum class pair_kind : std::uint32_t {
  none = 0,
  raw_jpeg = 1,
  live_photo = 2,
};

struct listed_item {
  dir_entry primary;
  dir_entry secondary;  // empty path when unpaired
  pair_kind kind = pair_kind::none;
};

namespace pairing_detail {

inline void ascii_lower_inplace(std::string& s) noexcept {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
}

inline std::string_view extension(std::string_view name) noexcept {
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size()) return {};
  return name.substr(dot);
}

inline std::string_view stem(std::string_view name) noexcept {
  const auto slash = name.find_last_of("\\/");
  const std::string_view base = slash == std::string_view::npos ? name : name.substr(slash + 1);
  const auto dot = base.find_last_of('.');
  if (dot == std::string_view::npos) return base;
  return base.substr(0, dot);
}

inline bool eq_lower(std::string_view a, const char* b) noexcept {
  std::size_t i = 0;
  for (; b[i] != '\0'; ++i) {
    if (i >= a.size()) return false;
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    char d = b[i];
    if (d >= 'A' && d <= 'Z') d = static_cast<char>(d - 'A' + 'a');
    if (c != d) return false;
  }
  return i == a.size();
}

inline bool is_jpeg(std::string_view ext) noexcept {
  return eq_lower(ext, ".jpg") || eq_lower(ext, ".jpeg");
}
inline bool is_heic(std::string_view ext) noexcept {
  return eq_lower(ext, ".heic") || eq_lower(ext, ".heif") || eq_lower(ext, ".hif");
}
inline bool is_still_pair(std::string_view ext) noexcept { return is_jpeg(ext) || is_heic(ext); }
inline bool is_mov(std::string_view ext) noexcept { return eq_lower(ext, ".mov"); }
inline bool is_raw(std::string_view ext) noexcept {
  static constexpr const char* k[] = {
      ".cr2", ".cr3", ".nef", ".nrw", ".arw", ".srf", ".sr2", ".orf", ".raf", ".rw2",
      ".pef", ".ptx", ".srw", ".rwl", ".dng", ".3fr", ".fff", ".iiq", ".mef", ".mos", ".raw",
  };
  for (const char* e : k) {
    if (eq_lower(ext, e)) return true;
  }
  return false;
}

inline std::string key_of(std::string_view name) {
  std::string k(stem(name));
  ascii_lower_inplace(k);
  return k;
}

}  // namespace pairing_detail

// Collapse a directory listing into navigation stops. Input is the extension-
// filtered scan (hidden/system already dropped). Output keeps original relative
// order of each group's primary.
inline std::vector<listed_item> pair_listing(std::vector<dir_entry> entries) {
  using namespace pairing_detail;
  std::vector<listed_item> out;
  std::vector<char> used(entries.size(), 0);

  auto take = [&](std::size_t i, std::size_t j, pair_kind kind, bool jpeg_is_primary) {
    listed_item item;
    if (jpeg_is_primary) {
      item.primary = std::move(entries[j]);
      item.secondary = std::move(entries[i]);
    } else {
      item.primary = std::move(entries[i]);
      item.secondary = std::move(entries[j]);
    }
    item.kind = kind;
    used[i] = 1;
    used[j] = 1;
    out.push_back(std::move(item));
  };

  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (used[i]) continue;
    const auto ext_i = extension(entries[i].name_utf8);
    const auto key_i = key_of(entries[i].name_utf8);

    std::vector<std::size_t> group;
    group.push_back(i);
    for (std::size_t j = i + 1; j < entries.size(); ++j) {
      if (used[j]) continue;
      if (key_of(entries[j].name_utf8) == key_i) group.push_back(j);
    }

    // Ambiguous: more than two files sharing a stem → show them all.
    if (group.size() > 2) {
      for (std::size_t g : group) {
        if (used[g]) continue;
        listed_item item;
        item.primary = std::move(entries[g]);
        used[g] = 1;
        out.push_back(std::move(item));
      }
      continue;
    }

    if (group.size() == 2) {
      const std::size_t a = group[0];
      const std::size_t b = group[1];
      const auto ext_a = extension(entries[a].name_utf8);
      const auto ext_b = extension(entries[b].name_utf8);
      const bool raw_jpeg = (is_raw(ext_a) && is_still_pair(ext_b)) ||
                            (is_raw(ext_b) && is_still_pair(ext_a));
      const bool live = (is_heic(ext_a) && is_mov(ext_b)) || (is_heic(ext_b) && is_mov(ext_a));
      if (raw_jpeg) {
        const bool jpeg_primary = is_still_pair(ext_b);
        take(a, b, pair_kind::raw_jpeg, jpeg_primary);
        continue;
      }
      if (live) {
        const bool heic_primary = is_heic(ext_a);
        take(a, b, pair_kind::live_photo, !heic_primary);
        continue;
      }
    }

    listed_item item;
    item.primary = std::move(entries[i]);
    used[i] = 1;
    out.push_back(std::move(item));
  }
  return out;
}

}  // namespace mv::io
