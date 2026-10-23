// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/present_lab_linux.h"

// GL entry points straight from glvnd's libOpenGL (OpenGL::OpenGL).
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include <limits.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include "core/boot_timeline.h"
#include "core/trace.h"
#include "gfx/pace_json.h"
#include "gfx/present_policy.h"
#include "shell/home_theme.h"

using mv::gfx::k_occlusion_poll_ms;
using mv::gfx::k_warmup_seconds;

namespace mv::shell {

namespace {

double monotonic_seconds() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

double process_cpu_seconds() noexcept {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) * 1e-6 +
         static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) * 1e-6;
}

// The same fifteen lines as the Win32 and Metal labs: ImGui fed from the
// published snapshot on the render thread, never from the window's callbacks
// (shell/input_state.h).
void feed_imgui(const input_snapshot& s, float delta_seconds, float wheel) noexcept {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(s.width), static_cast<float>(s.height));
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  io.DeltaTime = delta_seconds > 0.0f ? delta_seconds : 1.0f / 60.0f;
  if (s.mouse_in_client) {
    io.AddMousePosEvent(s.mouse_x, s.mouse_y);
  } else {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
  for (int i = 0; i < 3; ++i) io.AddMouseButtonEvent(i, s.mouse_down[i]);
  if (wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);
}

// CozetteVector.ttf beside the binary (the build tree), or in
// ../share/mediaviewer/ (an installed tree).
void try_load_font() noexcept {
  char exe[PATH_MAX]{};
  const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) return;
  exe[n] = '\0';
  char* slash = std::strrchr(exe, '/');
  if (!slash) return;
  *slash = '\0';
  for (const char* rel : {"/CozetteVector.ttf", "/../share/mediaviewer/CozetteVector.ttf"}) {
    char font_path[PATH_MAX]{};
    if (std::snprintf(font_path, sizeof(font_path), "%s%s", exe, rel) <= 0) continue;
    if (::access(font_path, R_OK) != 0) continue;
    if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path, 16.0f)) {
      ImGui::GetIO().FontDefault = font;
    }
    return;
  }
}

const char* clock_name(int id) noexcept {
  switch (id) {
    case CLOCK_MONOTONIC:     return "CLOCK_MONOTONIC";
    case CLOCK_MONOTONIC_RAW: return "CLOCK_MONOTONIC_RAW";
    case CLOCK_REALTIME:      return "CLOCK_REALTIME";
    case CLOCK_BOOTTIME:      return "CLOCK_BOOTTIME";
    case -1:                  return "not given";
    default:                  return "other";
  }
}

double refresh_seconds_from_mhz(std::uint32_t mhz) noexcept {
  return mhz > 0 ? 1000.0 / static_cast<double>(mhz) : 0.0;
}

}  // namespace

present_lab_linux::~present_lab_linux() {
  stop();
  for (int* fd : {&wake_fd_, &events_fd_, &ready_fd_}) {
    if (*fd >= 0) ::close(*fd);
    *fd = -1;
  }
}

expected present_lab_linux::start(void* wl_display, void* wl_surface,
                                  const input_snapshot& initial,
                                  const linux_lab_options& options) noexcept {
  if (!wl_display || !wl_surface || initial.width == 0 || initial.height == 0)
    return err(status::invalid_arg);
  if (render_thread_.joinable()) return err(status::busy);
  display_ = wl_display;
  surface_ = wl_surface;
  options_ = options;
  initial_ = initial;
  overlay_visible_ = options.overlay_visible;
  animating_ = options.start_animating;
  input_.publish(initial);

  wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  events_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ready_fd_ = ::eventfd(0, EFD_CLOEXEC);
  if (wake_fd_ < 0 || events_fd_ < 0 || ready_fd_ < 0) return err(status::out_of_memory);

  running_.store(true, std::memory_order_release);
  try {
    render_thread_ = std::thread([this] { render_thread_main(); });
  } catch (...) {
    running_.store(false, std::memory_order_release);
    return err(status::out_of_memory);
  }
  // The device and surface exist (or failed) before the host goes on: the
  // Windows lab's ready_event_, without its spin.
  std::uint64_t ready = 0;
  while (::read(ready_fd_, &ready, sizeof ready) < 0 && errno == EINTR) {
  }
  if (exit_code_.load(std::memory_order_acquire) != 0) {
    if (render_thread_.joinable()) render_thread_.join();
    return err(status::internal);
  }
  return {};
}

