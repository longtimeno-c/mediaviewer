// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The local check where there is no product host (the headless portable build,
// Linux): memory from /proc/meminfo and no accelerator. Enhance ships on
// Windows and macOS (machine_win.cpp, machine_mac.cpp).
#include <pthread.h>
#include <sched.h>

#include <cstdio>

#include "addons/enhance/machine.h"

namespace mv::enhance {

platform_facts platform_probe() {
  platform_facts f;
  if (std::FILE* in = std::fopen("/proc/meminfo", "r")) {
    char line[256];
    while (std::fgets(line, sizeof line, in)) {
      unsigned long long kib = 0;
      if (std::sscanf(line, "MemTotal: %llu kB", &kib) == 1) f.ram_total = kib * 1024;
      if (std::sscanf(line, "MemAvailable: %llu kB", &kib) == 1) f.ram_free = kib * 1024;
    }
    std::fclose(in);
  }
  return f;
}

void enter_background() noexcept {
  // SCHED_IDLE: the thread runs only when nothing else wants the CPU (as the
  // AI pack's workers, src/addons/ai/platform_posix.cpp). Best effort.
  sched_param sp{};
  (void)pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
}

}  // namespace mv::enhance
