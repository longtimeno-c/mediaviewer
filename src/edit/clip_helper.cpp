// SPDX-License-Identifier: GPL-2.0-or-later
// The queue's side of clip_wire.h: one job in one MediaViewerClipJob process.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "edit/clip_helper.h"
#include "edit/clip_wire.h"
#include "io/child_process.h"
#include "io/file_port.h"

namespace mv::edit::clip {
namespace {

// A helper asked to cancel removes its own temporaries; after this long it is
// killed and the queue removes them instead.
constexpr auto kCancelGrace = std::chrono::seconds(5);

bool cancelled(const std::atomic<bool>* c) noexcept { return c != nullptr && c->load(std::memory_order_relaxed); }

std::filesystem::path fs_path(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

}  // namespace

void sweep_temporaries(const std::string& utf8_dir) noexcept {
  // `.<name>.mvpart` is the staged-output name (clip_common.cpp): a dot file
  // with an extension no listing shows, ours by construction.
  std::error_code ec;
  for (std::filesystem::directory_iterator it(fs_path(utf8_dir), ec), end; !ec && it != end; it.increment(ec)) {
    const std::u8string name = it->path().filename().u8string();
    const std::u8string suffix = u8".mvpart";
    if (name.size() > suffix.size() + 1 && name.front() == u8'.' &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
      std::error_code rm;
      std::filesystem::remove(it->path(), rm);
    }
  }
}

result<outcome> run_in_helper(const std::string& helper_utf8, const request& req, const control& ctl) {
  const std::string dir = req.out_dir.empty() ? std::string(io::parent_of(req.source)) : req.out_dir;
  io::child_process child;
  const std::string args[] = {"--job"};
  if (!child.start(helper_utf8, args)) return err(status::io);
  if (!child.write_line(wire::encode_request(req))) {
    child.kill();
    (void)child.wait();
    return err(status::internal);
  }

  // Cancellation runs beside the blocking reader: ask once, then kill.
  std::mutex mu;
  std::condition_variable cv;
  bool finished = false;
  bool asked = false;
  bool killed = false;
  std::thread watcher([&] {
    std::unique_lock lock(mu);
    std::chrono::steady_clock::time_point asked_at{};
    while (!finished) {
      cv.wait_for(lock, std::chrono::milliseconds(50));
      if (finished) break;
      if (!asked && cancelled(ctl.cancel)) {
        asked = true;
        asked_at = std::chrono::steady_clock::now();
        lock.unlock();
        (void)child.write_line("cancel");
        child.close_stdin();
        lock.lock();
      } else if (asked && !killed && std::chrono::steady_clock::now() - asked_at > kCancelGrace) {
        killed = true;
        child.kill();
      }
    }
  });

  wire::reply reply;
  std::string line;
  while (child.read_line(line)) wire::apply_line(line, reply, ctl);
  const int code = child.wait();
  {
    std::lock_guard lock(mu);
    finished = true;
  }
  cv.notify_all();
  watcher.join();

  if (reply.done && code == 0) return std::move(reply.result);
  // Anything else may have left a staged temporary behind: a crash, a kill,
  // a helper that exited without a verdict.
  sweep_temporaries(dir);
  if (cancelled(ctl.cancel)) return err(status::cancelled);
  if (reply.failed) return err(reply.error);
  return err(status::internal);  // the helper died: the viewer did not
}

}  // namespace mv::edit::clip
