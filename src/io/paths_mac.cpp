// SPDX-License-Identifier: GPL-2.0-or-later
// Darwin twin of paths_win.cpp: the process-wide thumbnail cache lives under
// ~/Library/Caches/MediaViewer/thumbs, never in the folder being browsed
// (browsing must not write into a user's photo folder, and the cached
// thumbnails would otherwise show up in that folder's own listing).
#include "io/paths.h"

#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <string>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mv::io {
namespace {

std::mutex g_mu;
std::string g_override;
std::string g_addons_override;

std::string home_dir() {
  if (const char* home = std::getenv("HOME"); home && home[0] == '/') return home;
  if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir && pw->pw_dir[0] == '/') {
    return pw->pw_dir;
  }
  return {};
}

bool ensure_dir(const std::string& path) {
  if (::mkdir(path.c_str(), 0755) == 0) return true;
  if (errno != EEXIST) return false;
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

}  // namespace

void set_thumb_cache_dir_override(std::string_view utf8_dir) {
  std::lock_guard lock(g_mu);
  g_override.assign(utf8_dir);
}

result<std::string> thumb_cache_dir() {
  {
    std::lock_guard lock(g_mu);
    if (!g_override.empty()) return g_override;
  }

  const std::string home = home_dir();
  if (home.empty()) return err(status::io);
  const std::string caches = home + "/Library/Caches";
  const std::string app = caches + "/MediaViewer";
  const std::string dir = app + "/thumbs";
  // ~/Library/Caches always exists on macOS; create only our own two levels.
  if (!ensure_dir(app) || !ensure_dir(dir)) return err(status::io);
  return dir;
}

void set_addons_dir_override(std::string_view utf8_dir) {
  std::lock_guard lock(g_mu);
  g_addons_override.assign(utf8_dir);
}

result<std::string> addons_dir() {
  {
    std::lock_guard lock(g_mu);
    if (!g_addons_override.empty()) return g_addons_override;
  }
  const std::string home = home_dir();
  if (home.empty()) return err(status::io);
  const std::string support = home + "/Library/Application Support";
  const std::string app = support + "/MediaViewer";
  const std::string dir = app + "/Add-ons";
  if (!ensure_dir(support) || !ensure_dir(app) || !ensure_dir(dir)) return err(status::io);
  return dir;
}

result<std::string> default_library_dir() {
  const std::string home = home_dir();
  if (home.empty()) return err(status::io);
  return home + "/Pictures/MediaViewer";
}

}  // namespace mv::io
