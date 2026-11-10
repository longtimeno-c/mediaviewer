// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// ort.h's platform half on iOS: ONNX Runtime is linked statically (an app may
// not load downloaded code, docs/plans/mobile.md section 8.1), so there is no
// library to open and OrtGetApiBase is called directly. Nothing else is
// reachable through this seam: a request for any other library or symbol
// fails as a missing file would.
#include <onnxruntime_c_api.h>

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "infer/ort.h"

namespace mv::infer::dylib {

namespace {
constexpr std::string_view kLibrary = "libonnxruntime.a";
// Not a real handle: what runtime::load keeps and hands back to symbol().
char g_linked_runtime;
}  // namespace

void* open(const std::string& path_utf8) noexcept {
  const std::string_view path = path_utf8;
  if (path.size() < kLibrary.size() || path.substr(path.size() - kLibrary.size()) != kLibrary) return nullptr;
  // Rule 6. This build of ORT carries Microsoft's telemetry uploader and it is
  // on by default. DisableTelemetryEvents (runtime::load) leaves the uploader
  // running and its ProcessInfo event still goes; only this variable, read
  // when the environment is first created, stops the uploader being made.
  setenv("ORT_DISABLE_TELEMETRY", "1", 1);
  return &g_linked_runtime;
}

void* symbol(void* handle, const char* name) noexcept {
  if (handle != &g_linked_runtime || !name || std::strcmp(name, "OrtGetApiBase") != 0) return nullptr;
  return reinterpret_cast<void*>(&OrtGetApiBase);
}

void close(void*) noexcept {}

std::vector<char> native_path(const std::string& path_utf8) {
  std::vector<char> out(path_utf8.size() + 1, 0);
  std::memcpy(out.data(), path_utf8.data(), path_utf8.size());
  return out;
}

const char* library_name() noexcept { return kLibrary.data(); }

}  // namespace mv::infer::dylib
