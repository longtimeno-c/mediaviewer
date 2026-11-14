// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// platform.h on iOS. The SDK has no IOKit power sources, and the phone answers
// queries but never indexes, so the battery rule that power_state feeds does
// not run here: it reports "unknown" (platform.h's defaults).
#include <dlfcn.h>
#include <pthread.h>
#include <sys/qos.h>
#include <sys/sysctl.h>

#include <string>
#include <thread>

#include "addons/ai/platform.h"

namespace mv::ai::platform {

namespace {
std::string sysctl_string(const char* name) {
  std::size_t n = 0;
  if (sysctlbyname(name, nullptr, &n, nullptr, 0) != 0 || n == 0) return {};
  std::string s(n, '\0');
  if (sysctlbyname(name, s.data(), &n, nullptr, 0) != 0) return {};
  while (!s.empty() && s.back() == '\0') s.pop_back();
  return s;
}
}  // namespace

std::string self_dir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<const void*>(&self_dir), &info) || !info.dli_fname) return {};
  std::string path = info.dli_fname;
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  path.resize(slash);
  return path;
}

power power_state() noexcept { return {}; }

void enter_background() noexcept { pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0); }

unsigned hardware_threads() noexcept { return std::thread::hardware_concurrency(); }

machine_facts machine() noexcept {
  machine_facts m;
  std::uint64_t mem = 0;
  std::size_t n = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &n, nullptr, 0) == 0) m.ram_bytes = mem;
  m.chip = sysctl_string("hw.machine");  // "iPhone17,1"
  m.apple_silicon = true;                // Core ML, as on an arm64 Mac
  m.laptop = true;                       // runs on a battery
  return m;
}

}  // namespace mv::ai::platform
