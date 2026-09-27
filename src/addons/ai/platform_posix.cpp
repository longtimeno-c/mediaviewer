// SPDX-License-Identifier: GPL-2.0-or-later
#include <dlfcn.h>
#include <pthread.h>

#include <string>
#include <thread>

#if defined(__APPLE__)
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

}  // namespace mv::ai::platform
