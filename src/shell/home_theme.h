// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The native empty view is drawn by ImGui on both hosts, outside WinUI/SwiftUI.
// POD, no ImGui here: input_state.h carries it (home_ink.h draws with it).
// Its colours travel in the UI-thread snapshot as the chrome's own resolved
// tokens (docs/design/25 §8): nothing here invents a colour from another.
//
// Issue #408 (owner, 2026-10-15): there is no canvas background. `canvas` is
// the clear behind the welcome card, behind a still and behind a video, the
// same colour the chrome paints for the theme in use. The card reads surface,
// title, body, hairline and accent. Default is the system's tokens, and high
// contrast the system's contrast colours: the chrome resolves both, so this
// header never asks which one is on.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mv::shell {

// The ground effects a built-in theme may name (BuiltinThemes.swift / .cs,
// "Effects"). Not a token: an add-on theme has none, and high contrast clears
// it with the theme.
enum class home_effect : std::uint8_t {
  none = 0,
  dots,
  rain,
  starfield,
  fireflies,
  petals,
  synapse,
  embers,
  sparkles,
  perlin_flow,
  count,
};

// The chromes' names, in the order above. -1 for a name not in the table.
[[nodiscard]] inline int home_effect_from_name(const char* name) noexcept {
  static constexpr const char* kNames[] = {"none",    "dots",   "rain",     "starfield",
                                           "fireflies", "petals", "synapse", "embers",
                                           "sparkles", "perlin-flow"};
  if (name == nullptr) return -1;
  for (int i = 0; i < static_cast<int>(home_effect::count); ++i) {
    const char* a = kNames[i];
    const char* b = name;
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*a == '\0' && *b == '\0') return i;
  }
  return -1;
}

[[nodiscard]] constexpr bool home_effect_moves(home_effect e) noexcept {
  return e != home_effect::none && e != home_effect::dots && e < home_effect::count;
}

// POD, copied with the snapshot. Colours are 0xRRGGBBAA (sRGB, straight
// alpha). The defaults are the Windows chrome's dark Default, so a snapshot
// published before the chrome has spoken still reads as the app.
struct home_theme {
  std::uint32_t canvas = 0x21232AFFu;
  std::uint32_t surface = 0x30323AFFu;
  std::uint32_t title = 0xEBECF0FFu;
  std::uint32_t body = 0xB1B5BFFFu;
  std::uint32_t hairline = 0x4E515BFFu;
  std::uint32_t accent = 0x60CDFFFFu;
  // The built-in's ground (BuiltinThemes "effect" columns): its ink, and an
  // intensity, a speed and a size in 0.01 .. 1.
  home_effect effect = home_effect::none;
  std::uint32_t effect_rgb = 0xFFFFFFu;
  float intensity = 1.0f;
  float speed = 1.0f;
  float size = 1.0f;

  friend bool operator==(const home_theme&, const home_theme&) = default;

  // Light or dark ground, for the runner's own art (dino_draw*.h), which is a
  // game and keeps its two palettes.
  [[nodiscard]] bool light() const noexcept {
    return (((canvas >> 24) & 255u) * 299u + ((canvas >> 16) & 255u) * 587u +
            ((canvas >> 8) & 255u) * 114u) > 128000u;
  }
};

// The ground effect's motion rule (home_ground.h draws it) as a pure clock, so
// both labs share it and a test can drive it. The chromes' panes run the same
// rule (ThemeField.swift / ThemeField.cs ThemeMotion). Owner, 2026-10-15:
//   - 30 frames a second while the app is in front and the pointer or a key
//     moved in the last 5 seconds;
//   - otherwise 10 a second while the window is visible (behind another app,
//     or in front but quiet), for up to 60 seconds after the last input;
//   - then it rests, and never moves while hidden, minimised or covered, or
//     under Reduce Motion / Windows animations off.
// Input is ours only: the app cannot see the pointer in other apps, so a
// window left behind another app rests a minute after you leave it.
// Times are the render thread's own seconds.
struct home_ground_clock {
  static constexpr double kFastTick = 1.0 / 30.0;
  static constexpr double kSlowTick = 0.1;
  static constexpr double kFastWindow = 5.0;   // input this recent, in front: fast
  static constexpr double kSlowWindow = 60.0;  // input this recent, visible: slow
  static constexpr double kMaxStep = 0.1;      // a late tick never jumps further
  static constexpr double kEarly = 0.002;      // a wait may wake this early

