// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Encoding sound the caller made (Enhance, #297): the shared half of
// sound_writer.h and the picture writer. FFmpeg types: clip TUs only.
//
// docs/design/11: no software AAC encoder anywhere. The sound is AAC from the
// operating system's own encoder (AudioToolbox, `aac_at`, on the Mac; Media
// Foundation, `aac_mf`, on Windows) and, where there is none (a Linux build,
// a Windows without the MF AAC encoder), FLAC: lossless and royalty-free.
// FFmpeg's own `aac` is never opened.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "edit/clip_internal.h"

namespace mv::edit::clip::detail {

class audio_encoder {
 public:
  // The first encoder that opens at this rate and channel count. `global`:
  // the container wants codec headers out of band (MP4, MOV, Matroska).
  [[nodiscard]] static result<audio_encoder> open(std::uint32_t sample_rate, std::uint32_t channels, bool global);

  [[nodiscard]] bool lossless() const noexcept { return lossless_; }
  [[nodiscard]] const char* name() const noexcept { return ctx_ ? ctx_->codec->name : ""; }
  [[nodiscard]] AVCodecContext* context() const noexcept { return ctx_.get(); }

  // Queues interleaved float PCM; `sink` gets each packet ready, in the
  // encoder's time base.
  template <typename Sink>
  expected push(std::span<const float> pcm, Sink&& sink) {
    fifo_.insert(fifo_.end(), pcm.begin(), pcm.end());
    const std::size_t whole = static_cast<std::size_t>(frame_size()) * channels_;
    while (fifo_.size() >= whole) {
      MV_TRY_VOID(send(frame_size()));
      MV_TRY_VOID(drain(sink));
    }
    return {};
  }
  // The rest, then the encoder's own tail.
  template <typename Sink>
  expected flush(Sink&& sink) {
    if (!fifo_.empty()) {
      // An encoder that takes no short last frame gets it padded with silence
      // (under one frame, about 21 ms at 48 kHz).
      const auto caps = ctx_->codec->capabilities;
      if (!(caps & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE))) {
        fifo_.resize(static_cast<std::size_t>(frame_size()) * channels_, 0.0f);
      }
      MV_TRY_VOID(send(static_cast<int>(fifo_.size() / channels_)));
      MV_TRY_VOID(drain(sink));
    }
    if (avcodec_send_frame(ctx_.get(), nullptr) < 0) return err(status::internal);
    return drain(sink);
  }

 private:
  [[nodiscard]] int frame_size() const noexcept { return ctx_->frame_size > 0 ? ctx_->frame_size : 1024; }
  [[nodiscard]] expected send(int samples);
  template <typename Sink>
  expected drain(Sink& sink) {
    while (true) {
      const int rc = avcodec_receive_packet(ctx_.get(), pkt_.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
      if (rc < 0) return err(status::internal);
      MV_TRY_VOID(sink(pkt_.get()));
      av_packet_unref(pkt_.get());
    }
  }

  codec_ptr ctx_;
  frame_ptr frame_;
  packet_ptr pkt_;
  std::vector<float> fifo_;
  std::uint32_t channels_ = 1;
  std::int64_t next_pts_ = 0;
  bool lossless_ = false;
};

}  // namespace mv::edit::clip::detail
