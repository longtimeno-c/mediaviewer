// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The label vocabulary (plan/17 "Nothing found at scale", issue #85): 705
// everyday labels, embedded once per picture tower and cached. A row is a
// result only when the query scores at least like the row's ninth-best label
// (vector_store::set_labels): in a large library every string finds some
// photo that scores well against it, but gibberish rarely beats "keyboard" or
// "text" on the photos it lands on, while "a dog" beats every label but a few
// near-synonyms on a dog.
#pragma once

#include <span>
#include <string_view>

namespace mv::ai {

[[nodiscard]] std::span<const std::string_view> labels() noexcept;

// A row passes when fewer than this many labels beat the query on it.
inline constexpr std::size_t kLabelsAbove = 8;

}  // namespace mv::ai