void present_lab_linux::stop() noexcept {
  running_.store(false, std::memory_order_release);
  wake();
  if (render_thread_.joinable()) render_thread_.join();
}

void present_lab_linux::wake() noexcept {
  if (wake_fd_ < 0) return;
  const std::uint64_t one = 1;
  (void)!::write(wake_fd_, &one, sizeof one);
}

void present_lab_linux::signal_events() noexcept {
  const std::uint64_t one = 1;
  (void)!::write(events_fd_, &one, sizeof one);
}

void present_lab_linux::fail_start() noexcept {
  exit_code_.store(2, std::memory_order_release);
  if (imgui_ready_) {
    ImGui_ImplOpenGL3_Shutdown();
    imgui_ready_ = false;
  }
  clock_.destroy();
  gl_surface_.destroy();
  device_.destroy();
  ImGui::DestroyContext();
  finished_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  const std::uint64_t one = 1;
  (void)!::write(ready_fd_, &one, sizeof one);
  signal_events();
}

void present_lab_linux::absorb_feedback() noexcept {
  gfx::presentation_sample samples[64];
  for (;;) {
    const std::size_t n = clock_.drain(samples, 64);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& s = samples[i];
      pacer_.on_feedback(s);
      if (!s.presented) continue;
      // The compositor's own prediction for this surface is the refresh the
      // gate is judged against; the output's mode was the first guess. Zero
      // is a variable-rate output under presentation v1: no gate is defined.
      const double current_ms = pacer_.refresh_seconds() * 1000.0;
      if (s.refresh_ns == 0) {
        if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
        continue;
      }
      const double feedback_ms = static_cast<double>(s.refresh_ns) / 1.0e6;
      if (std::abs(feedback_ms - current_ms) > current_ms * 0.001) {
        if (warmed_up_ && options_.soak_seconds > 0.0 && current_ms > 0.0)
          measurement_valid_ = false;
        pacer_.set_refresh(feedback_ms / 1000.0);
      }
    }
    if (n < 64) return;
  }
}

void present_lab_linux::drain_feedback_at_end() noexcept {
  // The last commits' answers come a refresh or two after them. Wait a
  // bounded 100 ms for them; what has not come by then is unavailable.
  const double deadline = monotonic_seconds() + 0.1;
  while (pacer_.pending_feedback() > 0) {
    const double left = deadline - monotonic_seconds();
    if (left <= 0.0) break;
    const auto r = clock_.wait(static_cast<int>(std::ceil(left * 1000.0)), -1);
    absorb_feedback();
    if (r == gfx::wayland_frame_clock::wait_result::failed) break;
  }
  pacer_.expire_pending();
}

bool present_lab_linux::rebuild_device() noexcept {
  // A GPU reset lost the context: everything made on it goes, then the
  // device and surface come back on the same wl_surface (Windows'
  // rebuild_device()). The frame callback state survives; it is the
  // compositor's, not the context's.
  ++device_rebuilds_;
  const std::uint32_t w = gl_surface_.width();
  const std::uint32_t h = gl_surface_.height();
  if (imgui_ready_) {
    ImGui_ImplOpenGL3_Shutdown();
    imgui_ready_ = false;
  }
  gl_surface_.destroy();
  device_.destroy();
  if (auto made = device_.create(display_); !made) return false;
  if (auto attached = gl_surface_.attach(display_, surface_, device_, w, h); !attached)
    return false;
  device_.query_gl_info();
  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) return false;
  imgui_ready_ = true;
  return true;
}

