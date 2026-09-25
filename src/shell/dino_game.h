// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The empty-window runner: Space on the welcome screen. A small easter egg, and
// the only thing that presents continuously on an empty canvas (it stops the
// moment the game is left, like any other animation).
//
// This file is the game's rules and nothing else: no ImGui, no platform types,
// no clock. The host feeds it `dt` and presses; `dino_draw.h` draws it. Units
// are sprite pixels, ground at y = 0, up positive, so the maths is the same at
// any DPI and can be unit tested (tests/test_dino_game.cpp).
#pragma once

#include <algorithm>
#include <cstdint>

namespace mv::shell {

class dino_game {
 public:
  enum class phase : std::uint8_t { idle, intro, playing, over, outro };

  static constexpr float kDinoW = 20.0f;
  static constexpr float kDinoH = 21.0f;
  static constexpr float kIntroSeconds = 1.25f;
  static constexpr float kOutroSeconds = 0.9f;
  static constexpr int kMaxObstacles = 8;

  struct obstacle {
    float x = 0.0f;  // left edge, world units
    float w = 0.0f;
    float h = 0.0f;
  };

  explicit dino_game(std::uint32_t seed = 0x9E3779B9u) noexcept : rng_(seed ? seed : 1u) {}

  // Space on the welcome screen: start the intro. Any later press is a jump.
  void press() noexcept {
    switch (phase_) {
      case phase::idle: start(); break;
      case phase::intro: break;  // the run has not begun; ignore
      case phase::playing: jump(); break;
      case phase::over: start(); break;
      case phase::outro:  // Space again on the way out: a fresh run, in the default view
        view_3d_ = false;
        start();
        break;
    }
  }

  // Esc: the runner sprints off and the scene folds away before the welcome card
  // returns (the intro in reverse). The best score survives.
  void leave() noexcept {
    if (phase_ == phase::idle || phase_ == phase::outro) return;
    phase_ = phase::outro;
    t_ = 0.0f;
  }

  // Straight back to idle, no outro: a file opened over the runner.
  void leave_now() noexcept {
    phase_ = phase::idle;
    count_ = 0;
    view_3d_ = false;
  }

  // Presentation only: switching cameras never resets the run or changes physics.
  void toggle_3d() noexcept { if (active() && phase_ != phase::outro) view_3d_ = !view_3d_; }
  bool view_3d() const noexcept { return view_3d_; }

  void update(float dt) noexcept {
    dt = std::clamp(dt, 0.0f, 0.05f);  // a stalled frame must not teleport the dino
    switch (phase_) {
      case phase::idle:
      case phase::over: return;
      case phase::intro:
        t_ += dt;
        run_clock_ += dt * 9.0f;
        if (t_ >= kIntroSeconds) begin_run();
        return;
      case phase::outro:
        // The world stops; the runner lands any jump and keeps running off.
        t_ += dt;
        run_clock_ += dt * 11.0f;
        if (airborne_) fall(dt);
        if (t_ >= kOutroSeconds) leave_now();
        return;
      case phase::playing: break;
    }

    t_ += dt;
    run_clock_ += dt * (7.0f + speed_ * 0.03f);
    speed_ = std::min(kMaxSpeed, speed_ + dt * 4.0f);
    distance_ += speed_ * dt;

    if (airborne_) fall(dt);

    // Obstacles scroll left; drop the ones that left the screen.
    int w = 0;
    for (int i = 0; i < count_; ++i) {
      obstacles_[i].x -= speed_ * dt;
      if (obstacles_[i].x + obstacles_[i].w > -40.0f) obstacles_[w++] = obstacles_[i];
    }
    count_ = w;

    next_spawn_in_ -= speed_ * dt;
    if (next_spawn_in_ <= 0.0f && count_ < kMaxObstacles) spawn();

    if (hits()) {
      phase_ = phase::over;
      best_ = std::max(best_, score());
    }
  }

  // The view width in world units, so the host can tell the game where the right
  // edge is (obstacles spawn just past it).
  void set_view_width(float units) noexcept { view_w_ = std::max(units, 120.0f); }

