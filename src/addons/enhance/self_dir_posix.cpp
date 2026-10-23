// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <dlfcn.h>

#include "addons/enhance/self_dir.h"

namespace mv::enhance {

std::string self_dir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<const void*>(&self_dir), &info) || !info.dli_fname) return {};
  std::string path = info.dli_fname;
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  path.resize(slash);
  return path;
}

}  // namespace mv::enhance
