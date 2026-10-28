// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire command line end to end (docs/design/28 "From a terminal"):
// two processes of its test build (mv_driftwire_cli_test), each in a
// pseudo-terminal of its own as a person's terminal is, over an internet code
// on loopback. The test reads the code off one and types it into the other,
// answers the questions they ask with keys, and checks that both close by
// themselves with exit 0 and the file arrived. Also: one session per user.
//
// POSIX only (a pty); on Windows the in-process session tests cover the flows.
#include "catch_compat.h"

#if !defined(_WIN32)

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "addons/driftwire/cli/session.h"
#include "import_fixture.h"

using namespace mv::test;

namespace {

// Text without the escape sequences that draw it.
std::string strip_vt(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\x1B') {
      out += s[i];
      continue;
    }
    if (i + 1 < s.size() && s[i + 1] == '[') {
      std::size_t j = i + 2;
      while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
      i = j;
    } else if (i + 1 < s.size() && s[i + 1] == ']') {
      std::size_t j = i + 2;
      while (j < s.size() && s[j] != '\x07') ++j;
      i = j;
    } else {
      ++i;
    }
  }
  return out;
}

// A card that asks for consent takes an answer once it has been on screen a
// moment with no key typed (session.h): a person reads it first.
void settle() {
  std::this_thread::sleep_for(std::chrono::milliseconds(mv::driftwire::cli::kAnswerSettleMs + 150));
}

