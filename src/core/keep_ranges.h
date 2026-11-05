// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Video Editor's cut applied to a clip's frames and sound (Enhance in the
// editor, the host table's frames_keep / sound_keep rows, ABI 0.35).
//
// The edit is a list of kept source ranges, (in, out) in nanoseconds on the
// player's timeline, in source order (shell/video_timeline's pieces). What
// falls outside is dropped; what is kept is put on the edit's own timeline:
// the first range starts at 0 and each next one follows the last, so a frame's
// pts and a sample's position are the program's. A frame is kept when
// in <= pts < out; a sample likewise, by its own time.
//
// Portable, no I/O (D9).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace mv {

class keep_ranges {
 public:
  // false (and nothing kept) for an odd count, a range that is empty or
  // negative, or ranges out of order.
  [[nodiscard]] bool set(std::span<const std::int64_t> pairs_ns) {
    ranges_.clear();
    if (pairs_ns.size() % 2 != 0) return false;
    std::int64_t program = 0;
    for (std::size_t i = 0; i < pairs_ns.size(); i += 2) {
      const std::int64_t in = pairs_ns[i], out = pairs_ns[i + 1];
      if (in < 0 || out <= in || (!ranges_.empty() && in < ranges_.back().out)) {
        ranges_.clear();
        return false;
      }
      ranges_.push_back({in, out, program});
      program += out - in;
    }
    return true;
  }

  [[nodiscard]] bool active() const noexcept { return !ranges_.empty(); }
  // The program's length: every kept range together.
  [[nodiscard]] std::int64_t kept_ns() const noexcept {
    return ranges_.empty() ? 0 : ranges_.back().program + (ranges_.back().out - ranges_.back().in);
  }
  // Past the last range: nothing more is kept (a reader may stop there).
  [[nodiscard]] bool past_end(std::int64_t source_ns) const noexcept {
    return !ranges_.empty() && source_ns >= ranges_.back().out;
  }

  // A frame at `source_ns`: its time on the program, or nullopt when it is cut.
  [[nodiscard]] std::optional<std::int64_t> program_of(std::int64_t source_ns) const noexcept {
    for (const range& r : ranges_) {
      if (source_ns < r.in) return std::nullopt;
      if (source_ns < r.out) return r.program + (source_ns - r.in);
    }
    return std::nullopt;
  }

  // Sound: `pcm` holds whole frames of `channels` samples, the first at
  // sample `first` of a stream whose sample 0 is at `base_ns`. Appends to
  // `out` the frames that are kept. The first time it is called (first == 0)
  // and the sound starts after the first range does, the gap is silence, so
  // the sound lines up with the program's frames.
  void keep_sound(std::span<const float> pcm, std::uint64_t first, std::int64_t base_ns, std::uint32_t rate,
                  std::uint32_t channels, std::vector<float>& out) const {
    if (channels == 0 || rate == 0) return;
    const std::uint64_t frames = pcm.size() / channels;
    if (first == 0 && !ranges_.empty() && base_ns > ranges_.front().in) {
      const std::int64_t gap = std::min(base_ns, ranges_.front().out) - ranges_.front().in;
      out.insert(out.end(), static_cast<std::size_t>(frame_at(gap, rate)) * channels, 0.0f);
    }
    for (const range& r : ranges_) {
      // Sample k's time is base + k / rate: the first sample at or after each edge.
      const std::int64_t a = std::max<std::int64_t>(0, frame_at(r.in - base_ns, rate));
      const std::int64_t b = std::max<std::int64_t>(0, frame_at(r.out - base_ns, rate));
      const std::uint64_t lo = std::max<std::uint64_t>(first, static_cast<std::uint64_t>(a));
      const std::uint64_t hi = std::min<std::uint64_t>(first + frames, static_cast<std::uint64_t>(b));
      if (lo >= hi) continue;
      const auto from = pcm.begin() + static_cast<std::ptrdiff_t>((lo - first) * channels);
      out.insert(out.end(), from, from + static_cast<std::ptrdiff_t>((hi - lo) * channels));
    }
  }

 private:
  struct range {
    std::int64_t in, out, program;
  };
  // The first sample index at or after `t_ns` from sample 0 (ceil), or
  // negative before it.
  static std::int64_t frame_at(std::int64_t t_ns, std::uint32_t rate) noexcept {
    // Whole seconds and the rest apart, so a long clip cannot overflow (no
    // 128-bit integer on MSVC).
    constexpr std::int64_t kSec = 1'000'000'000;
    std::int64_t s = t_ns / kSec, rest = t_ns % kSec;
    if (rest < 0) {
      rest += kSec;
      --s;
    }
    const std::int64_t part = rest * rate;  // < 1e9 x 192000
    return s * rate + part / kSec + (part % kSec != 0 ? 1 : 0);
  }
  std::vector<range> ranges_;
};

}  // namespace mv
