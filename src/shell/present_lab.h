// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The present lab: PR 1's instrument, and the app's actual render thread.
//
// plan/10-roadmap.md, PR 1: "Win32 + DComp + Dear ImGui host, D3D11 device,
// flip-model waitable swapchain, per-monitor-v2 DPI, clear to a colour, F3
// frame-time overlay reading real present-to-present intervals."
//
// It stays in the tree as a debug harness. Under the D1 amendment this window
// and this swapchain ARE the app — PR 3 hosts WinUI chrome inside them rather
// than re-implementing presentation, so nothing here is throwaway.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "abi/native.h"
#include "canvas/camera.h"
#include "canvas/refinement.h"
#include "codec/anim.h"
#include "core/spsc_ring.h"
#include "gfx/blit.h"
#include "gfx/video_blit.h"
#include "gfx/device.h"
#include "gfx/pacer.h"
#include "gfx/swapchain.h"
#include "image/gpu_image.h"
#include "image/linear.h"
#include "image/tiles.h"
#include "mediaviewer/mediaviewer.h"
#include "shell/dino_game.h"
#include "shell/edit_view.h"
#include "shell/input_state.h"

namespace mv::shell {

struct lab_options {
  // Run for this many seconds, write the JSON report, then exit. 0 = run until
  // the window is closed. This is what tools/frametime drives.
  double soak_seconds = 0.0;
  std::uint32_t av_soak_seconds = 0;
  std::wstring av_csv;
  std::wstring json_report_path;
  // With a soak, exit non-zero if the PR 1 gate did not hold. CI wants this;
  // a human running the lab does not.
  bool gate_exit_code = false;
  bool start_animating = false;
  // Soak only: once a still is up, pan it at 100 % across the whole image on
  // a fixed path (tiled pyramid / cached-image pan measurement). `--pan-soak`.
  bool scripted_pan = false;
  // Keep presenting while the window is in the background. `--browse-soak`
  // drives itself and must not stall if it is not the foreground window.
  bool present_when_inactive = false;
  bool overlay_visible = true;
};

// What the render thread is doing with an animated item, for the UI (Space,
// `,` `.`) and the slideshow. `none` means the item is not an animation.
enum class animation_state : std::uint8_t { none, playing, playing_forever, paused, finished };

class present_lab {
 public:
  present_lab() = default;
  ~present_lab();

  present_lab(const present_lab&) = delete;
  present_lab& operator=(const present_lab&) = delete;

  // The session owns decode jobs and the ready GPU image. Bound before start.
  void bind_session(mv_session_t session) noexcept { session_ = session; }

  // Called from the UI thread once the window exists.
  [[nodiscard]] expected start(HWND window, const lab_options& options) noexcept;

  // Signals the render thread and joins it. Safe to call twice.
  void stop() noexcept;

  // [ui-thread] Publishes the snapshot the render thread will consume next
  // frame. Never blocks.
  void publish(const input_snapshot& snapshot) noexcept { input_.publish(snapshot); }

  // [ui-thread][no-block] Wakes the render thread out of the idle wait.
  void wake() noexcept;

  // True once the render thread has stopped, for whatever reason — including
  // the soak finishing, which is how the UI thread knows to close the window.
  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
  }

  // Valid after finished(). 0 when the gate held or no gate was requested.
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

  // [any-thread][no-block] True while nothing is open or the camera is in fit
  // mode, as of the render thread's last input pass.
  [[nodiscard]] bool view_fitted() const noexcept {
    return view_fitted_.load(std::memory_order_relaxed);
  }

  // [any-thread][no-block] True while a still (not a clip) is on the canvas.
  [[nodiscard]] bool showing_still() const noexcept {
    return showing_still_.load(std::memory_order_relaxed);
  }

