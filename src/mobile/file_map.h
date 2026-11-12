// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A whole file, read-only, for as long as the object lives: a generation's tiny.pack and
// thumbs.idx. POSIX (the phone, the Mac): a read-only mmap. Windows, where src/mobile only
// runs in the tests: the file read into memory with the standard library, no Win32 (D9).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace mv::mobile {

class file_map {
 public:
  // Null when the file is missing, empty or cannot be read. `shared` maps MAP_SHARED
  // rather than MAP_PRIVATE (POSIX only; the bytes are the same for a file nobody writes).
  [[nodiscard]] static std::unique_ptr<file_map> open(const std::string& path, bool shared = false);
  ~file_map();
  file_map(const file_map&) = delete;
  file_map& operator=(const file_map&) = delete;

  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept { return {data_, size_}; }

 private:
  file_map() = default;
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  bool mapped_ = false;             // data_ is an mmap to undo
  std::vector<std::uint8_t> copy_;  // or the bytes themselves (Windows)
};

}  // namespace mv::mobile
