// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/present_lab.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <avrt.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "abi/native.h"
#include "codec/format.h"
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

double process_cpu_seconds() noexcept {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
  const auto ticks = [](FILETIME t) {
    return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 10000000.0;
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

}  // namespace

present_lab::~present_lab() { stop(); }

expected present_lab::start(HWND window, const lab_options& options) noexcept {
  window_ = window;
  options_ = options;
  overlay_visible_ = options.overlay_visible;
  animating_ = options.start_animating;

  wake_event_ = ::CreateEventW(nullptr, FALSE /*auto-reset*/, FALSE, nullptr);
  if (!wake_event_) return err(status::internal);
  ready_event_ = ::CreateEventW(nullptr, TRUE /*manual-reset*/, FALSE, nullptr);
  if (!ready_event_) return err(status::internal);

  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this] { render_thread_main(); });
  // Wait until the render thread has a device (or has failed to make one).
  // Opening a file from argv right after start() needs the device already
  // attached, otherwise the first decode uploads nowhere.
  ::WaitForSingleObject(ready_event_, INFINITE);
  if (start_error_.load(std::memory_order_acquire) != 0) return err(status::internal);
  return {};
}

void present_lab::stop() noexcept {
  // The render thread can stop itself; always join a joinable thread.
  running_.store(false, std::memory_order_release);
  wake();
  if (render_thread_.joinable()) render_thread_.join();
  if (wake_event_) {
    ::CloseHandle(wake_event_);
    wake_event_ = nullptr;
  }
  if (ready_event_) {
    ::CloseHandle(ready_event_);
    ready_event_ = nullptr;
  }
}

void present_lab::wake() noexcept {
  if (wake_event_) ::SetEvent(wake_event_);
}

