// SPDX-License-Identifier: GPL-2.0-or-later
// Copy / move collision naming (plan/16 "Marks, copy, move"): never overwrite,
// take `name (2).ext`, then `(3)`, … the way Explorer does.
//
// Pure: the existence check is the caller's, so this runs in tests without a
// filesystem and on the I/O pool without deciding anything about threads. UTF-8
// file names; no path separators are interpreted here.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace mv::io {

// `file_name` with " (n)" inserted before the last extension. A leading dot is
// part of the name, not an extension (".profile" → ".profile (2)"), and only
// the last extension moves ("a.tar.gz" → "a.tar (2).gz", as Explorer does).
[[nodiscard]] inline std::string collision_name(std::string_view file_name, int n) {
  std::size_t dot = file_name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) dot = file_name.size();
  std::string out;
  out.reserve(file_name.size() + 8);
  out.append(file_name.substr(0, dot));
  out.append(" (");
  out.append(std::to_string(n));
  out.push_back(')');
  out.append(file_name.substr(dot));
  return out;
}

// The first name that `exists` says is free: `file_name` itself, then (2), (3)
// … up to `max_attempts`. Returns an empty string if every candidate is taken,
// so the caller reports a failure instead of overwriting.
template <class Exists>
[[nodiscard]] std::string unique_name(std::string_view file_name, Exists&& exists,
                                      int max_attempts = 9999) {
  if (file_name.empty()) return {};
  if (!exists(std::string_view{file_name})) return std::string{file_name};
  for (int n = 2; n <= max_attempts; ++n) {
    std::string candidate = collision_name(file_name, n);
    if (!exists(std::string_view{candidate})) return candidate;
  }
  return {};
}

}  // namespace mv::io
