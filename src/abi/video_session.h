// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "core/spsc_ring.h"
#include "player/media_source.h"
namespace mv::abi {
// UI and loader only enqueue intent. The render thread owns current and held.
// Retiring a decoder joins its workers on a separate cleanup thread.
class video_session {
 public:
  video_session() : cleaner_([this] { cleanup(); }) {}
  ~video_session() {
    running_.store(false); signal_.fetch_add(1); signal_.notify_one(); cleaner_.join();
    player::close_media(current_); player::close_media(pending_);
    player::media_source* old = nullptr;
    while (retired_.try_pop(old)) player::close_media(old);
  }
  void publish(player::media_source* source, std::uint32_t generation) {
    player::media_source* old;
    { std::lock_guard lock(mutex_); old = pending_; pending_ = source; pending_gen_ = generation; }
    open_.store(source != nullptr, std::memory_order_release);
    player::close_media(old);
  }
  // [any-thread][wait-free] "A clip is on this session", from the moment the
  // loader publishes one until it is retired. The render thread needs this
  // BEFORE the first frame arrives: without it the empty-canvas welcome is
  // still what is on screen, and a lab that has stopped presenting has no
  // reason to paint again.
  [[nodiscard]] bool open() const noexcept { return open_.load(std::memory_order_acquire); }
  void command(std::function<void(player::media_source&)> command) {
    std::lock_guard lock(mutex_); commands_.push_back(std::move(command));
  }
  bool tick(std::uint32_t generation, player::time_ns vblank, player::video_frame& out, bool& active) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { active = active_.load(); return false; }
    if (current_ && current_gen_ != generation) {
      if (!retired_.try_push(current_)) { active = true; return false; }
      current_ = nullptr; held_ = nullptr;
      signal_.fetch_add(1); signal_.notify_one();
      info_ = {}; stats_ = {}; state_ = player::play_state::stopped;
      commands_.clear();
      open_.store(pending_ != nullptr, std::memory_order_release);
    }
    if (!current_ && pending_ && pending_gen_ == generation) {
      current_ = pending_; pending_ = nullptr; current_gen_ = generation;
      current_->play();
    }
    bool changed = false;
    if (current_) {
      for (auto& command : commands_) command(*current_);
      commands_.clear();
      if (auto* frame = vblank >= 0 ? current_->acquire_frame(generation, vblank) : nullptr) {
        if (held_) current_->release_frame(held_);
        held_ = frame; out = *frame; out.generation = generation;
        changed = true;
      }
      info_ = current_->info(); stats_ = current_->stats();
      position_ = current_->position_ns(); state_ = current_->state();
    }
    active = current_ && current_->needs_present();
    active_.store(active);
    open_.store(current_ != nullptr || pending_ != nullptr, std::memory_order_release);
    return changed;
  }
  void snapshot(player::media_info& info, player::clock_stats& stats,
                player::time_ns& position, player::play_state& state) {
    std::lock_guard lock(mutex_); info = info_; stats = stats_; position = position_; state = state_;
  }
 private:
  void cleanup() {
    while (running_.load()) {
      const auto observed = signal_.load();
      player::media_source* old = nullptr;
      while (retired_.try_pop(old)) player::close_media(old);
      if (running_.load()) signal_.wait(observed);
    }
  }
  std::mutex mutex_;
  player::media_source* pending_ = nullptr;
  std::uint32_t pending_gen_ = 0;
  std::vector<std::function<void(player::media_source&)>> commands_;
  player::media_info info_{};
  player::clock_stats stats_{};
  player::time_ns position_ = 0;
  player::play_state state_ = player::play_state::stopped;
  player::media_source* current_ = nullptr;
  player::video_frame* held_ = nullptr;
  std::uint32_t current_gen_ = 0;
  std::atomic<bool> active_{false}, open_{false}, running_{true};
  spsc_ring<player::media_source*, 64> retired_;
  std::atomic<unsigned> signal_{0};
  std::thread cleaner_;
};
} // namespace mv::abi
