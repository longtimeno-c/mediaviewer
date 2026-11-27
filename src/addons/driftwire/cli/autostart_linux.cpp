// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// autostart.h on Linux: a systemd --user unit, enabled with systemctl --user.
// Linger (running with nobody logged in) needs the person's sudo: printed,
// never run.
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
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

std::string unit_folder(const autostart_place& where) {
  // XDG_CONFIG_HOME only for the person's own home (the tests give one of their own).
  const char* xdg = where.apply ? std::getenv("XDG_CONFIG_HOME") : nullptr;  // NOLINT(concurrency-mt-unsafe)
  const std::string base = xdg && *xdg == '/' ? std::string(xdg) : where.home + "/.config";
  return base + "/systemd/user";
}

// systemctl --user ARGS, its output dropped; its exit code (-1: not run).
int systemctl(const std::vector<std::string>& args) {
  std::vector<std::string> argv{"systemctl", "--user"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::vector<char*> raw;
  for (auto& a : argv) raw.push_back(a.data());
  raw.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) return -1;
  (void)posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  (void)posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid = 0;
  const int r = posix_spawnp(&pid, raw[0], &fa, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (r != 0) return -1;
  int st = 0;
  while (::waitpid(pid, &st, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

}  // namespace

autostart_result set_autostart(bool on, const autostart_command& c, const autostart_place& where) {
  autostart_result out;
  if (where.home.empty()) {
    out.note = "no home folder to put the systemd unit in.";
    return out;
  }
  const std::string folder = unit_folder(where);
  const std::string path = folder + "/" + kAutostartUnit;
  std::error_code ec;
  if (!on) {
    if (where.apply) (void)systemctl({"disable", "--now", kAutostartUnit});
    std::filesystem::remove(u8(path), ec);
    if (where.apply) (void)systemctl({"daemon-reload"});
    out.ok = !ec;
    out.note = out.ok ? "It no longer starts at login." : "the systemd unit could not be removed.";
    return out;
  }
  std::filesystem::create_directories(u8(folder), ec);
  const std::string text = systemd_user_unit(c);
  {
    std::ofstream f(u8(path + ".part"), std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f.flush()) {
      out.note = "the systemd unit could not be written.";
      return out;
    }
  }
  (void)::chmod((path + ".part").c_str(), 0644);
  std::filesystem::rename(u8(path + ".part"), u8(path), ec);
  if (ec) {
    out.note = "the systemd unit could not be written.";
    return out;
  }
  out.ok = true;
  const char* user = std::getenv("USER");  // NOLINT(concurrency-mt-unsafe): read once here
  const std::string hint = linger_hint(user ? user : "");
  if (where.apply && (systemctl({"daemon-reload"}) != 0 || systemctl({"enable", "--now", kAutostartUnit}) != 0)) {
    out.note = "The unit is written, but systemctl --user could not enable it (is this a systemd session?).\n"
               "  systemctl --user enable --now " + std::string(kAutostartUnit) + "\n" + hint;
    return out;
  }
  out.note = "It starts when you log in (a systemd --user unit, no administrator), and runs now.\n" + hint;
  return out;
}

bool autostart_is_on(const autostart_place& where) {
  std::error_code ec;
  return !where.home.empty() &&
         std::filesystem::is_regular_file(u8(unit_folder(where) + "/" + kAutostartUnit), ec);
}

}  // namespace mv::driftwire::cli
