// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/crash_context.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace mv::crash_context {
namespace {

std::array<char, kSlotCount * kSlotBytes> g_slots{};
std::uint64_t g_last_call = 0;
std::atomic<std::size_t> g_next_slot{0};

thread_local std::size_t t_slot = static_cast<std::size_t>(-1);
thread_local std::uint64_t t_correlation = 0;
thread_local decode_info t_info{};

std::size_t my_slot() noexcept {
  if (t_slot == static_cast<std::size_t>(-1)) {
    t_slot = g_next_slot.fetch_add(1, std::memory_order_relaxed) % kSlotCount;
  }
  return t_slot;
}

char* slot_ptr(std::size_t i) noexcept { return g_slots.data() + i * kSlotBytes; }

void write_slot(std::uint32_t w, std::uint32_t h, std::uint32_t bits) noexcept {
  char* s = slot_ptr(my_slot());
  char tmp[kSlotBytes]{};
  // Whitelisted fields only (plan/13). Literals and integers.
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
  write_slot(0, 0, 0);
  return my_slot();
}

void note_geometry(std::uint32_t width, std::uint32_t height, std::uint32_t bit_depth) noexcept {
  write_slot(width, height, bit_depth);
}

void end_decode() noexcept { std::memset(slot_ptr(my_slot()), 0, kSlotBytes); }

void note_call(std::uint64_t correlation_id) noexcept { g_last_call = correlation_id; }

void set_thread_correlation(std::uint64_t correlation_id) noexcept {
  t_correlation = correlation_id;
}
std::uint64_t thread_correlation() noexcept { return t_correlation; }

std::span<char, kSlotCount * kSlotBytes> slots() noexcept {
  return std::span<char, kSlotCount * kSlotBytes>(g_slots);
}
const std::uint64_t* last_call_address() noexcept { return &g_last_call; }

const char* this_thread_slot() noexcept { return slot_ptr(my_slot()); }

}  // namespace mv::crash_context
