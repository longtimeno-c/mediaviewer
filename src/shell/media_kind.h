// SPDX-License-Identifier: GPL-2.0-or-later
// Is this listing entry a clip (played through player/) rather than a still
// (decoded through image/)? By extension only: the folder listing filters by
// extension too, and the player probes the real container when it opens it.
// Same set as io/dir_mac.cpp's video containers (D5).
#pragma once

#include <cstring>
#include <string_view>

namespace mv::shell {

[[nodiscard]] inline bool is_video_name(std::string_view name) noexcept {
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size()) return false;
  char ext[8]{};
  const std::size_t n = name.size() - dot;
  if (n >= sizeof(ext)) return false;
  for (std::size_t i = 0; i < n; ++i) {
    char c = name[dot + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    ext[i] = c;
  }
  static constexpr const char* kVideoExts[] = {".mp4", ".mov", ".mkv", ".webm", ".avi", ".ts", ".m4v"};
  for (const char* e : kVideoExts) {
    if (std::strcmp(ext, e) == 0) return true;
  }
  return false;
}

}  // namespace mv::shell
