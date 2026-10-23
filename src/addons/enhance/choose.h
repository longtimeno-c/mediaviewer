// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The rule (#297, docs/plans/enhance.md "How a model is chosen"): a model's
// stated needs against this machine, in order.
//
//   1. a pinned file over kMaxWeightsFile (5 GB)    too_large, never fetched
//   2. an x86_64 Mac (no ONNX Runtime build)        no_runtime
//   3. the provider: the machine's accelerator when the model lists it
//      (NVIDIA -> cuda, Apple silicon -> coreml); else the CPU when
//      cpu_unit_ms <= the kind's cpu_budget_ms     needs_gpu otherwise
//   4. memory: run_bytes within the provider's: offer() against the total
//      (CUDA: VRAM; Core ML and CPU: RAM), run_gate() against what is free;
//      a card too small for a model the CPU runs in time hands it to the CPU
//      when RAM holds it                             memory otherwise
//   5. disk (offer() only): the bytes still to fetch plus cache_bytes within
//      the free space                                disk
//
// The recommended model of a kind is the first, largest to smallest, that
// passes all five. Pure functions: no I/O, nothing probed here.
#pragma once

#include <cstdint>
#include <functional>

#include "addons/enhance/catalog.h"
#include "addons/enhance/machine.h"

namespace mv::enhance {

enum class provider : std::uint8_t { none, cpu, cuda, coreml };

enum class why : std::uint8_t {
  fits,
  too_large,   // a pinned file over 5 GB
  no_runtime,  // an Intel Mac
  needs_gpu,   // no accelerator it lists, and too slow on the CPU
  memory,      // need: run_bytes; have: the provider's memory
  disk,        // need: still to fetch + cache_bytes; have: free_disk
  no_files,    // nothing to fetch (never in a parsed catalogue)
};

struct fit {
  why reason = why::fits;
  provider on = provider::none;  // set from rule 3 on
  std::uint64_t need = 0;        // memory / disk: bytes
  std::uint64_t have = 0;
  [[nodiscard]] bool ok() const noexcept { return reason == why::fits; }
};

// Rule 3 alone: where the model would run here; none when nowhere.
[[nodiscard]] provider provider_for(const model& m, const kind_entry& k, const machine& here) noexcept;

// Whether the page may offer Download. `still_to_fetch`: the model's bytes not
// yet here (download_bytes() before any).
[[nodiscard]] fit offer(const model& m, const kind_entry& k, const machine& here,
                        std::uint64_t still_to_fetch) noexcept;

// E3, before the first tile: rules 1-3, then memory against what is free now
// (CUDA: VRAM, until there is a free-VRAM probe; otherwise ram_free). No disk.
[[nodiscard]] fit run_gate(const model& m, const kind_entry& k, const machine& here) noexcept;

// The first model of the kind, in its order, that offer() passes; null when none.
using fetch_left = std::function<std::uint64_t(const model&)>;
[[nodiscard]] const model* recommend(const catalog& c, kind k, const machine& here, const fetch_left& still_to_fetch);

}  // namespace mv::enhance
