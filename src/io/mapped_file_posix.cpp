// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io/mapped_file.h over POSIX mmap (macOS, Linux).
#include "io/mapped_file.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>

namespace mv::io {
namespace {

constexpr std::uint64_t kMaxMapBytes = 1ull << 30;  // 1 GiB

}  // namespace

result<mapped_file> mapped_file::open(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::string path(utf8_path);
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return err(errno == ENOENT ? status::not_found : status::io);
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return err(status::io);
  }
  if (st.st_size <= 0) {
    ::close(fd);
    return err(status::corrupt);
  }
  if (static_cast<std::uint64_t>(st.st_size) > kMaxMapBytes) {
    ::close(fd);
    return err(status::unsupported_format);
  }
  const auto size = static_cast<std::size_t>(st.st_size);
  void* p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);  // the mapping keeps the file referenced
  if (p == MAP_FAILED) return err(status::io);
  // The callers read the whole mapping soon after (a checksum, then lookups);
  // asking for it up front turns a thousand page faults into read-ahead.
  (void)::madvise(p, size, MADV_WILLNEED);
  mapped_file f;
  f.data_ = p;
  f.size_ = size;
  return f;
}

void mapped_file::close() noexcept {
  if (data_) ::munmap(const_cast<void*>(data_), size_);
  data_ = nullptr;
  size_ = 0;
}

}  // namespace mv::io
