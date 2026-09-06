// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/present_lab.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <avrt.h>

#include <cfloat>
#include <cmath>
#include <cstdio>

#include "core/trace.h"

namespace mv::shell {

namespace {

// Discarded before measurement begins. Long enough for DWM to pick the window
// up and for the font atlas to land; short enough that a 60 s soak is still a
// 60 s soak.
constexpr double kWarmupSeconds = 1.0;

std::int64_t qpc_now() noexcept {
  LARGE_INTEGER t{};
  ::QueryPerformanceCounter(&t);
  return t.QuadPart;
}

double qpc_seconds(std::int64_t ticks) noexcept {
  static const double inv = [] {
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    return 1.0 / static_cast<double>(f.QuadPart);
  }();
  return static_cast<double>(ticks) * inv;
}

const char* drop_source_label(gfx::drop_source s) noexcept {
  switch (s) {
    case gfx::drop_source::frame_statistics:   return "DXGI frame statistics";
    case gfx::drop_source::interval_heuristic: return "QPC intervals (weaker)";
    case gfx::drop_source::none:               return "no data";
  }
  return "unknown";
}

// Feeds ImGui from our own snapshot rather than from a window procedure. See
// the note in input_state.h for why the Win32 backend is not used.
void feed_imgui(const input_snapshot& s, float delta_seconds) noexcept {
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
  if (s.wheel != 0.0f) io.AddMouseWheelEvent(0.0f, s.wheel);
}

}  // namespace

present_lab::~present_lab() { stop(); }

expected present_lab::start(HWND window, const lab_options& options) noexcept {
  window_ = window;
  options_ = options;
  overlay_visible_ = options.overlay_visible;
  animating_ = options.start_animating;

  wake_event_ = ::CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr);
  if (!wake_event_) return err(status::internal);

  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this] { render_thread_main(); });
  return {};
}

void present_lab::stop() noexcept {
  // The join is unconditional. The render thread clears running_ itself when a
  // soak finishes or the device is unrecoverable, so keying the join off that
  // flag leaves a joinable std::thread to be destroyed — which is
  // std::terminate, reported as a stack-buffer-overrun exit code that looks
  // like anything except the missing join it is.
  running_.store(false, std::memory_order_release);
  wake();
  if (render_thread_.joinable()) render_thread_.join();
  if (wake_event_) {
    ::CloseHandle(wake_event_);
    wake_event_ = nullptr;
  }
}

void present_lab::wake() noexcept {
  if (wake_event_) ::SetEvent(wake_event_);
}

expected present_lab::rebuild_device() noexcept {
  if (imgui_ready_) {
    ImGui_ImplDX11_Shutdown();
    imgui_ready_ = false;
  }
  swapchain_.destroy();
  device_.destroy();

  RECT rc{};
  ::GetClientRect(window_, &rc);

  auto created = device_.create(window_);
  if (!created) return created;

  gfx::swapchain_desc desc{};
  desc.width = static_cast<std::uint32_t>(rc.right - rc.left);
  desc.height = static_cast<std::uint32_t>(rc.bottom - rc.top);
  // v1 presents 8-bit sRGB regardless of the display (D6). HDR output is v1.1;
  // the branch exists so it is one line then, not a rewrite.
  desc.hdr_output = false;

  auto sc = swapchain_.create(device_, window_, desc);
  if (!sc) return sc;

  if (!ImGui_ImplDX11_Init(device_.d3d(), device_.context())) return err(status::internal);
  imgui_ready_ = true;

  pacer_.begin_session(swapchain_.refresh_interval_seconds());
  return {};
}

