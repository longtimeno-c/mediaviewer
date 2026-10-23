// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/choose.h"

#include <algorithm>

namespace mv::enhance {
namespace {

bool cpu_in_time(const model& m, const kind_entry& k) noexcept {
  return m.cpu_unit_ms && *m.cpu_unit_ms <= k.cpu_budget_ms;
}

// Rules 1-3: what offer() and run_gate() share. Memory is left to the caller.
fit first_three(const model& m, const kind_entry& k, const machine& here) noexcept {
  fit f;
  if (m.files.empty()) {
    f.reason = why::no_files;
    return f;
  }
  if (std::any_of(m.files.begin(), m.files.end(), [](const pinned& p) { return p.size > kMaxWeightsFile; })) {
    f.reason = why::too_large;
    return f;
  }
  if (here.os == "macos" && here.arch == "x86_64") {
    f.reason = why::no_runtime;
    return f;
  }
  f.on = provider_for(m, k, here);
  if (f.on == provider::none) f.reason = why::needs_gpu;
  return f;
}

bool fits_memory(fit& f, std::uint64_t need, std::uint64_t have) noexcept {
  if (need <= have) return true;
  f.reason = why::memory;
  f.need = need;
  f.have = have;
  return false;
}

// Rule 4. A graphics card too small for a model the CPU runs in time hands it
// to the CPU when RAM holds it; otherwise the card's shortfall is the answer.
bool fits_memory_or_cpu(fit& f, const model& m, const kind_entry& k, std::uint64_t vram, std::uint64_t ram) noexcept {
  if (fits_memory(f, m.run_bytes, f.on == provider::cuda ? vram : ram)) return true;
  if (f.on == provider::cuda && cpu_in_time(m, k) && m.run_bytes <= ram) {
    f = fit{};
    f.on = provider::cpu;
    return true;
  }
  return false;
}

}  // namespace

provider provider_for(const model& m, const kind_entry& k, const machine& here) noexcept {
  if (here.accelerator == "nvidia" && m.cuda) return provider::cuda;
  if (here.accelerator == "apple" && m.coreml) return provider::coreml;
  if (cpu_in_time(m, k)) return provider::cpu;
  return provider::none;
}

fit offer(const model& m, const kind_entry& k, const machine& here, std::uint64_t still_to_fetch) noexcept {
  fit f = first_three(m, k, here);
  if (!f.ok()) return f;
  if (!fits_memory_or_cpu(f, m, k, here.vram, here.ram_total)) return f;
  const std::uint64_t need = still_to_fetch + m.cache_bytes;
  if (need > here.free_disk) {
    f.reason = why::disk;
    f.need = need;
    f.have = here.free_disk;
  }
  return f;
}

fit run_gate(const model& m, const kind_entry& k, const machine& here) noexcept {
  fit f = first_three(m, k, here);
  if (!f.ok()) return f;
  (void)fits_memory_or_cpu(f, m, k, here.vram, here.ram_free);
  return f;
}

const model* recommend(const catalog& c, kind k, const machine& here, const fetch_left& still_to_fetch) {
  const kind_entry& entry = c.of(k);
  for (const std::string& id : entry.models) {
    const model* m = c.find(id);
    if (!m || !m->serves(k)) continue;
    const std::uint64_t left = still_to_fetch ? still_to_fetch(*m) : m->download_bytes();
    if (offer(*m, entry, here, left).ok()) return m;
  }
  return nullptr;
}

}  // namespace mv::enhance
