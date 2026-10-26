// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/enhance/sound_job.h"

#include <algorithm>
#include <deque>

namespace mv::enhance {
namespace {

// About a tenth of a second at 48 kHz per read.
constexpr std::size_t kReadFrames = 4800;

struct channel {
  std::vector<float> state;
  std::vector<float> pending;  // input not yet a whole model frame
  std::deque<float> dry;       // input waiting for its model output (the mix)
  std::vector<float> out;      // aligned, mixed output not yet written
  std::uint64_t produced = 0;  // model output samples so far, delay included
};

}  // namespace

expected run_sound_job(const sound_source& src, frame_model& model, const sound_sink_port& sink,
                       const sound_job_options& o) {
  const std::size_t ch = src.channels;
  const std::size_t frame = model.frame();
  const std::size_t delay = model.delay();
  if (ch == 0 || ch > 8 || frame == 0 || !src.read || !sink.write) return err(status::invalid_arg);
  const float wet = static_cast<float>(std::clamp(o.strength, 0.0, 1.0));
  const float dry_gain = 1.0f - wet;
  std::vector<channel> chans(ch);
  for (channel& c : chans) c.state = model.initial_state();
  std::vector<float> y(frame);
  std::uint64_t in_frames = 0;
  std::uint64_t written = 0;
  std::uint64_t limit = UINT64_MAX;  // the input's length, once it is known

  // Runs every whole frame waiting in each channel, then writes what all
  // channels have ready, interleaved.
  const auto pump = [&]() -> expected {
    for (channel& c : chans) {
      std::size_t at = 0;
      for (; at + frame <= c.pending.size(); at += frame) {
        MV_TRY_VOID(model.process(std::span<const float>(c.pending.data() + at, frame), c.state, y));
        for (const float v : y) {
          if (c.produced++ < delay) continue;  // the model's lag
          const float x = c.dry.front();
          c.dry.pop_front();
          c.out.push_back(wet * v + dry_gain * x);
        }
      }
      c.pending.erase(c.pending.begin(), c.pending.begin() + static_cast<std::ptrdiff_t>(at));
    }
    std::size_t ready = chans[0].out.size();
    for (const channel& c : chans) ready = std::min(ready, c.out.size());
    ready = static_cast<std::size_t>(std::min<std::uint64_t>(ready, limit - written));
    if (ready == 0) return {};
    written += ready;
    std::vector<float> inter(ready * ch);
    for (std::size_t k = 0; k < ch; ++k) {
      for (std::size_t i = 0; i < ready; ++i) inter[i * ch + k] = chans[k].out[i];
      chans[k].out.erase(chans[k].out.begin(), chans[k].out.begin() + static_cast<std::ptrdiff_t>(ready));
    }
    return sink.write(inter);
  };

  while (true) {
    if (o.cancel && o.cancel->load()) return err(status::cancelled);
    MV_TRY(std::vector<float> got, src.read(kReadFrames));
    if (got.empty()) break;
    const std::size_t n = got.size() / ch;
    for (std::size_t k = 0; k < ch; ++k) {
      channel& c = chans[k];
      for (std::size_t i = 0; i < n; ++i) {
        c.pending.push_back(got[i * ch + k]);
        c.dry.push_back(got[i * ch + k]);
      }
    }
    in_frames += n;
    MV_TRY_VOID(pump());
    if (o.progress && src.total_frames > 0) {
      o.progress(std::min(1.0, static_cast<double>(in_frames) / static_cast<double>(src.total_frames)));
    }
  }
  // The tail: silence through the model until every input sample has its
  // output (the lag), then the last partial frame. The dry side of the mix
  // gets silence too, past the end, and is never written.
  limit = in_frames;
  const std::uint64_t want = in_frames + delay;
  while (chans[0].produced < want) {
    if (o.cancel && o.cancel->load()) return err(status::cancelled);
    for (channel& c : chans) {
      const std::size_t missing = frame - c.pending.size() % frame;
      c.pending.insert(c.pending.end(), missing, 0.0f);
      c.dry.insert(c.dry.end(), missing, 0.0f);
    }
    MV_TRY_VOID(pump());
  }
  MV_TRY_VOID(pump());
  return written == in_frames ? expected{} : expected{err(status::internal)};
}

}  // namespace mv::enhance