void present_lab::render_thread_main() noexcept {
  ::SetThreadDescription(::GetCurrentThread(), L"mv.render");
  // Above the decode pool, below the system. The frame is the thing that must
  // not be late.
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

  // MMCSS. Thread priority alone does not stop the scheduler preempting a
  // present loop on a machine that is doing anything else; registering as a
  // multimedia task is what gets the guaranteed slice, and it is what every
  // media application on Windows does. Without it a busy desktop costs a
  // handful of vblanks per minute, which is exactly the size of the D6 gate.
  DWORD mmcss_task_index = 0;
  HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Games", &mmcss_task_index);
  if (mmcss) {
    ::AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
  } else {
    MV_LOG_WARN("present_lab: MMCSS unavailable; pacing is at the mercy of the scheduler");
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;   // no imgui.ini beside the exe
  ImGui::StyleColorsDark();

  if (auto built = rebuild_device(); !built) {
    MV_LOG_ERROR("present_lab: device creation failed (%s)", status_name(built.error()));
    exit_code_ = 2;
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    ImGui::DestroyContext();
    finished_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    ::PostMessageW(window_, WM_CLOSE, 0, 0);
    return;
  }

  const std::int64_t start_qpc = qpc_now();
  std::int64_t last_frame_qpc = start_qpc;

  while (running_.load(std::memory_order_acquire)) {
    const input_snapshot snapshot = input_.acquire();
    const double elapsed = qpc_seconds(qpc_now() - start_qpc);

    // --- Edge-triggered commands from the UI thread ---------------------
    if (snapshot.toggle_overlay_seq != seen_overlay_seq_) {
      seen_overlay_seq_ = snapshot.toggle_overlay_seq;
      overlay_visible_ = !overlay_visible_;
    }
    if (snapshot.toggle_animation_seq != seen_animation_seq_) {
      seen_animation_seq_ = snapshot.toggle_animation_seq;
      animating_ = !animating_;
      // Leaving and re-entering the active state must not score the idle gap
      // as a stall.
      pacer_.reset_window();
    }
    if (snapshot.reset_stats_seq != seen_reset_seq_) {
      seen_reset_seq_ = snapshot.reset_stats_seq;
      pacer_.reset_window();
    }

    // --- Swapchain lifecycle --------------------------------------------
    if (snapshot.display_change_seq != seen_display_seq_) {
      seen_display_seq_ = snapshot.display_change_seq;
      // The window may have moved to a monitor on another GPU. Same recovery
      // path as device removal (plan/03, "Adapter selection & hybrid GPUs").
      if (device_.adapter_changed_for(window_)) {
        MV_LOG_INFO("present_lab: adapter changed under the window; rebuilding");
        if (auto r = rebuild_device(); !r) break;
      } else {
        swapchain_.refresh_output_info();
        pacer_.set_refresh(swapchain_.refresh_interval_seconds());
      }
    }
    if (snapshot.resize_seq != seen_resize_seq_) {
      seen_resize_seq_ = snapshot.resize_seq;
      if (auto r = swapchain_.resize(snapshot.width, snapshot.height); !r) {
        if (r.error() == status::device_lost) {
          if (auto rb = rebuild_device(); !rb) break;
        }
      }
      pacer_.set_refresh(swapchain_.refresh_interval_seconds());
    }

    // --- Should we present at all? --------------------------------------
    // plan/03 rule 4: idle means stop presenting entirely (0 % GPU on a static
    // image), and keep presenting for ~500 ms after the last input so a flick
    // does not stutter at the tail.
    if (snapshot.mouse_in_client || snapshot.wheel != 0.0f) last_input_time_ = elapsed;
    const bool recently_active = (elapsed - last_input_time_) < 0.5;
    const bool wants_frame =
        snapshot.window_visible && !occluded_ && (animating_ || recently_active);

    if (!wants_frame) {
      ::WaitForSingleObject(wake_event_, occluded_ ? 200 : 100);
      if (occluded_ && swapchain_.present_test() == S_OK) {
        occluded_ = false;
        pacer_.reset_window();
      }
      last_frame_qpc = qpc_now();
      continue;
    }

    // --- The frame ------------------------------------------------------
    // Wait BEFORE recording, never after Present. This ordering is the whole
    // point of the waitable object (plan/03).
    if (!swapchain_.wait_for_next_frame()) {
      if (device_.removed_reason() != S_OK) {
        if (auto r = rebuild_device(); !r) break;
      }
      continue;
    }
    pacer_.frame_begin();

    const std::int64_t frame_qpc = qpc_now();
    const auto delta = static_cast<float>(qpc_seconds(frame_qpc - last_frame_qpc));
    last_frame_qpc = frame_qpc;

    ImGui_ImplDX11_NewFrame();
    feed_imgui(snapshot, delta);
    ImGui::NewFrame();

    draw_frame(snapshot, elapsed);
    if (overlay_visible_) draw_overlay(snapshot);

    ImGui::Render();

    ID3D11RenderTargetView* rtv = swapchain_.back_buffer_rtv();
    device_.context()->OMSetRenderTargets(1, &rtv, nullptr);

    // Clear to a colour, per the PR 1 brief. The values are LINEAR: the render
    // target view is _SRGB, so the hardware encodes on write. Writing 0.05 here
    // and reading back 0.05 in a screenshot would mean the sRGB view was lost.
    const float clear[4] = {0.016f, 0.018f, 0.024f, 1.0f};
    device_.context()->ClearRenderTargetView(rtv, clear);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // A composition swapchain cannot tear, so this is always a vsync present
    // under D1. The flag is plumbed because the PR 1 lab can be re-hosted on an
    // HWND swapchain to measure VRR behaviour (plan/03).
    const HRESULT hr = swapchain_.present(false);
    pacer_.frame_end(swapchain_.dxgi());

    if (hr == DXGI_STATUS_OCCLUDED) {
      occluded_ = true;
    } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
      MV_LOG_WARN("present_lab: device removed (0x%08lx); rebuilding",
                  static_cast<unsigned long>(device_.removed_reason()));
      if (auto r = rebuild_device(); !r) break;
    }

    // --- Warm-up ---------------------------------------------------------
    // The first presents after swapchain creation are not steady state: DWM has
    // not yet picked the window up, and the first frame carries ImGui's font
    // atlas upload. Measuring them reports a stall that is not in the thing
    // being verified. The window is dropped exactly once, and the report states
    // how much was discarded — a declared warm-up, not a quiet trim.
    if (!warmed_up_ && qpc_seconds(qpc_now() - start_qpc) >= kWarmupSeconds) {
      warmed_up_ = true;
      pacer_.reset_window();
    }

    // --- Soak termination ------------------------------------------------
    if (options_.soak_seconds > 0.0) {
      const double soaked = qpc_seconds(qpc_now() - start_qpc) - kWarmupSeconds;
      if (soaked >= options_.soak_seconds) break;
    }
  }

  const auto final_stats = pacer_.stats();
  if (options_.soak_seconds > 0.0) {
    MV_LOG_INFO("soak: %llu frames over %.1f s, %llu dropped (%llu missed refreshes), "
                "p50 %.3f ms p99 %.3f ms max %.3f ms, refresh %.3f ms, source: %s",
                static_cast<unsigned long long>(final_stats.frames),
                final_stats.elapsed_seconds,
                static_cast<unsigned long long>(final_stats.dropped_frames),
                static_cast<unsigned long long>(final_stats.missed_refreshes),
                final_stats.p50_ms, final_stats.p99_ms, final_stats.max_ms,
                final_stats.refresh_interval_ms, drop_source_label(final_stats.source));
    write_json_report();
    if (options_.gate_exit_code && !final_stats.meets_pr1_gate()) exit_code_ = 1;
  }

  if (imgui_ready_) {
    ImGui_ImplDX11_Shutdown();
    imgui_ready_ = false;
  }
  swapchain_.destroy();
  device_.destroy();
  ImGui::DestroyContext();
  if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);

  finished_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  ::PostMessageW(window_, WM_CLOSE, 0, 0);
}

