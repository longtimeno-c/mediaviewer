// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// autostart.h on macOS: a LaunchAgent of this user's, loaded with launchctl.
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "addons/driftwire/cli/autostart.h"

extern char** environ;

namespace mv::driftwire::cli {

namespace {

std::filesystem::path u8(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string plist_path(const autostart_place& where) {
  return where.home + "/Library/LaunchAgents/" + kAutostartLabel + ".plist";
}

// Runs launchctl with `args`, its output dropped; its exit code (-1: not run).
int launchctl(const std::vector<std::string>& args) {
  std::vector<std::string> argv{"/bin/launchctl"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::vector<char*> raw;
  for (auto& a : argv) raw.push_back(a.data());
  raw.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) return -1;
  (void)posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  (void)posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int r = posix_spawn(&pid, raw[0], &fa, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (r != 0) return -1;
  int st = 0;
  while (::waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string domain() { return "gui/" + std::to_string(::getuid()); }

}  // namespace

autostart_result set_autostart(bool on, const autostart_command& c, const autostart_place& where) {
  autostart_result out;
  if (where.home.empty()) {
    out.note = "no home folder to put the LaunchAgent in.";
    return out;
  }
  const std::string path = plist_path(where);
  std::error_code ec;
  if (where.apply) (void)launchctl({"bootout", domain() + "/" + kAutostartLabel});  // a loaded one first, if any
  if (!on) {
    std::filesystem::remove(u8(path), ec);
    out.ok = !ec;
    out.note = out.ok ? "It no longer starts at login. A host running now keeps running until it is stopped."
                      : "the LaunchAgent could not be removed.";
    return out;
  }
  std::filesystem::create_directories(u8(where.home + "/Library/LaunchAgents"), ec);
  const std::string text = launch_agent_plist(c);
  {
    std::ofstream f(u8(path + ".part"), std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f.flush()) {
      out.note = "the LaunchAgent could not be written.";
      return out;
    }
  }
  (void)::chmod((path + ".part").c_str(), 0644);
  std::filesystem::rename(u8(path + ".part"), u8(path), ec);
  if (ec) {
    out.note = "the LaunchAgent could not be written.";
    return out;
  }
  if (where.apply && launchctl({"bootstrap", domain(), path}) != 0) {
    out.note = "the LaunchAgent is written but launchctl did not load it; it loads at the next login.";
    out.ok = true;
    return out;
  }
  out.ok = true;
  out.note = "It starts when you log in (a LaunchAgent, no administrator), and runs now.";
  return out;
}

bool autostart_is_on(const autostart_place& where) {
  std::error_code ec;
  return !where.home.empty() && std::filesystem::is_regular_file(u8(plist_path(where)), ec);
}

}  // namespace mv::driftwire::cli
