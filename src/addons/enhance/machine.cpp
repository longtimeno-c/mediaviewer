// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/machine.h"

#include <filesystem>
#include <thread>

namespace mv::enhance {

machine probe(const std::string& data_dir) {
  machine m;
#if defined(__APPLE__)
  m.os = "macos";
#elif defined(_WIN32)
  m.os = "windows";
#else
  m.os = "other";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  m.arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  m.arch = "x86_64";
#else
  m.arch = "other";
#endif
  m.cores = std::thread::hardware_concurrency();
  platform_facts f = platform_probe();
  m.ram_total = f.ram_total;
  m.ram_free = f.ram_free;
  m.accelerator = std::move(f.accelerator);
  m.gpu = std::move(f.gpu);
  m.vram = f.vram;
  // The folder may not exist before the first download: its nearest parent
  // that does is on the same volume.
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path at = fs::path(std::u8string(data_dir.begin(), data_dir.end()));
  while (!at.empty() && !fs::exists(at, ec) && at.has_parent_path() && at.parent_path() != at) at = at.parent_path();
  if (!at.empty()) {
    const auto space = fs::space(at, ec);
    if (!ec) m.free_disk = static_cast<std::uint64_t>(space.available);
  }
  return m;
}

}  // namespace mv::enhance
