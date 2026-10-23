// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// data_dir.h on Linux and macOS.
#include "addons/driftwire/cli/data_dir.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace mv::driftwire::cli {

namespace {

// `path` and every folder above it that is missing, each for this user alone
// (the XDG base directory rule: 0700). Folders already there keep their mode.
bool make_dirs(const std::string& path) {
  if (path.empty()) return false;
  struct stat st{};
  if (::stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0 && !make_dirs(path.substr(0, slash))) return false;
  if (::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) {
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
  return false;
}

}  // namespace

std::string home_dir() {
  std::string home;
  if (const char* h = std::getenv("HOME"); h && *h) {  // NOLINT(concurrency-mt-unsafe): read at start
    home = h;
  } else if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) {
    home = pw->pw_dir;
  }
  while (home.size() > 1 && home.back() == '/') home.pop_back();
  return home;
}

std::string data_dir() {
  const std::string home = home_dir();
  std::string dir;
#if defined(__APPLE__)
  if (home.empty()) return {};
  dir = home + "/Library/Application Support/Driftwire";
#else
  // XDG: state that should outlive a restart but is not worth backing up
  // (the DHT table, the router ledger, what stopped part way) and the
  // settings the person chose, kept together as the window keeps them.
  if (const char* x = std::getenv("XDG_STATE_HOME"); x && x[0] == '/') {  // NOLINT(concurrency-mt-unsafe)
    dir = x;
  } else if (!home.empty()) {
    dir = home + "/.local/state";
  } else {
    return {};
  }
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  dir += "/driftwire";
#endif
  return make_dirs(dir) ? dir : std::string();
}

std::vector<std::string> utf8_args(int argc, char** argv) {
  std::vector<std::string> out;
  for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
  return out;
}

void print(std::string_view utf8, bool to_stderr) {
  std::FILE* f = to_stderr ? stderr : stdout;
  std::fwrite(utf8.data(), 1, utf8.size(), f);
  std::fflush(f);
}

}  // namespace mv::driftwire::cli
