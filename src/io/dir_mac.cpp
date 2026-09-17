// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/dir.h"

#include <CoreServices/CoreServices.h>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>

namespace mv::io {
namespace {

// Same decodable set as dir_win.cpp (D9 parity) — plan/04, folded-in PR 7
// formats and PR 5 video containers included.
bool still_extension(std::string_view name) noexcept {
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot + 1 >= name.size()) return false;
  char ext[8]{};
  const std::size_t n = name.size() - dot;
  if (n >= sizeof(ext)) return false;
  for (std::size_t i = 0; i < n; ++i) {
    char c = name[dot + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    ext[i] = c;
  }
  static constexpr const char* kExt[] = {
      ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".tif", ".tiff", ".ico", ".heic",
      ".heif", ".hif", ".avif", ".dng", ".cr2", ".cr3", ".nef", ".nrw", ".arw", ".srf",
      ".sr2", ".orf", ".raf", ".rw2", ".pef", ".ptx", ".srw", ".rwl", ".3fr", ".fff",
      ".iiq", ".mef", ".mos", ".raw", ".mp4", ".mov", ".mkv", ".webm", ".avi", ".ts", ".m4v",
  };
  for (const char* e : kExt) {
    if (std::strcmp(ext, e) == 0) return true;
  }
  return false;
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

std::string join_utf8(std::string_view dir, std::string_view name) {
  std::string out;
  out.reserve(dir.size() + 1 + name.size());
  out.append(dir);
  if (!dir.empty() && dir.back() != '/') out.push_back('/');
  out.append(name);
  return out;
}

// Ordinal-ish case fold, ASCII only — a coarser approximation than Windows'
// CompareStringOrdinal on non-ASCII names, but stable and total (never ties
// std::sort to an inconsistent comparator).
bool less_casefold(std::string_view a, std::string_view b) noexcept {
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return ca < cb;
  }
  return a.size() < b.size();
}

}  // namespace

result<std::vector<dir_entry>> list_still_files(std::string_view utf8_dir) {
  if (utf8_dir.empty()) return err(status::invalid_arg);

  std::string dir_path(utf8_dir);
  DIR* d = ::opendir(dir_path.c_str());
  if (!d) return err(status::io);

  std::vector<dir_entry> out;
  while (dirent* ent = ::readdir(d)) {
    const std::string_view name(ent->d_name);
    if (name == "." || name == "..") continue;
    // Companion hiding (plan/04): dotfiles cover .DS_Store, AppleDouble
    // "._foo", and ordinary Unix hidden files in one check.
    if (!name.empty() && name.front() == '.') continue;
    if (iequals_ascii(name, "Thumbs.db") || iequals_ascii(name, "desktop.ini")) continue;
    if (!still_extension(name)) continue;

    const std::string full = join_utf8(utf8_dir, name);
    struct stat st{};
    if (::lstat(full.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) continue;
#ifdef UF_HIDDEN
    if (st.st_flags & UF_HIDDEN) continue;
#endif

    dir_entry e;
    e.name_utf8 = std::string(name);
    e.path_utf8 = full;
    e.size = static_cast<std::uint64_t>(st.st_size);
    e.mtime_unix = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
    out.push_back(std::move(e));
  }
  ::closedir(d);

  std::sort(out.begin(), out.end(), [](const dir_entry& a, const dir_entry& b) {
    return less_casefold(a.name_utf8, b.name_utf8);
  });
  return out;
}

result<bool> is_directory(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  struct stat st{};
  if (::stat(std::string(utf8_path).c_str(), &st) != 0) return err(status::io);
  return S_ISDIR(st.st_mode);
}

result<std::string> containing_dir(std::string_view utf8_path) {
  auto dir = is_directory(utf8_path);
  if (!dir) return err(dir.error());
  if (dir.value()) return std::string(utf8_path);

  const std::size_t slash = utf8_path.find_last_of('/');
  if (slash == std::string_view::npos) return err(status::invalid_arg);
  if (slash == 0) return std::string("/");
  return std::string(utf8_path.substr(0, slash));
}

namespace {

void fsevents_callback(ConstFSEventStreamRef, void* client_info, std::size_t num_events,
                       void* /*event_paths*/, const FSEventStreamEventFlags* /*flags*/,
                       const FSEventStreamEventId* /*ids*/) {
  auto* im = static_cast<directory_watcher::impl*>(client_info);
  if (im->cb && num_events > 0) im->cb(im->user);
}

}  // namespace

struct directory_watcher::impl {
  FSEventStreamRef stream = nullptr;
  std::thread thread;
  callback cb = nullptr;
  void* user = nullptr;

  std::mutex start_mutex;
  std::condition_variable start_cv;
  CFRunLoopRef run_loop = nullptr;
  bool started = false;
};

directory_watcher::directory_watcher() = default;
directory_watcher::~directory_watcher() { stop(); }

expected directory_watcher::start(std::string_view utf8_dir, callback cb, void* user) {
  std::lock_guard lock(mutex_);
  stop_locked();
  if (utf8_dir.empty() || !cb) return err(status::invalid_arg);

  auto next = std::make_unique<impl>();
  next->cb = cb;
  next->user = user;

  CFStringRef cf_path =
      CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8_dir.data()),
                              static_cast<CFIndex>(utf8_dir.size()), kCFStringEncodingUTF8, false);
  if (!cf_path) return err(status::invalid_arg);
  const void* path_ptr = cf_path;
  CFArrayRef cf_paths = CFArrayCreate(kCFAllocatorDefault, &path_ptr, 1, &kCFTypeArrayCallBacks);
  CFRelease(cf_path);
  if (!cf_paths) return err(status::out_of_memory);

