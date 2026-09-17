// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/present_lab_mac.h"

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/CAMetalLayer.h>

#include <limits.h>
#include <mach-o/dyld.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>

#include <imgui.h>
#include <imgui_impl_metal.h>

#include "core/trace.h"
#include "gfx/pace_json.h"
#include "image/pipeline_mac.h"
#include "image/upload_mac.h"

using mv::gfx::k_input_tail_seconds;
using mv::gfx::k_occlusion_poll_ms;
using mv::gfx::k_warmup_seconds;

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

// PR 18: the height of the canvas rect below the SwiftUI command bar. The
// swapchain itself still spans the full backing size (main_mac.mm's
// MvMetalView is never resized) — only fit/pan and the blit's origin_y see
// this, same as Windows' chrome_height_px (gfx/blit.h).
float usable_window_h(const mv::shell::input_snapshot& s) noexcept {
  return std::max(1.0f, static_cast<float>(s.height) - static_cast<float>(s.chrome_height_px));
}

void feed_imgui(const mv::shell::input_snapshot& s, float delta_seconds, float wheel) noexcept {
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

void try_load_font() noexcept {
  char exe[PATH_MAX]{};
  std::uint32_t size = sizeof(exe);
  if (_NSGetExecutablePath(exe, &size) != 0) return;
  char resolved[PATH_MAX]{};
  if (!realpath(exe, resolved)) return;
  char* slash = std::strrchr(resolved, '/');
  if (!slash) return;
  *slash = '\0';
  char font_path[PATH_MAX]{};
  if (std::snprintf(font_path, sizeof(font_path), "%s/CozetteVector.ttf", resolved) <= 0) return;
  if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(font_path, 16.0f)) {
    ImGui::GetIO().FontDefault = font;
  }
}

}  // namespace

@interface MvMetalLinkTarget : NSObject <CAMetalDisplayLinkDelegate> {
  std::atomic<void*>* _slot;
  std::condition_variable* _cv;
}
- (instancetype)initWithSlot:(std::atomic<void*>*)slot cv:(std::condition_variable*)cv;
@end

// The display-link callback is the waitable object: it fires when a drawable
// is ready, and only then does the lab encode. Sleep is not used.
@implementation MvMetalLinkTarget
- (instancetype)initWithSlot:(std::atomic<void*>*)slot cv:(std::condition_variable*)cv {
  self = [super init];
  if (self) {
    _slot = slot;
    _cv = cv;
  }
  return self;
}
- (void)metalDisplayLink:(CAMetalDisplayLink*)link
             needsUpdate:(CAMetalDisplayLinkUpdate*)update {
  (void)link;
  void* previous = _slot->exchange((__bridge_retained void*)update);
  if (previous) (void)(__bridge_transfer CAMetalDisplayLinkUpdate*)previous;
  _cv->notify_one();
}
@end

