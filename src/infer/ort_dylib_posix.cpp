// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ort.h's platform half on macOS (and the Linux test build): dlopen by
// absolute path. On the Mac the pack's libonnxruntime.dylib is signed with
// the app's Team ID and loads under library validation (plan/17).
#include <dlfcn.h>

#include <cstring>

#include "infer/ort.h"

namespace mv::infer::dylib {

void* open(const std::string& path_utf8) noexcept {
  return dlopen(path_utf8.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void* symbol(void* handle, const char* name) noexcept {
  return handle ? dlsym(handle, name) : nullptr;
}

void close(void* handle) noexcept {
  if (handle) dlclose(handle);
}

std::vector<char> native_path(const std::string& path_utf8) {
  std::vector<char> out(path_utf8.size() + 1, 0);
  std::memcpy(out.data(), path_utf8.data(), path_utf8.size());
  return out;
}

const char* library_name() noexcept {
#if defined(__APPLE__)
  return "libonnxruntime.dylib";
#else
  return "libonnxruntime.so";
#endif
}

}  // namespace mv::infer::dylib
