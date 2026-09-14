// SPDX-License-Identifier: GPL-2.0-or-later
// Decode crash scope: crash-report annotation + the PR 7 deliberate-crash hook.
//
// One line in decode(): `const decode_crash_scope crash_scope(bytes, ctx);`
//
//  1. Annotates this worker's crash-context slot with the format family and
//     bundled decoder name/version (never a path, filename or bytes —
//     plan/13) for the lifetime of the decode.
//  2. The verify hook (plan/10 PR 7: "a deliberately-corrupted RAW produces a
//     minidump containing no path, filename, or pixel data"). Compiled into
//     every build, inert unless BOTH:
//       - the environment variable MV_CRASH_TEST=decode is set, and
//       - the first 64 KB of the input contain the ASCII marker
//         MV-DELIBERATE-CRASH (tools/make-crash-raw.ps1 writes it).
//     Then it writes through a null pointer on the decode worker, so the
//     crash is a real access violation inside a decode job.
//
// The environment is read once per process. Unarmed cost: one branch.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "core/job_system.h"

namespace mv::codec {

inline constexpr std::string_view kCrashTestMarker = "MV-DELIBERATE-CRASH";
inline constexpr std::size_t kCrashTestScanBytes = 64 * 1024;

// True when MV_CRASH_TEST=decode is set (cached).
[[nodiscard]] bool crash_test_armed() noexcept;
// Marker in the first kCrashTestScanBytes. Exposed for tests; no side effects.
[[nodiscard]] bool crash_test_marker_present(std::span<const std::uint8_t> bytes) noexcept;

class decode_crash_scope {
 public:
  decode_crash_scope(std::span<const std::uint8_t> bytes, const job_context* ctx) noexcept;
  ~decode_crash_scope();
  decode_crash_scope(const decode_crash_scope&) = delete;
  decode_crash_scope& operator=(const decode_crash_scope&) = delete;
};

}  // namespace mv::codec
