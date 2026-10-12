// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #290: shared pieces of the portable readers (parse.h). Not part of the meta API.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mv::meta::parse::util {

using bytes = std::span<const std::uint8_t>;

// Big- and little-endian loads at `at`; the caller has checked the bounds.
[[nodiscard]] inline std::uint16_t be16(bytes b, std::size_t at) noexcept {
  return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}
[[nodiscard]] inline std::uint32_t be32(bytes b, std::size_t at) noexcept {
  return (static_cast<std::uint32_t>(b[at]) << 24) | (static_cast<std::uint32_t>(b[at + 1]) << 16) |
         (static_cast<std::uint32_t>(b[at + 2]) << 8) | static_cast<std::uint32_t>(b[at + 3]);
}
[[nodiscard]] inline std::uint64_t be64(bytes b, std::size_t at) noexcept {
  return (static_cast<std::uint64_t>(be32(b, at)) << 32) | be32(b, at + 4);
}
[[nodiscard]] inline std::uint16_t le16(bytes b, std::size_t at) noexcept {
  return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
[[nodiscard]] inline std::uint32_t le24(bytes b, std::size_t at) noexcept {
  return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
         (static_cast<std::uint32_t>(b[at + 2]) << 16);
}
[[nodiscard]] inline std::uint32_t le32(bytes b, std::size_t at) noexcept {
  return le24(b, at) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

// True when [at, at + n) lies inside `b` (overflow-safe).
[[nodiscard]] inline bool has(bytes b, std::size_t at, std::size_t n) noexcept {
  return at <= b.size() && n <= b.size() - at;
}

[[nodiscard]] inline std::string_view chars(bytes b) noexcept {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// A four-character code as text, non-printing bytes as '?'.
[[nodiscard]] inline std::string fourcc(bytes b, std::size_t at) {
  std::string s(4, '?');
  for (std::size_t i = 0; i < 4; ++i) {
    const std::uint8_t c = b[at + i];
    if (c >= 0x20 && c < 0x7F) s[i] = static_cast<char>(c);
  }
  return s;
}

[[nodiscard]] inline bool fourcc_is(bytes b, std::size_t at, const char* code) noexcept {
  return has(b, at, 4) && b[at] == static_cast<std::uint8_t>(code[0]) &&
         b[at + 1] == static_cast<std::uint8_t>(code[1]) && b[at + 2] == static_cast<std::uint8_t>(code[2]) &&
         b[at + 3] == static_cast<std::uint8_t>(code[3]);
}

// The NUL-terminated string at `at` (bounded by the span); `next` is set past the NUL,
// or to b.size() when there is none.
[[nodiscard]] inline std::string_view cstr(bytes b, std::size_t at, std::size_t& next) noexcept {
  if (at >= b.size()) {
    next = b.size();
    return {};
  }
  std::size_t end = at;
  while (end < b.size() && b[end] != 0) ++end;
  next = end < b.size() ? end + 1 : b.size();
  return {reinterpret_cast<const char*>(b.data() + at), end - at};
}

[[nodiscard]] std::string number_text(double v);

}  // namespace mv::meta::parse::util
