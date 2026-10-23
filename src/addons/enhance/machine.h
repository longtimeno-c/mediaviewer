// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance's local check (#297, docs/plans/enhance.md "How a model is chosen"):
// what this machine has, for choose.h's rule. Computed here and shown on the
// Models page; never sent anywhere.
//
// probe() is portable (machine.cpp); the OS facts come from platform_probe(),
// one TU per OS (D9): machine_win.cpp (memory, the NVIDIA adapter and its
// dedicated VRAM over DXGI, its CUDA driver), machine_mac.cpp (memory, the
// Apple silicon chip; its GPU shares RAM) and machine_posix.cpp (memory only:
// the portable build has no accelerator).
#pragma once

#include <cstdint>
#include <string>

namespace mv::enhance {

struct machine {
  std::string os;    // "macos" | "windows" | "other"
  std::string arch;  // "arm64" | "x86_64" | "other"
  std::uint32_t cores = 0;
  std::uint64_t ram_total = 0;
  std::uint64_t ram_free = 0;
  std::string accelerator;  // "apple" | "nvidia" (an adapter with its CUDA driver) | ""
  std::string gpu;          // the adapter's or the chip's name; may be empty
  std::uint64_t vram = 0;   // dedicated (NVIDIA); 0 on Apple silicon: it shares ram_total
  std::uint64_t free_disk = 0;  // on the add-on's data volume
};

// What the OS says (machine_<os>.cpp). Never blocks for long: a DXGI
// enumeration at most.
struct platform_facts {
  std::uint64_t ram_total = 0;
  std::uint64_t ram_free = 0;
  std::string accelerator;
  std::string gpu;
  std::uint64_t vram = 0;
};
[[nodiscard]] platform_facts platform_probe();

// This machine. `data_dir` is where the weights go (free space is read on its
// volume, or on its nearest existing parent's before it exists).
[[nodiscard]] machine probe(const std::string& data_dir_utf8);

// Called once at the top of the download worker: the lowest CPU and I/O
// priority the OS gives a thread (machine_<os>.cpp).
void enter_background() noexcept;

}  // namespace mv::enhance
