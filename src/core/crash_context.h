// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What a crash report is allowed to say about the work in flight.
//
// docs/design/13 Part 2, "Scrubbing": attach the format, codec, dimensions, bit depth
// and decoder version — never the path, filename, bytes or EXIF. This block is
// the ONLY app-supplied content of a minidump. The host (shell/) registers the
// slots with its crash reporter as fixed-size annotations; the core never
// includes a crash-reporter header and never knows one exists (D9).
//
// Layout: a fixed table of text slots plus the correlation id of the last ABI
// call. Crashpad annotations are process-global, so per-thread state is
// modelled as "slot claimed by this thread": begin_decode claims a free slot
// (one lock-free exchange) and end_decode returns it, so two decodes in flight
// never share a slot however many workers the pool has. A decode that finds
// every slot busy is left unannotated rather than overwrite another's. Slot
// text is unsynchronised on purpose: a torn slot in a dump is a diagnostic
// nuisance, a lock on the decode path is a pacing bug.
//
// Every string written here is built from compile-time literals and integers,
// or names a library keeps in static storage (a libavformat demuxer name, a
// libavcodec codec name: issue #233). There is deliberately no API that takes
// caller-built text: nothing derived from a path or from file bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace mv::crash_context {

inline constexpr std::size_t kSlotCount = 16;
inline constexpr std::size_t kSlotBytes = 128;

struct decode_info {
  const char* family = "unknown";   // literal: codec::format_name()
  const char* decoder = "unknown";  // literal: "libjpeg-turbo", "libraw", ...
  const char* version = "";         // literal or library-static string
  std::uint64_t correlation_id = 0;
  // Literal: which long-lived thread this is ("open", "demux", "video",
  // "audio", "clip"), or null for a still decode. Printed first when set.
  const char* role = nullptr;
};

// [any-thread] Claims a slot for this thread's decode. Returns the slot index,
// or kSlotCount when every slot is busy (the decode runs unannotated).
std::size_t begin_decode(const decode_info& info) noexcept;
// [any-thread] Decoders that know the geometry may add it; optional.
void note_geometry(std::uint32_t width, std::uint32_t height, std::uint32_t bit_depth) noexcept;
// [any-thread] Clears and releases this thread's slot.
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

// A slot for a thread that lives longer than one decode (the player's demux,
// video and audio threads; a clip job; issue #233). It holds a slot while the
// thread works and gives it back while the thread parks, so an idle open clip
// does not keep slots from the still decodes. busy() is one branch while held.
// If the thread already holds a slot (an outer decode), this one leaves it
// alone and annotates nothing. Move it only on the thread that owns it.
class thread_slot {
 public:
  thread_slot() noexcept = default;
  explicit thread_slot(const decode_info& info) noexcept : info_(info) {}
  ~thread_slot() { idle(); }
  thread_slot(thread_slot&& other) noexcept
      : info_(other.info_), width_(other.width_), height_(other.height_), bits_(other.bits_),
        held_(other.held_) {
    other.held_ = false;
  }
  thread_slot& operator=(thread_slot&& other) noexcept {
    if (this != &other) {
      idle();
      info_ = other.info_;
      width_ = other.width_;
      height_ = other.height_;
      bits_ = other.bits_;
      held_ = other.held_;
      other.held_ = false;
    }
    return *this;
  }
  thread_slot(const thread_slot&) = delete;
  thread_slot& operator=(const thread_slot&) = delete;

  // What the slot says: rewritten at once when held, else on the next busy().
  void describe(const decode_info& info, std::uint32_t width = 0, std::uint32_t height = 0,
                std::uint32_t bit_depth = 0) noexcept;
  void busy() noexcept {
    if (!held_) claim();
  }
  void idle() noexcept;
  [[nodiscard]] bool held() const noexcept { return held_; }

 private:
  void claim() noexcept;
  decode_info info_{};
  std::uint32_t width_ = 0, height_ = 0, bits_ = 0;
  bool held_ = false;
};

}  // namespace mv::crash_context
