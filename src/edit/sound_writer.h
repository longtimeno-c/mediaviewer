// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A new file beside a source, with sound the caller made (Enhance, #297
// slice 4): the host's side of the add-on table's sound_write_* rows.
//
//   An audio file      -> "<stem>-<suffix>.m4a": the new sound alone, AAC.
//   A clip             -> "<stem>-<suffix>.<ext>": the source's picture,
//                         stream-copied (never re-encoded), with the new sound
//                         in place of its own. MP4 and MOV keep their family;
//                         any other container becomes Matroska (.mkv), which
//                         holds every video codec in the D5 set beside AAC.
//
// The sound is encoded with FFmpeg's own AAC encoder (LGPL, docs/design/11).
// Rule 5: the source is opened read-only and never written. The output is a
// hidden `.mvpart` sibling until finish() publishes it under the first free
// name ("name (2)", ...); a writer destroyed before that leaves nothing.
// Rule 6: nothing here logs a path. Worker threads only.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "core/result.h"

namespace mv::edit::clip {

struct sound_out_options {
  std::uint32_t sample_rate = 48000;  // of the PCM handed to write()
  std::uint32_t channels = 2;         // 1..8, interleaved
  std::string suffix = "enhance";     // [a-z0-9_-]{1,32}
};

class sound_writer {
 public:
  ~sound_writer();
  sound_writer(const sound_writer&) = delete;
  sound_writer& operator=(const sound_writer&) = delete;

  // Opens the source (to copy its picture, if it has one) and the hidden
  // output. unsupported_format when no AAC encoder opens at that rate and
  // channel count, or the picture cannot go in the target container.
  [[nodiscard]] static result<std::unique_ptr<sound_writer>> open(std::string_view source_utf8,
                                                                  const sound_out_options& options,
                                                                  const std::atomic<bool>* cancel = nullptr);
  // Whether the output carries the source's picture (a clip).
  [[nodiscard]] bool with_picture() const noexcept;
  // Whole frames of interleaved float PCM in [-1, 1] (clipped), in order.
  [[nodiscard]] expected write(std::span<const float> interleaved);
  // Flushes the encoder, copies the rest of the picture, writes the trailer
  // and publishes. Returns the published path.
  [[nodiscard]] result<std::string> finish();

  struct impl;

 private:
  explicit sound_writer(std::unique_ptr<impl> p);
  std::unique_ptr<impl> p_;
};

}  // namespace mv::edit::clip
