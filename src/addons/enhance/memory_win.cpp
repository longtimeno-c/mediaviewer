// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "addons/enhance/memory.h"

namespace mv::enhance {

std::optional<std::uint64_t> available_memory() noexcept {
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  if (!GlobalMemoryStatusEx(&m)) return std::nullopt;
  return m.ullAvailPhys;
}

}  // namespace mv::enhance