expected present_lab::rebuild_device() noexcept {
  if (warmed_up_) measurement_valid_ = false;
  if (imgui_ready_) {
    ImGui_ImplDX11_Shutdown();
    imgui_ready_ = false;
  }
  blitter_.destroy();
  current_image_.reset();
  if (session_) mv::abi::detach_device(session_);
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

  if (auto blit = blitter_.create(device_.d3d()); !blit) return blit;

  if (session_) {
    const status st = mv::abi::attach_device(session_, device_.d3d());
    if (st != status::ok) return err(st);
  }

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

  // Give the render thread a bounded multimedia scheduling priority.
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
    start_error_.store(1, std::memory_order_release);
    if (ready_event_) ::SetEvent(ready_event_);
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    ImGui::DestroyContext();
    finished_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    ::PostMessageW(window_, WM_CLOSE, 0, 0);
    return;
  }
  if (ready_event_) ::SetEvent(ready_event_);

  const std::int64_t start_qpc = qpc_now();
  std::int64_t last_frame_qpc = start_qpc;

  while (running_.load(std::memory_order_acquire)) {
    const input_snapshot snapshot = input_.acquire();
    const double elapsed = qpc_seconds(qpc_now() - start_qpc);
    if (!warmed_up_ && elapsed >= kWarmupSeconds) {
      warmed_up_ = true;
      if (total_presents_ == 0 || !snapshot.window_visible) measurement_valid_ = false;
      pacer_.reset_window();
      measurement_start_qpc_ = qpc_now();
      idle_start_cpu_seconds_ = process_cpu_seconds();
    }
    if (warmed_up_ && options_.soak_seconds > 0.0 &&
        qpc_seconds(qpc_now() - measurement_start_qpc_) >= options_.soak_seconds) {
      soak_complete_ = true;
      break;
    }
    if (warmed_up_ && !snapshot.window_visible) measurement_valid_ = false;
    const bool input_activity = input_cursor_.consume_activity(snapshot);
    if (input_activity && warmed_up_ && !options_.start_animating) ++idle_stats_.input_events;
    bool redraw = input_activity;

    // --- Edge-triggered commands from the UI thread ---------------------
    if (snapshot.toggle_overlay_seq != seen_overlay_seq_) {
      if ((snapshot.toggle_overlay_seq - seen_overlay_seq_) & 1u)
        overlay_visible_ = !overlay_visible_;
      seen_overlay_seq_ = snapshot.toggle_overlay_seq;
      redraw = true;
    }
    if (snapshot.toggle_animation_seq != seen_animation_seq_) {
      if ((snapshot.toggle_animation_seq - seen_animation_seq_) & 1u) animating_ = !animating_;
      seen_animation_seq_ = snapshot.toggle_animation_seq;
      redraw = true;
      if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
      // Leaving and re-entering the active state must not score the idle gap
      // as a stall.
      if (options_.soak_seconds == 0.0) pacer_.reset_window();
    }
    if (snapshot.reset_stats_seq != seen_reset_seq_) {
      seen_reset_seq_ = snapshot.reset_stats_seq;
      redraw = true;
      if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
      if (options_.soak_seconds == 0.0) pacer_.reset_window();
    }

    // --- Swapchain lifecycle --------------------------------------------
    if (snapshot.display_change_seq != seen_display_seq_) {
      seen_display_seq_ = snapshot.display_change_seq;
      redraw = true;
      // The window may have moved to a monitor on another GPU. Same recovery
      // path as device removal (plan/03, "Adapter selection & hybrid GPUs").
      if (device_.adapter_changed_for(window_)) {
        MV_LOG_INFO("present_lab: adapter changed under the window; rebuilding");
        if (auto r = rebuild_device(); !r) { exit_code_ = 2; break; }
      } else {
        const double previous_refresh = swapchain_.refresh_interval_seconds();
        swapchain_.refresh_output_info();
        if (warmed_up_ && previous_refresh != swapchain_.refresh_interval_seconds())
          measurement_valid_ = false;
        pacer_.set_refresh(swapchain_.refresh_interval_seconds());
      }
    }
    if (snapshot.resize_seq != seen_resize_seq_) {
      seen_resize_seq_ = snapshot.resize_seq;
      redraw = true;
      if (auto r = swapchain_.resize(snapshot.width, snapshot.height); !r) {
        MV_LOG_WARN("present_lab: resize failed (%s); rebuilding", status_name(r.error()));
        if (auto rb = rebuild_device(); !rb) { exit_code_ = 2; break; }
      }
      pacer_.set_refresh(swapchain_.refresh_interval_seconds());
      // Snap-fit on resize so dragging the window never waits on a spring
      // (PR 2 verify: stay smooth while a large decode is in flight).
      if (current_image_ && camera_.fit_mode()) {
        camera_.fit(static_cast<float>(current_image_->width),
                    static_cast<float>(current_image_->height),
                    static_cast<float>(snapshot.width),
                    static_cast<float>(snapshot.height), true);
      }
    }

    if (session_) {
      if (image::gpu_image* ready = mv::abi::take_ready_image(session_)) {
        gfx::com_ptr<ID3D11Device> mine;
        if (device_.d3d()) device_.d3d()->QueryInterface(IID_PPV_ARGS(mine.GetAddressOf()));
        if (!ready->device || !mine || ready->device.Get() != mine.Get()) {
          mv::abi::release_gpu_image(ready);
        } else {
          current_image_.reset(ready);
          camera_.fit(static_cast<float>(current_image_->width),
                      static_cast<float>(current_image_->height),
                      static_cast<float>(snapshot.width),
                      static_cast<float>(snapshot.height), true);
          last_input_time_ = elapsed;
          redraw = true;
        }
      }
    }
    if (snapshot.fit_seq != seen_fit_seq_) {
      seen_fit_seq_ = snapshot.fit_seq;
      if (current_image_) {
        camera_.fit(static_cast<float>(current_image_->width),
                    static_cast<float>(current_image_->height),
                    static_cast<float>(snapshot.width),
                    static_cast<float>(snapshot.height), false);
      }
      redraw = true;
    }
    if (snapshot.one_to_one_seq != seen_one_seq_) {
      seen_one_seq_ = snapshot.one_to_one_seq;
      camera_.one_to_one();
      redraw = true;
    }

    const float wheel = input_cursor_.consume_wheel(snapshot);
    if (current_image_ && wheel != 0.0f) {
      camera_.wheel_toward(snapshot.mouse_x, snapshot.mouse_y, wheel,
                           static_cast<float>(snapshot.width),
                           static_cast<float>(snapshot.height),
                           static_cast<float>(current_image_->width),
                           static_cast<float>(current_image_->height));
      redraw = true;
    }
    {
      const bool left = snapshot.mouse_down[0];
      if (left && !was_left_down_) camera_.drag_begin();
      if (left && was_left_down_ && current_image_) {
        camera_.drag_delta(snapshot.mouse_x - last_mouse_x_, snapshot.mouse_y - last_mouse_y_);
      }
      if (!left && was_left_down_) camera_.drag_end();
      was_left_down_ = left;
      last_mouse_x_ = snapshot.mouse_x;
      last_mouse_y_ = snapshot.mouse_y;
    }

    // --- Should we present at all? --------------------------------------
    // plan/03 rule 4: idle means stop presenting entirely (0 % GPU on a static
    // image), and keep presenting for ~500 ms after the last input so a flick
    // does not stutter at the tail.
    if (redraw) last_input_time_ = elapsed;
    const bool recently_active = (elapsed - last_input_time_) < 0.5;
    const bool wants_frame =
        snapshot.window_visible && !occluded_ &&
        (animating_ || recently_active || camera_.moving());

    if (!wants_frame) {
      was_presenting_ = false;
      // Idle has no polling timer, except occlusion probes and soak deadlines.
      DWORD timeout = occluded_ ? 200u : INFINITE;
      if (options_.soak_seconds > 0.0) {
        const double remaining = warmed_up_
            ? options_.soak_seconds - qpc_seconds(qpc_now() - measurement_start_qpc_)
            : kWarmupSeconds - elapsed;
        const auto deadline_ms = static_cast<DWORD>(std::ceil(std::max(0.0, remaining) * 1000.0));
        timeout = std::min(timeout, deadline_ms);
      }
      HANDLE waits[2] = {wake_event_, nullptr};
      DWORD n = 1;
      if (void* ready = session_ ? mv::abi::image_ready_wait_handle(session_) : nullptr) {
        waits[1] = static_cast<HANDLE>(ready);
        n = 2;
      }
      ::WaitForMultipleObjects(n, waits, FALSE, timeout);
      if (occluded_ && swapchain_.present_test() == S_OK) {
        occluded_ = false;
        if (options_.soak_seconds == 0.0) pacer_.reset_window();
      }
      last_frame_qpc = qpc_now();
      continue;
    }

    if (!was_presenting_ && options_.soak_seconds == 0.0) pacer_.reset_window();
    was_presenting_ = true;

    // --- The frame ------------------------------------------------------
    // Wait BEFORE recording, never after Present. This ordering is the whole
    // point of the waitable object (plan/03).
    if (!swapchain_.wait_for_next_frame()) {
      if (device_.removed_reason() != S_OK) {
        if (auto r = rebuild_device(); !r) { exit_code_ = 2; break; }
      }
      continue;
    }
    pacer_.frame_begin();

    const std::int64_t frame_qpc = qpc_now();
    const auto delta = static_cast<float>(qpc_seconds(frame_qpc - last_frame_qpc));
    last_frame_qpc = frame_qpc;

    camera_.step(delta);

    ImGui_ImplDX11_NewFrame();
    feed_imgui(snapshot, delta, wheel);
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

    if (current_image_ && current_image_->srv) {
      D3D11_VIEWPORT vp{};
      vp.Width = static_cast<float>(swapchain_.width());
      vp.Height = static_cast<float>(swapchain_.height());
      vp.MaxDepth = 1.0f;
      device_.context()->RSSetViewports(1, &vp);
      gfx::blit_params bp{};
      bp.pan_x = camera_.pan_x();
      bp.pan_y = camera_.pan_y();
      bp.zoom = camera_.zoom();
      bp.window_w = static_cast<float>(swapchain_.width());
      bp.window_h = static_cast<float>(swapchain_.height());
      bp.image_w = static_cast<float>(current_image_->width);
      bp.image_h = static_cast<float>(current_image_->height);
      blitter_.draw(device_.context(), current_image_->srv.Get(), bp);
    }

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // A composition swapchain cannot tear, so this is always a vsync present
    // under D1. The flag is plumbed because the PR 1 lab can be re-hosted on an
    // HWND swapchain to measure VRR behaviour (plan/03).
    const HRESULT hr = swapchain_.present(false);
    if (warmed_up_ && !options_.start_animating) ++idle_stats_.presents;
    if (hr == S_OK) {
      ++total_presents_;
      pacer_.frame_end(swapchain_.dxgi());
    }
    else measurement_valid_ = false;

    if (hr == DXGI_STATUS_OCCLUDED) {
      occluded_ = true;
    } else if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
      MV_LOG_WARN("present_lab: device removed (0x%08lx); rebuilding",
                  static_cast<unsigned long>(device_.removed_reason()));
      if (auto r = rebuild_device(); !r) { exit_code_ = 2; break; }
    }

    if (FAILED(hr) && hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) {
      exit_code_ = 2;
      break;
    }
  }
  if (warmed_up_) {
    idle_stats_.elapsed_seconds = qpc_seconds(qpc_now() - measurement_start_qpc_);
    const double end_cpu = process_cpu_seconds();
    if (idle_start_cpu_seconds_ >= 0.0 && end_cpu >= idle_start_cpu_seconds_ &&
        idle_stats_.elapsed_seconds > 0.0) {
      idle_stats_.cpu_percent = (end_cpu - idle_start_cpu_seconds_) /
                                idle_stats_.elapsed_seconds * 100.0;
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
    if (!write_json_report()) exit_code_ = 2;
    const bool passed = options_.start_animating ? final_stats.meets_pr1_gate()
                                                 : idle_stats_.meets_pr1_gate();
    if (options_.gate_exit_code && exit_code_ == 0 &&
        (!passed || !measurement_valid_ || !soak_complete_)) exit_code_ = 1;
  }

  if (imgui_ready_) {
    ImGui_ImplDX11_Shutdown();
    imgui_ready_ = false;
  }
  blitter_.destroy();
  current_image_.reset();
  if (session_) mv::abi::detach_device(session_);
  swapchain_.destroy();
  device_.destroy();
  ImGui::DestroyContext();
  if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);

  finished_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  ::PostMessageW(window_, WM_CLOSE, 0, 0);
}

