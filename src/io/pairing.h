// SPDX-License-Identifier: GPL-2.0-or-later
// Scan-time pairing and companion filtering (plan/04, PR 7).
//
// RAW+JPEG: same basename, one filmstrip stop, JPEG/HEIC is the primary.
// Live Photo: HEIC+MOV same basename, still is primary, motion is the pair.
// iPhones set to "Most Compatible" write JPG+MOV for the same Live Photo, so a
// JPEG+MOV of one stem pairs the same way (plan/04 names HEIC; the JPG case is
// a PR 7 call, flagged for the decision log). MP4 never pairs.
// Ambiguous groups (three files, mixed stems) stay separate — never hide a
// file. Portable; no windows.h (D9).
//
// Stems match case-insensitively for ASCII only. DCF camera names are upper-
// case ASCII, so a real pair always matches; two non-ASCII stems that differ
// only in case are left as two stops, which is the safe failure (nothing
// hidden). Cost is O(n log n): the watcher re-runs this on every relist of a
// 2000-file dump.
#pragma once

#include <algorithm>
#include <cstdint>
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

// A RAW by its extension. The listing already filters by extension; this only
// drives the filmstrip badge, never a decode (decode probes magic bytes).
[[nodiscard]] inline bool is_raw_name(std::string_view name) noexcept {
  return pairing_detail::is_raw(pairing_detail::extension(name));
}

// Collapse a directory listing into navigation stops. Input is the extension-
// filtered scan (hidden/system already dropped), in the user's sort order.
// A pair sits where the first of its two files sat, so pairing never reorders
// the listing; everything unpaired keeps its own position.
inline std::vector<listed_item> pair_listing(std::vector<dir_entry> entries) {
  using namespace pairing_detail;
  const std::size_t n = entries.size();
  std::vector<std::string> keys;
  keys.reserve(n);
  for (const auto& e : entries) keys.push_back(key_of(e.name_utf8));

  // Group equal stems by sorting indices on (stem, original position).
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&keys](std::size_t a, std::size_t b) {
    const int c = keys[a].compare(keys[b]);
    return c != 0 ? c < 0 : a < b;
  });

  // One slot per original position; a pair fills its first member's slot.
  std::vector<listed_item> slots(n);
  std::vector<char> filled(n, 0);
  auto single = [&](std::size_t i) {
    slots[i].primary = std::move(entries[i]);
    filled[i] = 1;
  };

  for (std::size_t run = 0; run < n;) {
    std::size_t end = run + 1;
    while (end < n && keys[order[end]] == keys[order[run]]) ++end;

    if (end - run == 2) {
      const std::size_t a = order[run];      // earlier in the listing
      const std::size_t b = order[run + 1];
      const auto ext_a = extension(entries[a].name_utf8);
      const auto ext_b = extension(entries[b].name_utf8);
      pair_kind kind = pair_kind::none;
      bool a_primary = true;
      if (is_raw(ext_a) && is_still_pair(ext_b)) {
        kind = pair_kind::raw_jpeg;
        a_primary = false;
      } else if (is_raw(ext_b) && is_still_pair(ext_a)) {
        kind = pair_kind::raw_jpeg;
      } else if (is_still_pair(ext_a) && is_mov(ext_b)) {
        kind = pair_kind::live_photo;
      } else if (is_still_pair(ext_b) && is_mov(ext_a)) {
        kind = pair_kind::live_photo;
        a_primary = false;
      }
      if (kind != pair_kind::none) {
        listed_item& item = slots[a];
        item.primary = std::move(entries[a_primary ? a : b]);
        item.secondary = std::move(entries[a_primary ? b : a]);
        item.kind = kind;
        filled[a] = 1;
        run = end;
        continue;
      }
    }
    // One file, an unpairable two, or an ambiguous group: every file a stop.
    for (std::size_t k = run; k < end; ++k) single(order[k]);
    run = end;
  }

  std::vector<listed_item> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (filled[i]) out.push_back(std::move(slots[i]));
  }
  return out;
}

}  // namespace mv::io
