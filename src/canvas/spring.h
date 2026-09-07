// SPDX-License-Identifier: GPL-2.0-or-later
// Critically damped spring. plan/03: ω=18, ζ=1, driven from real QPC dt,
// never a fixed-step tween. Interruption-safe: grabbing mid-settle continues
// from the current position and velocity.
#pragma once

#include <cmath>

namespace mv::canvas {

inline constexpr float kSpringOmega = 18.0f;

// Closed form for ζ=1 (Ryan Juckett). `dt` is clamped so a hitch cannot
// explode the state.
inline void spring_step(float& x, float& v, float target, float dt,
                        float omega = kSpringOmega) noexcept {
  if (dt <= 0.0f) return;
  if (dt > 1.0f / 15.0f) dt = 1.0f / 15.0f;
  const float A = x - target;
  const float B = v + omega * A;
  const float e = std::exp(-omega * dt);
  x = target + (A + B * dt) * e;
  v = (B - omega * (A + B * dt)) * e;
}

inline bool spring_settled(float x, float v, float target, float pos_eps = 0.05f,
                           float vel_eps = 0.5f) noexcept {
  return std::fabs(x - target) < pos_eps && std::fabs(v) < vel_eps;
}

}  // namespace mv::canvas
