// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/dir.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <new>
#include <thread>

namespace mv::io {
namespace {

std::wstring wide_from_utf8(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), out.data(), n) <= 0) {
    return {};
  }
  return out;
}

std::string utf8_from_wide(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string out(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
  return out;
}

std::int64_t unix_from_filetime(FILETIME ft) noexcept {
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  constexpr std::uint64_t kEpochDiff = 11644473600ull;
  return static_cast<std::int64_t>(u.QuadPart / 10000000ull - kEpochDiff);
}

bool still_extension(std::wstring_view name) noexcept {
  const auto dot = name.find_last_of(L'.');
  if (dot == std::wstring_view::npos || dot + 1 >= name.size()) return false;
  wchar_t ext[8]{};
  const std::size_t n = name.size() - dot;
  if (n >= 8) return false;
  for (std::size_t i = 0; i < n; ++i) {
    wchar_t c = name[dot + i];
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    ext[i] = c;
  }
  return std::wcscmp(ext, L".jpg") == 0 || std::wcscmp(ext, L".jpeg") == 0 ||
         std::wcscmp(ext, L".png") == 0 || std::wcscmp(ext, L".bmp") == 0;
}

std::string join_utf8(std::string_view dir, std::string_view name) {
  if (dir.empty()) return std::string(name);
  const char last = dir.back();
  if (last == '\\' || last == '/') {
    std::string out;
    out.reserve(dir.size() + name.size());
    out.append(dir);
    out.append(name);
    return out;
  }
  std::string out;
  out.reserve(dir.size() + 1 + name.size());
  out.append(dir);
  out.push_back('\\');
  out.append(name);
  return out;
}

}  // namespace

result<std::vector<dir_entry>> list_still_files(std::string_view utf8_dir) {
  if (utf8_dir.empty()) return err(status::invalid_arg);
  const std::wstring wide = wide_from_utf8(utf8_dir);
  if (wide.empty()) return err(status::invalid_arg);

  std::wstring glob = wide;
  if (glob.back() != L'\\' && glob.back() != L'/') glob.push_back(L'\\');
  glob.push_back(L'*');

  WIN32_FIND_DATAW fd{};
  HANDLE find = ::FindFirstFileExW(glob.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) return err(status::io);

  std::vector<dir_entry> out;
  try {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      if (fd.cFileName[0] == L'.' &&
          (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
        continue;
      }
      if (!still_extension(fd.cFileName)) continue;

      dir_entry e;
      e.name_utf8 = utf8_from_wide(fd.cFileName);
      e.path_utf8 = join_utf8(utf8_dir, e.name_utf8);
      LARGE_INTEGER sz{};
      sz.LowPart = fd.nFileSizeLow;
      sz.HighPart = static_cast<LONG>(fd.nFileSizeHigh);
      e.size = static_cast<std::uint64_t>(sz.QuadPart);
      e.mtime_unix = unix_from_filetime(fd.ftLastWriteTime);
      out.push_back(std::move(e));
    } while (::FindNextFileW(find, &fd));
  } catch (const std::bad_alloc&) {
    ::FindClose(find);
    return err(status::out_of_memory);
  }
  ::FindClose(find);

  std::sort(out.begin(), out.end(), [](const dir_entry& a, const dir_entry& b) {
    return ::CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, a.name_utf8.c_str(), -1,
                            b.name_utf8.c_str(), -1) == CSTR_LESS_THAN;
  });
  return out;
}

result<bool> is_directory(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  const std::wstring wide = wide_from_utf8(utf8_path);
  if (wide.empty()) return err(status::invalid_arg);
  const DWORD attr = ::GetFileAttributesW(wide.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) return err(status::io);
  return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

result<std::string> containing_dir(std::string_view utf8_path) {
  auto dir = is_directory(utf8_path);
  if (!dir) return err(dir.error());
  if (dir.value()) return std::string(utf8_path);

  std::size_t slash = utf8_path.find_last_of("\\/");
  if (slash == std::string_view::npos) return err(status::invalid_arg);
  if (slash == 0) return std::string(utf8_path.substr(0, 1));
  return std::string(utf8_path.substr(0, slash));
}

struct directory_watcher::impl {
  HANDLE dir = INVALID_HANDLE_VALUE;
  HANDLE stop = nullptr;
  std::thread thread;
  callback cb = nullptr;
  void* user = nullptr;
  std::atomic<bool> running{false};
  alignas(8) std::uint8_t buf[65536]{};
};

directory_watcher::directory_watcher() = default;
directory_watcher::~directory_watcher() { stop(); }

expected directory_watcher::start(std::string_view utf8_dir, callback cb, void* user) {
  stop();
  if (utf8_dir.empty() || !cb) return err(status::invalid_arg);
  const std::wstring wide = wide_from_utf8(utf8_dir);
  if (wide.empty()) return err(status::invalid_arg);

  auto next = std::make_unique<impl>();
  next->cb = cb;
  next->user = user;
  next->stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!next->stop) return err(status::internal);

  next->dir = ::CreateFileW(wide.c_str(), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
  if (next->dir == INVALID_HANDLE_VALUE) {
    ::CloseHandle(next->stop);
    return err(status::io);
  }

  next->running.store(true, std::memory_order_release);
  impl* raw = next.get();
  try {
    next->thread = std::thread([raw] {
      OVERLAPPED ov{};
      ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!ov.hEvent) return;

      while (raw->running.load(std::memory_order_acquire)) {
        ::ResetEvent(ov.hEvent);
        DWORD dummy = 0;
        const BOOL ok = ::ReadDirectoryChangesW(
            raw->dir, raw->buf, static_cast<DWORD>(sizeof(raw->buf)), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &dummy, &ov, nullptr);
        if (!ok) {
          if (::GetLastError() != ERROR_IO_PENDING) break;
        }

        HANDLE waits[2] = {raw->stop, ov.hEvent};
        const DWORD wr = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0) {
          ::CancelIoEx(raw->dir, &ov);
          break;
        }
        DWORD transferred = 0;
        if (!::GetOverlappedResult(raw->dir, &ov, &transferred, FALSE)) continue;
        if (transferred == 0) continue;
        if (raw->cb) raw->cb(raw->user);
      }

      if (ov.hEvent) ::CloseHandle(ov.hEvent);
    });
  } catch (const std::bad_alloc&) {
    ::CloseHandle(next->dir);
    ::CloseHandle(next->stop);
    return err(status::out_of_memory);
  }

  impl_ = std::move(next);
  return {};
}

void directory_watcher::stop() noexcept {
  if (!impl_) return;
  impl_->running.store(false, std::memory_order_release);
  if (impl_->stop) ::SetEvent(impl_->stop);
  if (impl_->dir != INVALID_HANDLE_VALUE) ::CancelIoEx(impl_->dir, nullptr);
  if (impl_->thread.joinable()) impl_->thread.join();
  if (impl_->dir != INVALID_HANDLE_VALUE) ::CloseHandle(impl_->dir);
  if (impl_->stop) ::CloseHandle(impl_->stop);
  impl_.reset();
}

}  // namespace mv::io
