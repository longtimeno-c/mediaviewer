// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/browse_index.h"

namespace mv::shell {

void browse_index::reset(std::size_t count, std::size_t index) noexcept {
  count_ = count;
  index_ = count_ == 0 ? 0 : (index < count_ ? index : count_ - 1);
}

std::size_t browse_index::skip(std::ptrdiff_t delta) noexcept {
  if (count_ == 0) return index_;
  if (!wrap_) {
    // Clamp at the ends: the key does nothing past the last / first item.
    const auto last = static_cast<std::ptrdiff_t>(count_) - 1;
    const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(index_) + delta;
    index_ = static_cast<std::size_t>(target < 0 ? 0 : (target > last ? last : target));
    return index_;
  }
  // Reduce delta into [0, count_) first so a big skip (or a negative one)
  // never risks signed/unsigned overflow doing arithmetic on index_ directly.
  const auto n = static_cast<std::ptrdiff_t>(count_);
  std::ptrdiff_t step = delta % n;
  if (step < 0) step += n;
  const auto cur = static_cast<std::ptrdiff_t>(index_);
  std::ptrdiff_t moved = (cur + step) % n;
  if (moved < 0) moved += n;
  index_ = static_cast<std::size_t>(moved);
  return index_;
}

std::size_t browse_index::next() noexcept { return skip(1); }
std::size_t browse_index::prev() noexcept { return skip(-1); }

std::size_t browse_index::first() noexcept {
  if (count_ != 0) index_ = 0;
  return index_;
}

std::size_t browse_index::last() noexcept {
  if (count_ != 0) index_ = count_ - 1;
  return index_;
}

}  // namespace mv::shell