  double time = 0.0;         // the effect's clock; advances only while ticking
  double last_input = -1.0;  // < 0: none yet (the labs record launch as one)
  double last_tick = -1.0;   // < 0: resting

  void input(double now) noexcept { last_input = now; }

  // Seconds between ticks now, or 0 to rest. `home` is the welcome on screen
  // (nothing open, no error, no runner); `active` the app in front; `visible`
  // the window on screen and not covered.
  [[nodiscard]] double interval(bool home, home_effect effect, bool active, bool visible,
                                bool reduce_motion, double now) const noexcept {
    if (!home || !home_effect_moves(effect) || !visible || reduce_motion || last_input < 0.0)
      return 0.0;
    const double quiet = now - last_input;
    if (active && quiet < kFastWindow) return kFastTick;
    return quiet < kSlowWindow ? kSlowTick : 0.0;
  }

  // Called once per loop pass. True when a tick is due: the caller draws one
  // frame. Resting clears the tick so a resume never jumps.
  [[nodiscard]] bool step(double every, double now) noexcept {
    if (every <= 0.0) {
      last_tick = -1.0;
      return false;
    }
    if (last_tick >= 0.0 && now - last_tick < every - kEarly) return false;
    if (last_tick >= 0.0) time += std::min(kMaxStep, now - last_tick);
    last_tick = now;
    return true;
  }

  // Seconds the idle wait may sleep before the next tick (moving only).
  [[nodiscard]] double until_next(double every, double now) const noexcept {
    return last_tick < 0.0 ? 0.0 : std::max(0.0, every - (now - last_tick));
  }
};

// 0xRRGGBB -> 0xRRGGBBFF.
[[nodiscard]] constexpr std::uint32_t home_opaque(std::uint32_t rgb) noexcept {
  return ((rgb & 0xFFFFFFu) << 8) | 0xFFu;
}

// `top` (0xRRGGBBAA, its alpha further scaled by `alpha`) over an opaque
// `bottom`, mixed in sRGB as the chromes' panes blend, and returned opaque.
// The labs blend in linear light on their _SRGB targets, which made a
// translucent mark several times brighter than the same mark in a pane; the
// home's ground and card sit on known solid colours, so they are pre-mixed.
[[nodiscard]] inline std::uint32_t home_over(std::uint32_t top, std::uint32_t bottom,
                                             float alpha = 1.0f) noexcept {
  const float k = std::clamp(alpha, 0.0f, 1.0f) * static_cast<float>(top & 255u) / 255.0f;
  std::uint32_t out = 0xFFu;
  for (const unsigned shift : {24u, 16u, 8u}) {
    const float t = static_cast<float>((top >> shift) & 255u);
    const float b = static_cast<float>((bottom >> shift) & 255u);
    out |= static_cast<std::uint32_t>(std::lround(b + (t - b) * k)) << shift;
  }
  return out;
}

// One channel of a 0xRRGGBBAA token, sRGB-decoded to linear for a clear (the
// render targets are _SRGB and encode on write). `shift` is 24, 16 or 8.
inline float home_linear_channel(std::uint32_t rgba, unsigned shift) noexcept {
  const float c = static_cast<float>((rgba >> shift) & 255u) / 255.0f;
  return c <= 0.04045f ? c / 12.92f
                       : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

}  // namespace mv::shell
