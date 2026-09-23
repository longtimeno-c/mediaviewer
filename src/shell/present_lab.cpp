// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/present_lab.h"
#include "shell/dino_draw.h"
#include "shell/welcome_screen.h"

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

// Usable canvas below the command-bar strip. Camera, blit, and wheel-toward
// all share this rect so a fitted image is not hidden under the island.
struct canvas_view {
  float x = 0.0f;
  float y = 0.0f;
  float w = 1.0f;
  float h = 1.0f;
};

// Gap between the photo and the filmstrip (and a smaller one under the bar)
// so chrome never sits on the pixels.
constexpr float kCanvasGutterDip = 16.0f;

canvas_view usable_canvas(const input_snapshot& s) noexcept {
  canvas_view v;
  v.w = static_cast<float>(s.width);
  v.h = static_cast<float>(s.height);
  const float left = static_cast<float>(s.chrome_left_px);
  if (left > 0.0f && left < v.w) {
    v.x = left;
    v.w -= left;
  }
  const float scale = s.dpi_scale > 0.0f ? s.dpi_scale : 1.0f;
  const float gutter = kCanvasGutterDip * scale;
  const float top = static_cast<float>(s.chrome_height_px);
  const float bottom = static_cast<float>(s.chrome_bottom_px);
  if (top > 0.0f && top < v.h) {
    v.y = top + gutter;
    v.h -= top + gutter;
  }
  if (bottom > 0.0f && bottom < v.h) v.h -= bottom + gutter;
  if (v.h < 1.0f) v.h = 1.0f;
  return v;
}

// Hold-Z loupe: a square at the cursor (or the canvas centre with no cursor),
// kept inside the canvas. The blit and the frame drawn around it both use this.
struct loupe_box {
  float x = 0.0f;
  float y = 0.0f;
  float size = 0.0f;
  float point_x = 0.0f;
  float point_y = 0.0f;
};

