// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// SHA-256 (FIPS 180-4) for the model downloader (fetch.h): a fetched file is
// kept only if it hashes to what the signed manifest says. The add-on links
// nothing of the core, whose hash is libsodium's (docs/design/18).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace mv::ai {

class sha256 {
 public:
  sha256() noexcept;
  void update(std::span<const std::uint8_t> bytes) noexcept;
  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept;
  [[nodiscard]] std::string finish_hex();

 private:
  void block(const std::uint8_t* p) noexcept;
  std::array<std::uint32_t, 8> h_;
  std::array<std::uint8_t, 64> buf_{};
  std::size_t used_ = 0;
  std::uint64_t total_ = 0;
};

// The lowercase hex SHA-256 of a whole file; nullopt when it cannot be read.
[[nodiscard]] std::optional<std::string> sha256_file_hex(const std::string& path_utf8);
// The same, looking at `stop` before each 1 MB block: nullopt once it is set,
// so a stop does not wait out a file of gigabytes.
[[nodiscard]] std::optional<std::string> sha256_file_hex(const std::string& path_utf8,
                                                         const std::atomic<bool>& stop);

}  // namespace mv::ai
