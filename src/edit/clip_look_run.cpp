// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Video Editor's Export exact with a look (docs/plans/video-editor-look.md),
// in the helper like every encode. The kept frames come from frame_reader
// (software decode, 8-bit sRGB), are put on the program's timeline, get the
// look's turn, mirror and crop, and go to picture_writer on the hardware
// encoder. The sound is read through the same cut, given the look's volume
// and fades, and encoded by the OS (AAC, else FLAC in Matroska); mute writes
// none. Rule 5: the source is only read. Rule 6: no path in a log.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "core/keep_ranges.h"
#include "edit/clip_internal.h"
#include "edit/clip_look.h"
#include "edit/clip_sample.h"
#include "edit/picture_io.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
}

namespace mv::edit::clip::detail {

namespace {

constexpr std::uint32_t kSoundRate = 48000;
constexpr std::int64_t kNsPerUs = 1000;
constexpr std::int64_t kSoundLeadNs = 500'000'000;  // sound written this far ahead of the frames

struct graph_deleter {
  void operator()(AVFilterGraph* g) const noexcept { avfilter_graph_free(&g); }
};

// The look's speed on the sound: FFmpeg's atempo (LGPL, docs/design/11), the
// pitch kept, as the player does it (player/transport.cpp). 0.5 and 2 are
// inside one instance's range. Interleaved float in and out.
class tempo_stage {
 public:
  static result<std::unique_ptr<tempo_stage>> open(float speed, std::uint32_t channels) {
    auto t = std::unique_ptr<tempo_stage>(new tempo_stage);
    t->channels_ = channels;
    t->graph_.reset(avfilter_graph_alloc());
    if (!t->graph_) return err(status::out_of_memory);
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, static_cast<int>(channels));
    char desc[64] = {};
    av_channel_layout_describe(&layout, desc, sizeof(desc));
    av_channel_layout_uninit(&layout);
    char args[160];
    std::snprintf(args, sizeof(args), "sample_rate=%u:sample_fmt=flt:channel_layout=%s:time_base=1/%u", kSoundRate,
                  desc, kSoundRate);
    char chain[64];
    std::snprintf(chain, sizeof(chain), "atempo=%.3f,aformat=sample_fmts=flt", static_cast<double>(speed));
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    const bool ok =
        outputs && inputs &&
        avfilter_graph_create_filter(&t->src_, avfilter_get_by_name("abuffer"), "in", args, nullptr,
                                     t->graph_.get()) >= 0 &&
        avfilter_graph_create_filter(&t->sink_, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr,
                                     t->graph_.get()) >= 0;
    if (ok) {
      outputs->name = av_strdup("in");
      outputs->filter_ctx = t->src_;
      inputs->name = av_strdup("out");
      inputs->filter_ctx = t->sink_;
    }
    const bool built = ok && avfilter_graph_parse_ptr(t->graph_.get(), chain, &inputs, &outputs, nullptr) >= 0 &&
                       avfilter_graph_config(t->graph_.get(), nullptr) >= 0;
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (!built) return err(status::internal);
    t->frame_.reset(av_frame_alloc());
    if (!t->frame_) return err(status::out_of_memory);
    return t;
  }

