// SPDX-License-Identifier: GPL-2.0-or-later
// Metal present lab — PR 16's instrument, kept as a debug harness the way
// the Win32 lab is. No SwiftUI. plan/10, plan/15.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "canvas/camera.h"
#include "core/job_system.h"
#include "core/result.h"
#include "core/spsc_ring.h"
#include "gfx/blit_metal.h"
#include "gfx/device_mac.h"
#include "gfx/metal_layer.h"
#include "gfx/metal_pacer.h"
#include "gfx/present_policy.h"
#include "gfx/video_blit_metal.h"
#include "abi/animation_session.h"
#include "codec/anim.h"
#include "image/gpu_image_mac.h"
#include "player/media_source.h"
#include "shell/input_state.h"

namespace mv::shell {

struct mac_lab_options {
  double soak_seconds = 0.0;
  std::string json_report_path;
  bool gate_exit_code = false;
  bool start_animating = false;
  bool overlay_visible = true;
  // PR 17: a still to open on start (--open PATH), decoded and uploaded on a
  // job_system worker, never on the render thread (rule 1, plan/02).
  std::string open_path;
  mv::job_system* jobs = nullptr;  // non-owning; started by main_mac.mm
};

class present_lab_mac {
 public:
  present_lab_mac() = default;
  ~present_lab_mac();

  present_lab_mac(const present_lab_mac&) = delete;
  present_lab_mac& operator=(const present_lab_mac&) = delete;

  [[nodiscard]] mv::expected start(void* nsview, const mac_lab_options& options) noexcept;
  void stop() noexcept;
  void publish(const input_snapshot& snapshot) noexcept { input_.publish(snapshot); }
  void wake() noexcept;

  // [any-thread] Loads `path_utf8`, replacing whatever is on screen once
  // decode+upload finishes. Safe to call repeatedly, including while a
  // previous call is still decoding: this bumps job_system's view generation
  // first (core/job_system.h — "every job carries a generation counter tied
  // to the current view intent; navigating away bumps it"), so a slow decode
  // a later open_item() has superseded is abandoned rather than clobbering
  // the newer selection. Only folder navigation should call this — thumbnail
  // and relist jobs (folder_model_mac) stay on background_generation and are
  // unaffected by the bump.
  void open_item(std::string path_utf8) noexcept;

  // [any-thread] What the SwiftUI transport strip shows. Published by the render
  // thread through atomics; `active` is false when no clip is on screen.
  struct video_status {
    bool active = false;
    bool playing = false;
    bool muted = false;
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    int rate_x100 = 100;
    float volume = 1.0f;
  };
  // [any-thread] Whether an animated still (GIF/APNG/WebP) is open on the
  // canvas right now -- read by main_mac.mm's keyDown: to route Space/,/.
  // to it the way key_router.cpp's item_kind::animation does on Windows.
  [[nodiscard]] bool anim_active() const noexcept {
    return anim_active_.load(std::memory_order_acquire);
  }

  [[nodiscard]] video_status video_status_snapshot() const noexcept {
    video_status s;
    s.active = vs_active_.load(std::memory_order_acquire);
    s.playing = vs_playing_.load(std::memory_order_relaxed);
    s.muted = vs_muted_.load(std::memory_order_relaxed);
    s.position_ms = vs_pos_ms_.load(std::memory_order_relaxed);
    s.duration_ms = vs_dur_ms_.load(std::memory_order_relaxed);
    s.rate_x100 = vs_rate_x100_.load(std::memory_order_relaxed);
    s.volume = vs_volume_.load(std::memory_order_relaxed);
    return s;
  }

