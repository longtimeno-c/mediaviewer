// SPDX-License-Identifier: GPL-2.0-or-later
// The unit of audio handed from the decode thread to the audio thread.
//
// core/spsc_ring.h static_asserts trivially_copyable and stores T by value, so
// a variable-length buffer cannot go through it. Fixed POD blocks are the
// answer: no allocation on the hot path, no arena bookkeeping, and the ring
// stays exactly the lock-free primitive plan/02 asks for.
#pragma once

#include <cstdint>

#include "player/video_source.h"  // time_ns

namespace mv::player {

// 1024 frames of stereo float32. At 48 kHz that is ~21.3 ms per block; 64 slots
// is ~1.4 s of audio, inside plan/05's "bounded, ~2 s" queue budget, for 512 KB
// total. Multichannel content fits fewer frames per block, which is correct
// rather than a bug: the block carries its own frame count.
inline constexpr std::uint32_t audio_block_samples = 2048;
inline constexpr std::uint32_t audio_ring_slots    = 64;

struct audio_block {
  time_ns       pts_ns   = 0;   // stream-relative, start_time already subtracted
  std::uint32_t frames   = 0;   // valid frames, not samples
  std::uint32_t channels = 0;
  std::uint32_t generation = 0; // discarded on seek, same rule as video
  std::uint32_t reserved0  = 0;
  // Interleaved float32, post-swresample. frames * channels <= audio_block_samples.
  float samples[audio_block_samples] = {};
};

static_assert(sizeof(audio_block) <= 8256, "audio_block grew; check the ring budget");

}  // namespace mv::player
