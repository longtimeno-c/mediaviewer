// SPDX-License-Identifier: GPL-2.0-or-later
// Status codes shared by the native core and the C ABI (plan/14-abi.md).
//
// The values here MUST stay numerically identical to `mv_status` in
// <mediaviewer/mediaviewer.h>. The ABI layer static_asserts that.
#pragma once

#include <cstdint>

namespace mv {

enum class status : std::int32_t {
  ok = 0,
  invalid_arg,
  out_of_memory,
  io,
  unsupported_format,
  corrupt,
  cancelled,
  device_lost,
  internal,
};

// Short, stable, allocation-free name. Suitable for logs and ETW.
constexpr const char* status_name(status s) noexcept {
  switch (s) {
    case status::ok:                 return "OK";
    case status::invalid_arg:        return "INVALID_ARG";
    case status::out_of_memory:      return "OUT_OF_MEMORY";
    case status::io:                 return "IO";
    case status::unsupported_format: return "UNSUPPORTED_FORMAT";
    case status::corrupt:            return "CORRUPT";
    case status::cancelled:          return "CANCELLED";
    case status::device_lost:        return "DEVICE_LOST";
    case status::internal:           return "INTERNAL";
  }
  return "UNKNOWN";
}

constexpr bool ok(status s) noexcept { return s == status::ok; }

}  // namespace mv
