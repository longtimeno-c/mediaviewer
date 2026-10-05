// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Reading entries out of a zip held in memory — enough for an Office Open XML
// package (DOCX, docs/plans/audio-and-documents.md §2.5). Stored and deflate
// entries only; no zip64, no encryption, no multi-disk. Every size is checked
// against the buffer and a cap before anything is allocated: a zip bomb is an
// error, not an allocation.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"

namespace mv::codec::zip {

// The largest entry read() will inflate.
inline constexpr std::size_t kMaxEntryBytes = std::size_t{64} << 20;

struct entry {
  std::string name;
  std::uint16_t method = 0;           // 0 stored, 8 deflate
  std::uint32_t compressed = 0;
  std::uint32_t size = 0;             // uncompressed
  std::uint32_t local_offset = 0;
};

class archive {
 public:
  // Reads the central directory. `corrupt` when there is none to be found.
  [[nodiscard]] static result<archive> open(std::span<const std::uint8_t> bytes);

  [[nodiscard]] const std::vector<entry>& entries() const noexcept { return entries_; }
  [[nodiscard]] const entry* find(std::string_view name) const noexcept;

  // The entry's bytes, inflated. `unsupported_format` for a method other than
  // stored or deflate; `corrupt` when the data does not inflate to its size.
  [[nodiscard]] result<std::vector<std::uint8_t>> read(const entry& e,
                                                       std::size_t cap = kMaxEntryBytes) const;
  [[nodiscard]] result<std::vector<std::uint8_t>> read(std::string_view name,
                                                       std::size_t cap = kMaxEntryBytes) const;

 private:
  std::span<const std::uint8_t> bytes_;
  std::vector<entry> entries_;
};

}  // namespace mv::codec::zip
