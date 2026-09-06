// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/trace.h"

#include <windows.h>

#include <TraceLoggingProvider.h>
#include <evntrace.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

TRACELOGGING_DEFINE_PROVIDER(
    g_mv_provider, "MediaViewer",
    // {6f4d1b2e-9c3a-4f57-9d21-0a7c4f8e2b16}
    (0x6f4d1b2e, 0x9c3a, 0x4f57, 0x9d, 0x21, 0x0a, 0x7c, 0x4f, 0x8e, 0x2b, 0x16));

namespace mv::trace {

void provider_register() noexcept { TraceLoggingRegister(g_mv_provider); }
void provider_unregister() noexcept { TraceLoggingUnregister(g_mv_provider); }

bool enabled() noexcept {
  return TraceLoggingProviderEnabled(g_mv_provider, TRACE_LEVEL_VERBOSE, 0) != FALSE;
}

void frame_present(std::uint64_t frame_index, double present_to_present_ms,
                   double cpu_frame_ms, std::uint32_t missed_refreshes) noexcept {
  TraceLoggingWrite(g_mv_provider, "FramePresent",
                    TraceLoggingLevel(TRACE_LEVEL_VERBOSE),
                    TraceLoggingUInt64(frame_index, "FrameIndex"),
                    TraceLoggingFloat64(present_to_present_ms, "PresentToPresentMs"),
                    TraceLoggingFloat64(cpu_frame_ms, "CpuFrameMs"),
                    TraceLoggingUInt32(missed_refreshes, "MissedRefreshes"));
}

void frame_dropped(std::uint64_t frame_index, std::uint32_t missed_refreshes) noexcept {
  TraceLoggingWrite(g_mv_provider, "FrameDropped",
                    TraceLoggingLevel(TRACE_LEVEL_WARNING),
                    TraceLoggingUInt64(frame_index, "FrameIndex"),
                    TraceLoggingUInt32(missed_refreshes, "MissedRefreshes"));
}

void job_submit(std::uint64_t job_id, std::uint32_t generation) noexcept {
  TraceLoggingWrite(g_mv_provider, "JobSubmit", TraceLoggingLevel(TRACE_LEVEL_INFORMATION),
                    TraceLoggingUInt64(job_id, "JobId"),
                    TraceLoggingUInt32(generation, "Generation"));
}

void job_begin(std::uint64_t job_id, std::uint32_t worker_index) noexcept {
  TraceLoggingWrite(g_mv_provider, "JobBegin", TraceLoggingLevel(TRACE_LEVEL_VERBOSE),
                    TraceLoggingUInt64(job_id, "JobId"),
                    TraceLoggingUInt32(worker_index, "Worker"));
}

void job_end(std::uint64_t job_id, std::int32_t status_code) noexcept {
  TraceLoggingWrite(g_mv_provider, "JobEnd", TraceLoggingLevel(TRACE_LEVEL_VERBOSE),
                    TraceLoggingUInt64(job_id, "JobId"),
                    TraceLoggingInt32(status_code, "Status"));
}

void job_cancelled(std::uint64_t job_id, std::uint32_t generation) noexcept {
  TraceLoggingWrite(g_mv_provider, "JobCancelled", TraceLoggingLevel(TRACE_LEVEL_INFORMATION),
                    TraceLoggingUInt64(job_id, "JobId"),
                    TraceLoggingUInt32(generation, "Generation"));
}

void abi_call(const char* function_literal, std::uint64_t correlation_id) noexcept {
  TraceLoggingWrite(g_mv_provider, "AbiCall", TraceLoggingLevel(TRACE_LEVEL_VERBOSE),
                    TraceLoggingString(function_literal, "Function"),
                    TraceLoggingUInt64(correlation_id, "CorrelationId"));
}

void abi_fault(const char* function_literal, std::uint64_t correlation_id,
               std::int32_t status_code) noexcept {
  TraceLoggingWrite(g_mv_provider, "AbiFault", TraceLoggingLevel(TRACE_LEVEL_ERROR),
                    TraceLoggingString(function_literal, "Function"),
                    TraceLoggingUInt64(correlation_id, "CorrelationId"),
                    TraceLoggingInt32(status_code, "Status"));
}

}  // namespace mv::trace

namespace mv::log {

namespace {
constexpr const char* level_tag(level lvl) noexcept {
  switch (lvl) {
    case level::trace: return "[trace] ";
    case level::debug: return "[debug] ";
    case level::info:  return "[info ] ";
    case level::warn:  return "[warn ] ";
    case level::error: return "[ERROR] ";
  }
  return "[?????] ";
}
}  // namespace

void write(level lvl, const char* fmt, ...) noexcept {
  char buf[512];
  const char* tag = level_tag(lvl);
  const int tag_len = static_cast<int>(std::strlen(tag));
  std::memcpy(buf, tag, static_cast<std::size_t>(tag_len));

  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf + tag_len, sizeof(buf) - static_cast<std::size_t>(tag_len) - 2,
                               fmt, args);
  va_end(args);

  int end = tag_len + (n > 0 ? n : 0);
  if (end > static_cast<int>(sizeof(buf)) - 2) end = static_cast<int>(sizeof(buf)) - 2;
  buf[end] = '\n';
  buf[end + 1] = '\0';

  ::OutputDebugStringA(buf);
  if (lvl >= level::warn) std::fputs(buf, stderr);
}

}  // namespace mv::log
