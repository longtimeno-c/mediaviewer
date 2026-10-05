// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Metal present lab — PR 16's instrument, kept as a debug harness the way
// the Win32 lab is. No SwiftUI. docs/design/10, docs/design/15.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <thread>
#include <utility>
#include <vector>

#include "canvas/camera.h"
#include "canvas/refinement.h"
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
#include "edit/edit_stack.h"
#include "image/gpu_image_mac.h"
#include "image/linear.h"
#include "player/media_source.h"
#include "player/playback_hold.h"
#include "shell/dino_game.h"
#include "shell/input_state.h"

namespace mv::shell {

struct mac_lab_options {
  double soak_seconds = 0.0;
  std::string json_report_path;
  bool gate_exit_code = false;
  bool start_animating = false;
  bool overlay_visible = true;
  // A measuring run with no --soak timer (--browse-soak): present whether or
  // not the window has focus, as a soak does, so a person using the machine
  // does not stall it.
  bool harness = false;
  // PR 17: a still to open on start (--open PATH), decoded and uploaded on a
  // job_system worker, never on the render thread (rule 1, docs/design/02).
  std::string open_path;
  mv::job_system* jobs = nullptr;  // non-owning; started by main_mac.mm
  // Milestone H, rule 3 for a clip opened on a moment: [pool thread] the cached
  // JPEG-512 to show while the clip opens and seeks (the moment's row, else the
  // clip's poster), "" for none. A cache lookup, never a decode. Unset: the
  // previous picture stays up until the sought frame, as before.
  std::function<std::string(const std::string& path_utf8, std::int64_t mtime_unix,
                            std::uint64_t size, std::int64_t moment_ms)>
      clip_thumb;
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
  // Returns the item id the images of this open will carry (PR 10: the UI tags
  // edit geometry with it, input_state.h edit_view).
  //
  // `mtime_unix`/`size` are the listing's stamp for the file, when the caller
  // has one: a still cached under that exact stamp is shown at once, full
  // resolution, with no read or decode. Without one the file always loads.
  static constexpr std::int64_t kNoStamp = INT64_MIN;
  //
  // Milestone H (docs/design/17 "Enter on a video tile opens the clip and seeks to
  // that PTS, paused on the frame"): a clip with `moment_ms` >= 0 does not
  // start playing; it is paused and sought exactly to the moment as the
  // render thread adopts it, so no frame of the clip's head is shown or heard.
  // A still ignores the moment.
  //
  // `page` opens that page of a multi-page file (TIFF, PDF, DOCX —
  // docs/plans/audio-and-documents.md §2.3); a still's page 0 is the file.
  // Pages past 0 are not cached: a page turn is a decode of that page.
  std::uint64_t open_item(std::string path_utf8, std::int64_t mtime_unix = kNoStamp,
                          std::uint64_t size = 0, std::int64_t moment_ms = -1,
                          std::uint32_t page = 0) noexcept;

  // [any-thread] Nothing on screen: the open item (still, clip, or a load still
  // in flight) is dropped and the canvas shows the empty-window welcome again.
  // Used when a result list opened from the welcome closes ("Back to folder"
  // with no folder behind it). A later open_item() is unaffected.
  void close_item() noexcept;

  // How many pages the still `item` (open_item's id) has, once its decode has
  // said; 0 until then or for another item. [any-thread]
  [[nodiscard]] std::uint32_t page_count(std::uint64_t item) const noexcept {
    return pages_item_.load(std::memory_order_acquire) == item
               ? pages_count_.load(std::memory_order_acquire)
               : 0u;
  }

  // --browse-soak (the Windows lab's twin, present_lab.h): mark a navigation
  // just before selecting, then poll until the new item's first image is on
  // screen. ready_ms is mark -> image adopted by the render thread, present_ms
  // mark -> the frame showing it committed; cached is 1 when that first image
  // was already full resolution. 64 marks per run.
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
  [[nodiscard]] bool showing_still() const noexcept {
    return shown_item_.load(std::memory_order_acquire) != 0;
  }

