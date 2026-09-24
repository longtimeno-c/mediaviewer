// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/file.h"

#include <windows.h>

#include <algorithm>
#include <new>
#include <string>

namespace mv::io {

namespace {

constexpr std::uint64_t kMaxFileBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB

}  // namespace

result<std::vector<std::uint8_t>> read_prefix(std::string_view utf8_path, std::size_t max_bytes) {
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
  if (max_bytes == 0 && static_cast<std::uint64_t>(size.QuadPart) > kMaxFileBytes) {
    ::CloseHandle(file);
    return err(status::unsupported_format);
  }

  const auto bytes = max_bytes > 0 ? std::min(max_bytes, static_cast<std::size_t>(size.QuadPart))
                                   : static_cast<std::size_t>(size.QuadPart);
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

result<std::vector<std::uint8_t>> read_all(std::string_view path) { return read_prefix(path, 0); }

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

namespace {

bool widen(std::string_view utf8, std::wstring& out) {
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return false;
  out.assign(static_cast<std::size_t>(n), L'\0');
  return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                               static_cast<int>(utf8.size()), out.data(), n) > 0;
}

bool write_handle(HANDLE file, std::span<const std::uint8_t> bytes) noexcept {
  std::size_t filled = 0;
  while (filled < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (bytes.size() - filled) > 0x10000000u ? 0x10000000u : (bytes.size() - filled));
    DWORD written = 0;
    if (!::WriteFile(file, bytes.data() + filled, chunk, &written, nullptr) || written == 0) {
      return false;
    }
    filled += written;
  }
  return ::FlushFileBuffers(file) != FALSE;
}

}  // namespace

expected write_new(std::string_view utf8_path, std::span<const std::uint8_t> bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  std::wstring wide;
  if (!widen(utf8_path, wide)) return err(status::invalid_arg);
  HANDLE file = ::CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return err(status::io);
  const bool ok = write_handle(file, bytes);
  ::CloseHandle(file);
  if (!ok) {
    ::DeleteFileW(wide.c_str());
    return err(status::io);
  }
  return {};
}

expected replace_atomic(std::string_view utf8_path, std::span<const std::uint8_t> bytes) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  std::wstring wide;
  if (!widen(utf8_path, wide)) return err(status::invalid_arg);
  const DWORD attrs = ::GetFileAttributesW(wide.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) return err(status::io);

  // Sibling temporary on the same volume; ReplaceFileW swaps it in and keeps
  // the original's attributes, ACL and creation time.
  std::wstring temp;
  HANDLE file = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 100 && file == INVALID_HANDLE_VALUE; ++attempt) {
    temp = wide + L".mvtmp" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
           std::to_wstring(attempt);
    file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  if (file == INVALID_HANDLE_VALUE) return err(status::io);
  const bool ok = write_handle(file, bytes);
  ::CloseHandle(file);
  if (!ok || !::ReplaceFileW(wide.c_str(), temp.c_str(), nullptr,
                             REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
    ::DeleteFileW(temp.c_str());
    return err(status::io);
  }
  return {};
}

bool file_exists(std::string_view utf8_path) noexcept {
  if (utf8_path.empty()) return false;

  const int wide_n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                                           static_cast<int>(utf8_path.size()), nullptr, 0);
  if (wide_n <= 0) return false;

  std::wstring wide;
  try {
    wide.assign(static_cast<std::size_t>(wide_n), L'\0');
  } catch (const std::bad_alloc&) {
    return false;
  }
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_path.data(),
                            static_cast<int>(utf8_path.size()), wide.data(), wide_n) <= 0) {
    return false;
  }

  const DWORD attr = ::GetFileAttributesW(wide.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace mv::io
