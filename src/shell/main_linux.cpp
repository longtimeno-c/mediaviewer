// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mediaviewer_lab on Linux (Linux PR 1): a Wayland toplevel and the OpenGL
// present lab in it. The UI thread runs the window and publishes input; the
// render thread presents (shell/present_lab_linux.h). No chrome: Qt arrives
// in Linux PR 3 (docs/plans/linux.md).

#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "core/boot_timeline.h"
#include "core/trace.h"
#include "shell/present_lab_linux.h"
#include "shell/wayland_window_linux.h"

namespace {

void usage() {
  std::fprintf(stderr,
               "mediaviewer_lab - MediaViewer's OpenGL present lab on Wayland (Linux PR 1)\n"
               "\n"
               "  mediaviewer_lab [--soak SECONDS [--static] [--json PATH] [--gate]]\n"
               "                  [--no-overlay] [--boot-report PATH]\n"
               "\n"
               "  --soak S          measure for S seconds after a 1 s warm-up, then quit;\n"
               "                    S in (0, 86400]. Animated (the judder sweep) unless --static\n"
               "  --static          the idle soak: one frame, then nothing presented\n"
               "  --json PATH       write the schema-2 frame-time report there\n"
               "  --gate            exit 1 unless the soak passes Linux PR 1's gate\n"
               "  --no-overlay      no F3 overlay during a soak\n"
               "  --boot-report P   write the boot timeline as JSON once the first frame is up,\n"
               "                    then quit\n"
               "\n"
               "Keys: Space sweep, F3 or F overlay, R reset the statistics, Esc quit.\n"
               "Needs a Wayland session (WAYLAND_DISPLAY). X11 is not built yet.\n"
               "Exit codes: 0 pass or no gate asked, 1 gate failed, 2 could not run or report.\n");
}

// Issue #486: the timeline counts from process creation, so the loader and the
// static initialisers before main are in it too. /proc/self/stat field 22 is
// the start time in clock ticks after boot: CLOCK_BOOTTIME's domain, at
// jiffy resolution.
void set_process_age() noexcept {
  FILE* f = std::fopen("/proc/self/stat", "rb");
  if (!f) return;
  char buf[1024];
  const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  buf[n] = '\0';
  // The command name (field 2) may hold spaces and parentheses: count from
  // the last ')'.
  const char* p = std::strrchr(buf, ')');
  if (!p) return;
  ++p;
  int field = 2;
  unsigned long long start_ticks = 0;
  while (*p && field < 22) {
    while (*p == ' ') ++p;
    ++field;
    if (field == 22) {
      start_ticks = std::strtoull(p, nullptr, 10);
      break;
    }
    while (*p && *p != ' ') ++p;
  }
  const long ticks_per_second = ::sysconf(_SC_CLK_TCK);
  timespec now{};
  if (start_ticks == 0 || ticks_per_second <= 0 || ::clock_gettime(CLOCK_BOOTTIME, &now) != 0)
    return;
  const auto now_ns = static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000LL + now.tv_nsec;
  const auto start_ns = static_cast<std::int64_t>(start_ticks) *
                        (1'000'000'000LL / static_cast<std::int64_t>(ticks_per_second));
  if (now_ns > start_ns) mv::boot::set_process_age(static_cast<std::uint64_t>(now_ns - start_ns));
}

struct options {
  mv::shell::linux_lab_options lab;
  std::string boot_report;
  bool overlay_off = false;
  bool static_requested = false;
};

// Validated as the Windows lab validates them: a bad value is an error, never
// a silent zero.
bool parse(int argc, char** argv, options& out, bool& help) {
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    const auto value = [&](const char*& v) {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "mediaviewer_lab: %s needs a value\n", arg);
        return false;
      }
      v = argv[++i];
      return true;
    };
    const char* v = nullptr;
    if (std::strcmp(arg, "--soak") == 0) {
      if (!value(v)) return false;
      char* end = nullptr;
      errno = 0;
      const double seconds = std::strtod(v, &end);
      if (end == v || *end != '\0' || errno != 0 || !std::isfinite(seconds) || seconds <= 0.0 ||
          seconds > 86400.0) {
        std::fprintf(stderr, "mediaviewer_lab: --soak must be a number in (0, 86400]\n");
        return false;
      }
      out.lab.soak_seconds = seconds;
    } else if (std::strcmp(arg, "--json") == 0) {
      if (!value(v)) return false;
      out.lab.json_report_path = v;
    } else if (std::strcmp(arg, "--gate") == 0) {
      out.lab.gate_exit_code = true;
    } else if (std::strcmp(arg, "--static") == 0) {
      out.static_requested = true;
    } else if (std::strcmp(arg, "--no-overlay") == 0) {
      out.overlay_off = true;
    } else if (std::strcmp(arg, "--boot-report") == 0) {
      if (!value(v)) return false;
      out.boot_report = v;
    } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      help = true;
      return true;
    } else {
      std::fprintf(stderr, "mediaviewer_lab: unrecognised argument %s\n", arg);
      return false;
    }
  }
  if (!out.lab.json_report_path.empty() && out.lab.soak_seconds <= 0.0) {
    std::fprintf(stderr, "mediaviewer_lab: --json writes a soak's report; it needs --soak\n");
    return false;
  }
  if (!out.boot_report.empty() && out.lab.soak_seconds > 0.0) {
    std::fprintf(stderr, "mediaviewer_lab: --boot-report cannot be combined with --soak\n");
    return false;
  }
  // Interactive: the hint and idle. A soak: the PR 1 sweep unless --static.
  out.lab.start_animating = out.lab.soak_seconds > 0.0 && !out.static_requested;
  // F3 is an instrument: off until asked, on in a soak unless --no-overlay
  // (the Windows and Mac labs do the same).
  out.lab.overlay_visible = out.lab.soak_seconds > 0.0 && !out.overlay_off;
  out.lab.harness = !out.boot_report.empty();
  return true;
}

