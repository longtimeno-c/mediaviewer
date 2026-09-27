// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Where the user is inside a tree of folders (plan/10 PR 26): the breadcrumb
// trail from the highest folder they have reached down to the one on screen,
// and what "up" means. Pure string logic, no I/O, no platform header — both
// '/' and '\\' separate components, so the same rules hold for a POSIX path and
// a Windows / UNC path carried as UTF-8.
//
// Opening something from outside the tree (Open, a drop) starts a new trail
// with reset(). Moving through the tree (a folder tile, a crumb, up) uses
// visit(): the trail keeps its root while the destination is inside it, and
// grows upward when the destination is above it, so "up" never strands the
// user without a way back down.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mv::shell {

struct crumb {
  std::string name;  // last component; the root of a volume keeps its separator
  std::string path;
};

class browse_path {
 public:
  void reset(std::string_view dir) {
    root_ = trim(dir);
    current_ = root_;
  }

  void visit(std::string_view dir) {
    std::string next = trim(dir);
    if (root_.empty() || !within(next, root_)) root_ = next;
    current_ = std::move(next);
  }

  [[nodiscard]] const std::string& root() const noexcept { return root_; }
  [[nodiscard]] const std::string& current() const noexcept { return current_; }
  [[nodiscard]] bool empty() const noexcept { return current_.empty(); }

  // The folder above `current`, or empty at a volume root.
  [[nodiscard]] std::string parent() const { return parent_of(current_); }

  // root … current, inclusive. One entry per component below the root, plus
  // the root itself.
  [[nodiscard]] std::vector<crumb> crumbs() const {
    std::vector<crumb> out;
    if (current_.empty()) return out;
    std::vector<std::string> chain;
    for (std::string p = current_;; p = parent_of(p)) {
      chain.push_back(p);
      if (p == root_ || parent_of(p).empty()) break;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      out.push_back({leaf(*it), *it});
    }
    return out;
  }

  // `child` is `parent` itself or inside it.
  [[nodiscard]] static bool within(std::string_view child, std::string_view parent) {
    if (parent.empty()) return false;
    if (child.size() < parent.size() || child.substr(0, parent.size()) != parent) return false;
    if (child.size() == parent.size()) return true;
    return is_sep(child[parent.size()]) || is_sep(parent.back());
  }

  [[nodiscard]] static std::string parent_of(std::string_view path) {
    const std::string p = trim(path);
    const std::size_t cut = p.find_last_of("/\\");
    if (cut == std::string::npos) return {};
    if (cut == 0) return p.size() > 1 ? p.substr(0, 1) : std::string{};  // "/x" -> "/"
    // "C:\x" -> "C:\"; "C:\" and "//host/share" have no parent worth showing.
    if (cut == 2 && p[1] == ':') return p.size() > 3 ? p.substr(0, 3) : std::string{};
    return p.substr(0, cut);
  }

  [[nodiscard]] static std::string leaf(std::string_view path) {
    const std::string p = trim(path);
    const std::size_t cut = p.find_last_of("/\\");
    if (cut == std::string::npos) return p;
    if (cut + 1 >= p.size()) return p;  // a volume root
    return p.substr(cut + 1);
  }

 private:
  static bool is_sep(char c) noexcept { return c == '/' || c == '\\'; }

  // Drops trailing separators, except the one that makes a root a root.
  static std::string trim(std::string_view path) {
    std::string p(path);
    while (p.size() > 1 && is_sep(p.back()) && !(p.size() == 3 && p[1] == ':')) p.pop_back();
    return p;
  }

  std::string root_;
  std::string current_;
};

}  // namespace mv::shell