  // [UI thread] Decode the neighbours of the item just opened into the still
  // cache (Windows' ±2 prefetch, abi.cpp submit_prefetch), at the current view
  // generation: the next open_item() abandons what has not finished. A later
  // open_item() of a cached file shows its full image in the next frame. Pass
  // stills only; animated families and clips are skipped by the lab.
  void prefetch(const std::vector<std::string>& paths_utf8) noexcept;

  // [any-thread] PR 10: the full-resolution size of the still on screen, if it
  // belongs to `item`. What the edit session constrains a crop against.
  [[nodiscard]] bool still_size(std::uint64_t item, std::uint32_t* w, std::uint32_t* h) const noexcept {
    if (item == 0 || shown_item_.load(std::memory_order_acquire) != item) return false;
    *w = shown_w_.load(std::memory_order_relaxed);
    *h = shown_h_.load(std::memory_order_relaxed);
    return *w > 0 && *h > 0;
  }

  // PR 11: the FP16 working texture (image/linear.h, D6) of the still being
  // adjusted, for the open `item` (open_item's id). [worker thread] Creates an
  // immutable RGBA16Float MTLTexture (image::upload_linear) and hands it to
  // the render thread through an atomic exchange, like pending_image_. False
  // when there is no device or the texture could not be made.
  [[nodiscard]] bool upload_working(const image::linear_image& img, std::uint64_t item) noexcept;
  // [any-thread] Releases it: the item changed, or nothing needs it.
  void drop_working() noexcept;

  // [any-thread] What the SwiftUI transport strip shows. Published by the render
  // thread through atomics; `active` is false when no clip is on screen.
  struct video_status {
    bool active = false;
    bool playing = false;
    bool muted = false;
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    std::int64_t position_ns = 0;  // PR 13: trim markers need the frame, not the ms
    int rate_x100 = 100;
    float volume = 1.0f;
    // The open_item() id the live clip belongs to. While a newer item is still
    // opening this is the previous clip's, so a seek meant for the new one can
    // tell it would land on the old (Milestone H: N / Shift+N).
    std::uint64_t item = 0;
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
    s.position_ns = vs_pos_ns_.load(std::memory_order_relaxed);
    s.duration_ms = vs_dur_ms_.load(std::memory_order_relaxed);
    s.rate_x100 = vs_rate_x100_.load(std::memory_order_relaxed);
    s.volume = vs_volume_.load(std::memory_order_relaxed);
    s.item = vs_item_.load(std::memory_order_relaxed);
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
  void submit_image_load(std::string path_utf8, std::uint64_t item_id,
                         std::uint32_t page = 0) noexcept;

