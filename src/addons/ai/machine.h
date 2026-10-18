// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The model picker's local check (#482 T4, docs/plans/model-tiers.md): what this
// machine is, and which tier it should start on. Computed here and shown in
// the picker; never sent anywhere.
//
//   Potato   <= 2 cores or <= 8 GB of memory
//   Intel    an x86_64 Mac, or a PC with no accelerator (CPU only)
//   Maxxed   an accelerator (Apple silicon, an NVIDIA adapter with CUDA) and >= 32 GB
//   Default  otherwise
//
// After install the provider self-test can still demote a tower (L/14 -> B/32).
#pragma once

#include <cstdint>
#include <string>

namespace mv::ai {

struct machine_info {
  std::string os;    // "macos" | "windows" | "other"
  std::string arch;  // "arm64" | "x86_64" | "other"
  std::uint32_t cores = 0;
  std::uint64_t ram_bytes = 0;
  std::uint64_t free_bytes = 0;  // on the add-on's data volume
  std::string chip;
  bool apple_silicon = false;
  bool nvidia = false;
  bool laptop = false;
  [[nodiscard]] bool accelerator() const noexcept { return apple_silicon || nvidia; }
};

// This machine; `data_dir` is where models would be written (free space).
[[nodiscard]] machine_info probe_machine(const std::string& data_dir_utf8);
[[nodiscard]] std::string recommend_tier(const machine_info& m);
// {"os","arch","cores","ram_bytes","free_bytes","chip","accelerator","laptop","recommended"}
[[nodiscard]] std::string machine_json(const machine_info& m);

}  // namespace mv::ai
