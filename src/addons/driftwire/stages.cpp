// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/stages.h"

#include <cstdio>

namespace mv::driftwire {

const char* stage_name(stage s) noexcept {
  switch (s) {
    case stage::read: return "read";
    case stage::hash: return "hash";
    case stage::seal: return "seal";
    case stage::send: return "send";
    case stage::wait_room: return "wait_room";
    case stage::recv: return "recv";
    case stage::open: return "open";
    case stage::write: return "write";
    case stage::flush: return "flush";
    case stage::read_back: return "read_back";
    case stage::back_hash: return "back_hash";
    case stage::rename: return "rename";
    case stage::verify_wait: return "verify_wait";
    case stage::count: break;
  }
  return "?";
}

void stage_times::reset() noexcept {
  for (int i = 0; i < kCount; ++i) {
    ns[i].store(0, std::memory_order_relaxed);
    bytes[i].store(0, std::memory_order_relaxed);
    calls[i].store(0, std::memory_order_relaxed);
  }
}

std::string stage_times::json(double wall_s) const {
  std::string out = "{";
  bool first = true;
  for (int i = 0; i < kCount; ++i) {
    const std::uint64_t n = calls[i].load(std::memory_order_relaxed);
    if (n == 0) continue;
    const double s = static_cast<double>(ns[i].load(std::memory_order_relaxed)) / 1e9;
    const double mb = static_cast<double>(bytes[i].load(std::memory_order_relaxed)) / 1e6;
    char item[200];
    int len = std::snprintf(item, sizeof item, "%s\"%s\":{\"ms\":%.1f,\"mb\":%.1f,\"mb_s\":%.1f,\"calls\":%llu",
                            first ? "" : ",", stage_name(static_cast<stage>(i)), s * 1000, mb,
                            s > 0 ? mb / s : 0.0, static_cast<unsigned long long>(n));
    if (len > 0 && wall_s > 0 && static_cast<std::size_t>(len) < sizeof item) {
      len += std::snprintf(item + len, sizeof item - static_cast<std::size_t>(len), ",\"share\":%.3f", s / wall_s);
    }
    out += item;
    out += '}';
    first = false;
  }
  out += '}';
  return out;
}

}  // namespace mv::driftwire
