// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// dlopen half of the add-on loader. On macOS the app runs with the hardened
// runtime and library validation, so only a library signed by the same Team
// ID loads (docs/design/18); the Linux core test build uses the same call.
#include <dlfcn.h>

#include <string>
#include <string_view>

#include "addon/host.h"

namespace mv::addon {

namespace {

thread_local std::string t_load_error;

// dlerror() names paths; this keeps only what kind of failure it was (rule 6).
std::string load_reason(const char* why) {
  const std::string_view w = why ? why : "";
  if (w.find("code signature") != std::string_view::npos || w.find("not valid for use in process") != std::string_view::npos) {
    return "macOS refused its code signature. Remove it and install it again.";
  }
  if (w.find("incompatible architecture") != std::string_view::npos ||
      w.find("mach-o file, but is an incompatible") != std::string_view::npos) {
    return "It was built for another kind of processor.";
  }
  if (w.find("Library not loaded") != std::string_view::npos || w.find("image not found") != std::string_view::npos) {
    return "A library it needs is missing. Remove it and install it again.";
  }
  if (w.find("Symbol not found") != std::string_view::npos) {
    return "This version of macOS lacks something it needs. Update macOS, then try again.";
  }
  return "macOS could not load it.";
}

}  // namespace

const std::string& shared_library::last_error() noexcept { return t_load_error; }

result<shared_library> shared_library::open(const std::string& utf8_path) {
  t_load_error.clear();
  if (utf8_path.empty()) return err(status::invalid_arg);
  void* h = ::dlopen(utf8_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    t_load_error = load_reason(::dlerror());
    return err(status::io);
  }
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
