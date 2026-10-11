// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A read-only memory-mapped file. Worker threads only: opening touches the
// file system (docs/design/02-architecture.md). Portable header; the POSIX
// impl is mapped_file_posix.cpp and the Windows impl mapped_file_win.cpp (D9).
//
// Used for large immutable data read in place (the place database,
// docs/design/06 "Place"): the OS pages it in on demand and shares the pages
// between processes, so nothing is copied onto the heap.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/result.h"

namespace mv::io {

class mapped_file {
 public:
  mapped_file() noexcept = default;
  mapped_file(mapped_file&& other) noexcept : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
  }
  mapped_file& operator=(mapped_file&& other) noexcept {
    if (this != &other) {
      close();
      data_ = other.data_;
      size_ = other.size_;
      other.data_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }
  mapped_file(const mapped_file&) = delete;
  mapped_file& operator=(const mapped_file&) = delete;
  ~mapped_file() { close(); }

  // Maps the whole file at `utf8_path` read-only. A missing file is
  // status::not_found, an empty one status::corrupt, one over 1 GiB
  // status::unsupported_format. The path is never logged (rule 6). The
  // mapping is page-aligned.
  [[nodiscard]] static result<mapped_file> open(std::string_view utf8_path);

  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
    return {static_cast<const std::uint8_t*>(data_), size_};
  }

 private:
  void close() noexcept;
  const void* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace mv::io
