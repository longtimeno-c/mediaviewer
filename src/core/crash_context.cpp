// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/crash_context.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace mv::crash_context {
namespace {

std::array<char, kSlotCount * kSlotBytes> g_slots{};
std::uint64_t g_last_call = 0;
std::array<std::atomic<bool>, kSlotCount> g_claimed{};
std::atomic<std::size_t> g_next_slot{0};

thread_local std::size_t t_slot = kSlotCount;  // kSlotCount = none held
thread_local std::uint64_t t_correlation = 0;
thread_local decode_info t_info{};

// First free slot from a rotating start, so one slot is not always the hot one.
std::size_t claim_slot() noexcept {
  const std::size_t start = g_next_slot.fetch_add(1, std::memory_order_relaxed);
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    const std::size_t slot = (start + i) % kSlotCount;
    if (!g_claimed[slot].exchange(true, std::memory_order_acquire)) return slot;
  }
  return kSlotCount;
}

char* slot_ptr(std::size_t i) noexcept { return g_slots.data() + i * kSlotBytes; }

void write_slot(std::uint32_t w, std::uint32_t h, std::uint32_t bits) noexcept {
  if (t_slot >= kSlotCount) return;
  char* s = slot_ptr(t_slot);
  char tmp[kSlotBytes]{};
  // Whitelisted fields only (docs/design/13). Literals and integers.
  if (w != 0 || h != 0) {
    (void)std::snprintf(tmp, sizeof(tmp), "fmt=%s dec=%s/%s cid=%llu %ux%u %ubit", t_info.family,
                        t_info.decoder, t_info.version,
                        static_cast<unsigned long long>(t_info.correlation_id), w, h, bits);
  } else {
    (void)std::snprintf(tmp, sizeof(tmp), "fmt=%s dec=%s/%s cid=%llu", t_info.family,
                        t_info.decoder, t_info.version,
                        static_cast<unsigned long long>(t_info.correlation_id));
  }
  std::memcpy(s, tmp, kSlotBytes);
}

}  // namespace

std::size_t begin_decode(const decode_info& info) noexcept {
  t_info = info;
  if (t_info.correlation_id == 0) t_info.correlation_id = t_correlation;
  if (t_slot >= kSlotCount) t_slot = claim_slot();
  write_slot(0, 0, 0);
  return t_slot;
}

void note_geometry(std::uint32_t width, std::uint32_t height, std::uint32_t bit_depth) noexcept {
  write_slot(width, height, bit_depth);
}

void end_decode() noexcept {
  if (t_slot >= kSlotCount) return;
  std::memset(slot_ptr(t_slot), 0, kSlotBytes);
  g_claimed[t_slot].store(false, std::memory_order_release);
  t_slot = kSlotCount;
}

void note_call(std::uint64_t correlation_id) noexcept { g_last_call = correlation_id; }

void set_thread_correlation(std::uint64_t correlation_id) noexcept {
  t_correlation = correlation_id;
}
std::uint64_t thread_correlation() noexcept { return t_correlation; }

std::span<char, kSlotCount * kSlotBytes> slots() noexcept {
  return std::span<char, kSlotCount * kSlotBytes>(g_slots);
}
const std::uint64_t* last_call_address() noexcept { return &g_last_call; }

const char* this_thread_slot() noexcept { return t_slot < kSlotCount ? slot_ptr(t_slot) : ""; }

}  // namespace mv::crash_context
