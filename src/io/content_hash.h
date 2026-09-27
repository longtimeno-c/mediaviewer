// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Content hashing for verified copies (plan/18-import.md "The engine").
//
// BLAKE3-256, taken under CC0 (plan/18: Apache-2.0 alone does not combine with
// GPL-2.0). A duplicate is decided by size first and this hash second, never
// by name. Portable; no platform header (D9). The hash of a user's file never
// leaves the machine (rule 6).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace mv::io {

struct content_hash {
  std::array<std::uint8_t, 32> bytes{};

  friend bool operator==(const content_hash&, const content_hash&) = default;

  // 64 lowercase hex digits.
  [[nodiscard]] std::string hex() const;
  // False, and `out` untouched, unless `hex` is exactly 64 hex digits.
  [[nodiscard]] static bool from_hex(std::string_view hex, content_hash& out) noexcept;
};

// Incremental BLAKE3. One per stream; not thread-safe.
class hasher {
 public:
  hasher();
  ~hasher();
  hasher(const hasher&) = delete;
  hasher& operator=(const hasher&) = delete;

  void update(std::span<const std::uint8_t> bytes) noexcept;
  // Finishes and resets, so one hasher can hash the next stream.
  [[nodiscard]] content_hash finish() noexcept;

 private:
  struct state;
  std::unique_ptr<state> state_;
};

// One-shot, for tests and small buffers.
[[nodiscard]] content_hash hash_bytes(std::span<const std::uint8_t> bytes);

}  // namespace mv::io
