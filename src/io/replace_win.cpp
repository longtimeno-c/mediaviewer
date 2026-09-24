// SPDX-License-Identifier: GPL-2.0-or-later
// Windows half of the io replace port (io/replace.h): CREATE_NEW for an
// export, a sibling temporary + ReplaceFileW for the viewer's lossless rotate.
#include "io/replace.h"

#include <windows.h>

#include <string>

namespace mv::io {
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

}  // namespace mv::io
