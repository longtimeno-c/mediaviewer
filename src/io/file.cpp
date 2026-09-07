// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/file.h"

#include <windows.h>

#include <new>
#include <string>

namespace mv::io {

namespace {

constexpr std::uint64_t kMaxFileBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB

}  // namespace

result<std::vector<std::uint8_t>> read_all(std::string_view utf8_path) {
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
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) return err(status::io);

  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    ::CloseHandle(file);
    return err(status::io);
  }
  if (size.QuadPart == 0) {
    ::CloseHandle(file);
    return err(status::corrupt);
  }
  if (static_cast<std::uint64_t>(size.QuadPart) > kMaxFileBytes) {
    ::CloseHandle(file);
    return err(status::unsupported_format);
  }

  const auto bytes = static_cast<std::size_t>(size.QuadPart);
  std::vector<std::uint8_t> buffer;
  try {
    buffer.resize(bytes);
  } catch (const std::bad_alloc&) {
    ::CloseHandle(file);
    return err(status::out_of_memory);
  }

  std::size_t filled = 0;
  while (filled < bytes) {
    const DWORD chunk = static_cast<DWORD>(
        (bytes - filled) > 0x10000000u ? 0x10000000u : (bytes - filled));
    DWORD read = 0;
    if (!::ReadFile(file, buffer.data() + filled, chunk, &read, nullptr) || read == 0) {
      ::CloseHandle(file);
      return err(status::io);
    }
    filled += read;
  }

  ::CloseHandle(file);
  return buffer;
}

expected write_all(std::string_view utf8_path, std::span<const std::uint8_t> bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);

  const int wide_n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                                           static_cast<int>(utf8_path.size()), nullptr, 0);
  if (wide_n <= 0) return err(status::invalid_arg);

  std::wstring wide(static_cast<std::size_t>(wide_n), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                            static_cast<int>(utf8_path.size()), wide.data(), wide_n) <= 0) {
    return err(status::invalid_arg);
  }

  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return err(status::io);

  std::size_t filled = 0;
  const std::size_t total = bytes.size();
  while (filled < total) {
    const DWORD chunk = static_cast<DWORD>(
        (total - filled) > 0x10000000u ? 0x10000000u : (total - filled));
    DWORD written = 0;
    if (!::WriteFile(file, bytes.data() + filled, chunk, &written, nullptr) || written == 0) {
      ::CloseHandle(file);
      return err(status::io);
    }
    filled += written;
  }
  ::CloseHandle(file);
  return {};
}

}  // namespace mv::io
