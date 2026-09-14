// SPDX-License-Identifier: GPL-2.0-or-later
// Which item a folder listing selects (plan/16, PR 6 verify: "a file dropped
// into the folder appears without restart").
//
// A fresh open selects the file that was asked for, else the first item. A
// watcher refresh keeps the item the user is on; if that file is gone (deleted
// to the Recycle Bin, moved with F8) the index stays put so the next item
// slides into view, or the last item when it was at the end. Pure, so the rule
// is tested without a filesystem.
#pragma once

#include <cstdint>
#include <string_view>

namespace mv::abi {

template <class Items, class PathOf>
[[nodiscard]] std::uint32_t reselect(const Items& items, PathOf&& path_of, bool changed,
                                     std::string_view previous_path,
                                     std::uint32_t previous_index,
                                     std::string_view wanted) {
  const auto n = static_cast<std::uint32_t>(items.size());
  if (n == 0) return 0;
  const bool keep_current = changed && !previous_path.empty();
  const std::string_view target = keep_current ? previous_path : wanted;
  if (!target.empty()) {
    for (std::uint32_t i = 0; i < n; ++i) {
      if (std::string_view{path_of(items[i])} == target) return i;
    }
  }
  if (keep_current) return previous_index < n ? previous_index : n - 1;
  return 0;
}

}  // namespace mv::abi
