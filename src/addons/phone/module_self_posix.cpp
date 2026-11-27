// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// module_self.h on macOS and Linux.
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdint>
#include <system_error>
#include <vector>

#include "addons/phone/module_self.h"

namespace mv::phone::module_self {

void pin() noexcept {
  Dl_info info{};
  if (::dladdr(reinterpret_cast<const void*>(&pin), &info) != 0 && info.dli_fname) {
    (void)::dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
  }
}

std::string dir() {
  Dl_info info{};
  if (::dladdr(reinterpret_cast<const void*>(&dir), &info) == 0 || !info.dli_fname) return {};
  std::string path = info.dli_fname;
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string join(const std::string& dir, const char* name) { return dir + "/" + name; }

std::filesystem::path fs_path(const std::string& utf8) { return std::filesystem::path(utf8); }

std::string app_clip_helper() {
  std::string exe;
#if defined(__APPLE__)
  std::uint32_t size = 0;
  (void)::_NSGetExecutablePath(nullptr, &size);
  std::vector<char> buf(size + 1, '\0');
  if (::_NSGetExecutablePath(buf.data(), &size) != 0) return {};
  char real[PATH_MAX];
  exe = ::realpath(buf.data(), real) ? std::string(real) : std::string(buf.data());
#else
  char buf[PATH_MAX];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return {};
  exe.assign(buf, static_cast<std::size_t>(n));
#endif
  const auto slash = exe.find_last_of('/');
  if (slash == std::string::npos) return {};
  const std::string bin = exe.substr(0, slash);
  std::error_code ec;
  for (const std::string& c : {bin + "/../Helpers/MediaViewerClipJob", bin + "/MediaViewerClipJob"}) {
    const auto p = std::filesystem::weakly_canonical(std::filesystem::path(c), ec);
    if (!ec && std::filesystem::is_regular_file(p, ec)) return p.string();
  }
  return {};
}

}  // namespace mv::phone::module_self