  // [any-thread] How many distinct stills have reached the screen (a
  // preview -> full refinement of the same item counts once). PR 20's
  // "after the first successful still open" default-viewer prompt polls it.
  [[nodiscard]] std::uint32_t stills_shown() const noexcept {
    return stills_shown_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

 private:
  void render_thread_main() noexcept;
  bool write_json_report() const noexcept;
  void submit_image_load(std::string path_utf8, std::uint64_t item_id) noexcept;
  // PR 19: opens a clip on a worker (open_media blocks on I/O) and posts it.
  void submit_video_open(std::string path_utf8, std::uint64_t item_id) noexcept;
  // [render-thread] Frees the current clip: releases its frames now, closes the
  // media_source (which joins its threads) on a worker, never here.
  void retire_media() noexcept;
  // Latched transport from the UI thread: video AND animated-still playback
  // share this (plan/16 "Video" -- Space/,/. mean the same thing on either;
  // key_router.cpp maps both item_kind::clip and item_kind::animation to
  // mode::video for exactly this reason). Returns true when it changed what
  // should be on screen, so the caller redraws.
  bool apply_playback_input(const input_snapshot& snapshot) noexcept;
  void update_video_status() noexcept;
  // The picture on the canvas, still or video frame. False when there is none.
  [[nodiscard]] bool picture_size(float* w, float* h) const noexcept;

  void* view_ = nullptr;
  void* display_link_ = nullptr;
  void* link_target_ = nullptr;
  mac_lab_options options_{};

  gfx::metal_device device_;
  gfx::metal_layer layer_;
  gfx::metal_pacer pacer_;
  gfx::blitter_mac blitter_;
  canvas::camera camera_;

  // Posted by the decode-worker job (submit_image_load), taken over by the
  // render thread. Never touched from two threads at once: the worker only
  // ever exchanges nullptr -> pointer, and the render thread only ever
  // exchanges pointer -> nullptr, so there is no ABA window.
  std::atomic<image::gpu_image_mac*> pending_image_{nullptr};
  // Bumped by every open_item(); stamped on the images that open produces.
  std::atomic<std::uint64_t> item_counter_{0};
  std::unique_ptr<image::gpu_image_mac> current_image_;
  std::atomic<std::uint32_t> stills_shown_{0};

  // PR 19 video. The render thread owns `media_` and `video_frame_`; a worker
  // opens the clip and posts it here, the same one-way handoff as
  // pending_image_. `item` ties it to the open_item() that asked for it.
  struct pending_media {
    player::media_source* source = nullptr;
    std::uint64_t item = 0;
  };
  gfx::video_blitter_mac video_blitter_;
  std::atomic<pending_media*> pending_media_{nullptr};
  std::atomic<std::uint64_t> video_opening_{0};  // item id being opened, 0 = none
  player::media_source* media_ = nullptr;
  std::uint64_t media_item_ = 0;
  player::video_frame* video_frame_ = nullptr;
  // Frames already presented but not yet released: the GPU may still be reading
  // them, and the decode thread reuses a released slot at once.
  static constexpr std::size_t kRetiredFrames = 2;
  player::video_frame* retired_frames_[kRetiredFrames] = {};
  bool media_fitted_ = false;
  int speed_rung_ = 2;  // index into the 0.25 .. 4 ladder; 2 = 1x
  std::uint32_t seen_anim_toggle_ = 0;
  std::int64_t seen_anim_steps_ = 0;
  std::int64_t seen_video_skip_ = 0;
  std::int32_t seen_video_speed_ = 0;
  std::uint32_t seen_video_mute_ = 0;
  bool video_muted_ = false;
  std::int32_t seen_video_volume_ = 0;
  std::uint32_t seen_video_volume_set_ = 0;
  float video_volume_ = 1.0f;  // survives from clip to clip
  std::uint32_t seen_video_seek_ = 0;

  // Animated GIF/APNG/WebP playback (plan/04, folded into PR 18's "animated
  // GIF/APNG/WebP on the display-link frame clock", plan/15). Frame 0 already
  // went through the still path (current_image_, rule 3); animation_session
  // decodes and uploads frames 1.. on its own thread into a small ring, and
  // anim_frame_ -- never current_image_ -- is what the draw call samples once
  // it exists, so a still's own camera fit is not re-run per frame.
  std::unique_ptr<mv::abi::animation_session<image::gpu_image_mac>> anim_session_;
  std::unique_ptr<image::gpu_image_mac> anim_frame_;
  // The live job_system generation this animation_session::publish() call was
  // for; compared against options_.jobs->current_generation() each tick the
  // same way Windows compares against mv_session_current_generation, so a
  // navigation retires the old feed and its queued frames without this class
  // needing its own separate "did the selection change" signal.
  std::uint32_t anim_generation_ = 0;
  codec::frame_schedule anim_schedule_;
  bool anim_finished_ = false;
  bool anim_seeking_ = false;
  std::uint32_t anim_index_ = 0;
  bool anim_live_ = false;  // presenting because it's actually animating, not paused/finished
  std::atomic<bool> anim_active_{false};

  std::atomic<bool> vs_active_{false};
  std::atomic<bool> vs_playing_{false};
  std::atomic<bool> vs_muted_{false};
  std::atomic<std::int64_t> vs_pos_ms_{0};
  std::atomic<std::int64_t> vs_dur_ms_{0};
  std::atomic<int> vs_rate_x100_{100};
  std::atomic<float> vs_volume_{1.0f};

  publish_slot<input_snapshot> input_;
  std::thread render_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> wake_flag_{false};
  std::atomic<bool> ready_{false};
  std::atomic<int> start_error_{0};
  int exit_code_ = 0;

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
  input_cursor input_cursor_;
  gfx::metal_idle_stats idle_stats_;
  double measurement_start_seconds_ = 0.0;
  double idle_start_cpu_seconds_ = -1.0;
  std::uint32_t seen_overlay_seq_ = 0;
  std::uint32_t seen_animation_seq_ = 0;
  std::uint32_t seen_reset_seq_ = 0;
  std::uint32_t seen_resize_seq_ = 0;
  std::uint32_t seen_display_seq_ = 0;
  double animation_phase_ = 0.0;
  double last_input_time_ = -1.0;

  // PR 17 pan/zoom input. Drag delta is computed from consecutive snapshots
  // (the snapshot itself carries only the absolute mouse position, same as
  // the Windows shell) rather than published as a delta.
  std::uint32_t seen_fit_seq_ = 0;
  std::uint32_t seen_one_to_one_seq_ = 0;
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;
  bool was_dragging_ = false;
};

}  // namespace mv::shell