namespace mv::shell {

namespace {
std::mutex g_wait_mutex;
std::condition_variable g_wait_cv;
std::atomic<void*> g_pending_update{nullptr};

CAMetalDisplayLinkUpdate* take_link_update() noexcept {
  void* p = g_pending_update.exchange(nullptr);
  return p ? (__bridge_transfer CAMetalDisplayLinkUpdate*)p : nil;
}
}  // namespace

present_lab_mac::~present_lab_mac() { stop(); }

// [any-thread]. Runs once, on the job pool: reads and decodes the --open
// file, then uploads an immutable MTLTexture. Never on the render thread
// (rule 1, CLAUDE.md / plan/02) -- device_.native_device() is safe to use
// from any thread. The file read is a plain blocking std::ifstream on the
// worker rather than io/file.h's async path: PR 17 is a one-shot lab arg,
// not folder navigation, and io/ has no Darwin port yet.
void present_lab_mac::submit_image_load() noexcept {
  if (options_.open_path.empty() || !options_.jobs || image_load_submitted_) return;
  image_load_submitted_ = true;

  const std::string path = options_.open_path;
  void* mtl_device = device_.native_device();
  std::atomic<image::gpu_image_mac*>* pending = &pending_image_;

  options_.jobs->submit([path, mtl_device, pending](const job_context& ctx) -> status {
    std::ifstream f(path, std::ios::binary);
    if (!f) return status::io;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
    if (bytes.empty()) return status::io;

    auto decoded = image::decode_bytes_mac(bytes, &ctx);
    if (!decoded) return decoded.error();
    auto uploaded = image::upload(mtl_device, decoded.value(), &ctx);
    if (!uploaded) return uploaded.error();

    auto* img = new image::gpu_image_mac(std::move(uploaded).value());
    image::gpu_image_mac* old = pending->exchange(img);
    delete old;  // not expected in PR 17's single-open use; safe if it happens
    return status::ok;
  });
}

expected present_lab_mac::start(void* nsview, const mac_lab_options& options) noexcept {
  if (!nsview) return err(status::invalid_arg);
  view_ = nsview;
  options_ = options;
  overlay_visible_ = options.overlay_visible;
  animating_ = options.start_animating;

  running_.store(true, std::memory_order_release);
  render_thread_ = std::thread([this] { render_thread_main(); });
  while (running_.load(std::memory_order_acquire) &&
         start_error_.load(std::memory_order_acquire) == 0 &&
         !ready_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (start_error_.load(std::memory_order_acquire) != 0) return err(status::internal);
  return {};
}

void present_lab_mac::stop() noexcept {
  running_.store(false, std::memory_order_release);
  wake();
  if (render_thread_.joinable()) render_thread_.join();
}

void present_lab_mac::wake() noexcept {
  wake_flag_.store(true, std::memory_order_release);
  g_wait_cv.notify_all();
}

void present_lab_mac::render_thread_main() noexcept {
  pthread_setname_np("mv.render");
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

  @autoreleasepool {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    try_load_font();

    if (auto built = device_.create(); !built) {
      MV_LOG_ERROR("present_lab_mac: device creation failed (%s)", status_name(built.error()));
      exit_code_ = 2;
      start_error_.store(1, std::memory_order_release);
      ImGui::DestroyContext();
      finished_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      return;
    }

    NSView* view = (__bridge NSView*)view_;
    const NSSize backing = [view convertSizeToBacking:view.bounds.size];
    const double scale = view.window.backingScaleFactor > 0.0 ? view.window.backingScaleFactor : 1.0;
    const auto bw = static_cast<std::uint32_t>(std::max(1.0, backing.width));
    const auto bh = static_cast<std::uint32_t>(std::max(1.0, backing.height));
    if (auto attached = layer_.attach(view_, device_, bw, bh, scale); !attached) {
      MV_LOG_ERROR("present_lab_mac: layer attach failed (%s)", status_name(attached.error()));
      exit_code_ = 2;
      start_error_.store(1, std::memory_order_release);
      device_.destroy();
      ImGui::DestroyContext();
      finished_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      return;
    }

    id<MTLDevice> mtl = (__bridge id<MTLDevice>)device_.native_device();
    ImGui_ImplMetal_Init(mtl);
    imgui_ready_ = true;

    // PR 17: the drawable's own pixel format, matching MvMetalView's
    // makeBackingLayer (main_mac.mm).
    if (auto built = blitter_.create(device_.native_device(),
                                     static_cast<std::uint64_t>(MTLPixelFormatBGRA8Unorm_sRGB));
        !built) {
      MV_LOG_ERROR("present_lab_mac: blitter create failed (%s)", status_name(built.error()));
      exit_code_ = 2;
      start_error_.store(1, std::memory_order_release);
      ImGui_ImplMetal_Shutdown();
      layer_.destroy();
      device_.destroy();
      ImGui::DestroyContext();
      finished_.store(true, std::memory_order_release);
      running_.store(false, std::memory_order_release);
      return;
    }
    submit_image_load();

    ready_.store(true, std::memory_order_release);

    MvMetalLinkTarget* target =
        [[MvMetalLinkTarget alloc] initWithSlot:&g_pending_update cv:&g_wait_cv];
    CAMetalDisplayLink* link =
        [[CAMetalDisplayLink alloc] initWithMetalLayer:(__bridge CAMetalLayer*)layer_.native_layer()];
    const double refresh = layer_.refresh_interval_seconds();
    if (refresh > 0.0) {
      const float fps = static_cast<float>(1.0 / refresh);
      link.preferredFrameRateRange = CAFrameRateRangeMake(fps, fps, fps);
    }
    link.delegate = target;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
    display_link_ = (__bridge_retained void*)link;
    link_target_ = (__bridge_retained void*)target;

    pacer_.begin_session(refresh);

    const double start = monotonic_seconds();
    double last_frame = start;

    while (running_.load(std::memory_order_acquire)) {
      @autoreleasepool {
        const input_snapshot snapshot = input_.acquire();
        const double elapsed = monotonic_seconds() - start;
        if (!warmed_up_ && elapsed >= k_warmup_seconds) {
          warmed_up_ = true;
          if (total_presents_ == 0 || !snapshot.window_visible) measurement_valid_ = false;
          pacer_.reset_window();
          measurement_start_seconds_ = monotonic_seconds();
          idle_start_cpu_seconds_ = process_cpu_seconds();
        }
        if (warmed_up_ && options_.soak_seconds > 0.0 &&
            monotonic_seconds() - measurement_start_seconds_ >= options_.soak_seconds) {
          soak_complete_ = true;
          break;
        }
        if (warmed_up_ && !snapshot.window_visible) measurement_valid_ = false;

        const bool input_activity = input_cursor_.consume_activity(snapshot);
        if (input_activity && warmed_up_ && !options_.start_animating) ++idle_stats_.input_events;
        bool redraw = input_activity;

        if (snapshot.toggle_overlay_seq != seen_overlay_seq_) {
          if ((snapshot.toggle_overlay_seq - seen_overlay_seq_) & 1u)
            overlay_visible_ = !overlay_visible_;
          seen_overlay_seq_ = snapshot.toggle_overlay_seq;
          redraw = true;
        }
        if (snapshot.toggle_animation_seq != seen_animation_seq_) {
          if ((snapshot.toggle_animation_seq - seen_animation_seq_) & 1u)
            animating_ = !animating_;
          seen_animation_seq_ = snapshot.toggle_animation_seq;
          redraw = true;
          if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
          if (options_.soak_seconds == 0.0) pacer_.reset_window();
        }
        if (snapshot.reset_stats_seq != seen_reset_seq_) {
          seen_reset_seq_ = snapshot.reset_stats_seq;
          redraw = true;
          if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
          if (options_.soak_seconds == 0.0) pacer_.reset_window();
        }
        if (snapshot.display_change_seq != seen_display_seq_) {
          seen_display_seq_ = snapshot.display_change_seq;
          const double previous = layer_.refresh_interval_seconds();
          layer_.refresh_output_info();
          if (warmed_up_ && previous != layer_.refresh_interval_seconds())
            measurement_valid_ = false;
          pacer_.set_refresh(layer_.refresh_interval_seconds());
          redraw = true;
        }
        if (snapshot.resize_seq != seen_resize_seq_) {
          seen_resize_seq_ = snapshot.resize_seq;
          const double sc =
              snapshot.dpi_scale > 0.0f ? static_cast<double>(snapshot.dpi_scale) : 1.0;
          if (auto r = layer_.resize(snapshot.width, snapshot.height, sc); !r) {
            MV_LOG_WARN("present_lab_mac: resize failed (%s)", status_name(r.error()));
            measurement_valid_ = false;
          }
          pacer_.set_refresh(layer_.refresh_interval_seconds());
          if (current_image_ && camera_.fit_mode()) {
            camera_.fit(static_cast<float>(current_image_->width),
                       static_cast<float>(current_image_->height),
                       static_cast<float>(snapshot.width), usable_window_h(snapshot),
                       /*immediate=*/false);
          }
          redraw = true;
        }

        // PR 17: a background job finished decoding --open. Take it over and
        // fit it once; a resize while in fit mode re-fits below.
        if (image::gpu_image_mac* loaded = pending_image_.exchange(nullptr)) {
          current_image_.reset(loaded);
          camera_.reset();
          camera_.fit(static_cast<float>(current_image_->width),
                     static_cast<float>(current_image_->height),
                     static_cast<float>(snapshot.width), usable_window_h(snapshot),
                     /*immediate=*/true);
          redraw = true;
          // A still popping in mid-measurement changes what's on screen just
          // like a resize/toggle/reset does -- same invalidation those
          // branches already apply.
          if (warmed_up_ && options_.soak_seconds > 0.0) measurement_valid_ = false;
        }

        const float wheel = input_cursor_.consume_wheel(snapshot);
        if (wheel != 0.0f) redraw = true;

        if (current_image_) {
          const auto image_w = static_cast<float>(current_image_->width);
          const auto image_h = static_cast<float>(current_image_->height);
          const auto window_w = static_cast<float>(snapshot.width);
          // PR 18: the SwiftUI command bar covers the top chrome_height_px of
          // the canvas, the same "swapchain spans the client area, chrome is
          // composited over it" shape as blit.h's origin_x/origin_y on
          // Windows (gfx/blit.h) -- fit/pan only see the rect below it.
          const float window_h = usable_window_h(snapshot);

          if (snapshot.fit_seq != seen_fit_seq_) {
            seen_fit_seq_ = snapshot.fit_seq;
            camera_.fit(image_w, image_h, window_w, window_h, /*immediate=*/false);
            redraw = true;
          }
          if (snapshot.one_to_one_seq != seen_one_to_one_seq_) {
            seen_one_to_one_seq_ = snapshot.one_to_one_seq;
            camera_.one_to_one();
            redraw = true;
          }

          // A drag that started inside the view keeps tracking on
          // mouse_down[0] alone once under way, even past the view's edge
          // (panning toward an edge is the common case this covers) --
          // mouse_in_client only gates *starting* a new drag.
          if (snapshot.mouse_down[0] && (was_dragging_ || snapshot.mouse_in_client)) {
            if (!was_dragging_) {
              camera_.drag_begin();
              was_dragging_ = true;
            } else {
              camera_.drag_delta(snapshot.mouse_x - last_mouse_x_, snapshot.mouse_y - last_mouse_y_);
            }
          } else if (was_dragging_) {
            camera_.drag_end();
            was_dragging_ = false;
          }
          last_mouse_x_ = snapshot.mouse_x;
          last_mouse_y_ = snapshot.mouse_y;

          if (wheel != 0.0f && snapshot.mouse_in_client) {
            camera_.wheel_toward(snapshot.mouse_x, snapshot.mouse_y, wheel, window_w, window_h,
                                 image_w, image_h);
          }
        }
        if (redraw) last_input_time_ = elapsed;

        gfx::present_request req;
        req.window_visible = snapshot.window_visible;
        req.window_active = snapshot.window_active;
        req.occluded = occluded_;
        req.soak = options_.soak_seconds > 0.0;
        req.animating = animating_;
        req.camera_moving = current_image_ && camera_.moving();
        req.has_still = current_image_ != nullptr;
        req.redraw = redraw;
        req.painted_static = painted_static_;
        req.elapsed_seconds = elapsed;
        req.last_input_time = last_input_time_;
        const auto decision = gfx::decide_present(req);

        CAMetalDisplayLink* live_link = (__bridge CAMetalDisplayLink*)display_link_;
        if (!decision.wants_frame) {
          if (was_presenting_ && options_.soak_seconds == 0.0) pacer_.reset_window();
          was_presenting_ = false;
          live_link.paused = YES;
          double timeout = occluded_ ? k_occlusion_poll_ms / 1000.0 : 3600.0;
          if (options_.soak_seconds > 0.0) {
            const double remaining = warmed_up_
                                         ? options_.soak_seconds -
                                               (monotonic_seconds() - measurement_start_seconds_)
                                         : k_warmup_seconds - elapsed;
            timeout = std::min(timeout, std::max(0.0, remaining));
          }
          std::unique_lock lock(g_wait_mutex);
          g_wait_cv.wait_for(lock, std::chrono::duration<double>(timeout), [&] {
            return !running_.load(std::memory_order_acquire) ||
                   wake_flag_.load(std::memory_order_acquire);
          });
          wake_flag_.store(false, std::memory_order_release);
          occluded_ = view.window.occlusionState & NSWindowOcclusionStateVisible ? false : true;
          continue;
        }

        if (!was_presenting_ && options_.soak_seconds == 0.0) pacer_.reset_window();
        was_presenting_ = true;
        live_link.paused = NO;

        // Wait BEFORE encode. The display-link callback is the waitable object.
        // Pump this thread's run loop so the link, which is attached here, can
        // fire; the callback stores the update and wakes the condvar.
        CAMetalDisplayLinkUpdate* update = take_link_update();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!update && running_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
          [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                   beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
          update = take_link_update();
        }
        if (!update) continue;

        pacer_.frame_begin();
        const double now = monotonic_seconds();
        const float delta = static_cast<float>(now - last_frame);
        last_frame = now;
        if (current_image_) camera_.step(delta);

        id<CAMetalDrawable> drawable = update.drawable;
        if (!drawable) continue;

        gfx::display_link_tick tick;
        tick.valid = true;
        tick.target_seconds = update.targetTimestamp;

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.016, 0.018, 0.024, 1.0);

        ImGui_ImplMetal_NewFrame(pass);
        feed_imgui(snapshot, delta, wheel);
        ImGui::NewFrame();

        // The lab sweep/idle text is the PR 16 instrument; once --open has
        // loaded a still, the image (drawn below, same render pass) replaces
        // it rather than drawing both.
        if (!current_image_ && animating_) {
          const auto w = static_cast<float>(snapshot.width);
          const auto h = static_cast<float>(snapshot.height);
          animation_phase_ = std::fmod(elapsed * 0.35, 1.0);
          const float bar_width = 6.0f * (snapshot.dpi_scale > 0.0f ? snapshot.dpi_scale : 1.0f);
          const float x = static_cast<float>(animation_phase_) * (w - bar_width);
          ImDrawList* bg = ImGui::GetBackgroundDrawList();
          bg->AddRectFilled(ImVec2(x, 0.0f), ImVec2(x + bar_width, h),
                            IM_COL32(230, 230, 235, 255));
        } else if (!current_image_ && overlay_visible_) {
          ImDrawList* bg = ImGui::GetBackgroundDrawList();
          const float w = static_cast<float>(snapshot.width);
          const float h = static_cast<float>(snapshot.height);
          bg->AddText(ImVec2(w * 0.5f - 80.0f, h * 0.5f), IM_COL32(220, 222, 228, 255),
                      "MediaViewer present lab");
        }

        if (overlay_visible_) {
          const auto stats = pacer_.stats();
          ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
          ImGui::SetNextWindowBgAlpha(0.72f);
          ImGui::Begin("##f3", nullptr,
                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoMove);
          ImGui::Text("Metal present lab   F3 overlay   space sweep   R reset");
          ImGui::Text("refresh   %.3f ms", stats.refresh_interval_ms);
          ImGui::Text("p50/p99   %.3f / %.3f ms", stats.p50_ms, stats.p99_ms);
          ImGui::Text("dropped   %llu  missed %llu",
                      static_cast<unsigned long long>(stats.dropped_frames),
                      static_cast<unsigned long long>(stats.missed_refreshes));
          ImGui::Text("source    %s", gfx::metal_drop_source_label(stats.source));
          ImGui::Text("%s", animating_ ? "animating (lab sweep)" : "idle-capable");
          ImGui::End();
        }

        ImGui::Render();

        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device_.native_queue();
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
        if (current_image_) {
          gfx::blit_params_mac bp;
          bp.pan_x = camera_.pan_x();
          bp.pan_y = camera_.pan_y();
          bp.zoom = camera_.zoom();
          bp.window_w = static_cast<float>(snapshot.width);
          bp.window_h = usable_window_h(snapshot);
          bp.origin_y = static_cast<float>(snapshot.chrome_height_px);
          bp.image_w = static_cast<float>(current_image_->width);
          bp.image_h = static_cast<float>(current_image_->height);
          bp.time_seconds = static_cast<float>(elapsed);
          blitter_.draw((__bridge void*)enc, current_image_->texture, bp);
        }
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];

        if (warmed_up_ && !options_.start_animating) ++idle_stats_.presents;
        ++total_presents_;
        pacer_.frame_end(tick);
        if (!decision.live) painted_static_ = true;
      }
    }

    // submit_image_load()'s job holds a raw (non-retaining) id<MTLDevice>
    // pointer, so it must be finished before device_.destroy() below, on
    // every exit path (soak completing here, not just an external stop()).
    // job_system::shutdown() is documented safe to call twice, so this does
    // not conflict with a caller's own shutdown (main_mac.mm's windowWillClose).
    if (options_.jobs) options_.jobs->shutdown();

    if (warmed_up_) {
      idle_stats_.elapsed_seconds = monotonic_seconds() - measurement_start_seconds_;
      const double end_cpu = process_cpu_seconds();
      if (idle_start_cpu_seconds_ >= 0.0 && end_cpu >= idle_start_cpu_seconds_ &&
          idle_stats_.elapsed_seconds > 0.0) {
        idle_stats_.cpu_percent =
            (end_cpu - idle_start_cpu_seconds_) / idle_stats_.elapsed_seconds * 100.0;
      }
    }

    const auto final_stats = pacer_.stats();
    if (options_.soak_seconds > 0.0) {
      MV_LOG_INFO("soak: %llu frames over %.1f s, %llu dropped, source: %s",
                  static_cast<unsigned long long>(final_stats.frames), final_stats.elapsed_seconds,
                  static_cast<unsigned long long>(final_stats.dropped_frames),
                  gfx::metal_drop_source_label(final_stats.source));
      if (!write_json_report()) exit_code_ = 2;
      const bool passed = options_.start_animating ? final_stats.meets_pr16_gate()
                                                   : idle_stats_.meets_pr16_gate();
      if (options_.gate_exit_code && !(passed && soak_complete_ && measurement_valid_))
        exit_code_ = 1;
    }

    if (display_link_) {
      CAMetalDisplayLink* link = (__bridge_transfer CAMetalDisplayLink*)display_link_;
      link.paused = YES;
      [link removeFromRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
      display_link_ = nullptr;
    }
    if (link_target_) {
      (void)(__bridge_transfer MvMetalLinkTarget*)link_target_;
      link_target_ = nullptr;
    }
    if (imgui_ready_) {
      ImGui_ImplMetal_Shutdown();
      imgui_ready_ = false;
    }
    current_image_.reset();
    delete pending_image_.exchange(nullptr);
    blitter_.destroy();
    layer_.destroy();
    device_.destroy();
    ImGui::DestroyContext();
  }

  // The loop above exits two ways: running_ went false (an external stop()
  // call — main_mac.mm's applicationShouldTerminate: already owns quitting
  // the app once this call returns) or a soak's `break` above completed on
  // its own with running_ still true (nothing else is going to ask the app
  // to quit, so the tail below must). Nothing between here and the loop
  // touches running_, so reading it now is equivalent to reading it right
  // after the loop exited, before teardown — just without the scoping
  // problem of declaring it inside the block above and using it after.
  const bool self_initiated_exit = running_.load(std::memory_order_acquire);

  finished_.store(true, std::memory_order_release);
  running_.store(false, std::memory_order_release);
  // Only self-terminate when nothing external asked us to stop: an
  // external stop() (main_mac.mm's applicationShouldTerminate:, mid its own
  // background-queue shutdown) already owns the one NSTerminateLater /
  // replyToApplicationShouldTerminate: cycle for this quit. Calling
  // [NSApp terminate:nil] again here would re-enter
  // applicationShouldTerminate: for a termination already in flight.
  if (self_initiated_exit) {
    dispatch_async(dispatch_get_main_queue(), ^{
      [NSApp terminate:nil];
    });
  }
}

bool present_lab_mac::write_json_report() const noexcept {
  if (options_.json_report_path.empty()) return true;
  FILE* f = std::fopen(options_.json_report_path.c_str(), "wb");
  if (!f) {
    MV_LOG_ERROR("present_lab_mac: cannot write report");
    return false;
  }
  gfx::pace_json r;
  r.pace = pacer_.stats();
  r.idle = idle_stats_;
  r.static_run = !options_.start_animating;
  r.measurement_complete = soak_complete_ && measurement_valid_ && exit_code_ == 0;
  r.meets_gate = r.measurement_complete &&
                 (options_.start_animating ? r.pace.meets_pr16_gate() : r.idle.meets_pr16_gate());
  const bool ok = gfx::write_pace_json(f, r);
  return std::fclose(f) == 0 && ok;
}

}  // namespace mv::shell