void write_boot_report(const std::string& path) noexcept {
  const mv::boot::snapshot s = mv::boot::read();
  char line[1024];
  if (mv::boot::write_line(s, line, sizeof line)) std::fprintf(stderr, "%s\n", line);
  char json[4096];
  const std::size_t n = mv::boot::write_json(s, json, sizeof json);
  if (FILE* f = n ? std::fopen(path.c_str(), "wb") : nullptr) {
    (void)std::fwrite(json, 1, n, f);
    (void)std::fclose(f);
  }
}

}  // namespace

int main(int argc, char** argv) {
  set_process_age();
  mv::boot::mark(mv::boot::stage::host_entry);

  options opts;
  bool help = false;
  if (!parse(argc, argv, opts, help)) {
    usage();
    return 2;
  }
  if (help) {
    usage();
    return 0;
  }
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  if ((!wayland || !*wayland) && !std::getenv("WAYLAND_SOCKET")) {
    std::fprintf(stderr,
                 "mediaviewer_lab: needs a Wayland session (WAYLAND_DISPLAY is not set); the X11 "
                 "path is not built yet\n");
    return 2;
  }
  mv::trace::provider_register();

  mv::shell::wayland_window_linux window;
  if (auto opened = window.open("MediaViewer present lab", 1280, 720); !opened) {
    std::fprintf(stderr, "mediaviewer_lab: cannot open a window (%s)\n",
                 mv::status_name(opened.error()));
    return 2;
  }
  mv::boot::mark(mv::boot::stage::window_created);

  opts.lab.before_commit = [&window](std::uint32_t w, std::uint32_t h) { window.before_commit(w, h); };
  opts.lab.output_refresh_mhz = [&window] { return window.refresh_mhz(); };
  mv::shell::present_lab_linux lab;
  (void)window.take_changed();
  if (auto started = lab.start(window.display(), window.surface(), window.snapshot(), opts.lab);
      !started) {
    std::fprintf(stderr, "mediaviewer_lab: the render thread did not start\n");
    window.close();
    return 2;
  }
  // The toplevel maps with the render thread's first commit.
  mv::boot::mark(mv::boot::stage::window_shown);
  mv::boot::mark(mv::boot::stage::interactive);

  std::thread boot_writer;
  bool connection_lost = false;
  for (;;) {
    bool lab_event = false;
    if (!window.dispatch(-1, lab.events_fd(), &lab_event)) {
      connection_lost = true;
      break;
    }
    if (lab_event) {
      std::uint64_t value = 0;
      (void)!::read(lab.events_fd(), &value, sizeof value);
    }
    if (window.take_changed()) {
      lab.publish(window.snapshot());
      lab.wake();
    }
    if (window.close_requested() || lab.finished()) break;
    if (!opts.boot_report.empty() && lab.first_frame_presented() && !boot_writer.joinable()) {
      // Off the UI thread (rule 1), then quit, as the Mac's --boot-report does.
      try {
        boot_writer = std::thread(write_boot_report, opts.boot_report);
      } catch (...) {
        write_boot_report(opts.boot_report);
      }
      break;
    }
  }
  lab.stop();
  if (boot_writer.joinable()) boot_writer.join();
  int code = lab.exit_code();
  if (connection_lost && code == 0) {
    std::fprintf(stderr, "mediaviewer_lab: the Wayland connection was lost\n");
    code = 2;
  }
  window.close();
  return code;
}
