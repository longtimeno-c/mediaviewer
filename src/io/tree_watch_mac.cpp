// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// io::tree_watcher on the Mac: one FSEvents stream over every root, with file
// events, so a folder moved in says it is a folder (a directory-level stream
// names only its parent, and the files inside would never be listed). The
// stream runs on a private serial queue, as directory_watcher's does
// (dir_mac.cpp: no run-loop start/stop window).
#include "io/tree_watch.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <strings.h>
#include <sys/stat.h>

#include <climits>
#include <cstdlib>
#include <tuple>
#include <utility>

namespace mv::io {
namespace {

struct watched_root {
  std::string given;  // as start() was given it, no trailing '/'
  std::string real;   // realpath: FSEvents reports /private/var for /var
  bool on = false;    // could be watched
};

std::string trim_slash(std::string s) {
  while (s.size() > 1 && s.back() == '/') s.pop_back();
  return s;
}

std::string parent_of(const std::string& p) {
  const std::size_t slash = p.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return p.substr(0, slash);
}

// `p` is `base` or below it. Case-insensitive (APFS and HFS+ by default).
bool under(const std::string& p, const std::string& base) {
  if (p.size() < base.size() || ::strncasecmp(p.data(), base.data(), base.size()) != 0) return false;
  return p.size() == base.size() || p[base.size()] == '/' || base == "/";
}

}  // namespace

struct tree_watcher::impl {
  FSEventStreamRef stream = nullptr;
  dispatch_queue_t queue = nullptr;
  callback cb = nullptr;
  void* user = nullptr;
  std::vector<watched_root> roots;  // start()'s list, in its order

  // The root `real_path` falls under (the longest), and the path re-spelled
  // under that root's given path; false when it is under none.
  bool map(const std::string& real_path, std::uint32_t& root, std::string& out) const {
    std::size_t best = SIZE_MAX;
    for (std::size_t i = 0; i < roots.size(); ++i) {
      if (roots[i].on && under(real_path, roots[i].real) && (best == SIZE_MAX || roots[i].real.size() > roots[best].real.size())) {
        best = i;
      }
    }
    if (best == SIZE_MAX) return false;
    root = static_cast<std::uint32_t>(best);
    out = roots[best].given + real_path.substr(roots[best].real.size());
    return true;
  }

  static void fsevents_callback(ConstFSEventStreamRef, void* info, std::size_t n, void* event_paths,
                                const FSEventStreamEventFlags* flags, const FSEventStreamEventId*) {
    auto* im = static_cast<impl*>(info);
    if (!im->cb || n == 0) return;
    const char* const* paths = static_cast<const char* const*>(event_paths);
    // One batch (the 0.5 s latency) of a camera-dump copy is many files in a
    // few folders: each folder is reported once.
    std::vector<std::tuple<std::uint32_t, std::string, std::uint32_t>> out;
    const auto add = [&](std::uint32_t root, std::string dir, std::uint32_t f) {
      for (auto& [r, d, g] : out) {
        if (r == root && d == dir) {
          g |= f;
          return;
        }
      }
      out.emplace_back(root, std::move(dir), f);
    };
    constexpr FSEventStreamEventFlags dropped =
        kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped;
    bool overflowed = false;
    for (std::size_t i = 0; i < n; ++i) {
      const FSEventStreamEventFlags f = flags[i];
      if (f & dropped) {
        overflowed = true;
        continue;
      }
      if (!paths[i]) continue;
      const std::string p = trim_slash(paths[i]);
      std::uint32_t root = 0;
      std::string dir;
      if (f & kFSEventStreamEventFlagMustScanSubDirs) {
        if (im->map(p, root, dir)) add(root, std::move(dir), subtree);
        continue;
      }
      if (f & kFSEventStreamEventFlagItemIsDir) {
        // A folder's own mtime moves with its contents, which report
        // themselves; only one that came, went or was renamed is news.
        constexpr FSEventStreamEventFlags moved = kFSEventStreamEventFlagItemCreated |
                                                  kFSEventStreamEventFlagItemRemoved |
                                                  kFSEventStreamEventFlagItemRenamed;
        if (!(f & moved)) continue;
        if (im->map(p, root, dir)) add(root, std::move(dir), subtree);
      }
      // A file (or a folder that moved): its folder's listing changed.
      const std::string parent = parent_of(p);
      if (im->map(parent, root, dir)) add(root, std::move(dir), 0);
    }
    if (overflowed) {
      for (std::uint32_t r = 0; r < im->roots.size(); ++r) {
        if (im->roots[r].on) im->cb(im->user, r, im->roots[r].given, overflow);
      }
      return;
    }
    for (const auto& [r, d, g] : out) im->cb(im->user, r, d, g);
  }

  static void stop_and_invalidate(void* ctx) noexcept {
    auto* stream = static_cast<FSEventStreamRef>(ctx);
    FSEventStreamStop(stream);
    FSEventStreamInvalidate(stream);
  }

  ~impl() {
    if (stream) {
      // On the stream's own queue: once this returns no callback is running
      // and none is queued.
      dispatch_sync_f(queue, stream, &stop_and_invalidate);
      FSEventStreamRelease(stream);
    }
    if (queue) dispatch_release(queue);
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
  next->roots.resize(roots.size());
  CFMutableArrayRef cf_paths = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
  if (!cf_paths) {
    stop();
    return err(status::out_of_memory);
  }
  for (std::size_t i = 0; i < roots.size(); ++i) {
    struct stat st{};
    if (roots[i].empty() || ::stat(roots[i].c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    char real[PATH_MAX];
    if (!::realpath(roots[i].c_str(), real)) continue;
    watched_root& r = next->roots[i];
    r.given = trim_slash(roots[i]);
    r.real = trim_slash(real);
    CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(r.real.data()),
                                            static_cast<CFIndex>(r.real.size()), kCFStringEncodingUTF8, false);
    if (!s) continue;
    CFArrayAppendValue(cf_paths, s);
    CFRelease(s);
    r.on = true;
  }
  if (CFArrayGetCount(cf_paths) == 0) {
    CFRelease(cf_paths);
    stop();
    return err(status::io);
  }
  FSEventStreamContext ctx{};
  ctx.info = next.get();
  // 0.5 s: a copy of many files arrives as a few batches, each folder once.
  FSEventStreamRef stream =
      FSEventStreamCreate(kCFAllocatorDefault, &impl::fsevents_callback, &ctx, cf_paths, kFSEventStreamEventIdSinceNow,
                          0.5, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
  CFRelease(cf_paths);
  if (!stream) {
    stop();
    return err(status::internal);
  }
  next->queue = dispatch_queue_create("mv.io.tree_watch", DISPATCH_QUEUE_SERIAL);
  if (!next->queue) {
    FSEventStreamRelease(stream);
    stop();
    return err(status::out_of_memory);
  }
  FSEventStreamSetDispatchQueue(stream, next->queue);
  if (!FSEventStreamStart(stream)) {
    FSEventStreamInvalidate(stream);
    FSEventStreamRelease(stream);
    stop();
    return err(status::internal);
  }
  next->stream = stream;
  for (std::size_t i = 0; i < roots.size(); ++i) out_ok[i] = next->roots[i].on;

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
