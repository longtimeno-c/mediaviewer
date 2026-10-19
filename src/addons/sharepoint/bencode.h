// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Bencoding (BEP 3), the BitTorrent DHT's wire format (KRPC, BEP 5; BEP 44
// items), for Sharepoint's opt-in DHT (issue #472 PR 3). Strict where it
// matters for a hostile input: bounded depth and size, canonical integers,
// string lengths checked against what is left. Dictionaries are written with
// their keys sorted, as BEP 44's signatures need. Portable (D9).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mv::sharepoint::bencode {

struct value {
  enum class kind : std::uint8_t { integer, bytes, list, dict } k = kind::bytes;
  std::int64_t i = 0;
  std::string s;
  std::vector<value> l;
  std::vector<std::pair<std::string, value>> d;

  [[nodiscard]] const value* get(std::string_view key) const noexcept;
  [[nodiscard]] const std::string* str(std::string_view key) const noexcept;
  [[nodiscard]] std::optional<std::int64_t> integer(std::string_view key) const noexcept;

  static value num(std::int64_t v) {
    value x;
    x.k = kind::integer;
    x.i = v;
    return x;
  }
  static value bytes(std::string v) {
    value x;
    x.s = std::move(v);
    return x;
  }
  static value list(std::vector<value> v) {
    value x;
    x.k = kind::list;
    x.l = std::move(v);
    return x;
  }
  static value dict(std::vector<std::pair<std::string, value>> v) {
    value x;
    x.k = kind::dict;
    x.d = std::move(v);
    return x;
  }
};

inline constexpr int kMaxDepth = 8;
inline constexpr std::size_t kMaxInput = 64 * 1024;

// The whole of `in` as one value; nullopt for anything malformed or past the
// limits. `rest`: when given, a value at the start of `in` and the rest left.
[[nodiscard]] std::optional<value> decode(std::string_view in, std::string_view* rest = nullptr);
void encode(const value& v, std::string& out);
[[nodiscard]] std::string encode(const value& v);

}  // namespace mv::sharepoint::bencode