void present_lab_linux::render_thread_main() noexcept {
  (void)pthread_setname_np(pthread_self(), "mv.render");
  // No unprivileged real-time class on Linux (MMCSS "Games" and the Mac's
  // USER_INTERACTIVE have no analogue without rtkit or the portal): the
  // render thread stays at the default and the workers step down instead
  // (core/job_system_posix.cpp; docs/design/12, 2026-10-23).

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::StyleColorsDark();
  try_load_font();

  if (auto made = device_.create(display_); !made) {
    MV_LOG_ERROR("present_lab_linux: device creation failed (%s)", status_name(made.error()));
    fail_start();
    return;
  }
  if (auto attached = gl_surface_.attach(display_, surface_, device_, initial_.width, initial_.height);
      !attached) {
    MV_LOG_ERROR("present_lab_linux: surface failed (%s)", status_name(attached.error()));
    fail_start();
    return;
  }
  device_.query_gl_info();
  if (auto clock = clock_.create(display_, surface_); !clock) {
    MV_LOG_ERROR("present_lab_linux: frame clock failed (%s)", status_name(clock.error()));
    fail_start();
    return;
  }
  if (!clock_.has_presentation()) {
    MV_LOG_WARN("present_lab_linux: the compositor offers no wp_presentation; every frame is "
                "unavailable to the gate");
  }
  if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
    MV_LOG_ERROR("present_lab_linux: the ImGui OpenGL 3 backend did not start");
    fail_start();
    return;
  }
  imgui_ready_ = true;
  mv::boot::mark(mv::boot::stage::render_started);
  {
    const std::uint64_t one = 1;
    (void)!::write(ready_fd_, &one, sizeof one);
  }

  pacer_.begin_session(
      refresh_seconds_from_mhz(options_.output_refresh_mhz ? options_.output_refresh_mhz() : 0));

  const bool soak = options_.soak_seconds > 0.0;
  const double start = monotonic_seconds();
  double last_frame = start;

  while (running_.load(std::memory_order_acquire)) {
    const input_snapshot snapshot = input_.acquire();
    const double elapsed = monotonic_seconds() - start;
    if (!warmed_up_ && elapsed >= k_warmup_seconds) {
      warmed_up_ = true;
      if (total_presents_ == 0 || !snapshot.window_visible) measurement_valid_ = false;
      absorb_feedback();
      pacer_.reset_window();
      measurement_start_seconds_ = monotonic_seconds();
      idle_start_cpu_seconds_ = process_cpu_seconds();
    }
    if (warmed_up_ && soak &&
        monotonic_seconds() - measurement_start_seconds_ >= options_.soak_seconds) {
      soak_complete_ = true;
      break;
    }
    if (warmed_up_ && soak && !snapshot.window_visible) measurement_valid_ = false;

    const bool input_activity = input_cursor_.consume_activity(snapshot);
    const float wheel = input_cursor_.consume_wheel(snapshot);
    if (input_activity && warmed_up_ && !options_.start_animating) ++idle_stats_.input_events;
    bool redraw = input_activity;

    if (snapshot.toggle_overlay_seq != seen_overlay_seq_) {
      if ((snapshot.toggle_overlay_seq - seen_overlay_seq_) & 1u) overlay_visible_ = !overlay_visible_;
      seen_overlay_seq_ = snapshot.toggle_overlay_seq;
      redraw = true;
    }
    if (snapshot.toggle_animation_seq != seen_animation_seq_) {
      if ((snapshot.toggle_animation_seq - seen_animation_seq_) & 1u) animating_ = !animating_;
      seen_animation_seq_ = snapshot.toggle_animation_seq;
      redraw = true;
      if (warmed_up_ && soak) measurement_valid_ = false;
      if (!soak) pacer_.reset_window();
    }
    if (snapshot.reset_stats_seq != seen_reset_seq_) {
      seen_reset_seq_ = snapshot.reset_stats_seq;
      redraw = true;
      if (warmed_up_ && soak) measurement_valid_ = false;
      if (!soak) pacer_.reset_window();
    }
    if (snapshot.display_change_seq != seen_display_seq_) {
      // The surface moved outputs or the mode changed. Re-read here, never on
      // a resize (issue #162). Feedback's own refresh replaces it once it comes.
      seen_display_seq_ = snapshot.display_change_seq;
      const double previous = pacer_.refresh_seconds() * 1000.0;
      const double now_refresh = refresh_seconds_from_mhz(
          options_.output_refresh_mhz ? options_.output_refresh_mhz() : 0);
      if (now_refresh > 0.0) pacer_.set_refresh(now_refresh);
      if (warmed_up_ && soak && std::abs(now_refresh * 1000.0 - previous) > previous * 0.001)
        measurement_valid_ = false;
      redraw = true;
    }
    if (snapshot.resize_seq != seen_resize_seq_) {
      seen_resize_seq_ = snapshot.resize_seq;
      if (auto resized = gl_surface_.resize(snapshot.width, snapshot.height); !resized) {
        MV_LOG_WARN("present_lab_linux: resize failed (%s)", status_name(resized.error()));
        measurement_valid_ = false;
      }
      redraw = true;  // the configure is acked on the commit of its size
    }
    absorb_feedback();
    if (redraw) last_input_time_ = elapsed;

    gfx::present_request req;
    req.window_visible = snapshot.window_visible;
    // A Wayland surface with no buffer is not on screen at all: its first
    // commit maps it, and compositors activate a toplevel only once it is
    // mapped. That one frame is presented whatever the activation says.
    req.window_active = snapshot.window_active || total_presents_ == 0;
    req.occluded = occluded_;
    req.soak = soak || options_.harness;
    req.animating = animating_;
    req.redraw = redraw || redraw_owed_;
    req.painted_static = painted_static_;
    req.elapsed_seconds = elapsed;
    req.last_input_time = last_input_time_;
    const auto decision = gfx::decide_present(req);

    if (!decision.wants_frame) {
      // Idle: commit nothing, so the compositor sends nothing back: 0
      // presents, ~0 % CPU (docs/design/03 rule 4). What was asked while
      // presenting was not allowed is owed for when it is.
      redraw_owed_ = req.redraw;
      if (was_presenting_ && !soak) pacer_.reset_window();
      was_presenting_ = false;
      int timeout_ms = occluded_ ? static_cast<int>(k_occlusion_poll_ms) : -1;
      if (soak) {
        const double remaining = warmed_up_ ? options_.soak_seconds -
                                                  (monotonic_seconds() - measurement_start_seconds_)
                                            : k_warmup_seconds - elapsed;
        const int soak_ms = static_cast<int>(std::ceil(std::max(0.0, remaining) * 1000.0));
        timeout_ms = timeout_ms < 0 ? soak_ms : std::min(timeout_ms, soak_ms);
      }
      const auto r = clock_.wait(timeout_ms, wake_fd_);
      absorb_feedback();
      if (r == gfx::wayland_frame_clock::wait_result::failed) {
        MV_LOG_ERROR("present_lab_linux: the Wayland connection is gone");
        exit_code_.store(2, std::memory_order_release);
        break;
      }
      // The pending callback was the probe: it fired, so the surface is shown.
      if (r == gfx::wayland_frame_clock::wait_result::frame) occluded_ = false;
      // As on the other labs: the idle time is not a spring step.
      last_frame = monotonic_seconds();
      continue;
    }

    if (!was_presenting_ && !soak) pacer_.reset_window();
    was_presenting_ = true;

    // Wait BEFORE recording: the previous commit's frame callback is the
    // waitable object (SetMaximumFrameLatency(1): never two frames in flight).
    // A compositor sends none to a hidden surface, so a callback that has not
    // come in k_occlusion_poll_ms means occluded: stop, and let the callback
    // still outstanding wake the idle wait when the surface is shown.
    const auto waited = clock_.wait_frame(static_cast<int>(k_occlusion_poll_ms), wake_fd_);
    if (waited == gfx::wayland_frame_clock::wait_result::failed) {
      MV_LOG_ERROR("present_lab_linux: the Wayland connection is gone");
      exit_code_.store(2, std::memory_order_release);
      break;
    }
    if (waited == gfx::wayland_frame_clock::wait_result::timeout) {
      occluded_ = true;
      if (warmed_up_ && soak) measurement_valid_ = false;
      continue;
    }
    if (waited == gfx::wayland_frame_clock::wait_result::woken) continue;  // newer input first
    absorb_feedback();

    pacer_.frame_begin();
    const double now = monotonic_seconds();
    const auto delta = static_cast<float>(now - last_frame);
    last_frame = now;

    draw_frame(snapshot, elapsed, delta, wheel);

    if (options_.before_commit) options_.before_commit(gl_surface_.width(), gl_surface_.height());
    const std::uint64_t id = ++frame_id_;
    clock_.request(id);  // pending state the swap's commit applies
    if (auto presented = gl_surface_.present(); !presented) {
      if (presented.error() == status::device_lost && rebuild_device()) {
        measurement_valid_ = false;
        continue;
      }
      exit_code_.store(2, std::memory_order_release);
      break;
    }
    if (warmed_up_ && !options_.start_animating) ++idle_stats_.presents;
    ++total_presents_;
    pacer_.frame_end(id);
    if (!decision.live) painted_static_ = true;
    redraw_owed_ = false;
    if (!first_frame_.load(std::memory_order_relaxed)) {
      mv::boot::mark(mv::boot::stage::first_frame);
      first_frame_.store(true, std::memory_order_release);
      signal_events();
    }
    if (device_.reset_detected()) {
      MV_LOG_WARN("present_lab_linux: the GPU reset the context; rebuilding");
      measurement_valid_ = false;
      if (!rebuild_device()) {
        exit_code_.store(2, std::memory_order_release);
        break;
      }
    }
  }

  drain_feedback_at_end();
  if (warmed_up_) {
    idle_stats_.elapsed_seconds = monotonic_seconds() - measurement_start_seconds_;
    const double end_cpu = process_cpu_seconds();
    if (idle_start_cpu_seconds_ >= 0.0 && end_cpu >= idle_start_cpu_seconds_ &&
        idle_stats_.elapsed_seconds > 0.0) {
      idle_stats_.cpu_percent =
          (end_cpu - idle_start_cpu_seconds_) / idle_stats_.elapsed_seconds * 100.0;
    }
  }

  if (soak) {
    const auto stats = pacer_.stats();
    // stderr, not MV_LOG_INFO: the POSIX log keeps only warnings and errors.
    std::fprintf(stderr,
                 "soak: %llu frames over %.1f s, %llu dropped, %llu discarded, %llu the display "
                 "could not judge, source: %s\n",
                 static_cast<unsigned long long>(stats.frames), stats.elapsed_seconds,
                 static_cast<unsigned long long>(stats.dropped_frames),
                 static_cast<unsigned long long>(stats.discarded_frames),
                 static_cast<unsigned long long>(stats.statistics_unavailable_frames),
                 gfx::presentation_drop_source_label(stats.source));
    if (!write_json_report()) exit_code_.store(2, std::memory_order_release);
    const bool passed = options_.start_animating ? stats.meets_linux_pr1_gate()
                                                 : idle_stats_.meets_linux_pr1_gate();
    if (options_.gate_exit_code && exit_code_.load(std::memory_order_acquire) == 0 &&
        !(passed && soak_complete_ && measurement_valid_))
      exit_code_.store(1, std::memory_order_release);
  }

  if (imgui_ready_) {
    ImGui_ImplOpenGL3_Shutdown();
    imgui_ready_ = false;
  }
  clock_.destroy();
  gl_surface_.destroy();
  device_.destroy();
  ImGui::DestroyContext();
  finished_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  signal_events();
}

