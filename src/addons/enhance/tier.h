// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's performance level (owner 2026-11-04: "like Local search's"):
// Potato, Intel, Default, Maxxed, or Automatic (recommended from this
// computer, by the rule Local search uses: addons/ai/machine.cpp). A level
// bounds what a run may use: threads, Restyle's steps and its largest quality.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace mv::enhance {

enum class tier : std::uint8_t { potato, intel, standard, maxxed };

struct tier_policy {
  int threads = 0;          // intra-op threads for the CPU models; 0 the default
  int restyle_steps = 20;   // diffusion steps
  int max_quality = 1;      // Restyle: 0 Standard, 1 High, 2 Best
};

[[nodiscard]] inline const char* tier_name(tier t) noexcept {
  switch (t) {
    case tier::potato: return "potato";
    case tier::intel: return "intel";
    case tier::standard: return "default";
    case tier::maxxed: return "maxxed";
  }
  return "default";
}

[[nodiscard]] inline const char* tier_words(tier t) noexcept {
  switch (t) {
    case tier::potato: return "Potato";
    case tier::intel: return "Intel";
    case tier::standard: return "Default";
    case tier::maxxed: return "Maxxed";
  }
  return "Default";
}

// "auto" (or anything else) is nullopt-like: the recommendation.
[[nodiscard]] inline bool tier_from(std::string_view s, tier& out) noexcept {
  for (const tier t : {tier::potato, tier::intel, tier::standard, tier::maxxed}) {
    if (s == tier_name(t)) {
      out = t;
      return true;
    }
  }
  return false;
}

// Local search's rule: two cores or 8 GB or less, Potato; an Intel Mac or a PC
// with no graphics accelerator, Intel; an accelerator with 32 GB, Maxxed.
[[nodiscard]] inline tier recommend_tier(unsigned cores, std::uint64_t ram, bool apple_silicon) noexcept {
  constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;
  if ((cores > 0 && cores <= 2) || (ram > 0 && ram <= 8 * kGiB + kGiB / 2)) return tier::potato;
  if (!apple_silicon) return tier::intel;
  if (ram >= 32 * kGiB - kGiB) return tier::maxxed;
  return tier::standard;
}

[[nodiscard]] inline tier_policy policy_of(tier t) noexcept {
  const int cores = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
  switch (t) {
    case tier::potato: return {2, 12, 0};
    case tier::intel: return {std::max(2, cores / 2), 16, 0};
    case tier::standard: return {cores - 1, 20, 1};
    case tier::maxxed: return {cores, 30, 2};
  }
  return {};
}

}  // namespace mv::enhance
