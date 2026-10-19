// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The agent's scope (docs/design/29 "Scope", owner 2026-10-18): the media the
// timeline plays plus the folders the editor gave. Nothing else: no
// library-wide search, no Photos library (`photos:` keys are refused), no
// camera roll, no path outside. The server checks every path a client names
// and every result before a client sees it, so the scope does not depend on
// the model behaving.
//
// Paths are compared after resolving `.`, `..` and symbolic links, so a link
// inside a given folder that points outside it is outside. Case-insensitive
// on Windows and macOS (their default file systems). Portable C++.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mv::agent {

class scope {
 public:
  scope() = default;
  scope(std::vector<std::string> media, std::vector<std::string> folders);

  enum class verdict { inside, photos, relative, outside, empty };
  [[nodiscard]] verdict check(std::string_view path) const;
  [[nodiscard]] bool allows(std::string_view path) const { return check(path) == verdict::inside; }
  // The sentence a refusal says.
  [[nodiscard]] static std::string refusal(verdict v);

  [[nodiscard]] const std::vector<std::string>& media() const noexcept { return media_; }
  [[nodiscard]] const std::vector<std::string>& folders() const noexcept { return folders_; }
  // Folders the media sit in (searched one level, then filtered to the media).
  [[nodiscard]] std::vector<std::string> media_dirs() const;

 private:
  std::vector<std::string> media_;    // as given
  std::vector<std::string> folders_;  // as given
  std::vector<std::string> media_key_, folder_key_;  // resolved, comparable
};

// A path resolved for comparison: absolute, `.`/`..` and links resolved where
// they exist, '/' separators, lower-cased where the file system folds case.
// "" when it is not an absolute path.
[[nodiscard]] std::string comparable(std::string_view path);

}  // namespace mv::agent