// One driftwire in a pty: the test holds the master (what the person's
// terminal would), the program's stderr comes back on a pipe (the test
// build's machine-readable lines).
class pty_process {
 public:
  pty_process(const std::string& exe, const std::vector<std::string>& args, const fs::path& cwd,
              const std::string& term = "xterm-256color") {
    master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master_ >= 0);
    REQUIRE(::grantpt(master_) == 0);
    REQUIRE(::unlockpt(master_) == 0);
    const std::string slave_name = ::ptsname(master_);
    winsize ws{};
    ws.ws_col = 100;
    ws.ws_row = 40;
    ::ioctl(master_, TIOCSWINSZ, &ws);
    int err[2];
    REQUIRE(::pipe(err) == 0);
    // Everything the child needs, made before fork: after it only
    // async-signal-safe calls (another thread may hold the allocator's lock).
    std::vector<std::string> argv{exe};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    std::vector<std::string> env{"TERM=" + term, "COLORTERM=truecolor"};
    for (char** e = environ; *e; ++e) {
      const std::string_view kv = *e;
      if (kv.rfind("TERM=", 0) == 0 || kv.rfind("COLORTERM=", 0) == 0 || kv.rfind("NO_COLOR=", 0) == 0) continue;
      env.emplace_back(kv);
    }
    std::vector<char*> cenv;
    for (auto& e : env) cenv.push_back(e.data());
    cenv.push_back(nullptr);
    const std::string dir = utf8(cwd);
    pid_ = ::fork();
    REQUIRE(pid_ >= 0);
    if (pid_ == 0) {
      // The child: its own session, the pty as its controlling terminal.
      ::setsid();
      const int slave = ::open(slave_name.c_str(), O_RDWR);
      if (slave < 0) ::_exit(126);
      ::ioctl(slave, TIOCSCTTY, 0);
      ::dup2(slave, 0);
      ::dup2(slave, 1);
      ::dup2(err[1], 2);
      ::close(slave);
      ::close(err[0]);
      ::close(err[1]);
      ::close(master_);
      if (::chdir(dir.c_str()) != 0) ::_exit(126);
      ::execve(cargv[0], cargv.data(), cenv.data());
      ::_exit(127);
    }
    ::close(err[1]);
    err_ = err[0];
    readers_.emplace_back([this] { pump(master_, screen_); });
    readers_.emplace_back([this] { pump(err_, events_); });
  }
  ~pty_process() {
    if (pid_ > 0 && !exited_) {
      ::kill(pid_, SIGKILL);
      (void)wait_exit(5);
    }
    if (master_ >= 0) ::close(master_);  // the readers see EOF / EIO
    for (auto& t : readers_) t.join();
    if (err_ >= 0) ::close(err_);
  }
  pty_process(const pty_process&) = delete;
  pty_process& operator=(const pty_process&) = delete;

  void type(std::string_view s) { REQUIRE(::write(master_, s.data(), s.size()) == static_cast<ssize_t>(s.size())); }

  // Waits for `part` on the screen (escape sequences stripped).
  bool shows(std::string_view part, int seconds = 30) {
    return wait([&] { return strip_vt(screen_).find(part) != std::string::npos; }, seconds);
  }
  // The first event line starting with `prefix`, its rest.
  std::optional<std::string> event(std::string_view prefix, int seconds = 30) {
    std::optional<std::string> hit;
    wait(
        [&] {
          const std::string tag = "@driftwire " + std::string(prefix);
          std::size_t at = 0;
          while ((at = events_.find(tag, at)) != std::string::npos) {
            if (at == 0 || events_[at - 1] == '\n') {
              const auto end = events_.find('\n', at);
              if (end == std::string::npos) return false;
              hit = events_.substr(at + tag.size(), end - at - tag.size());
              return true;
            }
            ++at;
          }
          return false;
        },
        seconds);
    return hit;
  }
  std::string errors() {
    std::lock_guard lock(m_);
    return events_;
  }
  std::string screen() {
    std::lock_guard lock(m_);
    return strip_vt(screen_);
  }
  // Everything written to the terminal, as written.
  std::string raw() {
    std::lock_guard lock(m_);
    return screen_;
  }
  // The exit code, or -1 when it did not exit in time (or died of a signal).
  int wait_exit(int seconds = 30) {
    if (exited_) return code_;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
      int st = 0;
      const pid_t r = ::waitpid(pid_, &st, WNOHANG);
      if (r == pid_) {
        exited_ = true;
        code_ = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        return code_;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return -1;
  }

 private:
  // Drains `fd` into `into` (the program would block on a full pty).
  void pump(int fd, std::string& into) {
    char buf[4096];
    while (true) {
      const ssize_t n = ::read(fd, buf, sizeof buf);
      if (n <= 0) {
        if (n < 0 && errno == EINTR) continue;
        break;
      }
      {
        std::lock_guard lock(m_);
        into.append(buf, static_cast<std::size_t>(n));
      }
      cv_.notify_all();
    }
  }
  template <typename F>
  bool wait(F f, int seconds) {
    std::unique_lock lock(m_);
    return cv_.wait_for(lock, std::chrono::seconds(seconds), f);
  }

  int master_ = -1, err_ = -1;
  pid_t pid_ = -1;
  bool exited_ = false;
  int code_ = -1;
  std::mutex m_;
  std::condition_variable cv_;
  std::string screen_, events_;
  std::vector<std::thread> readers_;
};

}  // namespace

