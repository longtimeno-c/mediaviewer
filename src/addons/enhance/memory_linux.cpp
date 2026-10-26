// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>

#include "addons/enhance/memory.h"

namespace mv::enhance {

std::optional<std::uint64_t> available_memory() noexcept {
  std::FILE* f = std::fopen("/proc/meminfo", "r");
  if (!f) return std::nullopt;
  char line[256];
  std::optional<std::uint64_t> out;
  while (std::fgets(line, sizeof line, f)) {
    unsigned long long kb = 0;
    if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
      out = static_cast<std::uint64_t>(kb) * 1024;
      break;
    }
  }
  std::fclose(f);
  return out;
}

}  // namespace mv::enhance
