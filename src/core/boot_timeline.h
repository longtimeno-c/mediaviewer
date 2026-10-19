// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Boot timeline (issue #486): when each stage of a launch was first reached,
// measured from the moment the OS created the process.
//
// One timeline for both hosts (D9): the core owns the stages, the clock and the
// report; each host says how old the process already was when it started
// counting (Windows: GetProcessTimes; macOS: the kinfo_proc start time) and
// marks the stages it passes. A stage a host never reaches stays unset (-1 in
// the report), so a Mac report and a Windows report have the same shape.
//
// Every mark is one relaxed compare-exchange on a static slot: cheap enough
// for the render thread's first present, safe from any thread, and the first
// mark of a stage wins (a relaunch of the chrome or a second still does not
// move it). Nothing here allocates.
//
// PRIVACY (rule 6): numbers only. A stage is an enum; no path or file name is
// ever part of the timeline or its report.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mv::boot {

// In the order a Windows launch passes them. A host may skip stages (a launch
// with no file has no first_pixel; --no-chrome has no chrome_*), and on a
// launch with a file, first_pixel can land before the chrome is ready: the
// order is the code path, not a promise about time.
enum class stage : std::uint8_t {
  host_entry,       // wWinMain / main: the first line of host code
  crash_armed,      // the crash reporter is started (out-of-process, async)
  settings_loaded,  // settings read, view settings, recents, key map applied
  session_created,  // mv_session_create returned: job workers, caches
  window_created,   // the top-level window exists (not yet shown)
  render_started,   // the render thread is up: device and swapchain made
  window_shown,     // ShowWindow / makeKeyAndOrderFront returned
  chrome_runtime,   // the chrome's runtime is loaded (.NET hostfxr + runtime; SwiftUI n/a)
  chrome_loaded,    // the chrome assembly's entry points are bound
  chrome_bar,       // the command bar is attached and drawn by the chrome
  chrome_islands,   // every other launch-time island (filmstrip, transport, gallery, panels)
  interactive,      // the UI thread is pumping input with the chrome attached
  first_frame,      // the render thread's first present
  first_pixel,      // first pixel of the opened still (a preview is fine)
  full_resolution,  // the opened still at full resolution
  count
};

inline constexpr std::size_t stage_count = static_cast<std::size_t>(stage::count);

// Short snake_case name used as the report key ("chrome_bar"). Static storage.
[[nodiscard]] const char* name(stage s) noexcept;

// The process was `ns_since_process_start` old when the host called this.
// Call once, as early as the host can; later calls are ignored. Without it,
// times count from the first call into this module instead (still useful for
// differences, but the report says origin "first_mark").
void set_process_age(std::uint64_t ns_since_process_start) noexcept;

// Records the first time a stage is reached. Any thread. Later marks of the
// same stage are ignored.
void mark(stage s) noexcept;

struct snapshot {
  // Milliseconds from the origin, or -1.0 if the stage has not been reached.
  double ms[stage_count];
  // True when the origin is the process creation time (set_process_age ran).
  bool from_process_start;
};

[[nodiscard]] snapshot read() noexcept;

// Milliseconds from the origin to now (for "how long have we been up").
[[nodiscard]] double now_ms() noexcept;

// Writes the snapshot as one JSON object ({"schema":1,"origin":...,"stages_ms":{...},
// "gaps_ms":{...}}) into `out`, NUL-terminated. Returns the length written, or 0
// if `cap` is too small. gaps_ms is each reached stage minus the previous
// reached stage in enum order: where the time between two lines of code went
// (negative when a stage ran in parallel and landed early, e.g. first_pixel).
std::size_t write_json(const snapshot& s, char* out, std::size_t cap) noexcept;

// One log line, "boot: host_entry=12.3 crash_armed=... ", for the debugger
// output and the F3 log.
std::size_t write_line(const snapshot& s, char* out, std::size_t cap) noexcept;

// Test seam: clears every mark and the origin.
void reset_for_test() noexcept;

}  // namespace mv::boot
