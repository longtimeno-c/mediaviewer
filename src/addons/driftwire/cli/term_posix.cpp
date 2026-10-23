// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The terminal port on Linux and macOS: termios, select() over the input,
// a wake pipe and a signal pipe. select(), not poll(): macOS's poll() does
// not take terminal devices.
#include "addons/driftwire/cli/term.h"

#include <fcntl.h>
#include <langinfo.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace mv::driftwire::cli {

namespace {

// What leaving raw mode writes: end a synchronized frame cut short, reset
// the colours, show the cursor, bracketed paste off.
constexpr std::string_view kRestore = "\x1B[?2026l\x1B[0m\x1B[?25h\x1B[?2004l";

// The signal pipe's write end, for the handler (async-signal-safe: write only).
volatile std::sig_atomic_t g_signal_fd = -1;

// The terminal as it was, for the exit hook: a std::exit() from anywhere
// still leaves the terminal as it was found.
struct saved_terminal {
  int fd = -1;
  int out = -1;
  termios state{};
  bool active = false;
};
saved_terminal g_saved;

void write_all(int fd, std::string_view s) {
  while (!s.empty()) {
    const ssize_t n = ::write(fd, s.data(), s.size());
    if (n > 0) {
      s.remove_prefix(static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      fd_set w;
      FD_ZERO(&w);
      FD_SET(fd, &w);
      ::select(fd + 1, nullptr, &w, nullptr, nullptr);
      continue;
    }
    return;  // the terminal is gone (EPIPE, EIO): nothing more to say
  }
}

void restore_at_exit() {
  if (!g_saved.active) return;
  write_all(g_saved.out, kRestore);
  ::tcsetattr(g_saved.fd, TCSANOW, &g_saved.state);
  g_saved.active = false;
}

void on_signal(int sig) {
  const int saved = errno;
  const char c = sig == SIGTERM ? 't' : sig == SIGHUP ? 'h' : sig == SIGWINCH ? 'w' : sig == SIGTSTP ? 'z'
                 : sig == SIGCONT ? 'c' : sig == SIGINT ? 'i' : '?';
  if (g_signal_fd >= 0) {
    if (::write(g_signal_fd, &c, 1) < 0) {
      // A full pipe already holds a wake-up; nothing to do.
    }
  }
  errno = saved;
}

bool make_pipe(int fds[2]) {
  if (::pipe(fds) != 0) return false;
  for (int i = 0; i < 2; ++i) {
    ::fcntl(fds[i], F_SETFL, ::fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
  }
  return true;
}

void drain(int fd, std::string* into) {
  char buf[64];
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n > 0) {
      if (into) into->append(buf, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return;
  }
}

constexpr int kSignals[] = {SIGTERM, SIGHUP, SIGWINCH, SIGTSTP, SIGCONT, SIGINT};

}  // namespace

struct console::impl {
  int in = STDIN_FILENO;
  int out = STDOUT_FILENO;
  bool signals = true;
  int wake[2] = {-1, -1};
  int sig[2] = {-1, -1};
  termios before{};
  bool raw = false;
  struct sigaction old[std::size(kSignals)]{};
  struct sigaction old_pipe{};

  void install_signals() {
    if (!make_pipe(sig)) {
      signals = false;
      return;
    }
    g_signal_fd = sig[1];
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (std::size_t i = 0; i < std::size(kSignals); ++i) ::sigaction(kSignals[i], &sa, &old[i]);
    struct sigaction ignore{};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGPIPE, &ignore, &old_pipe);
  }

  void remove_signals() {
    if (!signals || sig[0] < 0) return;
    for (std::size_t i = 0; i < std::size(kSignals); ++i) ::sigaction(kSignals[i], &old[i], nullptr);
    ::sigaction(SIGPIPE, &old_pipe, nullptr);
    g_signal_fd = -1;
  }
};

bool console::is_terminal() noexcept { return ::isatty(STDIN_FILENO) == 1 && ::isatty(STDOUT_FILENO) == 1; }

bool console::utf8() noexcept {
  // The locale the person chose (LC_ALL, LC_CTYPE, LANG), read and then put back.
  const char* now = std::setlocale(LC_CTYPE, nullptr);
  const std::string before = now ? now : "C";
  bool yes = false;
  if (std::setlocale(LC_CTYPE, "")) {
    const char* set = ::nl_langinfo(CODESET);
    std::string s = set ? set : "";
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    yes = s == "utf-8" || s == "utf8";
  }
  std::setlocale(LC_CTYPE, before.c_str());
  return yes;
}

console::console() : console(options{}) {}

console::console(options o) : console(STDIN_FILENO, STDOUT_FILENO, o) {}

console::console(int in_fd, int out_fd, options o) : p_(std::make_unique<impl>()) {
  p_->in = in_fd;
  p_->out = out_fd;
  p_->signals = o.signals;
  make_pipe(p_->wake);
  if (p_->signals) p_->install_signals();
}

console::~console() {
  leave_raw();
  p_->remove_signals();
  for (int fd : {p_->wake[0], p_->wake[1], p_->sig[0], p_->sig[1]}) {
    if (fd >= 0) ::close(fd);
  }
}

bool console::enter_raw() {
  if (p_->raw) return true;
  if (::tcgetattr(p_->in, &p_->before) != 0) return false;
  termios t = p_->before;
  t.c_iflag &= ~static_cast<tcflag_t>(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  t.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | IEXTEN | ISIG);
  t.c_cflag |= CS8;
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  // Output processing stays on: a stray "\n" from anywhere still starts a line.
  if (::tcsetattr(p_->in, TCSANOW, &t) != 0) return false;
  p_->raw = true;
  static bool hooked = false;
  if (!hooked) {
    std::atexit(restore_at_exit);
    hooked = true;
  }
  g_saved = {p_->in, p_->out, p_->before, true};
  write_all(p_->out, "\x1B[?2004h");  // bracketed paste
  return true;
}

void console::leave_raw() noexcept {
  if (!p_->raw) return;
  write_all(p_->out, kRestore);
  ::tcsetattr(p_->in, TCSANOW, &p_->before);
  p_->raw = false;
  g_saved.active = false;
}

bool console::raw() const noexcept { return p_->raw; }

void console::suspend_self() {
  const bool was_raw = p_->raw;
  leave_raw();
  // Stop as the shell expects (the default action), then take our hook back.
  struct sigaction dfl{}, mine{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  ::sigaction(SIGTSTP, &dfl, &mine);
  ::raise(SIGTSTP);
  ::sigaction(SIGTSTP, &mine, nullptr);
  if (was_raw) enter_raw();
}

term_size console::size() const {
  winsize ws{};
  if (::ioctl(p_->out, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    return {ws.ws_col, ws.ws_row};
  }
  if (::ioctl(p_->in, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
    return {ws.ws_col, ws.ws_row};
  }
  term_size s;
  if (const char* c = std::getenv("COLUMNS"); c && std::atoi(c) > 0) s.columns = std::atoi(c);  // NOLINT
  if (const char* l = std::getenv("LINES"); l && std::atoi(l) > 0) s.rows = std::atoi(l);        // NOLINT
  return s;
}

void console::wait(term_input& in, int timeout_ms) {
  in = {};
  using clock = std::chrono::steady_clock;
  const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
  while (true) {
    fd_set r;
    FD_ZERO(&r);
    int top = p_->in;
    FD_SET(p_->in, &r);
    for (int fd : {p_->wake[0], p_->sig[0]}) {
      if (fd < 0) continue;
      FD_SET(fd, &r);
      top = std::max(top, fd);
    }
    timeval tv{};
    timeval* wait_for = nullptr;
    if (timeout_ms >= 0) {
      const auto left = std::max<long long>(
          0, std::chrono::duration_cast<std::chrono::microseconds>(deadline - clock::now()).count());
      tv.tv_sec = static_cast<decltype(tv.tv_sec)>(left / 1000000);
      tv.tv_usec = static_cast<decltype(tv.tv_usec)>(left % 1000000);
      wait_for = &tv;
    }
    const int rc = ::select(top + 1, &r, nullptr, nullptr, wait_for);
    if (rc < 0) {
      if (errno == EINTR) continue;  // the signal pipe has the reason
      in.eof = true;
      return;
    }
    if (rc == 0) {
      in.timed_out = true;
      return;
    }
    if (p_->wake[0] >= 0 && FD_ISSET(p_->wake[0], &r)) {
      drain(p_->wake[0], nullptr);
      in.woken = true;
    }
    if (p_->sig[0] >= 0 && FD_ISSET(p_->sig[0], &r)) {
      std::string codes;
      drain(p_->sig[0], &codes);
      for (const char c : codes) {
        switch (c) {
          case 't':
          case 'h': in.quit = true; break;
          case 'w': in.resized = true; break;
          case 'z': in.suspend = true; break;
          case 'c': in.resumed = true; break;
          case 'i': in.interrupt = true; break;
          default: break;
        }
      }
    }
    if (FD_ISSET(p_->in, &r)) {
      char buf[4096];
      const ssize_t n = ::read(p_->in, buf, sizeof buf);
      if (n > 0) {
        in.bytes.append(buf, static_cast<std::size_t>(n));
      } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
        in.eof = true;
      }
    }
    return;
  }
}

void console::wake() noexcept {
  if (p_->wake[1] < 0) return;
  const char c = 'w';
  if (::write(p_->wake[1], &c, 1) < 0) {
    // Full: a wake-up is already waiting.
  }
}

void console::write(std::string_view bytes) { write_all(p_->out, bytes); }

}  // namespace mv::driftwire::cli