void present_lab_linux::draw_frame(const input_snapshot& snapshot, double elapsed, float delta,
                                   float wheel) noexcept {
  const auto w = static_cast<float>(gl_surface_.width());
  const auto h = static_cast<float>(gl_surface_.height());
  glViewport(0, 0, static_cast<GLsizei>(gl_surface_.width()),
             static_cast<GLsizei>(gl_surface_.height()));
  // LINEAR values: the framebuffer is sRGB-encoded, so the hardware encodes on
  // write. The theme's canvas colour, as on the other labs; alpha 1, the
  // surface is opaque.
  glClearColor(home_linear_channel(snapshot.home.canvas, 24),
               home_linear_channel(snapshot.home.canvas, 16),
               home_linear_channel(snapshot.home.canvas, 8), 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  ImGui_ImplOpenGL3_NewFrame();
  feed_imgui(snapshot, delta, wheel);
  ImGui::NewFrame();

  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  const float scale = snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f;
  if (animating_) {
    // The judder sweep: the PR 1 workload on every lab.
    const auto phase = static_cast<float>(std::fmod(elapsed * 0.35, 1.0));
    const float bar_width = 6.0f * scale;
    const float x = phase * (w - bar_width);
    bg->AddRectFilled(ImVec2(x, 0.0f), ImVec2(x + bar_width, h), IM_COL32(230, 230, 235, 255));
    for (int i = 1; i < 10; ++i) {
      const float tx = w * (static_cast<float>(i) / 10.0f);
      bg->AddLine(ImVec2(tx, h - 24.0f * scale), ImVec2(tx, h), IM_COL32(90, 95, 110, 255), 1.0f);
    }
  } else {
    const char* hint = "Space  sweep      F3  frame-time      R  reset      Esc  quit";
    const ImVec2 size = ImGui::CalcTextSize(hint);
    bg->AddText(ImVec2((w - size.x) * 0.5f, (h - size.y) * 0.5f), IM_COL32(177, 181, 191, 255), hint);
  }
  if (overlay_visible_) draw_overlay(snapshot);

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void present_lab_linux::draw_overlay(const input_snapshot& snapshot) noexcept {
  const auto stats = pacer_.stats();
  const auto& info = device_.info();
  ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.72f);
  ImGui::Begin("##f3", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoMove);
  ImGui::Text("OpenGL present lab (Wayland)   F3 overlay   space sweep   R reset");
  ImGui::Text("%s", info.renderer);
  ImGui::Text("%s%s%s", info.version, info.drm_node[0] ? "   " : "", info.drm_node);
  const double hz = stats.refresh_interval_ms > 0.0 ? 1000.0 / stats.refresh_interval_ms : 0.0;
  ImGui::Text("display   %.3f ms (%.2f Hz)", stats.refresh_interval_ms, hz);
  ImGui::Text("surface   %ux%u  sRGB  scale %.3f  interval 0, frame-callback paced",
              gl_surface_.width(), gl_surface_.height(), static_cast<double>(snapshot.dpi_scale));
  ImGui::Separator();
  ImGui::Text("present-to-present  last %.3f  mean %.3f  p50 %.3f  p99 %.3f  max %.3f ms",
              stats.last_present_to_present_ms, stats.mean_ms, stats.p50_ms, stats.p99_ms,
              stats.max_ms);
  ImGui::Text("cpu frame           last %.3f  mean %.3f  p99 %.3f  max %.3f ms",
              stats.last_cpu_frame_ms, stats.cpu_mean_ms, stats.cpu_p99_ms, stats.cpu_max_ms);
  const auto& history = pacer_.history();
  const float top = stats.refresh_interval_ms > 0.0
                        ? static_cast<float>(stats.refresh_interval_ms * 3.0)
                        : 50.0f;
  ImGui::PlotLines("##p2p", history.data(), static_cast<int>(history.size()),
                   static_cast<int>(pacer_.history_cursor()), nullptr, 0.0f, top,
                   ImVec2(480.0f, 60.0f));
  const bool clean = stats.dropped_frames == 0 && stats.missed_refreshes == 0;
  ImGui::TextColored(clean ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(1.0f, 0.62f, 0.2f, 1.0f),
                     "dropped %llu (%llu missed refreshes, %llu discarded)",
                     static_cast<unsigned long long>(stats.dropped_frames),
                     static_cast<unsigned long long>(stats.missed_refreshes),
                     static_cast<unsigned long long>(stats.discarded_frames));
  ImGui::Text("source    %s   %llu frames, %llu displayed over %.1f s",
              gfx::presentation_drop_source_label(stats.source),
              static_cast<unsigned long long>(stats.frames),
              static_cast<unsigned long long>(stats.displayed_presents), stats.elapsed_seconds);
  ImGui::Text("feedback  %s, last flags 0x%x, %llu waiting, %llu not judged by the display",
              clock_name(clock_.clock_id()), clock_.last_flags(),
              static_cast<unsigned long long>(stats.pending_feedback),
              static_cast<unsigned long long>(stats.statistics_unavailable_frames));
  if (stats.source == gfx::presentation_drop_source::interval_heuristic) {
    ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.2f, 1.0f),
                       "weaker: some frames had no VSYNC + HW_CLOCK answer to count refreshes "
                       "from; pacing cannot be judged here");
  }
  if (device_rebuilds_ > 0) {
    ImGui::Text("device    rebuilt %llu time(s) after a GPU reset",
                static_cast<unsigned long long>(device_rebuilds_));
  }
  const char* state = animating_ ? "animating (lab sweep)" : (occluded_ ? "occluded" : "presenting");
  ImGui::Text("%s   [space] sweep   [R] reset   [F3] overlay", state);
  ImGui::End();
}

bool present_lab_linux::write_json_report() const noexcept {
  if (options_.json_report_path.empty()) return true;
  FILE* f = std::fopen(options_.json_report_path.c_str(), "wb");
  if (!f) {
    MV_LOG_ERROR("present_lab_linux: cannot write report");
    return false;
  }
  gfx::presentation_pace_json r;
  r.pace = pacer_.stats();
  r.idle = idle_stats_;
  r.static_run = !options_.start_animating;
  r.measurement_complete =
      soak_complete_ && measurement_valid_ && exit_code_.load(std::memory_order_acquire) == 0;
  r.meets_gate = r.measurement_complete && (options_.start_animating
                                                ? r.pace.meets_linux_pr1_gate()
                                                : r.idle.meets_linux_pr1_gate());
  const bool ok = gfx::write_pace_json(f, r);
  return std::fclose(f) == 0 && ok;
}

}  // namespace mv::shell