void present_lab::draw_frame(const input_snapshot& snapshot, double elapsed_seconds) noexcept {
  if (current_image_) return;
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

  if (current_image_) {
    ImGui::Separator();
    ImGui::Text("image    %ux%u  %s  %s  %u mips", current_image_->width, current_image_->height,
                codec::format_name(current_image_->format),
                current_image_->icc_tagged ? "ICC tagged" : "untagged (sRGB)",
                current_image_->mip_levels);
    ImGui::Text("view     zoom %.2f  pan %.1f, %.1f  %s", camera_.zoom(), camera_.pan_x(),
                camera_.pan_y(), camera_.fit_mode() ? "fit" : (camera_.zoom() == 1.0f ? "100%" : ""));
    ImGui::Text("decode is off the render thread — pan must not start one");
  }

  ImGui::Separator();
  ImGui::Text("%s   [space] animation   [R] reset   [F3] overlay",
              animating_ ? "animating" : "idle (not presenting)");
  if (current_image_) {
    ImGui::Text("[0] fit   [1] 100%%   wheel zoom   drag pan   drop / Ctrl+O");
  }
  ImGui::End();
}

bool present_lab::write_json_report() const noexcept {
  if (options_.json_report_path.empty()) return true;

  FILE* f = nullptr;
  if (::_wfopen_s(&f, options_.json_report_path.c_str(), L"wb") != 0 || f == nullptr) {
    MV_LOG_ERROR("present_lab: cannot write %ls", options_.json_report_path.c_str());
    return false;
  }

  const auto s = pacer_.stats();
  std::fprintf(f,
               "{\n"
               "  \"schema\": 2,\n"
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
               "  \"statistics_unavailable_frames\": %llu,\n"
               "  \"displayed_presents\": %llu,\n"
               "  \"static\": %s,\n"
               "  \"measurement_complete\": %s,\n"
               "  \"idle_elapsed_seconds\": %.6f,\n"
               "  \"idle_cpu_percent\": %.6f,\n"
               "  \"idle_presents\": %llu,\n"
               "  \"idle_input_events\": %llu,\n"
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
               static_cast<unsigned long long>(s.statistics_unavailable_frames),
               static_cast<unsigned long long>(s.displayed_presents),
               options_.start_animating ? "false" : "true",
               soak_complete_ && measurement_valid_ && exit_code_ == 0 ? "true" : "false",
               idle_stats_.elapsed_seconds, idle_stats_.cpu_percent,
               static_cast<unsigned long long>(idle_stats_.presents),
               static_cast<unsigned long long>(idle_stats_.input_events),
               soak_complete_ && measurement_valid_ && exit_code_ == 0 &&
                   (options_.start_animating ? s.meets_pr1_gate() : idle_stats_.meets_pr1_gate())
                   ? "true" : "false");
  const bool written = std::ferror(f) == 0;
  return std::fclose(f) == 0 && written;
}

}  // namespace mv::shell
