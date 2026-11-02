// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io::tree_watcher on Windows: ReadDirectoryChangesW over each root's whole
// tree, every root on one thread through one completion port. Attributes are
// watched as well as names, sizes and times: OneDrive hydrating or freeing a
// file changes only its attributes (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#include "io/tree_watch.h"

namespace mv::io {
namespace {

std::wstring wide(std::string_view s) {
  if (s.empty()) return {};
  const int n = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string utf8(const wchar_t* s, std::size_t n) {
  if (n == 0) return {};
  const int len = ::WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string out(static_cast<std::size_t>(len), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), out.data(), len, nullptr, nullptr);
  return out;
}

std::string trim_sep(std::string s) {
  while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  return s;
}

constexpr ULONG_PTR kStopKey = ~ULONG_PTR{0};
// The network redirector refuses a notification buffer over 64 KB.
constexpr DWORD kBufferBytes = 64 * 1024;
constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                          FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES;

struct watched_root {
  std::string given;  // as start() was given it, no trailing separator
  std::wstring given_w;
  HANDLE dir = INVALID_HANDLE_VALUE;
  OVERLAPPED ov{};
  bool pending = false;  // a read is in flight (its completion still to come)
  alignas(8) std::uint8_t buf[kBufferBytes]{};
};

}  // namespace

struct tree_watcher::impl {
  HANDLE port = nullptr;
  std::thread thread;
  callback cb = nullptr;
  void* user = nullptr;
  std::vector<std::unique_ptr<watched_root>> roots;  // start()'s list; dir invalid: not watched

  bool arm(watched_root& r) {
    r.ov = OVERLAPPED{};
    DWORD unused = 0;
    r.pending = ::ReadDirectoryChangesW(r.dir, r.buf, kBufferBytes, TRUE, kFilter, &unused, &r.ov, nullptr) != 0;
    return r.pending;
  }

  // One completed buffer: each changed name's folder, once; a folder that
  // came or was renamed (or a name now gone, which may have been one) also
  // as a subtree. A folder moved in from outside the tree is one ADDED name,
  // with no events for what is inside it.
  void report(std::uint32_t index, watched_root& r, DWORD bytes) {
    if (bytes == 0) {
      cb(user, index, r.given, overflow);  // the buffer overflowed: records were lost
      return;
    }
    std::vector<std::pair<std::string, std::uint32_t>> out;
    const auto add = [&](std::string dir, std::uint32_t f) {
      for (auto& [d, g] : out) {
        if (d == dir) {
          g |= f;
          return;
        }
      }
      out.emplace_back(std::move(dir), f);
    };
    std::size_t at = 0;
    for (;;) {
      if (at + sizeof(FILE_NOTIFY_INFORMATION) > bytes) break;
      const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(r.buf + at);
      const std::size_t chars = info->FileNameLength / sizeof(wchar_t);
      const std::string rel = utf8(info->FileName, chars);
      const std::size_t slash = rel.find_last_of("\\/");
      const std::string parent = slash == std::string::npos ? r.given : r.given + "\\" + rel.substr(0, slash);
      const std::wstring full = r.given_w + L"\\" + std::wstring(info->FileName, chars);
      if (info->Action == FILE_ACTION_MODIFIED) {
        // A folder's own last-write moves with its contents, which report
        // themselves: walking its parent for it would be a walk a new file.
        const DWORD attrs = ::GetFileAttributesW(full.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          if (info->NextEntryOffset == 0) break;
          at += info->NextEntryOffset;
          continue;
        }
      }
      add(parent, 0);
      if (info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
        // Gone, so it cannot say whether it was a folder: its rows are.
        add(r.given + "\\" + rel, subtree);
      } else if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
        // One attribute read a new name (local: microseconds).
        const DWORD attrs = ::GetFileAttributesW(full.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          add(r.given + "\\" + rel, subtree);
        }
      }
      if (info->NextEntryOffset == 0) break;
      at += info->NextEntryOffset;
    }
    for (const auto& [d, g] : out) cb(user, index, d, g);
  }

