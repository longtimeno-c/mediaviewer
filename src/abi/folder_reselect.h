// SPDX-License-Identifier: GPL-2.0-or-later
// Which item a folder listing selects (plan/16, PR 6 verify: "a file dropped
// into the folder appears without restart").
//
// A fresh open selects the file that was asked for, else the first item. A
// watcher refresh keeps the item the user is on; if that file is gone (deleted
// to the Recycle Bin, moved with F8) the index stays put so the next item
// slides into view, or the last item when it was at the end. Pure, so the rule
// is tested without a filesystem.
//
// PR 7 pairs: an item is a stop with a primary and an optional secondary file
// (RAW+JPEG, Live Photo). Either half names the stop — opening the .NEF from
// Explorer lands on the JPEG+NEF stop, and a refresh that loses the JPEG keeps
// the user on the RAW that is left.
#pragma once

#include <cstdint>
#include <string_view>

namespace mv::abi {

template <class Items, class PathOf, class SecondaryOf>
[[nodiscard]] std::uint32_t reselect_pair(const Items& items, PathOf&& path_of,
                                          SecondaryOf&& secondary_of, bool changed,
                                          std::string_view previous_path,
                                          std::string_view previous_secondary,
                                          std::uint32_t previous_index,
                                          std::string_view wanted) {
  const auto n = static_cast<std::uint32_t>(items.size());
  if (n == 0) return 0;
  const bool keep_current = changed && !previous_path.empty();
  const std::string_view first = keep_current ? previous_path : wanted;
  const std::string_view second = keep_current ? previous_secondary : std::string_view{};
  const auto hit = [&](std::string_view s) {
    return !s.empty() && ((!first.empty() && s == first) || (!second.empty() && s == second));
  };
  if (!first.empty() || !second.empty()) {
    // A primary match wins over a secondary one: after a refresh the file the
    // user was looking at is the better anchor than its companion.
    for (std::uint32_t i = 0; i < n; ++i) {
      if (hit(std::string_view{path_of(items[i])})) return i;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      if (hit(std::string_view{secondary_of(items[i])})) return i;
    }
  }
  if (keep_current) return previous_index < n ? previous_index : n - 1;
  return 0;
}

template <class Items, class PathOf>
[[nodiscard]] std::uint32_t reselect(const Items& items, PathOf&& path_of, bool changed,
                                     std::string_view previous_path,
                                     std::uint32_t previous_index,
                                     std::string_view wanted) {
  return reselect_pair(
      items, path_of, [](const auto&) { return std::string_view{}; }, changed, previous_path,
      std::string_view{}, previous_index, wanted);
}

}  // namespace mv::abi
