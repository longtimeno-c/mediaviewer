// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What a crash report is allowed to say about the work in flight.
//
// plan/13 Part 2, "Scrubbing": attach the format, codec, dimensions, bit depth
// and decoder version — never the path, filename, bytes or EXIF. This block is
// the ONLY app-supplied content of a minidump. The host (shell/) registers the
// slots with its crash reporter as fixed-size annotations; the core never
// includes a crash-reporter header and never knows one exists (D9).
//
// Layout: a fixed table of text slots, one per worker (wrapping), plus the
// correlation id of the last ABI call. Crashpad annotations are process-global,
// so per-thread state is modelled as "slot claimed by this thread". Writes are
// unsynchronised on purpose: a torn slot in a dump is a diagnostic nuisance, a
// lock on the decode path is a pacing bug.
//
// Every string written here is built from compile-time literals and integers.
// There is deliberately no API that takes a `const char*` from a caller.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mv::crash_context {

inline constexpr std::size_t kSlotCount = 8;
inline constexpr std::size_t kSlotBytes = 128;

struct decode_info {
  const char* family = "unknown";   // literal: codec::format_name()
  const char* decoder = "unknown";  // literal: "libjpeg-turbo", "libraw", ...
  const char* version = "";         // literal or library-static string
  std::uint64_t correlation_id = 0;
};

// [any-thread] Marks this thread's slot as decoding. Returns the slot index.
std::size_t begin_decode(const decode_info& info) noexcept;
// [any-thread] Decoders that know the geometry may add it; optional.
void note_geometry(std::uint32_t width, std::uint32_t height, std::uint32_t bit_depth) noexcept;
// [any-thread] Clears this thread's slot.
void end_decode() noexcept;

// [any-thread] The ABI guard stamps every call. Process-wide "last call".
void note_call(std::uint64_t correlation_id) noexcept;

// [any-thread] The correlation id the decode job was submitted under, for the
// thread running it. 0 = none. Set by the ABI's job lambdas.
void set_thread_correlation(std::uint64_t correlation_id) noexcept;
[[nodiscard]] std::uint64_t thread_correlation() noexcept;

// Host registration. Stable addresses for the process lifetime.
[[nodiscard]] std::span<char, kSlotCount * kSlotBytes> slots() noexcept;
[[nodiscard]] const std::uint64_t* last_call_address() noexcept;

// Tests: the current text of this thread's slot (empty when idle).
[[nodiscard]] const char* this_thread_slot() noexcept;

// RAII for the job lambda: set_thread_correlation for the scope.
class correlation_scope {
 public:
  explicit correlation_scope(std::uint64_t id) noexcept : previous_(thread_correlation()) {
    set_thread_correlation(id);
  }
  ~correlation_scope() { set_thread_correlation(previous_); }
  correlation_scope(const correlation_scope&) = delete;
  correlation_scope& operator=(const correlation_scope&) = delete;

 private:
  std::uint64_t previous_;
};

}  // namespace mv::crash_context
