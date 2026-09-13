// SPDX-License-Identifier: GPL-2.0-or-later
// Folder stepping (plan/16 "Wrap at end of folder: on by default, toggle in
// settings"). Pure, shared by arrow keys / Space / A-D and the slideshow.
#pragma once

#include <cstdint>
#include <optional>

namespace mv::shell {

// The index `delta` items from `selected` in a folder of `count`, or nothing
// when that runs off an end and wrapping is off (the key then does nothing).
[[nodiscard]] constexpr std::optional<std::uint32_t> step_index(std::uint32_t selected, int delta,
                                                                std::uint32_t count,
                                                                bool wrap) noexcept {
  if (count == 0) return std::nullopt;
  const std::int64_t next = static_cast<std::int64_t>(selected) + delta;
  if (next >= 0 && next < static_cast<std::int64_t>(count)) {
    return static_cast<std::uint32_t>(next);
  }
  if (!wrap) return std::nullopt;
  const std::int64_t n = static_cast<std::int64_t>(count);
  return static_cast<std::uint32_t>(((next % n) + n) % n);
}

}  // namespace mv::shell
