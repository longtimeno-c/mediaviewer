// SPDX-License-Identifier: GPL-2.0-or-later
// ETW tracepoints + a minimal logger.
//
// ETW is how the frame-time harness (tools/frametime) and PresentMon see inside
// the app without a debugger attached, and it is why the D6 gate can be measured
// in CI rather than eyeballed (plan/09-build-and-test.md).
//
// PRIVACY (rule 6, plan/13): tracepoints carry NO path, filename, or pixel data.
// Everything emitted here is a number, an enum, or a compile-time literal. Do
// not add a `const char* path` field to an event, however convenient.
#pragma once

#include <cstdarg>
#include <cstdint>

namespace mv::trace {

// Provider GUID: {6f4d1b2e-9c3a-4f57-9d21-0a7c4f8e2b16}
// Register with: wpr -start GeneralProfile -start <profile>, or
//   xperf -on ... -start MediaViewer -on 6f4d1b2e-9c3a-4f57-9d21-0a7c4f8e2b16
void provider_register() noexcept;
void provider_unregister() noexcept;

// True when a session is actually consuming our provider. Guard anything that
// costs more than a few instructions to gather.
[[nodiscard]] bool enabled() noexcept;

// --- Frame pacing events ------------------------------------------------
// Emitted from the render thread, once per present. Keep these POD and cheap;
// they run inside the frame.
void frame_present(std::uint64_t frame_index,
                   double present_to_present_ms,
                   double cpu_frame_ms,
                   std::uint32_t missed_refreshes) noexcept;

void frame_dropped(std::uint64_t frame_index, std::uint32_t missed_refreshes) noexcept;

// --- Job system events --------------------------------------------------
void job_submit(std::uint64_t job_id, std::uint32_t generation) noexcept;
void job_begin(std::uint64_t job_id, std::uint32_t worker_index) noexcept;
void job_end(std::uint64_t job_id, std::int32_t status_code) noexcept;
void job_cancelled(std::uint64_t job_id, std::uint32_t generation) noexcept;

// --- ABI boundary -------------------------------------------------------
void abi_call(const char* function_literal, std::uint64_t correlation_id) noexcept;
void abi_fault(const char* function_literal, std::uint64_t correlation_id,
               std::int32_t status_code) noexcept;

}  // namespace mv::trace

namespace mv::log {

enum class level : std::uint8_t { trace, debug, info, warn, error };

// Writes to OutputDebugString and, when attached, stderr. Never allocates for
// messages under 512 bytes. Not for the hot path — use mv::trace there.
void write(level lvl, const char* fmt, ...) noexcept;

}  // namespace mv::log

#define MV_LOG_INFO(...)  ::mv::log::write(::mv::log::level::info, __VA_ARGS__)
#define MV_LOG_WARN(...)  ::mv::log::write(::mv::log::level::warn, __VA_ARGS__)
#define MV_LOG_ERROR(...) ::mv::log::write(::mv::log::level::error, __VA_ARGS__)
