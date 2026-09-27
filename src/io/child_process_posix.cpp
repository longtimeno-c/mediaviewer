// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// child_process on POSIX (macOS, and Linux for the core tests): posix_spawn
// with two pipes. SIGPIPE is ignored by the writes (MSG_NOSIGNAL is not
// available on a pipe), so a helper that died never takes the caller with it.
#include "io/child_process.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <mutex>
#include <pthread.h>
#include <vector>

extern char** environ;

namespace mv::io {

struct child_process::impl {
  pid_t pid = -1;
  int in_fd = -1;   // our end of the child's stdin
  int out_fd = -1;  // our end of the child's stdout
  bool reaped = false;
  int exit_code = -1;
  std::string pending;
  std::mutex write_mu;
};

namespace {

void close_fd(int& fd) noexcept {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

bool make_pipe(int fds[2]) noexcept {
  if (::pipe(fds) != 0) return false;
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  return true;
}

}  // namespace

child_process::child_process() : impl_(std::make_unique<impl>()) {}

child_process::~child_process() {
  if (impl_->pid > 0 && !impl_->reaped) {
    kill();
    (void)wait();
  }
  close_fd(impl_->in_fd);
  close_fd(impl_->out_fd);
}

expected child_process::start(std::string_view exe_utf8, std::span<const std::string> args) {
  if (impl_->pid > 0) return err(status::internal);
  int in_pipe[2] = {-1, -1};
  int out_pipe[2] = {-1, -1};
  if (!make_pipe(in_pipe)) return err(status::io);
  if (!make_pipe(out_pipe)) {
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    return err(status::io);
  }
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);

  const std::string exe(exe_utf8);
  std::vector<std::string> owned;
  owned.reserve(args.size() + 1);
  owned.push_back(exe);
  for (const std::string& a : args) owned.push_back(a);
  std::vector<char*> argv;
  for (std::string& a : owned) argv.push_back(a.data());
  argv.push_back(nullptr);

  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  if (rc != 0) {
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    return err(status::io);
  }
  impl_->pid = pid;
  impl_->in_fd = in_pipe[1];
  impl_->out_fd = out_pipe[0];
  impl_->reaped = false;
  return {};
}

bool child_process::write_line(std::string_view line) noexcept {
  std::lock_guard lock(impl_->write_mu);
  if (impl_->in_fd < 0) return false;
  // A dead reader would raise SIGPIPE on this thread: block it for the write.
  sigset_t block, old;
  sigemptyset(&block);
  sigaddset(&block, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &block, &old);
  std::string data(line);
  data.push_back('\n');
  bool ok = true;
  std::size_t done = 0;
  while (done < data.size()) {
    const ssize_t n = ::write(impl_->in_fd, data.data() + done, data.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      ok = false;
      break;
    }
    done += static_cast<std::size_t>(n);
  }
  // Consume a SIGPIPE raised above rather than deliver it when unblocking.
  // sigwait returns at once for a signal already pending (macOS has no
  // sigtimedwait).
  sigset_t pending;
  sigemptyset(&pending);
  if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
    int sig = 0;
    (void)sigwait(&block, &sig);
  }
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  return ok;
}

void child_process::close_stdin() noexcept {
  std::lock_guard lock(impl_->write_mu);
  close_fd(impl_->in_fd);
}

bool child_process::read_line(std::string& out) {
  for (;;) {
    const std::size_t nl = impl_->pending.find('\n');
    if (nl != std::string::npos) {
      out.assign(impl_->pending, 0, nl);
      impl_->pending.erase(0, nl + 1);
      if (!out.empty() && out.back() == '\r') out.pop_back();
      return true;
    }
    if (impl_->out_fd < 0) return false;
    char buf[4096];
    const ssize_t n = ::read(impl_->out_fd, buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      close_fd(impl_->out_fd);
      if (impl_->pending.empty()) return false;
      out.swap(impl_->pending);  // a last line without '\n'
      impl_->pending.clear();
      return true;
    }
    impl_->pending.append(buf, static_cast<std::size_t>(n));
  }
}

void child_process::kill() noexcept {
  if (impl_->pid > 0 && !impl_->reaped) ::kill(impl_->pid, SIGKILL);
}

int child_process::wait() noexcept {
  if (impl_->pid <= 0) return -1;
  if (impl_->reaped) return impl_->exit_code;
  int st = 0;
  pid_t r;
  do {
    r = ::waitpid(impl_->pid, &st, 0);
  } while (r < 0 && errno == EINTR);
  impl_->reaped = true;
  impl_->exit_code = (r > 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
  return impl_->exit_code;
}

}  // namespace mv::io
