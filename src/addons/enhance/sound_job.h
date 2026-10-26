// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Enhance (#297 slice 4): a file's sound through a streaming model, into a
// new file.
//
// The model works on one channel at a time, a fixed frame of samples per call
// with a state it carries from call to call (DeepFilterNet3's streaming
// export: 480 samples at 48 kHz). Its output lags its input by a fixed delay;
// the pipeline drops that many samples at the start and feeds silence at the
// end, so the new file lines up with the original sample for sample.
// `strength` mixes the model's output with the time-aligned original
// (1 = the model alone).
//
// Portable and model-agnostic: the source, the sink and the model are ports,
// so tests drive it with fakes. Runs on the job's own thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/result.h"

namespace mv::enhance {

class frame_model {
 public:
  virtual ~frame_model() = default;
  [[nodiscard]] virtual std::size_t frame() const noexcept = 0;  // samples per call
  [[nodiscard]] virtual std::size_t delay() const noexcept = 0;  // output lag, samples
  // A fresh state for one channel.
  [[nodiscard]] virtual std::vector<float> initial_state() const = 0;
  // One frame of one channel: `in` and `out` are frame() long.
  [[nodiscard]] virtual expected process(std::span<const float> in, std::vector<float>& state,
                                         std::span<float> out) = 0;
};

struct sound_source {
  std::uint32_t channels = 1;
  std::int64_t total_frames = 0;  // for progress; 0 when unknown
  // Up to `max_frames` interleaved frames; empty at the end.
  std::function<result<std::vector<float>>(std::size_t max_frames)> read;
};

struct sound_sink_port {
  std::function<expected(std::span<const float> interleaved)> write;
};

struct sound_job_options {
  double strength = 1.0;  // 0..1
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(double fraction)> progress;
};

// Reads the whole source, writes the whole result. status::cancelled when
// `cancel` was set (the sink's file is the caller's to discard).
[[nodiscard]] expected run_sound_job(const sound_source& src, frame_model& model, const sound_sink_port& sink,
                                     const sound_job_options& options);

}  // namespace mv::enhance