  // Pushes `in` (or, empty with `flush`, the end) and appends what comes out.
  expected push(std::span<const float> in, bool flush, std::vector<float>& out) {
    if (!in.empty()) {
      frame_ptr f(av_frame_alloc());
      if (!f) return err(status::out_of_memory);
      f->format = AV_SAMPLE_FMT_FLT;
      f->sample_rate = static_cast<int>(kSoundRate);
      av_channel_layout_default(&f->ch_layout, static_cast<int>(channels_));
      f->nb_samples = static_cast<int>(in.size() / channels_);
      f->pts = pts_;
      pts_ += f->nb_samples;
      if (av_frame_get_buffer(f.get(), 0) < 0) return err(status::out_of_memory);
      std::memcpy(f->data[0], in.data(), static_cast<std::size_t>(f->nb_samples) * channels_ * sizeof(float));
      if (av_buffersrc_add_frame(src_, f.get()) < 0) return err(status::internal);
    }
    if (flush && av_buffersrc_add_frame(src_, nullptr) < 0) return err(status::internal);
    for (;;) {
      const int rc = av_buffersink_get_frame(sink_, frame_.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
      if (rc < 0) return err(status::internal);
      const auto* p = reinterpret_cast<const float*>(frame_->data[0]);
      out.insert(out.end(), p, p + static_cast<std::size_t>(frame_->nb_samples) * channels_);
      av_frame_unref(frame_.get());
    }
    return {};
  }

 private:
  tempo_stage() = default;
  std::unique_ptr<AVFilterGraph, graph_deleter> graph_;
  AVFilterContext* src_ = nullptr;
  AVFilterContext* sink_ = nullptr;
  frame_ptr frame_;
  std::uint32_t channels_ = 1;
  std::int64_t pts_ = 0;
};


// The program's sound, pulled through the cut on demand.
class cut_sound {
 public:
  cut_sound(std::unique_ptr<audio_reader> r, const keep_ranges& keep, const look& l, std::int64_t program_ns)
      : r_(std::move(r)), keep_(keep), look_(l), channels_(r_->facts().channels) {
    total_frames_ = static_cast<std::uint64_t>(program_ns / 1000) * kSoundRate / 1'000'000;
  }
  [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
  // Frames of program sound handed out so far, as a time.
  [[nodiscard]] std::int64_t written_ns() const noexcept {
    return static_cast<std::int64_t>(written_ * 1'000'000 / kSoundRate) * 1000;
  }
  [[nodiscard]] bool done() const noexcept { return written_ >= total_frames_; }

  // Writes program sound until `until_ns` (or the end).
  expected write_until(picture_writer& w, std::int64_t until_ns) {
    while (!done() && written_ns() < until_ns) {
      if (pending_.empty()) {
        if (ended_) {
          // The source's sound ran out before the program did: silence.
          const std::uint64_t left = std::min<std::uint64_t>(total_frames_ - written_, kSoundRate / 10);
          pending_.assign(static_cast<std::size_t>(left) * channels_, 0.0f);
        } else {
          std::int64_t start_ms = 0;
          MV_TRY(std::vector<float> pcm, r_->read(static_cast<std::size_t>(kSoundRate / 10) * channels_, start_ms));
          if (pcm.empty()) {
            ended_ = true;
            continue;
          }
          if (read_ == 0) base_ns_ = start_ms * 1'000'000;
          keep_.keep_sound(pcm, read_, base_ns_, kSoundRate, channels_, pending_);
          read_ += pcm.size() / channels_;
          if (keep_.past_end(base_ns_ + static_cast<std::int64_t>(read_ * 1'000'000 / kSoundRate) * 1000)) {
            ended_ = true;
          }
          if (pending_.empty()) continue;
        }
      }
      std::uint64_t frames = pending_.size() / channels_;
      frames = std::min<std::uint64_t>(frames, total_frames_ - written_);
      std::span<float> block(pending_.data(), static_cast<std::size_t>(frames) * channels_);
      shape(block, written_);
      written_ += frames;
      if (tempo_) {
        // The speed: the same sound in less (or more) time, its pitch kept.
        tempo_out_.clear();
        MV_TRY_VOID(tempo_->push(block, done(), tempo_out_));
        if (!tempo_out_.empty()) MV_TRY_VOID(w.write_sound(tempo_out_));
      } else {
        MV_TRY_VOID(w.write_sound(block));
      }
      pending_.clear();
    }
    return {};
  }

  // The look's speed (not 1): the sound goes through atempo.
  expected set_speed(float speed) {
    if (speed == 1.0f) return {};
    MV_TRY(tempo_, tempo_stage::open(speed, channels_));
    return {};
  }

 private:
  // Volume and fades, on the program's frames [first, first + n).
  void shape(std::span<float> pcm, std::uint64_t first) const noexcept {
    const auto fade_frames = [](std::int32_t ms) {
      return static_cast<std::uint64_t>(ms) * kSoundRate / 1000;
    };
    const std::uint64_t fin = fade_frames(look_.fade_in_ms);
    const std::uint64_t fout = fade_frames(look_.fade_out_ms);
    if (look_.volume == 1.0f && fin == 0 && fout == 0) return;
    const std::uint64_t n = pcm.size() / channels_;
    for (std::uint64_t i = 0; i < n; ++i) {
      const std::uint64_t f = first + i;
      float g = look_.volume;
      if (f < fin) g *= static_cast<float>(f) / static_cast<float>(fin);
      if (fout > 0 && f + fout > total_frames_) {
        g *= static_cast<float>(total_frames_ - f) / static_cast<float>(fout);
      }
      for (std::uint32_t c = 0; c < channels_; ++c) pcm[i * channels_ + c] *= g;
    }
  }

  std::unique_ptr<audio_reader> r_;
  const keep_ranges& keep_;
  look look_;
  std::uint32_t channels_ = 1;
  std::uint64_t total_frames_ = 0;
  std::uint64_t read_ = 0;     // source frames read
  std::uint64_t written_ = 0;  // program frames written
  std::int64_t base_ns_ = 0;
  bool ended_ = false;
  std::vector<float> pending_;
  std::unique_ptr<tempo_stage> tempo_;
  std::vector<float> tempo_out_;
};

}  // namespace

result<render_done> render_look(std::string_view src_path, const render_spec& spec, const control& ctl) {
  if (spec.ranges.empty()) return err(status::invalid_arg);
  MV_TRY(auto frames, frame_reader::open(src_path, ctl.cancel));
  const frames_facts& f = frames->facts();
  if (f.width == 0 || f.height == 0) return err(status::unsupported_format);

  // The pieces, "to the end" made concrete.
  const std::int64_t end_ns = std::max<std::int64_t>(f.duration_ms, 1) * 1'000'000 + 1'000'000'000;
  std::vector<std::int64_t> pairs;
  pairs.reserve(spec.ranges.size() * 2);
  for (const range& r : spec.ranges) {
    pairs.push_back(r.in_ns);
    pairs.push_back(r.out_ns < 0 ? end_ns : r.out_ns);
  }
  keep_ranges keep;
  if (!keep.set(pairs)) return err(status::invalid_arg);
  const std::int64_t program_ns =
      std::min(keep.kept_ns(), std::max<std::int64_t>(0, f.duration_ms * 1'000'000 - spec.ranges.front().in_ns));

  const look l = sanitised(spec.lk);
  const frame_geometry g = geometry_of(f.width, f.height, static_cast<int>(f.rotation), l);
  const bool turn_pixels = !l.geometry_identity() || f.rotation != 0;

  picture_out_options o;
  o.width = turn_pixels ? g.out_w : f.width & ~1u;
  o.height = turn_pixels ? g.out_h : f.height & ~1u;
  o.rotation = 0;  // the frames are upright
  o.out_dir = spec.out_dir;
  o.out_stem = spec.out_stem;
  o.encoders = spec.encoders;
  o.allow_software = spec.allow_software;
  std::unique_ptr<cut_sound> sound;
  if (!l.mute) {
    auto r = audio_reader::open(src_path, kSoundRate, 0, ctl.cancel, 0);
    if (r) {
      sound = std::make_unique<cut_sound>(std::move(*r), keep, l, program_ns);
      MV_TRY_VOID(sound->set_speed(l.speed));
      o.sound_rate = kSoundRate;
      o.sound_channels = sound->channels();
    } else if (r.error() != status::unsupported_format) {
      return err(r.error());
    }
  }
  o.drop_sound = !sound;
  MV_TRY(auto w, picture_writer::open(src_path, o, ctl.cancel));

  std::vector<std::uint8_t> rgb, out;
  if (turn_pixels) out.resize(static_cast<std::size_t>(g.out_w) * g.out_h * 3);
  const colour_table light(l.light);
  // The speed: frames go out on the clip's own frame grid (constant rate):
  // each slot shows the latest frame due by then, so 2x keeps every other
  // frame and 0.5x shows each one twice.
  const double speed = l.speed;
  const double fps = f.fps > 1.0 && f.fps < 1000.0 ? f.fps : 30.0;
  std::vector<std::uint8_t> held;
  std::uint32_t held_stride = 0;
  std::int64_t slot = 0;
  const auto slot_us = [&](std::int64_t k) { return std::llround(static_cast<double>(k) * 1e6 / fps); };
  const auto emit_until = [&](std::int64_t t_us) -> expected {
    while (!held.empty() && slot_us(slot) < t_us) {
      MV_TRY_VOID(w->write_frame(held.data(), held_stride, slot_us(slot)));
      ++slot;
    }
    return {};
  };
  std::int64_t pts_us = 0;
  for (;;) {
    if (cancelled(ctl.cancel)) return err(status::cancelled);
    MV_TRY(bool more, frames->next(rgb, pts_us));
    if (!more) break;
    const std::int64_t src_ns = pts_us * kNsPerUs;
    if (keep.past_end(src_ns)) break;
    const auto prog = keep.program_of(src_ns);
    if (!prog) continue;
    if (sound) MV_TRY_VOID(sound->write_until(*w, *prog + kSoundLeadNs));
    std::uint8_t* px = rgb.data();
    std::uint32_t stride = f.width * 3;
    std::size_t bytes = static_cast<std::size_t>(f.width) * f.height * 3;
    if (turn_pixels) {
      apply_geometry(g, rgb, f.width, f.height, 3, out);
      px = out.data();
      stride = g.out_w * 3;
      bytes = out.size();
    }
    light.apply(std::span<std::uint8_t>(px, bytes), 3);
    if (speed == 1.0) {
      MV_TRY_VOID(w->write_frame(px, stride, *prog / kNsPerUs));
    } else {
      MV_TRY_VOID(emit_until(std::llround(static_cast<double>(*prog) / kNsPerUs / speed)));
      held.assign(px, px + bytes);
      held_stride = stride;
    }
    if (ctl.progress && program_ns > 0) {
      ctl.progress(ctl.user, std::min(1.0, static_cast<double>(*prog) / static_cast<double>(program_ns)));
    }
  }
  if (speed != 1.0) MV_TRY_VOID(emit_until(std::llround(static_cast<double>(program_ns) / kNsPerUs / speed)));
  if (sound) MV_TRY_VOID(sound->write_until(*w, program_ns));
  if (cancelled(ctl.cancel)) return err(status::cancelled);
  render_done done;
  done.encoder = w->encoder();
  MV_TRY(done.path, w->finish());
  return done;
}

}  // namespace mv::edit::clip::detail