  void run() {
    std::size_t in_flight = 0;
    for (const auto& r : roots) in_flight += r->pending ? 1 : 0;
    bool stopping = false;
    while (in_flight > 0 || !stopping) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* ov = nullptr;
      const BOOL ok = ::GetQueuedCompletionStatus(port, &bytes, &key, &ov, INFINITE);
      if (key == kStopKey && !ov) {
        stopping = true;
        // CancelIoEx only asks: each read still completes into its buffer,
        // so every one is waited for before the buffers go.
        for (const auto& r : roots) {
          if (r->pending) ::CancelIoEx(r->dir, &r->ov);
        }
        continue;
      }
      if (!ov || key >= roots.size()) continue;
      watched_root& r = *roots[key];
      r.pending = false;
      --in_flight;
      if (stopping) continue;
      const auto index = static_cast<std::uint32_t>(key);
      if (!ok) {
        // ERROR_NOTIFY_ENUM_DIR, or the folder went (a pulled drive): rescan
        // the root, and keep watching if it is still there.
        cb(user, index, r.given, overflow);
      } else {
        report(index, r, bytes);
      }
      if (arm(r)) ++in_flight;
    }
  }

  ~impl() {
    if (thread.joinable()) {
      ::PostQueuedCompletionStatus(port, 0, kStopKey, nullptr);
      thread.join();
    } else {
      // No thread ever ran (it could not start): wait out each read here.
      for (const auto& r : roots) {
        if (!r->pending) continue;
        ::CancelIoEx(r->dir, &r->ov);
        DWORD unused = 0;
        (void)::GetOverlappedResult(r->dir, &r->ov, &unused, TRUE);
      }
    }
    for (const auto& r : roots) {
      if (r->dir != INVALID_HANDLE_VALUE) ::CloseHandle(r->dir);
    }
    if (port) ::CloseHandle(port);
  }
};

tree_watcher::tree_watcher() = default;
tree_watcher::~tree_watcher() { stop(); }

expected tree_watcher::start(const std::vector<std::string>& roots, callback cb, void* user,
                             std::vector<bool>& out_ok) {
  out_ok.assign(roots.size(), false);
  if (roots.empty() || !cb) {
    stop();
    return err(status::invalid_arg);
  }
  auto next = std::make_unique<impl>();
  next->cb = cb;
  next->user = user;
  next->port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (!next->port) {
    stop();
    return err(status::internal);
  }
  std::size_t armed = 0;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    auto r = std::make_unique<watched_root>();
    r->given = trim_sep(roots[i]);
    r->given_w = wide(r->given);
    if (!r->given_w.empty()) {
      r->dir = ::CreateFileW(r->given_w.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    }
    if (r->dir != INVALID_HANDLE_VALUE) {
      // The kernel records changes once the first read is in flight, which
      // is here, before start() returns (dir_win.cpp's "armed" lesson).
      if (!::CreateIoCompletionPort(r->dir, next->port, static_cast<ULONG_PTR>(i), 0) || !next->arm(*r)) {
        ::CloseHandle(r->dir);
        r->dir = INVALID_HANDLE_VALUE;
      }
    }
    if (r->dir != INVALID_HANDLE_VALUE) {
      out_ok[i] = true;
      ++armed;
    }
    next->roots.push_back(std::move(r));
  }
  if (armed == 0) {
    stop();
    return err(status::io);
  }
  impl* raw = next.get();
  try {
    next->thread = std::thread([raw] { raw->run(); });
  } catch (const std::system_error&) {
    stop();
    return err(status::internal);  // ~impl waits out the reads already in flight
  }
  std::unique_ptr<impl> old;
  {
    std::lock_guard lock(mutex_);
    old = std::move(impl_);
    impl_ = std::move(next);
  }
  return {};  // `old` stops here, outside the lock
}

void tree_watcher::stop() noexcept {
  std::unique_ptr<impl> old;
  {
    std::lock_guard lock(mutex_);
    old = std::move(impl_);
  }
}

}  // namespace mv::io
