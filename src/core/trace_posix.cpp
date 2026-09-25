// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Darwin / POSIX trace sink. Same events as ETW, no paths, filenames, or pixels
// (plan/13). os_signpost is the Instruments analog of PresentMon.
#include "core/trace.h"

#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include <os/log.h>
#endif

namespace mv::trace {

namespace {
#if defined(__APPLE__)
os_log_t provider() noexcept {
  static os_log_t log = os_log_create("org.mediaviewer", "trace");
  return log;
}
#endif
}  // namespace

void provider_register() noexcept {}
void provider_unregister() noexcept {}

bool enabled() noexcept {
#if defined(__APPLE__)
  return os_log_type_enabled(provider(), OS_LOG_TYPE_DEBUG);
#else
  return false;
#endif
}

void frame_present(std::uint64_t frame_index, double present_to_present_ms, double cpu_frame_ms,
                   std::uint32_t missed_refreshes) noexcept {
#if defined(__APPLE__)
  os_log_debug(provider(),
               "FramePresent index=%{public}llu p2p_ms=%{public}f cpu_ms=%{public}f missed=%{public}u",
               static_cast<unsigned long long>(frame_index), present_to_present_ms, cpu_frame_ms,
               missed_refreshes);
#else
  (void)frame_index;
  (void)present_to_present_ms;
  (void)cpu_frame_ms;
  (void)missed_refreshes;
#endif
}

void frame_dropped(std::uint64_t frame_index, std::uint32_t missed_refreshes) noexcept {
#if defined(__APPLE__)
  os_log_error(provider(), "FrameDropped index=%{public}llu missed=%{public}u",
               static_cast<unsigned long long>(frame_index), missed_refreshes);
#else
  (void)frame_index;
  (void)missed_refreshes;
#endif
}

void job_submit(std::uint64_t job_id, std::uint32_t generation) noexcept {
#if defined(__APPLE__)
  os_log_info(provider(), "JobSubmit id=%{public}llu gen=%{public}u",
              static_cast<unsigned long long>(job_id), generation);
#else
  (void)job_id;
  (void)generation;
#endif
}

void job_begin(std::uint64_t job_id, std::uint32_t worker_index) noexcept {
#if defined(__APPLE__)
  os_log_debug(provider(), "JobBegin id=%{public}llu worker=%{public}u",
               static_cast<unsigned long long>(job_id), worker_index);
#else
  (void)job_id;
  (void)worker_index;
#endif
}

void job_end(std::uint64_t job_id, std::int32_t status_code) noexcept {
#if defined(__APPLE__)
  os_log_debug(provider(), "JobEnd id=%{public}llu status=%{public}d",
               static_cast<unsigned long long>(job_id), status_code);
#else
  (void)job_id;
  (void)status_code;
#endif
}

void job_cancelled(std::uint64_t job_id, std::uint32_t generation) noexcept {
#if defined(__APPLE__)
  os_log_info(provider(), "JobCancelled id=%{public}llu gen=%{public}u",
              static_cast<unsigned long long>(job_id), generation);
#else
  (void)job_id;
  (void)generation;
#endif
}

void abi_call(const char* function_literal, std::uint64_t correlation_id) noexcept {
#if defined(__APPLE__)
  os_log_debug(provider(), "AbiCall %{public}s corr=%{public}llu", function_literal,
               static_cast<unsigned long long>(correlation_id));
#else
  (void)function_literal;
  (void)correlation_id;
#endif
}

void abi_fault(const char* function_literal, std::uint64_t correlation_id,
               std::int32_t status_code) noexcept {
#if defined(__APPLE__)
  os_log_error(provider(), "AbiFault %{public}s corr=%{public}llu status=%{public}d",
               function_literal, static_cast<unsigned long long>(correlation_id), status_code);
#else
  (void)function_literal;
  (void)correlation_id;
  (void)status_code;
#endif
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

  if (lvl >= level::warn) std::fputs(buf, stderr);
}

}  // namespace mv::log
