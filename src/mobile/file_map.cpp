// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mobile/file_map.h"

#if defined(_WIN32)
#include <filesystem>
#include <fstream>
#include <iterator>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mv::mobile {

#if defined(_WIN32)

std::unique_ptr<file_map> file_map::open(const std::string& path, bool /*shared*/) {
  // The path is UTF-8, as everywhere in the core.
  std::ifstream f(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())),
                  std::ios::binary);
  if (!f) return nullptr;
  std::unique_ptr<file_map> m(new file_map());
  m->copy_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  if (f.bad() || m->copy_.empty()) return nullptr;
  m->data_ = m->copy_.data();
  m->size_ = m->copy_.size();
  return m;
}

file_map::~file_map() = default;

#else

std::unique_ptr<file_map> file_map::open(const std::string& path, bool shared) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  struct stat st {};
  void* data = MAP_FAILED;
  if (::fstat(fd, &st) == 0 && st.st_size > 0) {
    data = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
  }
  ::close(fd);
  if (data == MAP_FAILED) return nullptr;
  std::unique_ptr<file_map> m(new file_map());
  m->data_ = static_cast<const std::uint8_t*>(data);
  m->size_ = static_cast<std::size_t>(st.st_size);
  m->mapped_ = true;
  return m;
}

file_map::~file_map() {
  if (mapped_) ::munmap(const_cast<std::uint8_t*>(data_), size_);
}

#endif

}  // namespace mv::mobile