  // [any-thread][no-block] Media size and target zoom for the status line.
  [[nodiscard]] std::uint32_t status_width() const noexcept {
    return status_width_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint32_t status_height() const noexcept {
    return status_height_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint32_t status_zoom_percent() const noexcept {
    return status_zoom_pct_.load(std::memory_order_relaxed);
  }

  // [any-thread][no-block] PR 10: the full-resolution size of the still on the
  // canvas, if it is the item whose path hashes to `item_key`. What the edit
  // session constrains a crop against (it needs the aspect, before any edit).
  [[nodiscard]] bool still_size(std::uint64_t item_key, std::uint32_t* w, std::uint32_t* h) const noexcept {
    if (item_key == 0 || shown_key_.load(std::memory_order_acquire) != item_key) return false;
    *w = shown_w_.load(std::memory_order_relaxed);
    *h = shown_h_.load(std::memory_order_relaxed);
    return *w > 0 && *h > 0;
  }

  // PR 11: the FP16 working texture (image/linear.h, D6) of the still being
  // adjusted. [worker thread] Creates an immutable R16G16B16A16_FLOAT texture
  // from `img` on the render thread's device (free-threaded, created with
  // D3D11_SUBRESOURCE_DATA — plan/02) and queues it for the edit_view slot
  // (`item`, `generation`). False when there is no device or the create
  // failed. The render thread picks it up with a try_lock: it never waits.
  [[nodiscard]] bool upload_working(const image::linear_image& img, std::uint64_t item,
                                    std::uint32_t generation) noexcept;
  // [any-thread][no-block] Releases it: the item changed, or nothing needs it.
  void drop_working() noexcept;

  // [any-thread][no-block] The animated item's state, as of the last frame.
  [[nodiscard]] animation_state animation() const noexcept {
    return static_cast<animation_state>(anim_state_.load(std::memory_order_relaxed));
  }

  // Time from mark_navigation() (call it immediately before mv_folder_select)
  // to the present that first draws the new still. `cached` means that texture
  // was already the full-resolution image, not a preview. Seq 0 means the
  // 64-sample log is full. UI thread marks; the render thread fills the sample.
  struct nav_sample {
    double ready_ms = -1.0;
    double present_ms = -1.0;
    double refresh_ms = 0.0;
    int cached = 0;
    int valid = 0;
  };
  [[nodiscard]] std::uint64_t mark_navigation() noexcept;
  [[nodiscard]] bool navigation_done(std::uint64_t seq) const noexcept;
  [[nodiscard]] nav_sample navigation_sample(std::uint64_t seq) const noexcept;

 private:
  void render_thread_main() noexcept;
  [[nodiscard]] expected rebuild_device() noexcept;
  void draw_frame(const input_snapshot& snapshot, double elapsed_seconds) noexcept;
  void draw_overlay(const input_snapshot& snapshot) noexcept;
  // Loupe frame, hold-previous label, info line. ImGui, same present.
  void draw_view_overlays(const input_snapshot& snapshot) noexcept;
  bool write_json_report() const noexcept;

 public:
  // [any-thread] What the eyedropper last read, as `#RRGGBB  rgb(r, g, b)  x y`;
  // empty when it is off or the cursor is not over a readable still.
  [[nodiscard]] std::string eyedropper_text() const {
    std::lock_guard<std::mutex> lock(eye_mutex_);
    return eye_text_;
  }

 private:
  // Eyedropper (PR 9). Image textures are immutable GPU memory, so one texel is
  // copied into a 1x1 staging texture and mapped with DO_NOT_WAIT on a later
  // frame: the render thread never stalls on the GPU (rule 1).
  struct eyedropper_sample {
    const void* texture = nullptr;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint8_t rgba[4] = {};
    bool valid = false;    // rgba holds the texel for (texture, x, y)
    bool pending = false;  // a copy is in flight into staging
  };
  eyedropper_sample eye_;
  gfx::com_ptr<ID3D11Texture2D> eye_staging_;
  mutable std::mutex eye_mutex_;
  std::string eye_text_;
  std::uint32_t seen_meta_seq_ = 0;
  std::uint8_t seen_view_flags2_ = 0;
  float seen_eye_x_ = -1.0f;
  float seen_eye_y_ = -1.0f;

  HWND window_ = nullptr;
  lab_options options_{};
  mv_session_t session_ = nullptr;

  gfx::device device_;
  gfx::swapchain swapchain_;
  gfx::pacer pacer_;
  gfx::blitter blitter_;
  gfx::video_blitter video_blitter_;
  player::video_frame current_video_{};
  bool video_active_ = false;
  // A clip is open on the session — true before its first frame exists, which
  // is the window in which the canvas must not paint the empty-window welcome.
  bool video_open_ = false;
  // 0 when nothing is open. These used to dereference current_image_ whenever
  // no video texture was live and relied on every caller checking first — the
  // same shape as the F3 crash, one guard away from being the same bug.
  // PR 10: a still's size is its *edited* size (a quarter turn swaps it, a
  // crop shrinks it): the camera frames what the edit shows.
  float media_width() const {
    if (current_video_.texture) return static_cast<float>(current_video_.width);
    return current_image_ ? static_cast<float>(place_image(*current_image_).cropped.w) : 0.0f;
  }
  float media_height() const {
    if (current_video_.texture) return static_cast<float>(current_video_.height);
    return current_image_ ? static_cast<float>(place_image(*current_image_).cropped.h) : 0.0f;
  }
  // PR 10 edit geometry (shell/edit_view.h). The snapshot's slot for `img`, or
  // null; `img` placed through it (identity when there is none).
  [[nodiscard]] const edit_view* edit_for(const image::gpu_image& img) const noexcept {
    return match_edit(edit_slots_, img.item_key, img.view_generation);
  }
  [[nodiscard]] edit::placement place_image(const image::gpu_image& img) const noexcept {
    return place_through(edit_for(img), img.width, img.height);
  }
  void draw_crop_overlay(const input_snapshot& snapshot) noexcept;
  // PR 11 working texture. `working_` is render-thread only; the other three
  // are the hand-over, under a mutex the render thread only ever try_locks.
  struct working_texture {
    gfx::com_ptr<ID3D11ShaderResourceView> srv;
    ID3D11Device* device = nullptr;  // identity only: a rebuilt device drops it
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t item = 0;
    std::uint32_t generation = 0;
  };
  std::unique_ptr<working_texture> working_;
  std::mutex working_mutex_;
  std::unique_ptr<working_texture> working_incoming_;
  bool working_drop_ = false;
  bool take_working() noexcept;  // render thread: true when what it draws changed
  // The working texture if it belongs to `ev` (the shown still's slot).
  [[nodiscard]] const working_texture* working_for(const edit_view* ev) const noexcept {
    if (!working_ || !ev || ev->item == 0) return nullptr;
    if (working_->item != ev->item || working_->generation != ev->generation) return nullptr;
    return working_->device == device_.d3d() ? working_.get() : nullptr;
  }
  // The device workers upload on. Copied (AddRef) under the lock, used after it.
  std::mutex upload_device_mutex_;
  gfx::com_ptr<ID3D11Device> upload_device_;
  // Records the still that just became current: its slot, and its size for
  // still_size(). Called wherever current_image_ takes a published image.
  void note_still_landed() noexcept;
  void note_nav_image(const image::gpu_image& ready) noexcept;
  void commit_nav_present() noexcept;
  std::atomic<std::uint64_t> nav_seq_{0};
  std::atomic<std::int64_t> nav_qpc_{0};
  std::atomic<std::uint64_t> nav_done_seq_{0};
  nav_sample nav_samples_[64]{};
  std::uint64_t nav_latched_seq_ = 0;
  std::int64_t nav_latch_qpc_ = 0;
  double nav_ready_ms_ = -1.0;
  std::uint64_t nav_key_ = 0;
  int nav_cached_ = 0;
  bool nav_have_ready_ = false;
  edit_view edit_slots_[2];   // copied from the snapshot each iteration
  edit_view applied_edit_{};  // what the current still was last fitted with
  std::atomic<std::uint64_t> shown_key_{0};
  std::atomic<std::uint32_t> shown_w_{0};
  std::atomic<std::uint32_t> shown_h_{0};
  canvas::camera camera_;
  mv::abi::gpu_image_ptr current_image_;
  // plan/04 step 4, preview → full: the texture a refinement replaced, drawn
  // under the incoming one while `fade_` runs, then released.
  mv::abi::gpu_image_ptr fade_from_;
  canvas::crossfade fade_;
  // A preview sharper than a tiled image's overview stays under its tiles until
  // the item changes, so the swap to the tiled pyramid never blurs first.
  mv::abi::gpu_image_ptr refine_base_;
  std::span<const gfx::tile_quad> tile_draws_;  // tiles_frame, this frame only
  std::uint64_t seen_tile_seq_ = 0;
  std::uint64_t refinements_ = 0;
  std::uint64_t stale_drops_ = 0;
  // PR 7 verify, "the full decode replaces it without a visible pop" — the
  // measurable half of it, so a report can fail a pop instead of a person
  // squinting at a RAW. A pop is one of three things, and each has a counter:
  //   * the view jumping — refine_max_edge_shift_px_ / refine_max_scale_step_
  //   * the fade not running to its end, i.e. a hard cut — started vs completed
  //   * a dropped frame inside the fade window — refine_fade_dropped_
  // A refinement that never started a fade is not a pass either: it would read
  // as zero shift and zero drops. The gate requires started >= 1.
  std::uint64_t refine_fades_started_ = 0;
  std::uint64_t refine_fades_completed_ = 0;
  std::uint64_t refine_fades_cancelled_ = 0;
  std::uint64_t refine_fade_frames_ = 0;   // frames recorded while a fade ran
  std::uint64_t refine_fade_dropped_ = 0;  // pacer drops inside fade windows
  std::uint64_t refine_fade_drops_at_start_ = 0;
  bool refine_fade_running_ = false;
  // Worst corner displacement (px) of the picture's on-screen rectangle across
  // a refinement, and the worst ratio of its on-screen width. 0 and 1 when
  // nothing refined.
  double refine_max_edge_shift_px_ = 0.0;
  double refine_max_scale_step_ = 1.0;
  // Render thread only: the refinement branch and every site that drops a fade.
  void refine_fade_begun() noexcept;
  void refine_fade_abandoned() noexcept;
  void refine_fade_tick(double elapsed) noexcept;
  // --json: seconds since the render thread started (so, for --open, since
  // launch) at which the last item showed its first pixel, its
  // full-quality publish, and (tiled) the first frame with every visible tile.
  double item_start_seconds_ = -1.0;
  double first_pixel_seconds_ = -1.0;
  double full_seconds_ = -1.0;
  double tiles_complete_seconds_ = -1.0;
  double scripted_pan_start_ = -1.0;
  // The last different still that was on screen, for hold `\` (plan/16). Kept
  // here, so showing it is a draw of a texture already in VRAM — never a
  // second session or an mv_image_open.
  mv::abi::gpu_image_ptr previous_image_;
  // Animation (plan/04): the current frame in its own slot, like current_video_
  // — never current_image_, so hold-previous and the camera are not touched per
  // frame. Frame 0 arrived as the still and fitted the camera once.
  mv::abi::gpu_image_ptr anim_frame_;
  codec::frame_schedule anim_schedule_;
  // Review note 44: animation cadence for the --json report, across the whole
  // run (the schedule resets per item). Render thread only.
  std::uint64_t anim_frames_shown_ = 0;   // taken on the schedule, not by stepping
  std::uint64_t anim_delay_sum_ms_ = 0;   // of those frames' file delays
  std::uint64_t anim_late_total_ = 0;     // frames that restarted the cadence
  std::uint32_t anim_generation_ = 0;
  std::uint32_t anim_index_ = 0;
  std::uint32_t seen_anim_toggle_seq_ = 0;
  std::int64_t seen_anim_steps_ = 0;
  bool anim_finished_ = false;
  bool anim_seeking_ = false;  // take the next frame even though paused
  bool anim_live_ = false;
  // Review must-have A, on the instrument: how many times hold-previous's
  // texture changed. Playing an animation must leave this where it was.
  std::uint32_t previous_image_changes_ = 0;
  std::uint8_t seen_view_flags_ = 0;
  std::int32_t seen_loupe_steps_x_ = 0;
  std::int32_t seen_loupe_steps_y_ = 0;
  std::uint32_t seen_marked_count_ = 0;
  bool seen_blackout_ = false;
  std::uint32_t seen_item_index_ = 0;
  std::uint32_t seen_item_count_ = 0;

  publish_slot<input_snapshot> input_;
  std::thread render_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> view_fitted_{true};
  std::atomic<bool> showing_still_{false};
  std::atomic<std::uint8_t> anim_state_{0};
  std::atomic<std::uint32_t> status_width_{0};
  std::atomic<std::uint32_t> status_height_{0};
  std::atomic<std::uint32_t> status_zoom_pct_{0};
  HANDLE wake_event_ = nullptr;
  HANDLE ready_event_ = nullptr;
  std::atomic<int> start_error_{0};
  int exit_code_ = 0;

  // Render-thread-only state.
  bool overlay_visible_ = true;
  bool animating_ = true;
  bool occluded_ = false;
  bool imgui_ready_ = false;
  bool warmed_up_ = false;
  bool measurement_valid_ = true;
  bool soak_complete_ = false;
  std::uint64_t total_presents_ = 0;
  bool was_presenting_ = false;
  bool painted_static_ = false;
  bool live_presenting_ = false;  // this frame: sweep, springs, or input tail
  input_cursor input_cursor_;
  gfx::idle_stats idle_stats_;
  std::int64_t measurement_start_qpc_ = 0;
  double idle_start_cpu_seconds_ = -1.0;
  std::uint32_t seen_overlay_seq_ = 0;
  std::uint32_t seen_animation_seq_ = 0;
  // Space on an empty window runs the runner (dino_game.h); a soak keeps the
  // judder sweep, which the frame-time gate depends on.
  bool sweep_mode_ = false;
  dino_game game_;
  std::uint32_t seen_game_exit_seq_ = 0;
  std::uint32_t seen_game_view_seq_ = 0;
  double last_game_elapsed_ = 0.0;
  std::uint32_t seen_reset_seq_ = 0;
  std::uint32_t seen_resize_seq_ = 0;
  std::uint32_t seen_display_seq_ = 0;
  std::uint32_t seen_fit_seq_ = 0;
  std::uint32_t seen_one_seq_ = 0;
  std::uint32_t seen_zoom_in_seq_ = 0;
  std::uint32_t seen_zoom_out_seq_ = 0;
  std::uint32_t seen_zoom_preset_seq_ = 0;
  std::uint32_t seen_fill_seq_ = 0;
  std::uint32_t seen_discard_seq_ = 0;
  double animation_phase_ = 0.0;
  double last_input_time_ = -1.0;  // < 0: no input yet, do not fake a 500 ms tail
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;
  bool was_left_down_ = false;
};

}  // namespace mv::shell