void present_lab::draw_frame(const input_snapshot& snapshot, double elapsed_seconds) noexcept {
  if (!animating_) return;

  // A bar sweeping at a constant rate. This is the oldest judder instrument
  // there is and still the best one: a single dropped frame shows up as a
  // visible hitch in the sweep, before any number on the overlay changes.
  animation_phase_ = std::fmod(elapsed_seconds * 0.35, 1.0);

  const auto w = static_cast<float>(snapshot.width);
  const auto h = static_cast<float>(snapshot.height);
  if (w <= 0.0f || h <= 0.0f) return;

  const float bar_width = 6.0f * snapshot.dpi_scale;
  const float x = static_cast<float>(animation_phase_) * (w - bar_width);

  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(ImVec2(x, 0.0f), ImVec2(x + bar_width, h),
                    IM_COL32(230, 230, 235, 255));

  // Static reference ticks, so the sweep has something to be judged against.
  for (int i = 1; i < 10; ++i) {
    const float tx = w * (static_cast<float>(i) / 10.0f);
    bg->AddLine(ImVec2(tx, h - 24.0f * snapshot.dpi_scale), ImVec2(tx, h),
                IM_COL32(90, 95, 110, 255), 1.0f);
  }
}

void present_lab::draw_overlay(const input_snapshot& snapshot) noexcept {
  const auto stats = pacer_.stats();
  const float pad = 12.0f * snapshot.dpi_scale;

  ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.82f);
  ImGui::Begin("Frame time (F3)", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav);

  const double implied_hz =
      stats.refresh_interval_ms > 0.0 ? 1000.0 / stats.refresh_interval_ms : 0.0;

  ImGui::Text("%ls", device_.info().description);
  ImGui::Text("display   %.3f ms  (%.2f Hz)", stats.refresh_interval_ms, implied_hz);
  ImGui::Text("swapchain %ux%u  %s", swapchain_.width(), swapchain_.height(),
              swapchain_.tearing_supported() ? "tearing-capable" : "no tearing");
  ImGui::Separator();

  ImGui::Text("present-to-present");
  ImGui::Text("  last %6.3f ms   cpu %6.3f ms", stats.last_present_to_present_ms,
              stats.last_cpu_frame_ms);
  ImGui::Text("  mean %6.3f   p50 %6.3f   p99 %6.3f   max %6.3f", stats.mean_ms, stats.p50_ms,
              stats.p99_ms, stats.max_ms);
  ImGui::Text("cpu frame  mean %6.3f   p99 %6.3f   max %6.3f", stats.cpu_mean_ms,
              stats.cpu_p99_ms, stats.cpu_max_ms);

  ImGui::PlotLines("##frametimes", pacer_.history().data(),
                   static_cast<int>(gfx::pacer::history_size),
                   static_cast<int>(pacer_.history_cursor()), nullptr, 0.0f,
                   static_cast<float>(stats.refresh_interval_ms * 3.0),
                   ImVec2(240.0f * snapshot.dpi_scale, 48.0f * snapshot.dpi_scale));

  ImGui::Separator();

  // The verify line, stated as a result rather than left for the reader to
  // work out from the numbers above.
  const bool gate = stats.meets_pr1_gate();
  const ImVec4 colour = gate ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
                             : ImVec4(0.95f, 0.55f, 0.35f, 1.0f);
  ImGui::TextColored(colour, "dropped %llu  (%llu missed refreshes)",
                     static_cast<unsigned long long>(stats.dropped_frames),
                     static_cast<unsigned long long>(stats.missed_refreshes));
  ImGui::Text("source    %s", drop_source_label(stats.source));
  ImGui::Text("frames    %llu over %.1f s",
              static_cast<unsigned long long>(stats.frames), stats.elapsed_seconds);
  if (stats.source == gfx::drop_source::interval_heuristic) {
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                       "DXGI frame statistics unavailable: this is inferred,");
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                       "not the D6 gate.");
  }

  ImGui::Separator();
  ImGui::Text("%s   [space] animation   [R] reset   [F3] overlay",
              animating_ ? "animating" : "idle (not presenting)");
  ImGui::End();
}

