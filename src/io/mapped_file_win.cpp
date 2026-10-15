// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io/mapped_file.h over CreateFileMappingW / MapViewOfFile (D9: the Windows impl).
#include "io/mapped_file.h"

#include <windows.h>

#include <string>

namespace mv::io {
namespace {

constexpr std::uint64_t kMaxMapBytes = 1ull << 30;  // 1 GiB

}  // namespace

result<mapped_file> mapped_file::open(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const int wide_n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                                           static_cast<int>(utf8_path.size()), nullptr, 0);
  if (wide_n <= 0) return err(status::invalid_arg);
  std::wstring wide(static_cast<std::size_t>(wide_n), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                            static_cast<int>(utf8_path.size()), wide.data(), wide_n) <= 0) {
    return err(status::invalid_arg);
  }
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD e = ::GetLastError();
    return err(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? status::not_found : status::io);
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    ::CloseHandle(file);
    return err(status::io);
  }
  if (size.QuadPart == 0) {
    ::CloseHandle(file);
    return err(status::corrupt);
  }
  if (static_cast<std::uint64_t>(size.QuadPart) > kMaxMapBytes) {
    ::CloseHandle(file);
    return err(status::unsupported_format);
  }
  HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  ::CloseHandle(file);  // the mapping keeps the file referenced
  if (!mapping) return err(status::io);
  void* p = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  ::CloseHandle(mapping);  // the view keeps the mapping alive
  if (!p) return err(status::io);
  mapped_file f;
  f.data_ = p;
  f.size_ = static_cast<std::size_t>(size.QuadPart);
  return f;
}

void mapped_file::close() noexcept {
  if (data_) ::UnmapViewOfFile(data_);
  data_ = nullptr;
  size_ = 0;
}

}  // namespace mv::io
