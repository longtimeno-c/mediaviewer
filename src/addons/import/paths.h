// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Path strings inside the add-on. It links no part of the core, so it keeps
// its own two-line join rather than io/file_port.h's.
#pragma once

#include <string>
#include <string_view>

namespace mv::import {

#if defined(_WIN32)
inline constexpr char kNativeSep = '\\';
#else
inline constexpr char kNativeSep = '/';
#endif

// `root` + native separator + `rel` ('/'-separated) with native separators.
[[nodiscard]] inline std::string join_native(std::string_view root, std::string_view rel) {
  std::string out(root);
  if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back(kNativeSep);
  for (char c : rel) out.push_back(c == '/' || c == '\\' ? kNativeSep : c);
  return out;
}

[[nodiscard]] inline std::string parent_native(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  if (slash == std::string_view::npos) return {};
  if (slash == 0) return std::string(path.substr(0, 1));
  if (slash == 2 && path.size() > 1 && path[1] == ':') return std::string(path.substr(0, 3));
  return std::string(path.substr(0, slash));
}

}  // namespace mv::import
