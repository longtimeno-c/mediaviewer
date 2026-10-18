// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <dlfcn.h>
#include <pthread.h>

#include <string>
#include <thread>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <sys/qos.h>
#endif

#include "addons/ai/platform.h"

namespace mv::ai::platform {

std::string self_dir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<const void*>(&self_dir), &info) || !info.dli_fname) return {};
  std::string path = info.dli_fname;
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return {};
  path.resize(slash);
  return path;
}

power power_state() noexcept {
  power p;
#if defined(__APPLE__)
  CFTypeRef info = IOPSCopyPowerSourcesInfo();
  if (!info) return p;
  CFArrayRef list = IOPSCopyPowerSourcesList(info);
  if (list) {
    for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
      CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
      if (!d) continue;
      const auto* state = static_cast<CFStringRef>(CFDictionaryGetValue(d, CFSTR(kIOPSPowerSourceStateKey)));
      if (state && CFStringCompare(state, CFSTR(kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo) {
        p.on_battery = true;
      }
      int cur = 0, max = 0;
      const auto* c = static_cast<CFNumberRef>(CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey)));
      const auto* m = static_cast<CFNumberRef>(CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey)));
      if (c && m && CFNumberGetValue(c, kCFNumberIntType, &cur) && CFNumberGetValue(m, kCFNumberIntType, &max) &&
          max > 0) {
        p.percent = cur * 100 / max;
      }
    }
    CFRelease(list);
  }
  CFRelease(info);
#endif
  return p;
}

void enter_background() noexcept {
#if defined(__APPLE__)
  // QOS_CLASS_BACKGROUND throttles CPU, I/O and timers, and keeps the work
  // off the performance cores on Apple silicon.
  pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
  sched_param sp{};
  pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
#endif
}

unsigned hardware_threads() noexcept { return std::thread::hardware_concurrency(); }

#if defined(__APPLE__)
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
#endif

machine_facts machine() noexcept {
  machine_facts m;
#if defined(__APPLE__)
  std::uint64_t mem = 0;
  std::size_t n = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &n, nullptr, 0) == 0) m.ram_bytes = mem;
  m.chip = sysctl_string("machdep.cpu.brand_string");
#if defined(__aarch64__)
  m.apple_silicon = true;
#endif
  // A battery says laptop (MacBook); IOKit lists no internal battery otherwise.
  if (CFTypeRef info = IOPSCopyPowerSourcesInfo()) {
    if (CFArrayRef list = IOPSCopyPowerSourcesList(info)) {
      for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
        CFDictionaryRef d = IOPSGetPowerSourceDescription(info, CFArrayGetValueAtIndex(list, i));
        CFStringRef type = d ? static_cast<CFStringRef>(CFDictionaryGetValue(d, CFSTR(kIOPSTypeKey))) : nullptr;
        if (type && CFStringCompare(type, CFSTR(kIOPSInternalBatteryType), 0) == kCFCompareEqualTo) m.laptop = true;
      }
      CFRelease(list);
    }
    CFRelease(info);
  }
#endif
  return m;
}

}  // namespace mv::ai::platform
