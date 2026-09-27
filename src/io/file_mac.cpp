// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "io/file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <new>
#include <string>

namespace mv::io {
namespace {

constexpr std::uint64_t kMaxFileBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB

}  // namespace

result<std::vector<std::uint8_t>> read_prefix(std::string_view utf8_path, std::size_t max_bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);

  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return err(status::io);

  struct stat st{};
  if (::fstat(fd, &st) != 0 || st.st_size < 0) {
    ::close(fd);
    return err(status::io);
  }
  if (st.st_size == 0) {
    ::close(fd);
    return err(status::corrupt);
  }
  if (max_bytes == 0 && static_cast<std::uint64_t>(st.st_size) > kMaxFileBytes) {
    ::close(fd);
    return err(status::unsupported_format);
  }

  const std::size_t bytes = max_bytes > 0
                                ? std::min(max_bytes, static_cast<std::size_t>(st.st_size))
                                : static_cast<std::size_t>(st.st_size);
  std::vector<std::uint8_t> buffer;
  try {
    buffer.resize(bytes);
  } catch (const std::bad_alloc&) {
    ::close(fd);
    return err(status::out_of_memory);
  }

  std::size_t filled = 0;
  while (filled < bytes) {
    const ssize_t n = ::read(fd, buffer.data() + filled, bytes - filled);
    if (n <= 0) {
      ::close(fd);
      return err(status::io);
    }
    filled += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return buffer;
}

result<std::vector<std::uint8_t>> read_all(std::string_view path) { return read_prefix(path, 0); }

expected write_all(std::string_view utf8_path, std::span<const std::uint8_t> bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);

  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return err(status::io);

  std::size_t filled = 0;
  const std::size_t total = bytes.size();
  while (filled < total) {
    const ssize_t n = ::write(fd, bytes.data() + filled, total - filled);
    if (n <= 0) {
      ::close(fd);
      return err(status::io);
    }
    filled += static_cast<std::size_t>(n);
  }
  ::close(fd);
  return {};
}

bool file_exists(std::string_view utf8_path) noexcept {
  if (utf8_path.empty()) return false;
  struct stat st{};
  if (::stat(std::string(utf8_path).c_str(), &st) != 0) return false;
  return S_ISREG(st.st_mode);
}

}  // namespace mv::io
