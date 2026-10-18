// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/boot_timeline.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace mv::boot {
namespace {

constexpr const char* kNames[stage_count] = {
    "host_entry",     "crash_armed",   "settings_loaded", "session_created", "window_created",
    "render_started", "window_shown",  "chrome_runtime",  "chrome_loaded",   "chrome_bar",
    "chrome_islands", "interactive",   "first_frame",     "first_pixel",     "full_resolution",
};

// steady_clock nanoseconds; 0 means unset (the clock's epoch is boot or
// earlier on both hosts, so a real reading is never 0).
std::atomic<std::int64_t> g_origin{0};
std::atomic<bool> g_from_process{false};
std::atomic<std::int64_t> g_marks[stage_count]{};

std::int64_t steady_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::int64_t origin_or_now() noexcept {
  std::int64_t origin = g_origin.load(std::memory_order_acquire);
  if (origin != 0) return origin;
  const std::int64_t now = steady_ns();
  if (g_origin.compare_exchange_strong(origin, now, std::memory_order_acq_rel)) return now;
  return origin;
}

// Appends to a fixed buffer; remembers overflow so the caller can return 0.
struct writer {
  char* out;
  std::size_t cap;
  std::size_t len = 0;
  bool overflow = false;

  void put(const char* fmt, ...) noexcept {
    if (overflow) return;
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(out + len, cap - len, fmt, args);
    va_end(args);
    if (n < 0 || static_cast<std::size_t>(n) >= cap - len) {
      overflow = true;
      return;
    }
    len += static_cast<std::size_t>(n);
  }

  std::size_t finish() noexcept {
    if (overflow) {
      if (cap) out[0] = '\0';
      return 0;
    }
    return len;
  }
};

}  // namespace

const char* name(stage s) noexcept {
  const auto i = static_cast<std::size_t>(s);
  return i < stage_count ? kNames[i] : "unknown";
}

void set_process_age(std::uint64_t ns_since_process_start) noexcept {
  bool expected = false;
  if (!g_from_process.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
  g_origin.store(steady_ns() - static_cast<std::int64_t>(ns_since_process_start), std::memory_order_release);
}

void mark(stage s) noexcept {
  const auto i = static_cast<std::size_t>(s);
  if (i >= stage_count) return;
  (void)origin_or_now();
  std::int64_t unset = 0;
  (void)g_marks[i].compare_exchange_strong(unset, steady_ns(), std::memory_order_relaxed);
}

snapshot read() noexcept {
  snapshot s{};
  const std::int64_t origin = origin_or_now();
  s.from_process_start = g_from_process.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < stage_count; ++i) {
    const std::int64_t t = g_marks[i].load(std::memory_order_relaxed);
    s.ms[i] = t == 0 ? -1.0 : static_cast<double>(t - origin) / 1.0e6;
  }
  return s;
}

double now_ms() noexcept { return static_cast<double>(steady_ns() - origin_or_now()) / 1.0e6; }

std::size_t write_json(const snapshot& s, char* out, std::size_t cap) noexcept {
  if (!out || cap == 0) return 0;
  writer w{out, cap};
  w.put("{\n  \"schema\": 1,\n  \"origin\": \"%s\",\n  \"stages_ms\": {",
        s.from_process_start ? "process_start" : "first_mark");
  for (std::size_t i = 0; i < stage_count; ++i) {
    w.put("%s\n    \"%s\": %.3f", i ? "," : "", kNames[i], s.ms[i]);
  }
  w.put("\n  },\n  \"gaps_ms\": {");
  double previous = 0.0;
  bool first = true;
  for (std::size_t i = 0; i < stage_count; ++i) {
    if (s.ms[i] < 0.0) continue;
    w.put("%s\n    \"%s\": %.3f", first ? "" : ",", kNames[i], s.ms[i] - previous);
    previous = s.ms[i];
    first = false;
  }
  w.put("\n  }\n}\n");
  return w.finish();
}

std::size_t write_line(const snapshot& s, char* out, std::size_t cap) noexcept {
  if (!out || cap == 0) return 0;
  writer w{out, cap};
  w.put("boot (ms from %s):", s.from_process_start ? "process start" : "first mark");
  for (std::size_t i = 0; i < stage_count; ++i) {
    if (s.ms[i] >= 0.0) w.put(" %s=%.1f", kNames[i], s.ms[i]);
  }
  return w.finish();
}

void reset_for_test() noexcept {
  for (auto& m : g_marks) m.store(0, std::memory_order_relaxed);
  g_from_process.store(false, std::memory_order_relaxed);
  g_origin.store(0, std::memory_order_release);
}

}  // namespace mv::boot
