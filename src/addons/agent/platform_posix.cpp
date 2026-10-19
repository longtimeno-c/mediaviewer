// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// platform.h on macOS (and any POSIX build).
#include "addons/agent/platform.h"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace mv::agent::platform {
namespace {

std::string home() {
  const char* h = std::getenv("HOME");
  if (h && *h) return h;
  const passwd* pw = getpwuid(getuid());
  return pw && pw->pw_dir ? pw->pw_dir : "";
}

bool executable(const std::string& p) { return !p.empty() && access(p.c_str(), X_OK) == 0; }

}  // namespace

std::string self_executable() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buf(size, '\0');
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  buf.resize(std::strlen(buf.c_str()));
  char real[PATH_MAX];
  return realpath(buf.c_str(), real) ? std::string(real) : buf;
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
#endif
}

bool run(const std::string& exe, const std::vector<std::string>& args, std::string& output, int& exit_code,
         int timeout_ms) {
  output.clear();
  exit_code = -1;
  int pipefd[2];
  if (pipe(pipefd) != 0) return false;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
  posix_spawn_file_actions_adddup2(&fa, pipefd[1], 2);
  posix_spawn_file_actions_addclose(&fa, pipefd[0]);
  posix_spawn_file_actions_addclose(&fa, pipefd[1]);
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(pipefd[1]);
  if (rc != 0) {
    close(pipefd[0]);
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  char buf[4096];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      kill(pid, SIGKILL);
      break;
    }
    pollfd p{pipefd[0], POLLIN, 0};
    const int r = poll(&p, 1, static_cast<int>(left.count()));
    if (r <= 0) continue;
    const ssize_t n = read(pipefd[0], buf, sizeof buf);
    if (n <= 0) break;
    if (output.size() < 65536) output.append(buf, static_cast<std::size_t>(n));
  }
  close(pipefd[0]);
  int status = 0;
  if (waitpid(pid, &status, 0) != pid) return false;
  if (!WIFEXITED(status)) return false;
  exit_code = WEXITSTATUS(status);
  return true;
}

std::string module_dir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<const void*>(&module_dir), &info) || !info.dli_fname) return {};
  std::string p = info.dli_fname;
  const std::size_t cut = p.rfind('/');
  return cut == std::string::npos ? std::string() : p.substr(0, cut);
}

std::string claude_skills_dir() { return home() + "/.claude/skills"; }

std::string find_claude() {
  const std::string h = home();
  for (const std::string& p : {h + "/.local/bin/claude", h + "/.claude/local/claude", std::string("/opt/homebrew/bin/claude"),
                               std::string("/usr/local/bin/claude"), h + "/.npm-global/bin/claude"}) {
    if (executable(p)) return p;
  }
  // A shell's PATH (an app started from the Dock has a short one).
  std::string out;
  int code = 0;
  if (run("/bin/zsh", {"-lc", "command -v claude"}, out, code, 5000) && code == 0) {
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    const std::size_t nl = out.rfind('\n');
    if (nl != std::string::npos) out = out.substr(nl + 1);
    if (executable(out)) return out;
  }
  return {};
}

std::string desktop_config_path() {
#if defined(__APPLE__)
  return home() + "/Library/Application Support/Claude/claude_desktop_config.json";
#else
  return home() + "/.config/Claude/claude_desktop_config.json";
#endif
}

bool desktop_installed() {
#if defined(__APPLE__)
  struct stat st {};
  return stat("/Applications/Claude.app", &st) == 0 || stat((home() + "/Applications/Claude.app").c_str(), &st) == 0;
#else
  return false;
#endif
}

bool open_in_final_cut(const std::string& fcpxml, std::string* why) {
#if defined(__APPLE__)
  std::string out;
  int code = 0;
  for (const char* id : {"com.apple.FinalCut", "com.apple.FinalCutTrial"}) {
    if (run("/usr/bin/open", {"-b", id, fcpxml}, out, code, 15000) && code == 0) return true;
  }
  if (why) *why = "Final Cut Pro did not open it: import the file with File > Import > XML";
  return false;
#else
  (void)fcpxml;
  if (why) *why = "Final Cut Pro is Mac-only: import the file into your editor";
  return false;
#endif
}

bool read_line(std::intptr_t in, std::string& pending, std::string& line) {
  for (;;) {
    const std::size_t nl = pending.find('\n');
    if (nl != std::string::npos) {
      line.assign(pending, 0, nl);
      pending.erase(0, nl + 1);
      return true;
    }
    if (pending.size() > (64u << 20)) return false;  // a line past 64 MiB is not a message
    char buf[65536];
    const ssize_t n = read(static_cast<int>(in), buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      if (!pending.empty()) {
        line.swap(pending);
        pending.clear();
        return true;
      }
      return false;
    }
    pending.append(buf, static_cast<std::size_t>(n));
  }
}

bool write_all(std::intptr_t out, std::string_view data) {
  while (!data.empty()) {
    const ssize_t n = write(static_cast<int>(out), data.data(), data.size());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

bool write_file_atomic(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".mvtmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return false;
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace mv::agent::platform