  FSEventStreamContext ctx{};
  ctx.info = next.get();
  // Latency 150 ms: coalesces a camera-dump burst copy into one callback
  // instead of one per file, matching the "relist on any change" contract
  // dir.h documents (transferred==0 on Windows fires for the same reason).
  FSEventStreamRef stream = FSEventStreamCreate(
      kCFAllocatorDefault, &fsevents_callback, &ctx, cf_paths, kFSEventStreamEventIdSinceNow,
      0.15, kFSEventStreamCreateFlagNoDefer);
  CFRelease(cf_paths);
  if (!stream) return err(status::internal);
  next->stream = stream;

  impl* raw = next.get();
  try {
    next->thread = std::thread([raw] {
      CFRunLoopRef rl = CFRunLoopGetCurrent();
      FSEventStreamScheduleWithRunLoop(raw->stream, rl, kCFRunLoopDefaultMode);
      if (!FSEventStreamStart(raw->stream)) {
        FSEventStreamInvalidate(raw->stream);
        std::lock_guard<std::mutex> l(raw->start_mutex);
        raw->started = true;
        raw->start_cv.notify_all();
        return;
      }
      {
        std::lock_guard<std::mutex> l(raw->start_mutex);
        raw->run_loop = rl;
        raw->started = true;
      }
      raw->start_cv.notify_all();
      CFRunLoopRun();  // returns when stop_locked() calls CFRunLoopStop(rl)
      FSEventStreamStop(raw->stream);
      FSEventStreamInvalidate(raw->stream);
    });
  } catch (const std::bad_alloc&) {
    FSEventStreamRelease(next->stream);
    return err(status::out_of_memory);
  }

  {
    std::unique_lock<std::mutex> l(raw->start_mutex);
    raw->start_cv.wait(l, [raw] { return raw->started; });
  }
  if (!raw->run_loop) {
    // Start failed inside the thread; join and report.
    raw->thread.join();
    FSEventStreamRelease(raw->stream);
    return err(status::internal);
  }

  impl_ = std::move(next);
  return {};
}

void directory_watcher::stop() noexcept {
  std::lock_guard lock(mutex_);
  stop_locked();
}

void directory_watcher::stop_locked() noexcept {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> l(impl_->start_mutex);
    if (impl_->run_loop) CFRunLoopStop(impl_->run_loop);
  }
  if (impl_->thread.joinable()) impl_->thread.join();
  if (impl_->stream) FSEventStreamRelease(impl_->stream);
  impl_.reset();
}

}  // namespace mv::io