TEST_CASE("driftwire cli e2e: two programs in ptys, a code over loopback, typed across; one session per user",
          "[driftwire][cli][e2e]") {
  scratch_dir s("driftwire_cli_e2e");
  const std::string exe = MV_DRIFTWIRE_CLI_TEST_PATH;
  const fs::path file = s / "out" / "IMG_0042.HEIC";
  write_bytes(file, pattern(1024 * 1024 + 11, 8));
  fs::create_directories(s / "received");

  // Bea makes a code; it asks to turn Over the internet on first.
  pty_process bea(exe,
                  {"code", "--test-loopback", "--test-events", "--test-data", utf8(s / "bea"), "--save-to",
                   utf8(s / "received"), "--name", "Bea"},
                  s.root());
  REQUIRE(bea.event("ready"));
  REQUIRE(bea.shows("Receiving from anywhere needs Over the internet."));
  bea.type("y");
  const auto code = bea.event("code ");
  REQUIRE(code);
  REQUIRE(bea.shows("Give this code to the person sending"));
  CHECK(bea.shows(code->substr(0, 4)));  // the code is on Bea's screen

  // Ann sends the file to it.
  pty_process ann(exe,
                  {"send", utf8(file), "--test-loopback", "--test-events", "--test-data", utf8(s / "ann"), "--name",
                   "Ann"},
                  s.root());
  REQUIRE(ann.event("ready"));

  // One session per user: a second one on Ann's data folder is refused.
  {
    pty_process again(exe, {"--test-loopback", "--test-data", utf8(s / "ann")}, s.root());
    CHECK(again.wait_exit() == 2);
    CHECK(again.errors().find("Driftwire is already open in another terminal.") != std::string::npos);
  }

  ann.type("/send-to");
  ann.type("\r");
  REQUIRE(ann.shows("Sending to a code needs Over the internet."));
  ann.type("y");
  REQUIRE(ann.shows("to a code"));
  ann.type("\x1B[200~" + *code + "\x1B[201~");
  ann.type("\r");
  const auto check = ann.event("out naming ");
  REQUIRE(check);
  REQUIRE(ann.shows("Connected to Bea (new device)"));
  // Bea's screen shows the same check, to compare.
  REQUIRE(bea.shows("Ann (new device) wants to send you files"));
  settle();
  ann.type("y");
  REQUIRE(bea.event("in asking"));
  REQUIRE(bea.shows("wants to send you 1 file"));
  settle();
  bea.type("y");

  // Both are one-shots: they close by themselves when it went through.
  CHECK(ann.wait_exit() == 0);
  CHECK(bea.wait_exit() == 0);
  CHECK(ann.event("quit 0"));
  CHECK(bea.event("quit 0"));
  CHECK(ann.screen().find("Sent 1 file to Bea.") != std::string::npos);
  CHECK(bea.screen().find("Received 1 file from Ann.") != std::string::npos);
  CHECK(bea.screen().find("Driftwire closed") != std::string::npos);
  REQUIRE(fs::exists(s / "received" / "IMG_0042.HEIC"));
  CHECK(fs::file_size(s / "received" / "IMG_0042.HEIC") == 1024 * 1024 + 11);
}

TEST_CASE("driftwire cli e2e: TERM=dumb or --plain is plain mode, with no escape sequence at all",
          "[driftwire][cli][e2e]") {
  scratch_dir s("driftwire_cli_dumb");
  const std::string exe = MV_DRIFTWIRE_CLI_TEST_PATH;
  // TERM=dumb, or --plain on a terminal that would take sequences.
  bool flag = false;
  SECTION("TERM=dumb") {}
  SECTION("--plain") { flag = true; }
  std::vector<std::string> args{"--test-loopback", "--test-events", "--test-data", utf8(s / "d"), "--name", "Dee"};
  if (flag) args.emplace_back("--plain");
  pty_process p(exe, args, s.root(), flag ? "xterm-256color" : "dumb");
  REQUIRE(p.event("ready"));
  REQUIRE(p.shows("driftwire>"));
  p.type("/help");
  p.type("\r");
  REQUIRE(p.shows("/send-to"));
  // A file:// link decodes to any byte: a missing one is named, its control
  // characters shown as "?", never written to the terminal.
  p.type("file:///nowhere/%1B]2;PWNED%07%1B[2J.jpg");
  p.type("\r");
  REQUIRE(p.shows("Not found: /nowhere/?]2;PWNED??[2J.jpg"));
  p.type("/quit");
  p.type("\r");
  CHECK(p.wait_exit() == 0);
  CHECK(p.screen().find("Driftwire closed") != std::string::npos);
  // Lines, a prompt, the person's own typing echoed: nothing a dumb terminal
  // would print as junk, nothing a screen reader would read out.
  CHECK(p.raw().find('\x1B') == std::string::npos);
  CHECK(p.raw().find('\x07') == std::string::npos);
}

#endif