  phase state() const noexcept { return phase_; }
  bool active() const noexcept { return phase_ != phase::idle; }
  float intro_progress() const noexcept {
    return phase_ == phase::intro ? std::clamp(t_ / kIntroSeconds, 0.0f, 1.0f) : 1.0f;
  }
  float outro_progress() const noexcept {
    return phase_ == phase::outro ? std::clamp(t_ / kOutroSeconds, 0.0f, 1.0f) : 0.0f;
  }
  float dino_x() const noexcept { return kDinoX; }
  float dino_y() const noexcept { return y_; }
  bool airborne() const noexcept { return airborne_; }
  float run_clock() const noexcept { return run_clock_; }
  float speed() const noexcept { return speed_; }
  float distance() const noexcept { return distance_; }
  int score() const noexcept { return static_cast<int>(distance_ / 8.0f); }
  int best() const noexcept { return best_; }
  int obstacle_count() const noexcept { return count_; }
  const obstacle& obstacle_at(int i) const noexcept { return obstacles_[i]; }
  float seconds_in_phase() const noexcept { return t_; }

  // Test hooks: place an obstacle and step exactly, without the RNG.
  void debug_clear_obstacles() noexcept { count_ = 0; }
  void debug_add_obstacle(float x, float w, float h) noexcept {
    if (count_ < kMaxObstacles) obstacles_[count_++] = {x, w, h};
  }
  void debug_begin_run() noexcept { begin_run(); }

 private:
  static constexpr float kGravity = 930.0f;
  static constexpr float kJumpV = 280.0f;
  static constexpr float kStartSpeed = 150.0f;
  static constexpr float kMaxSpeed = 340.0f;
  static constexpr float kDinoX = 28.0f;

  std::uint32_t next() noexcept {  // xorshift32; the game is deterministic per seed
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
  }
  float rand01() noexcept { return static_cast<float>(next() & 0xFFFFu) / 65535.0f; }

  void start() noexcept {
    phase_ = phase::intro;
    t_ = 0.0f;
    run_clock_ = 0.0f;
    y_ = 0.0f;
    vy_ = 0.0f;
    airborne_ = false;
    speed_ = kStartSpeed;
    distance_ = 0.0f;
    count_ = 0;
  }

  void begin_run() noexcept {
    phase_ = phase::playing;
    t_ = 0.0f;
    next_spawn_in_ = view_w_ * 0.9f;  // the first cactus is a moment away
  }

  // Jump arc.
  void fall(float dt) noexcept {
    vy_ -= kGravity * dt;
    y_ += vy_ * dt;
    if (y_ <= 0.0f) {
      y_ = 0.0f;
      vy_ = 0.0f;
      airborne_ = false;
    }
  }

  void jump() noexcept {
    if (airborne_) return;
    airborne_ = true;
    vy_ = kJumpV;
  }

  void spawn() noexcept {
    // A cluster of one to three, with a beatable gap: the air distance of a jump
    // at the current speed plus room to land and turn around.
    const int n = 1 + static_cast<int>(next() % 3u);
    float x = view_w_ + 8.0f;
    for (int i = 0; i < n && count_ < kMaxObstacles; ++i) {
      const float w = 6.0f + rand01() * 4.0f;
      const float h = 12.0f + rand01() * 10.0f;
      obstacles_[count_++] = {x, w, h};
      x += w + 7.0f;
    }
    const float air = speed_ * (2.0f * kJumpV / kGravity);
    next_spawn_in_ = std::max(60.0f, air * 1.25f + rand01() * speed_ * 0.55f) + (x - view_w_);
  }

  bool hits() const noexcept {
    // A hitbox inside the sprite, so a grazing pass is forgiven.
    const float bx0 = kDinoX + 4.0f, bx1 = kDinoX + kDinoW - 4.0f;
    const float by0 = y_ + 1.0f, by1 = y_ + kDinoH - 4.0f;
    for (int i = 0; i < count_; ++i) {
      const obstacle& o = obstacles_[i];
      const float ox0 = o.x + 1.0f, ox1 = o.x + o.w - 1.0f;
      if (bx1 > ox0 && bx0 < ox1 && by0 < o.h - 1.0f && by1 > 0.0f) return true;
    }
    return false;
  }

  phase phase_ = phase::idle;
  bool view_3d_ = false;
  std::uint32_t rng_;
  float t_ = 0.0f;
  float run_clock_ = 0.0f;
  float y_ = 0.0f;
  float vy_ = 0.0f;
  bool airborne_ = false;
  float speed_ = kStartSpeed;
  float distance_ = 0.0f;
  float next_spawn_in_ = 0.0f;
  float view_w_ = 400.0f;
  int best_ = 0;
  int count_ = 0;
  obstacle obstacles_[kMaxObstacles] = {};
};

}  // namespace mv::shell
