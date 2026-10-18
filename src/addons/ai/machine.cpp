// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/ai/machine.h"

#include <filesystem>

#include "addons/ai/platform.h"
#include "core/json.h"

namespace mv::ai {

machine_info probe_machine(const std::string& data_dir) {
  machine_info m;
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
  m.cores = platform::hardware_threads();
  const platform::machine_facts f = platform::machine();
  m.ram_bytes = f.ram_bytes;
  m.chip = f.chip;
  m.apple_silicon = f.apple_silicon;
  m.nvidia = f.nvidia;
  m.laptop = f.laptop;
  std::error_code ec;
  const auto space = std::filesystem::space(std::filesystem::path(std::u8string(data_dir.begin(), data_dir.end())), ec);
  if (!ec) m.free_bytes = static_cast<std::uint64_t>(space.available);
  return m;
}

std::string recommend_tier(const machine_info& m) {
  // Marketing GB are a little under the GiB a machine reports: "8 GB" is
  // 8 GiB of RAM, "32 GB" is 32 GiB.
  constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;
  if ((m.cores > 0 && m.cores <= 2) || (m.ram_bytes > 0 && m.ram_bytes <= 8 * kGiB + kGiB / 2)) return "potato";
  if ((m.os == "macos" && m.arch == "x86_64") || (m.os != "macos" && !m.accelerator())) return "intel";
  if (m.accelerator() && m.ram_bytes >= 32 * kGiB - kGiB) return "maxxed";
  return "default";
}

std::string machine_json(const machine_info& m) {
  json::writer w;
  w.begin_object();
  w.key("os").string(m.os);
  w.key("arch").string(m.arch);
  w.key("cores").integer(m.cores);
  w.key("ram_bytes").integer(static_cast<std::int64_t>(m.ram_bytes));
  w.key("free_bytes").integer(static_cast<std::int64_t>(m.free_bytes));
  w.key("chip").string(m.chip);
  w.key("accelerator").string(m.apple_silicon ? "apple" : m.nvidia ? "nvidia" : "");
  w.key("laptop").boolean(m.laptop);
  w.key("recommended").string(recommend_tier(m));
  w.end_object();
  return w.take();
}

}  // namespace mv::ai
