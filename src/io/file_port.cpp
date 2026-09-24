// SPDX-License-Identifier: GPL-2.0-or-later
// The portable part of io/file_port.h: buffers and path strings.
#include "io/file_port.h"

#include <new>
#include <utility>

namespace mv::io {

namespace {
#if defined(_WIN32)
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif
bool is_sep(char c) noexcept { return c == '/' || c == '\\'; }
}  // namespace

aligned_buffer::aligned_buffer(std::size_t bytes) noexcept {
  const std::size_t rounded = ((bytes + kIoAlign - 1) / kIoAlign) * kIoAlign;
  if (rounded == 0) return;
  data_ = static_cast<std::uint8_t*>(
      ::operator new(rounded, std::align_val_t{kIoAlign}, std::nothrow));
  if (data_) size_ = rounded;
}

aligned_buffer::~aligned_buffer() {
  if (data_) ::operator delete(data_, std::align_val_t{kIoAlign});
}

aligned_buffer::aligned_buffer(aligned_buffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

aligned_buffer& aligned_buffer::operator=(aligned_buffer&& other) noexcept {
  if (this != &other) {
    if (data_) ::operator delete(data_, std::align_val_t{kIoAlign});
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

std::string_view parent_of(std::string_view path) noexcept {
  std::size_t end = path.size();
  while (end > 1 && is_sep(path[end - 1])) --end;
  const std::string_view trimmed = path.substr(0, end);
  const auto slash = trimmed.find_last_of("/\\");
  if (slash == std::string_view::npos) return {};
  if (slash == 0) return trimmed.substr(0, 1);
  // "C:\" keeps its separator.
  if (slash == 2 && trimmed[1] == ':') return trimmed.substr(0, 3);
  return trimmed.substr(0, slash);
}

std::string_view file_name_of(std::string_view path) noexcept {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string join_path(std::string_view dir, std::string_view name) {
  std::string out;
  out.reserve(dir.size() + name.size() + 1);
  out.append(dir);
  if (!out.empty() && !is_sep(out.back())) out.push_back(kSep);
  while (!name.empty() && is_sep(name.front())) name.remove_prefix(1);
  out.append(name);
  return out;
}

std::string native_relative(std::string_view relative_slash) {
  std::string out(relative_slash);
  for (char& c : out) {
    if (is_sep(c)) c = kSep;
  }
  return out;
}

}  // namespace mv::io
