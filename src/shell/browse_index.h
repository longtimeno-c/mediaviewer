// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Wrapping index arithmetic for folder navigation (plan/16-commands.md's
// Browse table: prev/next, first/last, skip ~10, all wrapping by default —
// "Wrap at end of folder: on by default, toggle in settings"). Pure logic,
// no I/O, no AppKit — deliberately unit-testable on its own rather than only
// exercisable through the AppKit glue that drives it.
#pragma once

#include <cstddef>

namespace mv::shell {

class browse_index {
 public:
  // Resets the count and clamps/sets the current index into range. `index`
  // beyond `count` clamps to the last item (folder_model's relist can shrink
  // the count out from under a selection that no longer exists).
  void reset(std::size_t count, std::size_t index = 0) noexcept;

  [[nodiscard]] std::size_t current() const noexcept { return index_; }
  [[nodiscard]] std::size_t count() const noexcept { return count_; }
  [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

  // "Wrap at the end of the folder" (plan/16, Settings). Off: next/prev/skip
  // stop at the ends instead of going round. On by default.
  void set_wrap(bool wrap) noexcept { wrap_ = wrap; }
  [[nodiscard]] bool wrap() const noexcept { return wrap_; }

  // Each returns the new current index. No-op (index unchanged, still
  // returns current()) when empty().
  std::size_t next() noexcept;                      // -> / D / Space
  std::size_t prev() noexcept;                       // <- / A / Backspace
  std::size_t first() noexcept;                      // Home
  std::size_t last() noexcept;                        // End
  // PageUp/PageDown: skip ~10, wrapping. Negative moves back.
  std::size_t skip(std::ptrdiff_t delta) noexcept;

 private:
  std::size_t count_ = 0;
  std::size_t index_ = 0;
  bool wrap_ = true;
};

}  // namespace mv::shell