  // Full-resolution stills already on the GPU, keyed by path + size + mtime:
  // the item on screen and its prefetched neighbours. Budgeted by bytes (a
  // 42 MP RAW with mips is ~230 MB); most recent last. Workers insert, the UI
  // thread looks up, so it is locked, never touched by the render thread.
  struct cached_still {
    std::string path;
    std::int64_t mtime = 0;
    std::uint64_t size = 0;
    image::gpu_image_mac image;
  };
  bool cache_publish(const std::string& path, std::int64_t mtime, std::uint64_t size,
                     std::uint64_t item_id) noexcept;
  void cache_put(const std::string& path, std::int64_t mtime, std::uint64_t size,
                 const image::gpu_image_mac& image) noexcept;
  [[nodiscard]] bool cache_has(const std::string& path) const noexcept;
  // Neighbours wait for the item on screen: queued at once they took the
  // cores its full decode needed. prefetch() parks the list while that item
  // is loading; its load job releases it once the full image is up.
  void submit_prefetch(const std::vector<std::string>& paths_utf8) noexcept;
  void release_prefetch(std::uint64_t item_id) noexcept;
  std::mutex prefetch_mutex_;
  std::vector<std::string> prefetch_parked_;
  std::uint64_t prefetch_parked_for_ = 0;
  std::atomic<std::uint64_t> loading_item_{0};
  // page_count(): written by the decode worker, count first, then the item.
  std::atomic<std::uint32_t> pages_count_{0};
  std::atomic<std::uint64_t> pages_item_{0};
  // Render thread only, but for the atomics the UI polls.
  void note_nav_image(const image::gpu_image_mac& ready) noexcept;
  void commit_nav_present() noexcept;
  std::atomic<std::uint64_t> nav_seq_{0};
  std::atomic<double> nav_mark_seconds_{0.0};
  std::atomic<std::uint64_t> nav_done_seq_{0};
  nav_sample nav_samples_[64]{};
  std::uint64_t nav_latched_seq_ = 0;
  double nav_latch_seconds_ = 0.0;
  double nav_ready_ms_ = -1.0;
  std::uint64_t nav_item_ = 0;
  int nav_cached_ = 0;
  bool nav_have_ready_ = false;
  mutable std::mutex still_cache_mutex_;
  std::vector<cached_still> still_cache_;
  std::uint64_t still_cache_bytes_ = 0;
  // PR 19: opens a clip on a worker (open_media blocks on I/O) and posts it.
  void submit_video_open(std::string path_utf8, std::uint64_t item_id,
                         std::int64_t moment_ms) noexcept;
  // Milestone H: the clip's cached thumbnail (options_.clip_thumb) up as a
  // preview of `item_id` while the clip opens; its first frame replaces it.
  void submit_clip_placeholder(std::string path_utf8, std::int64_t mtime_unix, std::uint64_t size,
                               std::int64_t moment_ms, std::uint64_t item_id) noexcept;
  // [render-thread] Frees the current clip: releases its frames now, closes the
  // media_source (which joins its threads) on a worker, never here.
  void retire_media() noexcept;
  // Latched transport from the UI thread: video AND animated-still playback
  // share this (docs/design/16 "Video" -- Space/,/. mean the same thing on either;
  // key_router.cpp maps both item_kind::clip and item_kind::animation to
  // mode::video for exactly this reason). Returns true when it changed what
  // should be on screen, so the caller redraws.
  bool apply_playback_input(const input_snapshot& snapshot) noexcept;
  void update_video_status() noexcept;
  // The picture on the canvas, still or video frame. False when there is none.
  [[nodiscard]] bool picture_size(float* w, float* h) const noexcept;
  // [render-thread] PR 9 overlays: the info lines, AF quads and eyedropper. All
  // three draw bytes the UI thread already published in `snapshot.meta`; none of
  // them touches a file or the metadata store (docs/design/16).
  void draw_photo_overlays(const input_snapshot& snapshot) noexcept;
  // [render-thread] PR 10 edit geometry. The snapshot's slot for `item`, or
  // null; the placement of `img` through it (identity when there is none).
  [[nodiscard]] const edit_view* edit_for(std::uint64_t item) const noexcept;
  [[nodiscard]] edit::placement place_image(const image::gpu_image_mac& img) const noexcept;
  void draw_crop_overlay(const input_snapshot& snapshot) noexcept;
  // PR 11 working texture: one-way hand-offs (worker -> render thread) the
  // same shape as pending_image_; `working_` is render-thread only.
  struct working_texture_mac {
    image::gpu_image_mac image;
    std::uint64_t item = 0;
  };
  std::atomic<working_texture_mac*> pending_working_{nullptr};
  std::atomic<bool> drop_working_{false};
  std::unique_ptr<working_texture_mac> working_;
  bool take_working() noexcept;  // render thread: true when what it draws changed
  edit_view edit_slots_[2];      // copied from the snapshot each iteration
  edit_view applied_edit_{};     // what the current still was last fitted with
  std::atomic<std::uint64_t> shown_item_{0};
  std::atomic<std::uint32_t> shown_w_{0};
  std::atomic<std::uint32_t> shown_h_{0};
 public:
  // [any-thread] What the eyedropper last read, ready for the clipboard; empty when
  // the cursor is off the picture or the eyedropper is off.
  [[nodiscard]] std::string eyedropper_text() const {
    std::lock_guard<std::mutex> lock(eye_mutex_);
    return eye_text_;
  }

 private:

  // Eyedropper: the last texel read, so an idle cursor costs no readback.
  struct eyedropper_sample {
    const void* texture = nullptr;
    std::uint32_t x = 0, y = 0;
    std::uint8_t rgba[4] = {};
    bool valid = false;
  };
  eyedropper_sample eye_;
  mutable std::mutex eye_mutex_;
  std::string eye_text_;  // "#RRGGBB  rgb(r, g, b)  x, y" for the texel under the cursor

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
  // close_item(): every item id up to this one is off screen for good. The
  // render thread drops what it holds (and a late load) once it sees it move.
  std::atomic<std::uint64_t> cleared_through_{0};
  std::uint64_t cleared_applied_ = 0;  // render thread
  std::unique_ptr<image::gpu_image_mac> current_image_;
  std::unique_ptr<image::gpu_image_mac> fade_from_;
  canvas::crossfade fade_;
  std::atomic<std::uint32_t> stills_shown_{0};

  // PR 19 video. The render thread owns `media_` and `video_frame_`; a worker
  // opens the clip and posts it here, the same one-way handoff as
  // pending_image_. `item` ties it to the open_item() that asked for it.
  struct pending_media {
    player::media_source* source = nullptr;
    std::uint64_t item = 0;
    // Milestone H: >= 0 opens the clip paused on this moment (open_item's
    // moment_ms), the Mac twin of the core's open_video_worker seek.
    std::int64_t moment_ms = -1;
  };
  gfx::video_blitter_mac video_blitter_;
  std::atomic<pending_media*> pending_media_{nullptr};
  std::atomic<std::uint64_t> video_opening_{0};  // item id being opened, 0 = none
  // The item id of the clip open_item() last opened (0 after a still): a
  // preview image carrying it is that clip's placeholder, not a still.
  std::atomic<std::uint64_t> clip_item_{0};
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
  // Issue #44: the snapshot's video_hold, applied on this thread (the Windows
  // twin is abi/video_session). Keyed by item id, which only moves forward.
  player::playback_hold hold_;
  std::int64_t seen_anim_steps_ = 0;
  std::int64_t seen_video_skip_ = 0;
  std::int32_t seen_video_speed_ = 0;
  std::uint32_t seen_video_mute_ = 0;
  bool video_muted_ = false;
  std::int32_t seen_video_volume_ = 0;
  std::uint32_t seen_video_volume_set_ = 0;
  float video_volume_ = 1.0f;  // survives from clip to clip
  std::uint32_t seen_video_seek_ = 0;
  std::uint32_t seen_video_loop_ = 0;  // PR 13

  // Animated GIF/APNG/WebP playback (docs/design/04, folded into PR 18's "animated
  // GIF/APNG/WebP on the display-link frame clock", docs/design/15). Frame 0 already
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
  std::atomic<std::int64_t> vs_pos_ns_{0};
  std::atomic<std::int64_t> vs_dur_ms_{0};
  std::atomic<int> vs_rate_x100_{100};
  std::atomic<float> vs_volume_{1.0f};
  std::atomic<std::uint64_t> vs_item_{0};

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
  // Render-thread seconds at which the current item's first pixel and its
  // full-resolution image landed (-1 until they do); the soak report's
  // still_first_pixel_s / still_full_s. Render thread only.
  double first_pixel_seconds_ = -1.0;
  double full_seconds_ = -1.0;
  bool soak_complete_ = false;
  std::uint64_t total_presents_ = 0;
  bool was_presenting_ = false;
  // docs/design/17 "Yield policy": the last dropped frame holds the busy signal for
  // background add-on work (the AI indexer) for two seconds.
  std::uint64_t busy_drops_seen_ = 0;
  double busy_drop_at_ = -1.0e9;
  bool painted_static_ = false;
  // A frame asked for while presenting was not allowed (the window not key, a
  // search panel over it): owed once it is, or the new picture waits for input.
  bool redraw_owed_ = false;
  input_cursor input_cursor_;
  gfx::metal_idle_stats idle_stats_;
  double measurement_start_seconds_ = 0.0;
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
  std::uint8_t seen_background_ = 255;
  std::uint32_t seen_home_background_rgb_ = 0xFFFFFFFFu;
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