loupe_box loupe_rect(const input_snapshot& s, const canvas_view& v) noexcept {
  const float scale = s.dpi_scale > 0.0f ? s.dpi_scale : 1.0f;
  loupe_box b;
  b.size = std::min({240.0f * scale, v.w, v.h});
  // Keyboard nudges (arrows while Z is held) offset the cursor or centre.
  const float step = 0.05f * std::min(v.w, v.h);
  const float base_x = s.mouse_in_client ? s.mouse_x : v.x + v.w * 0.5f;
  const float base_y = s.mouse_in_client ? s.mouse_y : v.y + v.h * 0.5f;
  b.point_x = std::clamp(base_x + static_cast<float>(s.loupe_steps_x) * step, v.x, v.x + v.w);
  b.point_y = std::clamp(base_y + static_cast<float>(s.loupe_steps_y) * step, v.y, v.y + v.h);
  b.x = std::clamp(b.point_x - b.size * 0.5f, v.x, v.x + v.w - b.size);
  b.y = std::clamp(b.point_y - b.size * 0.5f, v.y, v.y + v.h - b.size);
  return b;
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
  sweep_mode_ = options.soak_seconds > 0.0;

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

// --- PR 7 no-pop instrument -------------------------------------------------
// plan/10 PR 7: "the full decode replaces it without a visible pop". A person
// still has to look at a RAW once; these three make the rest of it a number in
// the report, so a hard cut, a jumped view or a stutter inside the fade fails a
// gate instead of passing quietly. Render thread only.
void present_lab::refine_fade_begun() noexcept {
  // A fade that starts while one is running is the same pop, counted honestly:
  // the one it interrupts never reached alpha 1.
  if (refine_fade_running_) ++refine_fades_cancelled_;
  ++refine_fades_started_;
  refine_fade_running_ = true;
  refine_fade_drops_at_start_ = pacer_.dropped_frames_so_far();
}

void present_lab::refine_fade_abandoned() noexcept {
  if (!refine_fade_running_) return;
  ++refine_fades_cancelled_;
  refine_fade_running_ = false;
}

void present_lab::refine_fade_tick(double elapsed) noexcept {
  if (!refine_fade_running_) return;
  if (fade_.active(elapsed)) {
    ++refine_fade_frames_;
    return;
  }
  // Reached alpha 1 on real time: the swap is finished, not cut short.
  ++refine_fades_completed_;
  refine_fade_running_ = false;
  const std::uint64_t now = pacer_.dropped_frames_so_far();
  if (now > refine_fade_drops_at_start_) refine_fade_dropped_ += now - refine_fade_drops_at_start_;
}

expected present_lab::rebuild_device() noexcept {
  if (warmed_up_) measurement_valid_ = false;
  if (imgui_ready_) {
    ImGui_ImplDX11_Shutdown();
    imgui_ready_ = false;
  }
  blitter_.destroy();
  video_blitter_.destroy();
  current_video_ = {};
  current_image_.reset();
  previous_image_.reset();  // hold-previous's texture belongs to the same device
  fade_from_.reset();
  refine_base_.reset();
  fade_.cancel();
  refine_fade_abandoned();
  tile_draws_ = {};
  anim_frame_.reset();
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
  if (auto blit = video_blitter_.create(device_.d3d()); !blit) return blit;

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
  {
    // Same CozetteVector.ttf the chrome island loads, so empty-canvas type
    // and the Open/View/About bar match. 16 px is a readable terminal size.
    // Falls back to ImGui's embedded ProggyClean if the file is missing.
    char exe[MAX_PATH]{};
    if (::GetModuleFileNameA(nullptr, exe, MAX_PATH) > 0) {
      char* slash = nullptr;
      for (char* p = exe; *p; ++p) {
        if (*p == '\\' || *p == '/') slash = p;
      }
      if (slash) {
        *slash = '\0';
        char font_path[MAX_PATH]{};
        if (::sprintf_s(font_path, "%s\\CozetteVector.ttf", exe) > 0) {
          if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path, 16.0f)) {
            ImGui::GetIO().FontDefault = font;
          }
        }
      }
    }
  }

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
    if (snapshot.game_exit_seq != seen_game_exit_seq_) {
      seen_game_exit_seq_ = snapshot.game_exit_seq;
      if (!sweep_mode_ && game_.active()) {
        game_.leave();
        animating_ = false;
        redraw = true;
      }
    }
    if (snapshot.toggle_animation_seq != seen_animation_seq_) {
      const std::uint32_t presses = snapshot.toggle_animation_seq - seen_animation_seq_;
      if (sweep_mode_) {
        if (presses & 1u) animating_ = !animating_;
      } else {
        for (std::uint32_t i = 0; i < presses && i < 4u; ++i) game_.press();
        animating_ = game_.state() == dino_game::phase::intro || game_.state() == dino_game::phase::playing;
      }
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
      if ((current_image_ || current_video_.texture) && camera_.fit_mode()) {
        const auto view = usable_canvas(snapshot);
        camera_.fit(static_cast<float>(media_width()),
                    static_cast<float>(media_height()),
                    view.w, view.h, true);
      } else if ((current_image_ || current_video_.texture) && camera_.fill_mode()) {
        // Fill is a mode too: `4` then `F` must still cover the new canvas.
        const auto view = usable_canvas(snapshot);
        camera_.fill(media_width(), media_height(), view.w, view.h, true);
      }
    }

    if (snapshot.discard_media_seq != seen_discard_seq_) {
      seen_discard_seq_ = snapshot.discard_media_seq;
      current_image_.reset();
      previous_image_.reset();
      fade_from_.reset();
      refine_base_.reset();
      fade_.cancel();
      refine_fade_abandoned();
      anim_frame_.reset();
      current_video_ = {};
      redraw = true;
    }
    if (session_) {
      if (image::gpu_image* ready = mv::abi::take_ready_image(session_)) {
        gfx::com_ptr<ID3D11Device> mine;
        if (device_.d3d()) device_.d3d()->QueryInterface(IID_PPV_ARGS(mine.GetAddressOf()));
        if (!ready->device || !mine || ready->device.Get() != mine.Get()) {
          mv::abi::release_gpu_image(ready);
        } else {
          const float old_w = media_width();
          const float old_h = media_height();
          const bool had_media = old_w > 0.0f && old_h > 0.0f;
          const auto identity = [](const image::gpu_image& g) {
            return canvas::publish_identity{g.item_key, g.view_generation,
                                            static_cast<canvas::image_quality>(g.quality)};
          };
          const bool has_still = current_image_ != nullptr && !current_video_.texture;
          const canvas::publish_kind kind = canvas::classify_publish(
              has_still, has_still ? identity(*current_image_) : canvas::publish_identity{},
              identity(*ready));
          if (kind == canvas::publish_kind::stale) {
            // A preview that lost the race to its own full decode.
            mv::abi::release_gpu_image(ready);
            ++stale_drops_;
          } else if (kind == canvas::publish_kind::refinement) {
            // plan/04 step 4: the same item at a better quality. The view is
            // kept as a fraction of the image — including a zoom or pan made
            // while it loaded — and the new texture fades in over the old one.
            // Hold-previous and a playing animation are not touched.
            ++refinements_;
            const auto view = usable_canvas(snapshot);
            // The pop this verify line is about is geometric: where the
            // picture's edges sit on screen before the swap, and where they sit
            // after. Measured around camera_.refine, because that call is the
            // only thing that can move them — the fade is alpha only.
            const float old_zoom = camera_.zoom();
            const float old_pan_x = camera_.pan_x();
            const float old_pan_y = camera_.pan_y();
            camera_.refine(old_w, old_h, static_cast<float>(ready->width),
                           static_cast<float>(ready->height), view.w, view.h);
            if (old_w > 0.0f && old_h > 0.0f) {
              // A screen position is (p - pan) * zoom plus a constant the two
              // views share, so the constant cancels in the difference.
              const auto edge = [](float p, float pan, float zoom) {
                return static_cast<double>((p - pan) * zoom);
              };
              const float nw = static_cast<float>(ready->width);
              const float nh = static_cast<float>(ready->height);
              const double before[4] = {
                  edge(0.0f, old_pan_x, old_zoom), edge(old_w, old_pan_x, old_zoom),
                  edge(0.0f, old_pan_y, old_zoom), edge(old_h, old_pan_y, old_zoom)};
              const double after[4] = {
                  edge(0.0f, camera_.pan_x(), camera_.zoom()),
                  edge(nw, camera_.pan_x(), camera_.zoom()),
                  edge(0.0f, camera_.pan_y(), camera_.zoom()),
                  edge(nh, camera_.pan_y(), camera_.zoom())};
              for (int i = 0; i < 4; ++i) {
                const double shift = std::abs(after[i] - before[i]);
                if (shift > refine_max_edge_shift_px_) refine_max_edge_shift_px_ = shift;
              }
              const double shown_before = before[1] - before[0];
              const double shown_after = after[1] - after[0];
              if (shown_before > 0.0 && shown_after > 0.0) {
                const double ratio = shown_after > shown_before ? shown_after / shown_before
                                                                : shown_before / shown_after;
                if (ratio > refine_max_scale_step_) refine_max_scale_step_ = ratio;
              }
            }
            if (current_image_->texture.Get() != ready->texture.Get()) {
              const bool keep_as_base = ready->tiles && !current_image_->tiles &&
                                        current_image_->texture_width > ready->texture_width;
              if (keep_as_base) {
                // Sharper than the overview: stays under the tiles, which then
                // sharpen what is already there. Nothing to fade.
                refine_base_ = std::move(current_image_);
                fade_from_.reset();
                fade_.cancel();
                refine_fade_abandoned();
              } else if (fade_.active(elapsed) && fade_from_ &&
                         current_image_->width == ready->width &&
                         current_image_->height == ready->height) {
                // A still refines twice: full_top (the top level, as soon as it
                // exists) and then full (with its mip chain), the same pixels
                // both times. Restarting the fade here would snap the outgoing
                // preview from wherever it had got to straight back out —
                // measured at 0.6 alpha on a CR2, which on a RAW is a
                // brightness step of most of the preview-to-render difference.
                // One fade, from the preview, running to its end: the incoming
                // texture is swapped under it instead.
                current_image_.reset();
              } else {
                fade_.begin(elapsed, canvas::refine_fade_seconds(current_image_->mean_luma,
                                                                 ready->mean_luma));
                fade_from_ = std::move(current_image_);
                refine_fade_begun();
              }
            }
            current_image_.reset(ready);
            if (ready->quality == image::gpu_quality::full && full_seconds_ < 0.0) {
              full_seconds_ = elapsed;
            }
            redraw = true;
          } else {
            fade_from_.reset();
            refine_base_.reset();
            fade_.cancel();
            refine_fade_abandoned();
            seen_tile_seq_ = 0;
            item_start_seconds_ = elapsed;
            first_pixel_seconds_ = elapsed;
            full_seconds_ = ready->quality == image::gpu_quality::full ? elapsed : -1.0;
            tiles_complete_seconds_ = -1.0;
            current_video_ = {};
            // Hold-previous keeps the last *different* still; a re-publish of
            // the same texture is not an advance.
            if (current_image_ && current_image_->texture.Get() != ready->texture.Get()) {
              previous_image_ = std::move(current_image_);
              ++previous_image_changes_;
            }
            // A playing animation is not reset here: a re-publish of the same
            // item (a refinement, an LRU revisit) must not jump back to frame 0.
            // A new item is a new generation, which retires it in the
            // animation block (review note 35).
            current_image_.reset(ready);
            {
              const auto view = usable_canvas(snapshot);
              // plan/16 sticky zoom: off (default) fits every item; on keeps the
              // mode, or the zoom and pan fraction. Camera state only, so
              // prefetch is untouched.
              if (snapshot.sticky_zoom && had_media && camera_.fill_mode()) {
                camera_.fill(media_width(), media_height(), view.w, view.h, true);
              } else if (snapshot.sticky_zoom && had_media && !camera_.fit_mode()) {
                camera_.carry(old_w, old_h, media_width(), media_height(), view.w, view.h);
              } else {
                camera_.fit(static_cast<float>(media_width()),
                            static_cast<float>(media_height()),
                            view.w, view.h, true);
              }
            }
            last_input_time_ = elapsed;
            redraw = true;
          }
        }
      }
    }
    // A tile landing is one more frame, not an input tail.
    bool paint_once = false;
    if (current_image_ && current_image_->tiles) {
      const std::uint64_t seq = current_image_->tiles->ready_sequence();
      if (seq != seen_tile_seq_) {
        seen_tile_seq_ = seq;
        paint_once = true;
      }
    }
    if (session_) {
      std::uint32_t generation = 0;
      (void)mv_session_current_generation(session_, &generation);
      if (current_video_.texture && current_video_.generation != generation) { current_video_ = {}; redraw = true; }
      player::video_frame unused;
      (void)mv::abi::poll_video(session_, -1, unused, video_active_);
      // A clip becoming open is a reason to paint. The loader signals the
      // image-ready event when it publishes one, so the render thread does
      // wake — but nothing here used to set `redraw`, so a paused clip (or one
      // whose first frame had not landed by the wake) left the empty-canvas
      // welcome on screen and parked again with painted_static_ already true.
      const bool was_video_open = video_open_;
      video_open_ = mv::abi::video_open(session_);
      if (video_open_ != was_video_open) redraw = true;
    }

    // Animation (plan/04, PR 6). Frames come from the session's decode ring
    // into anim_frame_ — never through the ready-image branch — so hold-previous
    // keeps the last *item* and the camera is not refitted per frame.
    if (session_) {
      std::uint32_t anim_gen = 0;
      (void)mv_session_current_generation(session_, &anim_gen);
      if (anim_gen != anim_generation_) {
        mv::abi::animation_retire(session_, anim_gen);
        anim_generation_ = anim_gen;
        anim_frame_.reset();
        anim_schedule_.reset();
        anim_finished_ = false;
        anim_seeking_ = false;
      }
      const bool anim_open = current_image_ && !current_video_.texture &&
                             mv::abi::animation_open(session_, anim_gen);
      const auto now_ms = static_cast<std::uint64_t>(qpc_seconds(qpc_now()) * 1000.0);
      // A frame more than two refreshes late restarts the cadence (and counts).
      const auto slack_ms =
          static_cast<std::uint32_t>(swapchain_.refresh_interval_seconds() * 2000.0) + 1;

      if (snapshot.anim_toggle_seq != seen_anim_toggle_seq_) {
        seen_anim_toggle_seq_ = snapshot.anim_toggle_seq;
        if (anim_open) {
          if (anim_finished_) {
            // Space on a played-out animation plays it again from the start.
            mv::abi::animation_seek(session_, 0);
            anim_schedule_.reset();
            anim_finished_ = false;
          } else if (anim_schedule_.paused()) {
            anim_schedule_.resume(now_ms);
          } else {
            anim_schedule_.pause(now_ms);
          }
        }
        redraw = true;
      }
      if (snapshot.anim_steps != seen_anim_steps_) {
        const std::int64_t steps = snapshot.anim_steps - seen_anim_steps_;
        seen_anim_steps_ = snapshot.anim_steps;
        if (anim_open) {
          anim_schedule_.pause(now_ms);
          if (steps < 0) {
            mv::abi::animation_seek(session_, anim_index_ > 0 ? anim_index_ - 1 : 0);
          } else if (anim_finished_) {
            mv::abi::animation_seek(session_, 0);
          }
          anim_finished_ = false;
          anim_seeking_ = true;
        }
        redraw = true;
      }

      if (anim_open && (anim_seeking_ || anim_schedule_.due(now_ms))) {
        image::gpu_image* texture = nullptr;
        std::uint32_t delay_ms = 0;
        std::uint32_t index = 0;
        if (mv::abi::take_animation_frame(session_, anim_gen, texture, delay_ms, index)) {
          anim_frame_.reset(texture);
          anim_index_ = index;
          if (anim_seeking_) {
            anim_schedule_.stepped(delay_ms);
            anim_seeking_ = false;
          } else {
            const std::uint32_t late_before = anim_schedule_.late();
            anim_schedule_.shown(delay_ms, now_ms, slack_ms);
            anim_late_total_ += anim_schedule_.late() - late_before;
            ++anim_frames_shown_;
            anim_delay_sum_ms_ += delay_ms;
          }
          redraw = true;
        }
      }
      if (anim_open && !anim_seeking_) {
        anim_finished_ = mv::abi::animation_finished(session_, anim_gen);
      }
      // Presents while it plays (or while a step is on its way); paused or
      // played out, the canvas idles like any still.
      anim_live_ = anim_open && (anim_seeking_ || (!anim_schedule_.paused() && !anim_finished_));
      const animation_state state =
          !anim_open                     ? animation_state::none
          : anim_schedule_.paused()      ? animation_state::paused
          : anim_finished_               ? animation_state::finished
          : mv::abi::animation_loops_forever(session_) ? animation_state::playing_forever
                                                       : animation_state::playing;
      anim_state_.store(static_cast<std::uint8_t>(state), std::memory_order_relaxed);
    }

    if (snapshot.fit_seq != seen_fit_seq_) {
      seen_fit_seq_ = snapshot.fit_seq;
      if (current_image_ || current_video_.texture) {
        const auto view = usable_canvas(snapshot);
        camera_.fit(static_cast<float>(media_width()),
                    static_cast<float>(media_height()),
                    view.w, view.h, false);
      }
      redraw = true;
    }
    if (snapshot.one_to_one_seq != seen_one_seq_) {
      seen_one_seq_ = snapshot.one_to_one_seq;
      camera_.one_to_one();
      redraw = true;
    }
    if (snapshot.zoom_in_seq != seen_zoom_in_seq_) {
      seen_zoom_in_seq_ = snapshot.zoom_in_seq;
      if (current_image_ || current_video_.texture) {
        const auto view = usable_canvas(snapshot);
        camera_.wheel_toward(view.w * 0.5f, view.h * 0.5f, 1.0f, view.w, view.h,
                             static_cast<float>(media_width()),
                             static_cast<float>(media_height()));
      }
      redraw = true;
    }
    if (snapshot.zoom_out_seq != seen_zoom_out_seq_) {
      seen_zoom_out_seq_ = snapshot.zoom_out_seq;
      if (current_image_ || current_video_.texture) {
        const auto view = usable_canvas(snapshot);
        camera_.wheel_toward(view.w * 0.5f, view.h * 0.5f, -1.0f, view.w, view.h,
                             static_cast<float>(media_width()),
                             static_cast<float>(media_height()));
      }
      redraw = true;
    }
    if (snapshot.zoom_preset_seq != seen_zoom_preset_seq_) {
      seen_zoom_preset_seq_ = snapshot.zoom_preset_seq;
      if (current_image_ || current_video_.texture) {
        const auto view = usable_canvas(snapshot);
        camera_.set_zoom(snapshot.zoom_preset, static_cast<float>(media_width()),
                         static_cast<float>(media_height()), view.w, view.h);
      }
      redraw = true;
    }

    {
      const auto flags = static_cast<std::uint8_t>(
          (snapshot.background & 0x3) | (snapshot.sticky_zoom ? 0x4 : 0) |
          (snapshot.clipping ? 0x8 : 0) | (snapshot.loupe ? 0x10 : 0) |
          (snapshot.hold_previous ? 0x20 : 0) | (snapshot.info_overlay ? 0x40 : 0) |
          (snapshot.item_marked ? 0x80 : 0));
      if (flags != seen_view_flags_ || snapshot.loupe_steps_x != seen_loupe_steps_x_ ||
          snapshot.loupe_steps_y != seen_loupe_steps_y_ ||
          snapshot.marked_count != seen_marked_count_ || snapshot.blackout != seen_blackout_ ||
          snapshot.item_index != seen_item_index_ || snapshot.item_count != seen_item_count_) {
        seen_view_flags_ = flags;
        seen_marked_count_ = snapshot.marked_count;
        seen_blackout_ = snapshot.blackout;
        seen_item_index_ = snapshot.item_index;
        seen_item_count_ = snapshot.item_count;
        seen_loupe_steps_x_ = snapshot.loupe_steps_x;
        seen_loupe_steps_y_ = snapshot.loupe_steps_y;
        redraw = true;  // one frame for a toggle or a nudge; idle again after
      }
    }
    if (snapshot.fill_seq != seen_fill_seq_) {
      seen_fill_seq_ = snapshot.fill_seq;
      if (current_image_ || current_video_.texture) {
        const auto view = usable_canvas(snapshot);
        camera_.fill(media_width(), media_height(), view.w, view.h, false);
      }
      redraw = true;
    }
    {
      // One keyboard pan step is a tenth of the canvas, whatever the zoom:
      // what moves is what you see, not a fixed number of image pixels.
      constexpr float kPanStepFraction = 0.1f;
      std::int64_t steps_x = 0;
      std::int64_t steps_y = 0;
      if (input_cursor_.consume_pan(snapshot, steps_x, steps_y) &&
          (current_image_ || current_video_.texture)) {
        const auto view = usable_canvas(snapshot);
        camera_.pan_by_screen(static_cast<float>(steps_x) * view.w * kPanStepFraction,
                              static_cast<float>(steps_y) * view.h * kPanStepFraction,
                              media_width(), media_height(), view.w, view.h);
        redraw = true;
      }
      // The UI thread reads this to let ↑ ↓ fall through at fit (plan/16:
      // they pan only when zoomed). Lock-free; the UI never waits on it.
      view_fitted_.store(!(current_image_ || current_video_.texture) || camera_.fit_mode(),
                         std::memory_order_relaxed);
      showing_still_.store(current_image_ != nullptr, std::memory_order_relaxed);
      // The title-bar status line reads these (plan/16 "Status / title").
      status_width_.store(static_cast<std::uint32_t>(media_width()), std::memory_order_relaxed);
      status_height_.store(static_cast<std::uint32_t>(media_height()), std::memory_order_relaxed);
      status_zoom_pct_.store(static_cast<std::uint32_t>(std::lround(camera_.target_zoom() * 100.0f)),
                             std::memory_order_relaxed);
    }

    const float wheel = input_cursor_.consume_wheel(snapshot);
    if ((current_image_ || current_video_.texture) && wheel != 0.0f) {
      const auto view = usable_canvas(snapshot);
      camera_.wheel_toward(snapshot.mouse_x - view.x, snapshot.mouse_y - view.y, wheel,
                           view.w, view.h,
                           static_cast<float>(media_width()),
                           static_cast<float>(media_height()));
      redraw = true;
    }
    {
      const bool left = snapshot.mouse_down[0];
      if (left && !was_left_down_) camera_.drag_begin();
      if (left && was_left_down_ && (current_image_ || current_video_.texture)) {
        camera_.drag_delta(snapshot.mouse_x - last_mouse_x_, snapshot.mouse_y - last_mouse_y_);
      }
      if (!left && was_left_down_) camera_.drag_end();
      was_left_down_ = left;
      last_mouse_x_ = snapshot.mouse_x;
      last_mouse_y_ = snapshot.mouse_y;
    }

    // --pan-soak: 100 % and a Lissajous path across the whole still, driven by
    // the same drag the mouse uses — never a decode, only the camera.
    if (options_.scripted_pan && current_image_ && warmed_up_) {
      const auto view = usable_canvas(snapshot);
      const float w = media_width();
      const float h = media_height();
      if (scripted_pan_start_ < 0.0) {
        scripted_pan_start_ = elapsed;
        // Snap to 100 % first: the springs do not step while dragging, so a
        // sprung zoom would stay at fit for the whole soak.
        camera_.set_zoom(1.0f, w, h, view.w, view.h);
        camera_.carry(w, h, w, h, view.w, view.h);
        camera_.drag_begin();
      }
      if (camera_.dragging()) {
        const double t = elapsed - scripted_pan_start_;
        const float ax = std::max(0.0f, (w - view.w) * 0.5f);
        const float ay = std::max(0.0f, (h - view.h) * 0.5f);
        const float want_x = w * 0.5f + ax * static_cast<float>(std::sin(t * 6.2831853 / 20.0));
        const float want_y = h * 0.5f + ay * static_cast<float>(std::sin(t * 6.2831853 / 13.0));
        const float z = camera_.zoom() > 0.0f ? camera_.zoom() : 1.0f;
        camera_.drag_delta((camera_.pan_x() - want_x) * z, (camera_.pan_y() - want_y) * z);
      }
    }

    // --- Should we present at all? --------------------------------------
    // plan/03 rule 4: idle means stop presenting entirely (0 % GPU on a static
    // image), and keep presenting for ~500 ms after the last input so a flick
    // does not stutter at the tail.
    //
    // The empty window is static. A 500 ms refresh-rate tail there (and a fake
    // tail from last_input_time=0 at launch) kept Present running while the
    // WinUI island composed — DXGI counted those as missed frames, and the
    // overlay froze on them. Paint once, then wait.
    if (redraw) last_input_time_ = elapsed;
    const bool pan_tail = current_image_ && last_input_time_ >= 0.0 &&
                          (elapsed - last_input_time_) < 0.5;
    // "Clip open, no frame yet" is live: it ends the instant the first frame
    // arrives, so this is a bounded wait for the decoder, not a spin.
    const bool video_loading = video_open_ && !current_video_.texture;
    // plan/03 rule 4's one labelled exception: blinkies animate, so a still with
    // them on presents until C turns them off. Everything else here idles.
    const bool blinkies = snapshot.clipping && current_image_ != nullptr;
    const bool fading = fade_from_ && fade_.active(elapsed);
    const bool live = video_active_ || video_loading || animating_ || camera_.moving() ||
                      pan_tail || blinkies || anim_live_ || fading;
    live_presenting_ = live;
    const bool allowed = snapshot.window_visible && !occluded_ &&
                         (options_.soak_seconds > 0.0 || snapshot.window_active);
    bool wants_frame = false;
    if (allowed) {
      if (live) {
        wants_frame = true;
        painted_static_ = false;
      } else {
        wants_frame = !painted_static_ || redraw || paint_once;
      }
    }

    if (!wants_frame) {
      if (was_presenting_ && options_.soak_seconds == 0.0) pacer_.reset_window();
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
    // Sample the master only AFTER the frame-latency wait, immediately before drawing.
    if (session_) {
      player::video_frame frame;
      const bool first_video = !current_video_.texture;
      if (mv::abi::poll_video(session_, static_cast<player::time_ns>(swapchain_.refresh_interval_seconds() * 1'000'000'000.0), frame, video_active_)) {
        // A clip is not a burst: do not keep a 4K still pinned behind it.
        current_image_.reset(); previous_image_.reset(); anim_frame_.reset();
        fade_from_.reset(); refine_base_.reset(); fade_.cancel(); refine_fade_abandoned();
        current_video_ = std::move(frame);
        if (first_video) {
          const auto view = usable_canvas(snapshot);
          camera_.fit(media_width(), media_height(), view.w, view.h, true);
        }
      }
    }


    const std::int64_t frame_qpc = qpc_now();
    const auto delta = static_cast<float>(qpc_seconds(frame_qpc - last_frame_qpc));
    last_frame_qpc = frame_qpc;

    camera_.step(delta);

    ImGui_ImplDX11_NewFrame();
    feed_imgui(snapshot, delta, wheel);
    ImGui::NewFrame();

    // Inside the recorded frame, so a fade's frames and drops are the ones the
    // pacer scored (PR 7 no-pop instrument).
    refine_fade_tick(elapsed);
    draw_frame(snapshot, elapsed);
    draw_view_overlays(snapshot);
    if (overlay_visible_) draw_overlay(snapshot);

    ImGui::Render();

    ID3D11RenderTargetView* rtv = swapchain_.back_buffer_rtv();
    device_.context()->OMSetRenderTargets(1, &rtv, nullptr);

    // Clear to a colour, per the PR 1 brief. The values are LINEAR: the render
    // target view is _SRGB, so the hardware encodes on write. Writing 0.05 here
    // and reading back 0.05 in a screenshot would mean the sRGB view was lost.
    const float clear_level = gfx::background_clear(snapshot.background);
    const float clear[4] = {snapshot.blackout ? 0.0f : (snapshot.background == 0 ? 0.016f : clear_level),
                            snapshot.blackout ? 0.0f : clear_level,
                            snapshot.blackout ? 0.0f : (snapshot.background == 0 ? 0.024f : clear_level),
                            1.0f};
    device_.context()->ClearRenderTargetView(rtv, clear);

    const bool show_previous =
        snapshot.hold_previous && previous_image_ && previous_image_->srv && current_image_;
    // The animation's current frame stands in for the still (same size), so the
    // loupe, blinkies, grid and background all draw it.
    const image::gpu_image* shown =
        show_previous ? previous_image_.get()
                      : (anim_frame_ && anim_frame_->srv ? anim_frame_.get() : current_image_.get());
    if (shown && shown->srv && !snapshot.blackout) {
      const auto view = usable_canvas(snapshot);
      D3D11_VIEWPORT vp{};
      vp.TopLeftX = view.x;
      vp.TopLeftY = view.y;
      vp.Width = view.w;
      vp.Height = view.h;
      vp.MaxDepth = 1.0f;
      device_.context()->RSSetViewports(1, &vp);
      gfx::blit_params bp{};
      const auto shown_w = static_cast<float>(shown->width);
      const auto shown_h = static_cast<float>(shown->height);
      if (show_previous && (shown_w != media_width() || shown_h != media_height())) {
        // A same-size burst keeps the camera so the pick is like for like; a
        // different frame is shown whole rather than at the wrong crop.
        bp.zoom = canvas::camera::fit_zoom(shown_w, shown_h, view.w, view.h);
        bp.pan_x = shown_w * 0.5f;
        bp.pan_y = shown_h * 0.5f;
      } else {
        bp.pan_x = camera_.pan_x();
        bp.pan_y = camera_.pan_y();
        bp.zoom = camera_.zoom();
      }
      bp.window_w = view.w;
      bp.window_h = view.h;
      bp.origin_x = view.x;
      bp.origin_y = view.y;
      bp.image_w = shown_w;
      bp.image_h = shown_h;
      bp.texture_w = static_cast<float>(shown->texture_width);
      bp.texture_h = static_cast<float>(shown->texture_height);
      bp.background = snapshot.background;
      bp.clipping = snapshot.clipping;
      bp.time_seconds = static_cast<float>(elapsed);

      const bool is_current = !show_previous && shown == current_image_.get();
      if (fade_from_ && !fade_.active(elapsed)) fade_from_.reset();
      const bool fade_now = is_current && fade_from_ && fade_from_->srv;
      const float fade_alpha = fade_now ? fade_.alpha(elapsed) : 1.0f;
      const bool tiled = is_current && shown->tiles;
      if (tiled) {
        tile_draws_ = mv::abi::tiles_frame(*shown, {bp.pan_x, bp.pan_y, bp.zoom, view.w, view.h});
        if (tiles_complete_seconds_ < 0.0 && !shown->tiles->pending() &&
            mv::abi::tiles_stats(*shown).requested == 0 && seen_tile_seq_ > 0) {
          tiles_complete_seconds_ = elapsed;
        }
      } else {
        tile_draws_ = {};
      }
      // The still as it should look this frame, into the current viewport:
      // the outgoing texture under a fade, the incoming one (an overview for
      // a tiled image), a sharper preview kept under the tiles, then tiles
      // coarse to fine. Every layer maps the same full-resolution rect.
      const auto draw_still = [&](const gfx::blit_params& p, bool with_tiles) {
        if (fade_now) {
          gfx::blit_params fp = p;
          fp.texture_w = static_cast<float>(fade_from_->texture_width);
          fp.texture_h = static_cast<float>(fade_from_->texture_height);
          fp.opacity = 1.0f;
          blitter_.draw(device_.context(), fade_from_->srv.Get(), fp);
        }
        gfx::blit_params cp = p;
        cp.opacity = fade_alpha;
        blitter_.draw(device_.context(), shown->srv.Get(), cp);
        if (is_current && refine_base_ && refine_base_->srv) {
          gfx::blit_params rp = cp;
          rp.texture_w = static_cast<float>(refine_base_->texture_width);
          rp.texture_h = static_cast<float>(refine_base_->texture_height);
          blitter_.draw(device_.context(), refine_base_->srv.Get(), rp);
        }
        // Tiles join once the fade is over (80-250 ms); before that the
        // overview is what fades in.
        if (with_tiles && tiled && !fade_now && !tile_draws_.empty()) {
          blitter_.draw_tiles(device_.context(), tile_draws_, cp);
        }
      };
      draw_still(bp, true);

      // Hold Z: the same texture again, through a second viewport — a camera
      // change, not a decode. 100 %, or twice the zoom when already past it.
      if (snapshot.loupe && !show_previous) {
        const loupe_box box = loupe_rect(snapshot, view);
        gfx::blit_params lp = bp;
        lp.pan_x = bp.pan_x + (box.point_x - (view.x + view.w * 0.5f)) / bp.zoom;
        lp.pan_y = bp.pan_y + (box.point_y - (view.y + view.h * 0.5f)) / bp.zoom;
        lp.zoom = bp.zoom < 1.0f ? 1.0f : std::min(64.0f, bp.zoom * 2.0f);
        lp.window_w = box.size;
        lp.window_h = box.size;
        lp.origin_x = box.x;
        lp.origin_y = box.y;
        const D3D11_VIEWPORT lv{box.x, box.y, box.size, box.size, 0.0f, 1.0f};
        device_.context()->RSSetViewports(1, &lv);
        // The loupe's zoom differs from the view's, so its tiles would be a
        // second LOD with its own requests; it draws the overview (and any
        // kept preview) of a tiled image instead.
        draw_still(lp, false);
      }
    }

    if (current_video_.texture && !snapshot.blackout) {
      const auto view = usable_canvas(snapshot);
      D3D11_VIEWPORT vp{view.x, view.y, view.w, view.h, 0.0f, 1.0f};
      device_.context()->RSSetViewports(1, &vp);
      D3D11_TEXTURE2D_DESC desc{}; current_video_.texture->GetDesc(&desc);
      gfx::video_blit_params bp;
      bp.pan_x = camera_.pan_x(); bp.pan_y = camera_.pan_y(); bp.zoom = camera_.zoom();
      bp.window_w = view.w; bp.window_h = view.h; bp.origin_x = view.x; bp.origin_y = view.y;
      bp.image_w = media_width(); bp.image_h = media_height();
      bp.texture_w = static_cast<float>(desc.Width); bp.texture_h = static_cast<float>(desc.Height);
      video_blitter_.draw(device_.context(), current_video_.luma.Get(), current_video_.chroma.Get(), current_video_.colour, bp);
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
      if (!live) painted_static_ = true;
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
  video_blitter_.destroy();
  current_video_ = {};
  current_image_.reset();
  previous_image_.reset();  // hold-previous's texture belongs to the same device
  fade_from_.reset();
  refine_base_.reset();
  fade_.cancel();
  refine_fade_abandoned();
  tile_draws_ = {};
  anim_frame_.reset();
  if (session_) mv::abi::detach_device(session_);
  swapchain_.destroy();
  device_.destroy();
  ImGui::DestroyContext();
  if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);

  finished_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  ::PostMessageW(window_, WM_CLOSE, 0, 0);
}

void present_lab::draw_view_overlays(const input_snapshot& snapshot) noexcept {
  if (!current_image_ && !current_video_.texture) return;
  if (snapshot.blackout) return;
  ImDrawList* fg = ImGui::GetForegroundDrawList();
  ImFont* font = ImGui::GetFont();
  const auto view = usable_canvas(snapshot);
  const float scale = snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f;
  const float fs = 16.0f * scale;
  const float pad = 12.0f * scale;
  const ImU32 text = IM_COL32(230, 230, 235, 255);
  const ImU32 shadow = IM_COL32(0, 0, 0, 200);
  const auto label = [&](float x, float y, const char* s) {
    fg->AddText(font, fs, ImVec2(x + scale, y + scale), shadow, s);
    fg->AddText(font, fs, ImVec2(x, y), text, s);
  };

  const bool show_previous = snapshot.hold_previous && previous_image_ && current_image_;
  if (snapshot.loupe && current_image_ && !show_previous) {
    const loupe_box box = loupe_rect(snapshot, view);
    fg->AddRect(ImVec2(box.x, box.y), ImVec2(box.x + box.size, box.y + box.size), text, 0.0f,
                0, 1.5f * scale);
  }
  if (show_previous) label(view.x + pad, view.y + pad, "\\  previous");

  // Marks are visible without the mouse and without O (PR 6 verify: "mark").
  if (snapshot.marked_count > 0) {
    char marks[64];
    std::snprintf(marks, sizeof(marks), "%s%u marked", snapshot.item_marked ? "[marked]  " : "",
                  snapshot.marked_count);
    const ImVec2 size = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, marks);
    label(view.x + view.w - size.x - pad, view.y + pad, marks);
  }

  if (snapshot.info_overlay) {
    char line[400];
    const int zoom_pct = static_cast<int>(std::lround(camera_.zoom() * 100.0f));
    const auto w = static_cast<unsigned>(media_width());
    const auto h = static_cast<unsigned>(media_height());
    if (snapshot.item_count > 0) {
      std::snprintf(line, sizeof(line), "%s  -  %u / %u  -  %ux%u  -  %d %%", snapshot.item_name,
                    snapshot.item_index + 1, snapshot.item_count, w, h, zoom_pct);
    } else {
      std::snprintf(line, sizeof(line), "%ux%u  -  %d %%", w, h, zoom_pct);
    }
    label(view.x + pad, view.y + view.h - fs - pad, line);
  }
}

void present_lab::draw_frame(const input_snapshot& snapshot, double elapsed_seconds) noexcept {
  // video_open_ and no texture is a clip still opening, not an empty window.
  // Painting "drop a photo here" over it is the bug that made an open clip
  // look like it had not opened at all.
  if (current_image_ || current_video_.texture || video_open_) {
    if (!sweep_mode_ && game_.active()) {  // a file opened over the runner
      game_.leave();
      animating_ = false;
    }
    return;
  }

  const auto w = static_cast<float>(snapshot.width);
  const auto h = static_cast<float>(snapshot.height);
  if (w <= 0.0f || h <= 0.0f) return;

  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  const float chrome = static_cast<float>(snapshot.chrome_height_px);
  const float scale = snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f;

  if (sweep_mode_ && animating_) {
    // Present-lab judder instrument. Space turns it on; the default empty
    // view is the welcome below, not this sweep.
    animation_phase_ = std::fmod(elapsed_seconds * 0.35, 1.0);
    const float bar_width = 6.0f * scale;
    const float x = static_cast<float>(animation_phase_) * (w - bar_width);
    bg->AddRectFilled(ImVec2(x, chrome), ImVec2(x + bar_width, h),
                      IM_COL32(230, 230, 235, 255));
    for (int i = 1; i < 10; ++i) {
      const float tx = w * (static_cast<float>(i) / 10.0f);
      bg->AddLine(ImVec2(tx, h - 24.0f * scale), ImVec2(tx, h),
                  IM_COL32(90, 95, 110, 255), 1.0f);
    }
    return;
  }

  const welcome_text text{
      .open_hint = "or press Ctrl+O to choose a file, Ctrl+Shift+O for a folder",
      .keys = "0 fit    1 100%    + / -  zoom    Space  play/pause    F  fullscreen    F3  frame-time"};
  if (!sweep_mode_ && game_.active()) {
    const float dt =
        last_game_elapsed_ > 0.0 ? static_cast<float>(elapsed_seconds - last_game_elapsed_) : 0.0f;
    last_game_elapsed_ = elapsed_seconds;
    game_.set_view_width(w / (3.0f * scale));
    game_.update(dt);
    if (game_.state() == dino_game::phase::over) animating_ = false;  // idle again: nothing moves
    draw_welcome(bg, ImGui::GetFont(), w, h, chrome, scale, text, welcome_alpha(game_));
    draw_dino(bg, ImGui::GetFont(), game_, w, h, chrome, scale);
    return;
  }
  last_game_elapsed_ = 0.0;
  draw_welcome(bg, ImGui::GetFont(), w, h, chrome, scale, text);
}

void present_lab::draw_overlay(const input_snapshot& snapshot) noexcept {
  const auto stats = pacer_.stats();
  const float pad = 12.0f * snapshot.dpi_scale;
  const float top = static_cast<float>(snapshot.chrome_height_px) + pad;

  ImGui::SetNextWindowPos(ImVec2(pad, top), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.82f);
  ImGui::Begin("Frame time (F)", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                   ImGuiWindowFlags_NoNav);

  const double implied_hz =
      stats.refresh_interval_ms > 0.0 ? 1000.0 / stats.refresh_interval_ms : 0.0;

  if (current_video_.texture && session_) {
    mv_video_stats video{}; mv_video_info info{};
    (void)mv_video_get_stats(session_, &video); (void)mv_video_get_info(session_, &info);
    ImGui::Text("video %s | %s | %.2fx", info.codec_name,
        info.decoder == MV_DECODER_D3D11VA ? "D3D11VA" : "SOFTWARE DECODE", video.playback_rate);
    ImGui::Text("clock %s | error p50 %.2f p99 %.2f ms | drift %.3f ms/min",
        video.audio_master ? "audio" : "host fallback", video.err_ms_p50, video.err_ms_p99, video.drift_slope_ms_per_min);
    ImGui::Text("shown %llu dropped %llu cadence %llu starved %llu",
        video.frames_presented, video.frames_dropped_late, video.holds_cadence, video.holds_starved);
    ImGui::Separator();
  }
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

  // Colour the drop count, not the 60 s soak gate — interactively that gate
  // is almost always false and painted every empty window orange.
  const bool clean = stats.dropped_frames == 0 && stats.missed_refreshes == 0;
  const ImVec4 colour = clean ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f)
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

  // A clip is not an image. current_image_ is null the whole time a video is
  // on the canvas, so the format/ICC/mip line belongs to the image branch
  // only — reading it under "image OR video" dereferenced null and took the
  // process down every time F3 was pressed on a clip, playing or paused.
  // media_width()/media_height() are floats; %u on a vararg float is garbage
  // even when the pointer happens to be live, so print the source dimensions.
  if (current_image_ || current_video_.texture) {
    ImGui::Separator();
    if (current_image_) {
      ImGui::Text("image    %ux%u  %s  %s  %u mips", current_image_->width,
                  current_image_->height, codec::format_name(current_image_->format),
                  current_image_->icc_tagged ? "ICC tagged" : "untagged (sRGB)",
                  current_image_->mip_levels);
      if (current_image_->tiles) {
        const auto t = mv::abi::tiles_stats(*current_image_);
        ImGui::Text("tiles    lod %u (overview %u)  drawn %u  resident %u  %.0f MB (peak %.0f)",
                    t.lod, t.overview_level, t.drawn, t.resident,
                    static_cast<double>(t.vram_bytes) / 1048576.0,
                    static_cast<double>(t.vram_peak_bytes) / 1048576.0);
        ImGui::Text("         pending %u  created %llu  evicted %llu  create %.2f ms (max %.2f)"
                    "  build %.2f ms  incomplete frames %llu",
                    t.requested, static_cast<unsigned long long>(t.created),
                    static_cast<unsigned long long>(t.evicted), t.last_create_us / 1000.0,
                    t.max_create_us / 1000.0, t.last_build_us / 1000.0,
                    static_cast<unsigned long long>(t.incomplete_frames));
      }
      ImGui::Text("refine   %llu refinements  %llu stale previews dropped%s",
                  static_cast<unsigned long long>(refinements_),
                  static_cast<unsigned long long>(stale_drops_),
                  refine_base_ ? "  preview kept under tiles" : "");
      // The no-pop numbers, beside the ones they qualify: a cut fade, a jumped
      // view or a stutter inside the fade all show here as well as in --json.
      ImGui::Text("no-pop   fades %llu started / %llu completed / %llu cut  %llu frames"
                  "  %llu dropped  view shift %.2f px  scale x%.4f",
                  static_cast<unsigned long long>(refine_fades_started_),
                  static_cast<unsigned long long>(refine_fades_completed_),
                  static_cast<unsigned long long>(refine_fades_cancelled_),
                  static_cast<unsigned long long>(refine_fade_frames_),
                  static_cast<unsigned long long>(refine_fade_dropped_),
                  refine_max_edge_shift_px_, refine_max_scale_step_);
    } else {
      ImGui::Text("clip     %ux%u  %s", current_video_.width, current_video_.height,
                  current_video_.ten_bit ? "P010" : "NV12");
    }
    ImGui::Text("view     zoom %.2f  pan %.1f, %.1f  %s", camera_.zoom(), camera_.pan_x(),
                camera_.pan_y(), camera_.fit_mode() ? "fit" : (camera_.zoom() == 1.0f ? "100%" : ""));
    ImGui::Text("decode is off the render thread — pan must not start one");
    if (snapshot.clipping && current_image_) {
      ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                         "blinkies on: presenting until C (plan/03 rule 4 exception)");
    }
    if (session_ && mv::abi::animation_open(session_, anim_generation_)) {
      const auto anim_stats = mv::abi::animation_stats_now(session_);
      // Review note B: worker uploads count against the pacing budget, so the
      // cost and the late frames are on the instrument, not guessed at.
      // "make" is colour conversion plus CreateTexture2D; "icc" is the first
      // part of it (review note 36).
      ImGui::Text("animation frame %u  ring %u/%u  make %.2f ms (icc %.2f)  late %u  %s",
                  anim_index_, anim_stats.queued, anim_stats.depth,
                  static_cast<double>(anim_stats.last_upload_us) / 1000.0,
                  static_cast<double>(anim_stats.last_icc_us) / 1000.0,
                  anim_schedule_.late(),
                  anim_schedule_.paused() ? "paused" : (anim_finished_ ? "finished" : "playing"));
      ImGui::Text("hold-previous texture changes %u (steady while an animation plays)",
                  previous_image_changes_);
    }
  }

  ImGui::Separator();
  // Space is the lab sweep, not "is a photo open". A still image is meant to
  // stop presenting; pan/zoom/the 500 ms input tail are presenting without
  // the sweep, and must not be labelled idle.
  const char* status = animating_          ? "animating (lab sweep)"
                       : live_presenting_  ? "presenting"
                                           : "idle (not presenting)";
  ImGui::Text("%s   [space] sweep   [R] reset   [F] overlay", status);
  if (current_image_ || current_video_.texture) {
    ImGui::Text("[0] fit   [1] 100%%   [+]/[-] zoom   wheel   drag   drop / Ctrl+O");
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
  // Review note 44: pacing alone passes a starved animation (the cost sits on
  // the decode thread), so the report carries the animation's own cadence.
  const auto anim = mv::abi::animation_stats_now(session_);
  const image::tile_stats tile_stats =
      current_image_ ? mv::abi::tiles_stats(*current_image_) : image::tile_stats{};
  const double mean_delay_ms =
      anim_frames_shown_ > 0
          ? static_cast<double>(anim_delay_sum_ms_) / static_cast<double>(anim_frames_shown_)
          : 0.0;
  const double nominal_fps = mean_delay_ms > 0.0 ? 1000.0 / mean_delay_ms : 0.0;
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
               "  \"animation_frames_shown\": %llu,\n"
               "  \"animation_frames_made\": %llu,\n"
               "  \"animation_late\": %llu,\n"
               "  \"animation_mean_delay_ms\": %.3f,\n"
               "  \"animation_nominal_fps\": %.3f,\n"
               "  \"animation_last_make_ms\": %.3f,\n"
               "  \"animation_last_icc_ms\": %.3f,\n"
               "  \"still_first_pixel_s\": %.3f,\n"
               "  \"still_full_s\": %.3f,\n"
               "  \"still_tiles_complete_s\": %.3f,\n"
               "  \"still_refinements\": %llu,\n"
               "  \"refine_fades_started\": %llu,\n"
               "  \"refine_fades_completed\": %llu,\n"
               "  \"refine_fades_cancelled\": %llu,\n"
               "  \"refine_fade_frames\": %llu,\n"
               "  \"refine_fade_dropped\": %llu,\n"
               "  \"refine_max_edge_shift_px\": %.4f,\n"
               "  \"refine_max_scale_step\": %.6f,\n"
               "  \"tiles_created\": %llu,\n"
               "  \"tiles_evicted\": %llu,\n"
               "  \"tiles_vram_peak_mb\": %.1f,\n"
               "  \"tiles_max_create_ms\": %.3f,\n"
               "  \"tiles_incomplete_frames\": %llu,\n"
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
               static_cast<unsigned long long>(anim_frames_shown_),
               static_cast<unsigned long long>(anim.frames_made),
               static_cast<unsigned long long>(anim_late_total_),
               mean_delay_ms, nominal_fps,
               static_cast<double>(anim.last_upload_us) / 1000.0,
               static_cast<double>(anim.last_icc_us) / 1000.0,
               first_pixel_seconds_, full_seconds_, tiles_complete_seconds_,
               static_cast<unsigned long long>(refinements_),
               static_cast<unsigned long long>(refine_fades_started_),
               static_cast<unsigned long long>(refine_fades_completed_),
               static_cast<unsigned long long>(refine_fades_cancelled_),
               static_cast<unsigned long long>(refine_fade_frames_),
               static_cast<unsigned long long>(refine_fade_dropped_),
               refine_max_edge_shift_px_, refine_max_scale_step_,
               static_cast<unsigned long long>(tile_stats.created),
               static_cast<unsigned long long>(tile_stats.evicted),
               static_cast<double>(tile_stats.vram_peak_bytes) / 1048576.0,
               static_cast<double>(tile_stats.max_create_us) / 1000.0,
               static_cast<unsigned long long>(tile_stats.incomplete_frames),
               soak_complete_ && measurement_valid_ && exit_code_ == 0 &&
                   (options_.start_animating ? s.meets_pr1_gate() : idle_stats_.meets_pr1_gate())
                   ? "true" : "false");
  const bool written = std::ferror(f) == 0;
  return std::fclose(f) == 0 && written;
}

}  // namespace mv::shell
