// SPDX-License-Identifier: GPL-2.0-or-later
// Metal present lab — PR 16's instrument, kept as a debug harness the way
// the Win32 lab is. No SwiftUI. plan/10, plan/15.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "core/result.h"
#include "core/spsc_ring.h"
#include "gfx/device_mac.h"
#include "gfx/metal_layer.h"
#include "gfx/metal_pacer.h"
#include "gfx/present_policy.h"
#include "shell/input_state.h"

namespace mv::shell {

struct mac_lab_options {
  double soak_seconds = 0.0;
  std::string json_report_path;
  bool gate_exit_code = false;
  bool start_animating = false;
  bool overlay_visible = true;
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

  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
  }
  [[nodiscard]] int exit_code() const noexcept { return exit_code_; }

 private:
  void render_thread_main() noexcept;
  bool write_json_report() const noexcept;

  void* view_ = nullptr;
  void* display_link_ = nullptr;
  void* link_target_ = nullptr;
  mac_lab_options options_{};

  gfx::metal_device device_;
  gfx::metal_layer layer_;
  gfx::metal_pacer pacer_;

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
};

}  // namespace mv::shell
