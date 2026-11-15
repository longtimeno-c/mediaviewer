// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// module_self.h on macOS and Linux.
#include <dlfcn.h>

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

}  // namespace mv::phone::module_self