void present_lab::write_json_report() const noexcept {
  if (options_.json_report_path.empty()) return;

  FILE* f = nullptr;
  if (::_wfopen_s(&f, options_.json_report_path.c_str(), L"wb") != 0 || f == nullptr) {
    MV_LOG_ERROR("present_lab: cannot write %ls", options_.json_report_path.c_str());
    return;
  }

  const auto s = pacer_.stats();
  std::fprintf(f,
               "{\n"
               "  \"schema\": 1,\n"
               "  \"warmup_seconds_discarded\": %.3f,\n"
               "  \"frames\": %llu,\n"
               "  \"elapsed_seconds\": %.6f,\n"
               "  \"refresh_interval_ms\": %.6f,\n"
               "  \"dropped_frames\": %llu,\n"
               "  \"missed_refreshes\": %llu,\n"
               "  \"statistics_discontinuities\": %llu,\n"
               "  \"mean_ms\": %.6f,\n"
               "  \"p50_ms\": %.6f,\n"
               "  \"p99_ms\": %.6f,\n"
               "  \"max_ms\": %.6f,\n"
               "  \"cpu_mean_ms\": %.6f,\n"
               "  \"cpu_p99_ms\": %.6f,\n"
               "  \"cpu_max_ms\": %.6f,\n"
               "  \"drop_source\": \"%s\",\n"
               "  \"meets_pr1_gate\": %s\n"
               "}\n",
               kWarmupSeconds,
               static_cast<unsigned long long>(s.frames),
               s.elapsed_seconds,
               s.refresh_interval_ms,
               static_cast<unsigned long long>(s.dropped_frames),
               static_cast<unsigned long long>(s.missed_refreshes),
               static_cast<unsigned long long>(s.statistics_discontinuities),
               s.mean_ms, s.p50_ms, s.p99_ms, s.max_ms,
               s.cpu_mean_ms, s.cpu_p99_ms, s.cpu_max_ms,
               drop_source_label(s.source),
               s.meets_pr1_gate() ? "true" : "false");
  std::fclose(f);
}

}  // namespace mv::shell
