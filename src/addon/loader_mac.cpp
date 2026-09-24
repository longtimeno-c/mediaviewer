// SPDX-License-Identifier: GPL-2.0-or-later
// dlopen half of the add-on loader. On macOS the app runs with the hardened
// runtime and library validation, so only a library signed by the same Team
// ID loads (plan/18); the Linux core test build uses the same call.
#include <dlfcn.h>

#include "addon/host.h"

namespace mv::addon {

result<shared_library> shared_library::open(const std::string& utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  void* h = ::dlopen(utf8_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) return err(status::corrupt);
  shared_library lib;
  lib.handle_ = h;
  return lib;
}

void* shared_library::symbol(const char* name) const noexcept {
  return handle_ ? ::dlsym(handle_, name) : nullptr;
}

void shared_library::close() noexcept {
  if (handle_) ::dlclose(handle_);
  handle_ = nullptr;
}

}  // namespace mv::addon
