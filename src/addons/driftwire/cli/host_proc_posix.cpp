// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// host_proc.h on Linux and macOS: posix_spawn with a pipe for stdin.
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include "addons/driftwire/cli/host_proc.h"

extern char** environ;

namespace mv::driftwire::cli {

namespace {

class posix_child final : public child_process {
 public:
  posix_child(pid_t pid, int to_stdin) : pid_(pid), to_stdin_(to_stdin) {}
  ~posix_child() override {
    if (!done_) {
      kill();
      (void)wait(5000);
    }
    close_pipe();
  }

  bool wait(int ms) override {
    if (done_) return true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
      int st = 0;
      const pid_t r = ::waitpid(pid_, &st, WNOHANG);
      if (r == pid_) {
        done_ = true;
        if (WIFSIGNALED(st)) {
          how_.crashed = true;
          how_.code = WTERMSIG(st);
        } else {
          how_.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
        return true;
      }
      if (r < 0 && errno != EINTR) {
        done_ = true;  // not ours to wait for any more
        how_.crashed = true;
        how_.code = -1;
        return true;
      }
      if (std::chrono::steady_clock::now() >= until) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  ended how() const override { return how_; }
  long pid() const override { return static_cast<long>(pid_); }
  void ask_to_stop() override { close_pipe(); }
  void kill() override {
    if (!done_) (void)::kill(pid_, SIGKILL);
  }

 private:
  void close_pipe() {
    if (to_stdin_ >= 0) ::close(to_stdin_);
    to_stdin_ = -1;
  }
  pid_t pid_;
  int to_stdin_;
  bool done_ = false;
  ended how_;
};

}  // namespace

std::unique_ptr<child_process> child_process::start(const std::string& program, const std::vector<std::string>& args) {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return nullptr;
  (void)::fcntl(fds[1], F_SETFD, FD_CLOEXEC);  // the child keeps only its read end, as fd 0
  std::vector<std::string> argv{program};
  argv.insert(argv.end(), args.begin(), args.end());
  std::vector<char*> raw;
  for (auto& a : argv) raw.push_back(a.data());
  raw.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return nullptr;
  }
  if (fds[0] != 0) {  // a supervisor started with stdin closed gets the pipe as fd 0 already
    (void)posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
    (void)posix_spawn_file_actions_addclose(&fa, fds[0]);
  }
  pid_t pid = 0;
  const int r = posix_spawn(&pid, program.c_str(), &fa, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[0]);
  if (r != 0) {
    ::close(fds[1]);
    return nullptr;
  }
  return std::make_unique<posix_child>(pid, fds[1]);
}

void wait_for_stdin_end() {
  char buf[256];
  for (;;) {
    const ssize_t n = ::read(0, buf, sizeof buf);
    if (n == 0) return;
    if (n < 0 && errno != EINTR) return;
  }
}

void leave_own_console() {}

}  // namespace mv::driftwire::cli
