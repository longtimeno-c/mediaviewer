// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/sort_order.h"

#include <algorithm>
#include <tuple>

namespace mv::io {
namespace {

char fold(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int compare_casefold(std::string_view a, std::string_view b) noexcept {
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const char ca = fold(a[i]), cb = fold(b[i]);
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

std::string_view extension_of(std::string_view name) noexcept {
  const std::size_t dot = name.find_last_of('.');
  return dot == std::string_view::npos || dot == 0 ? std::string_view{} : name.substr(dot + 1);
}

int compare_ints(std::int64_t a, std::int64_t b) noexcept { return a == b ? 0 : (a < b ? -1 : 1); }

}  // namespace

const char* sort_key_label(sort_key k) noexcept {
  switch (k) {
    case sort_key::name: return "Name";
    case sort_key::modified: return "Date modified";
    case sort_key::size: return "Size";
    case sort_key::type: return "Type";
    case sort_key::date_taken: return "Date taken";
    case sort_key::count: break;
  }
  return "Name";
}

void sort_entries(std::vector<io::dir_entry>& entries, sort_order order,
                  const date_lookup_fn& dates) {
  const auto primary = [&](const io::dir_entry& a, const io::dir_entry& b) -> int {
    switch (order.key) {
      case sort_key::name: return compare_casefold(a.name_utf8, b.name_utf8);
      case sort_key::modified: return compare_ints(a.mtime_unix, b.mtime_unix);
      case sort_key::size:
        return compare_ints(static_cast<std::int64_t>(a.size), static_cast<std::int64_t>(b.size));
      case sort_key::type:
        return compare_casefold(extension_of(a.name_utf8), extension_of(b.name_utf8));
      case sort_key::date_taken: {
        const auto da = dates ? dates(a) : std::nullopt;
        const auto db = dates ? dates(b) : std::nullopt;
        return compare_ints(da.value_or(a.mtime_unix), db.value_or(b.mtime_unix));
      }
      case sort_key::count: break;
    }
    return 0;
  };
  // Descending flips the primary key only; the tie-breakers stay ascending so
  // equal items do not reverse relative to each other.
  std::stable_sort(entries.begin(), entries.end(),
                   [&](const io::dir_entry& a, const io::dir_entry& b) {
                     int c = primary(a, b);
                     if (order.descending) c = -c;
                     if (c != 0) return c < 0;
                     c = compare_casefold(a.name_utf8, b.name_utf8);
                     if (c != 0) return c < 0;
                     return a.path_utf8 < b.path_utf8;
                   });
}

std::int32_t pack_sort(sort_order o) noexcept {
  return static_cast<std::int32_t>(o.key) | (o.descending ? 8 : 0);
}

sort_order unpack_sort(std::int32_t v) noexcept {
  sort_order o;
  const std::int32_t k = v & 7;
  o.key = k < static_cast<std::int32_t>(sort_key::count) ? static_cast<sort_key>(k) : sort_key::name;
  o.descending = (v & 8) != 0;
  return o;
}

}  // namespace mv::io
