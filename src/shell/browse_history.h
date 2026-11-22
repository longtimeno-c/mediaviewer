// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Back and forward through the folders the user has been to, as Finder and
// Explorer do (docs/design/16 "Folder navigation"). Pure, no I/O, no platform
// header: both hosts keep one, beside their browse_path.
//
// A place is a folder plus what was selected in it when it was left (the item,
// and the folder tile the keyboard was on), so going back lands where the user
// was, not on the first item. Every move to another folder (a tile, a crumb,
// up, a sibling, the tree, a sidebar place, Open) calls visit() with the place
// being left; back() and forward() hand back the place to open and take the
// one being left, so the two stacks always swap one entry. A result list is
// not a place: the host passes nothing while one is open.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mv::shell {

struct browse_place {
  std::string dir;
  std::string item;    // the selected item's path, or empty
  std::string folder;  // the folder tile the keyboard was on, or empty
};

class browse_history {
 public:
  // Enough for any walk a person makes; the oldest drop off first.
  static constexpr std::size_t kMaxDepth = 64;

  // Leaving `from` for `to`. Pushes `from` and clears forward, as a browser
  // does. Nothing open (`from.dir` empty) or the same folder again is no move.
  void visit(browse_place from, std::string_view to) {
    if (from.dir.empty() || same(from.dir, to)) return;
    push(back_, std::move(from));
    forward_.clear();
  }

  // The place to open on Back, with `here` saved for Forward; nullopt when
  // there is nowhere to go (then `here` is not recorded).
  [[nodiscard]] std::optional<browse_place> back(browse_place here) {
    return step(back_, forward_, std::move(here));
  }
  [[nodiscard]] std::optional<browse_place> forward(browse_place here) {
    return step(forward_, back_, std::move(here));
  }

  [[nodiscard]] bool can_back() const noexcept { return !back_.empty(); }
  [[nodiscard]] bool can_forward() const noexcept { return !forward_.empty(); }
  [[nodiscard]] std::size_t back_depth() const noexcept { return back_.size(); }
  [[nodiscard]] std::size_t forward_depth() const noexcept { return forward_.size(); }

  // A folder that has gone (deleted, renamed, its volume ejected) is dropped
  // from both stacks, with everything below it.
  void forget(std::string_view dir) {
    auto gone = [&](const browse_place& p) { return within(p.dir, dir); };
    std::erase_if(back_, gone);
    std::erase_if(forward_, gone);
  }

  void clear() noexcept {
    back_.clear();
    forward_.clear();
  }

 private:
  static bool is_sep(char c) noexcept { return c == '/' || c == '\\'; }

  // Trailing separators do not make another folder ("/a/" is "/a").
  static std::string_view trim(std::string_view p) noexcept {
    while (p.size() > 1 && is_sep(p.back()) && !(p.size() == 3 && p[1] == ':')) p.remove_suffix(1);
    return p;
  }
  static bool same(std::string_view a, std::string_view b) noexcept { return trim(a) == trim(b); }
  static bool within(std::string_view child, std::string_view parent) noexcept {
    child = trim(child);
    parent = trim(parent);
    if (parent.empty() || child.size() < parent.size() || child.substr(0, parent.size()) != parent) return false;
    return child.size() == parent.size() || is_sep(child[parent.size()]) || is_sep(parent.back());
  }

  static void push(std::vector<browse_place>& stack, browse_place p) {
    // The same folder twice in a row (back and forth by hand) is one entry.
    if (!stack.empty() && same(stack.back().dir, p.dir)) {
      stack.back() = std::move(p);
      return;
    }
    stack.push_back(std::move(p));
    if (stack.size() > kMaxDepth) stack.erase(stack.begin());
  }

  static std::optional<browse_place> step(std::vector<browse_place>& from, std::vector<browse_place>& to,
                                          browse_place here) {
    if (from.empty()) return std::nullopt;
    browse_place next = std::move(from.back());
    from.pop_back();
    if (!here.dir.empty() && !same(here.dir, next.dir)) push(to, std::move(here));
    return next;
  }

  std::vector<browse_place> back_;
  std::vector<browse_place> forward_;
};

}  // namespace mv::shell
