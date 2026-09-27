// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The index sampler (clip_sample.h). One pass over the file: every video
// packet goes to a keyframe-only decoder; a GOP longer than max_gap also
// replays its kept packets into a full decoder that fills the grid.
#include "edit/clip_sample.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

#include "edit/clip_internal.h"
#include "edit/clip_pixels.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace mv::edit::clip {
namespace {

using namespace detail;

constexpr std::int64_t kNever = std::numeric_limits<std::int64_t>::min() / 4;

[[nodiscard]] bool is_hdr(const AVCodecParameters* p) noexcept {
  return p->color_trc == AVCOL_TRC_SMPTE2084 || p->color_trc == AVCOL_TRC_ARIB_STD_B67;
}

[[nodiscard]] bool frame_is_key(const AVFrame* f) noexcept {
#ifdef AV_FRAME_FLAG_KEY
  return (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
  return f->key_frame != 0;
#endif
}

// Display size of a coded frame (pixel aspect applied), fitted to `edge`.
void fit(int w, int h, AVRational sar, std::uint32_t edge, int& out_w, int& out_h) noexcept {
  double dw = w, dh = h;
  if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) dw = dw * sar.num / sar.den;
  const double longest = std::max(dw, dh);
  const double scale = longest > edge && edge > 0 ? edge / longest : 1.0;
  out_w = std::max(1, static_cast<int>(std::lround(dw * scale)));
  out_h = std::max(1, static_cast<int>(std::lround(dh * scale)));
}

// RGBA (converter output) -> packed RGB, in place into `out`.
void to_rgb(const std::vector<std::uint8_t>& rgba, sample_rgb& out) {
  const std::size_t px = rgba.size() / 4;
  out.rgb.resize(px * 3);
  for (std::size_t i = 0; i < px; ++i) {
    out.rgb[i * 3] = rgba[i * 4];
    out.rgb[i * 3 + 1] = rgba[i * 4 + 1];
    out.rgb[i * 3 + 2] = rgba[i * 4 + 2];
  }
}

}  // namespace

struct frame_sampler::impl {
  sample_options opt;
  std::atomic<bool> never{false};
  const std::atomic<bool>* cancel = nullptr;
  source src;
  AVStream* st = nullptr;
  codec_ptr key_dec;
  codec_ptr fill_dec;
  bool key_dec_full = false;  // the decoder ignored skip_frame (dav1d): it decodes every frame
  bool in_fill = false;
  bool eof = false;
  std::deque<packet_ptr> gop;  // this GOP's packets from its keyframe, for a fill
  std::size_t gop_bytes = 0;
  bool gop_overflow = false;
  std::int64_t gop_key_ms = kNever;
  std::int64_t last_kept_ms = kNever;
  int rotation = 0;
  rgba_converter conv;
  std::vector<std::uint8_t> rgba;
  std::deque<sampled> ready;
  sample_facts facts;
  packet_ptr pkt;
  frame_ptr frame;

  [[nodiscard]] std::int64_t ms_of(std::int64_t ts) const noexcept {
    return to_timeline(src, st, ts) / 1'000'000;
  }

  [[nodiscard]] expected emit(const AVFrame* f, std::int64_t t_ms, std::int64_t ts, bool key) {
    int w = 0, h = 0;
    fit(f->width, f->height, f->sample_aspect_ratio.num > 0 ? f->sample_aspect_ratio
                                                            : st->codecpar->sample_aspect_ratio,
        opt.max_long_edge, w, h);
    if (!conv.convert(f, st->codecpar, w, h, rgba)) return err(status::internal);
    rotate_rgba(rgba, w, h, rotation);
    sampled s;
    s.image.width = static_cast<std::uint32_t>(w);
    s.image.height = static_cast<std::uint32_t>(h);
    to_rgb(rgba, s.image);
    s.pts_ms = t_ms;
    s.pts_tb = ts;
    s.tb_num = st->time_base.num;
    s.tb_den = st->time_base.den;
    s.keyframe = key;
    last_kept_ms = std::max(last_kept_ms, t_ms);
    ready.push_back(std::move(s));
    return {};
  }

  // A decoded frame from either decoder: keep it if the rules say so.
  [[nodiscard]] expected consider(const AVFrame* f, bool from_fill) {
    const std::int64_t ts = frame_ts(f);
    if (ts == AV_NOPTS_VALUE) return {};
    const std::int64_t t = ms_of(ts);
    if (t + 1 < opt.start_ms) return {};
    const bool key = frame_is_key(f);
    if (!from_fill && !key) key_dec_full = true;  // skip_frame not honoured
    if (key && !from_fill) {
      if (last_kept_ms != kNever && t - last_kept_ms < static_cast<std::int64_t>(opt.min_gap_ms)) {
        return {};
      }
      return emit(f, t, ts, true);
    }
    if (key) return {};  // the fill decoder's copy of a keyframe the key decoder handles
    // A grid fill: only where the keyframes left a hole.
    const std::int64_t ref = std::max(last_kept_ms, gop_key_ms);
    if (ref == kNever || t - ref < static_cast<std::int64_t>(opt.max_gap_ms)) return {};
    return emit(f, t, ts, false);
  }

  [[nodiscard]] expected drain(AVCodecContext* dec, bool from_fill) {
    while (true) {
      const int rc = avcodec_receive_frame(dec, frame.get());
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return {};
      if (rc < 0) return {};  // a damaged frame: skip it, as the player does
      auto r = consider(frame.get(), from_fill);
      av_frame_unref(frame.get());
      MV_TRY_VOID(r);
    }
  }

  [[nodiscard]] expected end_fill() {
    if (!in_fill) return {};
    (void)avcodec_send_packet(fill_dec.get(), nullptr);
    MV_TRY_VOID(drain(fill_dec.get(), true));
    avcodec_flush_buffers(fill_dec.get());
    in_fill = false;
    return {};
  }

  [[nodiscard]] expected start_fill() {
    if (!fill_dec) {
      MV_TRY(codec_ptr d, open_decoder(st, opt.decoder_threads));
      fill_dec = std::move(d);
      // Grid frames may be P or B; skipping non-reference B-frames keeps a
      // long-GOP fill cheap and still lands within a frame of the grid.
      fill_dec->skip_frame = AVDISCARD_NONREF;
    }
    avcodec_flush_buffers(fill_dec.get());
    in_fill = true;
    for (const packet_ptr& p : gop) {
      (void)avcodec_send_packet(fill_dec.get(), p.get());
      MV_TRY_VOID(drain(fill_dec.get(), true));
    }
    return {};
  }

  [[nodiscard]] expected step() {
    if (cancelled(cancel)) return err(status::cancelled);
    const int rc = av_read_frame(src.format.get(), pkt.get());
    if (rc < 0) {
      if (rc == AVERROR_EXIT) return err(status::cancelled);
      (void)avcodec_send_packet(key_dec.get(), nullptr);
      MV_TRY_VOID(drain(key_dec.get(), false));
      MV_TRY_VOID(end_fill());
      eof = true;
      return {};
    }
    struct unref {
      AVPacket* p;
      ~unref() { av_packet_unref(p); }
    } const done{pkt.get()};
    if (pkt->stream_index != src.video) return {};
    const bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    const std::int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (key) {
      MV_TRY_VOID(end_fill());
      gop.clear();
      gop_bytes = 0;
      gop_overflow = false;
      if (pts != AV_NOPTS_VALUE) gop_key_ms = ms_of(pts);
    }
    if (!gop_overflow && !key_dec_full) {
      if (gop_bytes + static_cast<std::size_t>(pkt->size) > opt.max_gop_bytes) {
        gop_overflow = true;
        gop.clear();
        gop_bytes = 0;
      } else {
        packet_ptr copy(av_packet_alloc());
        if (!copy || av_packet_ref(copy.get(), pkt.get()) < 0) return err(status::out_of_memory);
        gop_bytes += static_cast<std::size_t>(pkt->size);
        gop.push_back(std::move(copy));
      }
    }
    (void)avcodec_send_packet(key_dec.get(), pkt.get());  // a bad packet is skipped, not fatal
    MV_TRY_VOID(drain(key_dec.get(), false));
    if (in_fill) {
      (void)avcodec_send_packet(fill_dec.get(), pkt.get());
      return drain(fill_dec.get(), true);
    }
    // A hole: this GOP runs past max_gap and its keyframes cannot cover it.
    if (!key && !key_dec_full && !gop_overflow && !gop.empty() && pts != AV_NOPTS_VALUE &&
        (gop.front()->flags & AV_PKT_FLAG_KEY) != 0) {
      const std::int64_t ref = std::max(last_kept_ms, gop_key_ms);
      if (ref != kNever && ms_of(pts) - ref >= static_cast<std::int64_t>(opt.max_gap_ms)) {
        return start_fill();
      }
    }
    return {};
  }
};

frame_sampler::frame_sampler(std::unique_ptr<impl> p) : p_(std::move(p)) {}
frame_sampler::~frame_sampler() = default;

result<std::unique_ptr<frame_sampler>> frame_sampler::open(std::string_view utf8_path,
                                                          const sample_options& options,
                                                          const std::atomic<bool>* cancel) {
  auto p = std::make_unique<impl>();
  p->opt = options;
  if (p->opt.max_long_edge == 0) p->opt.max_long_edge = 512;
  if (p->opt.max_gap_ms == 0) p->opt.max_gap_ms = 2000;
  if (p->opt.min_gap_ms > p->opt.max_gap_ms) p->opt.min_gap_ms = p->opt.max_gap_ms;
  p->cancel = cancel ? cancel : &p->never;
  MV_TRY(source s, open_source(utf8_path, p->cancel));
  if (s.video < 0) return err(status::unsupported_format);
  p->src = std::move(s);
  p->st = p->src.format->streams[p->src.video];
  for (unsigned i = 0; i < p->src.format->nb_streams; ++i) {
    if (static_cast<int>(i) != p->src.video) p->src.format->streams[i]->discard = AVDISCARD_ALL;
  }
  MV_TRY(codec_ptr dec, open_decoder(p->st, options.decoder_threads));
  p->key_dec = std::move(dec);
  p->key_dec->skip_frame = AVDISCARD_NONKEY;
  p->pkt.reset(av_packet_alloc());
  p->frame.reset(av_frame_alloc());
  if (!p->pkt || !p->frame) return err(status::out_of_memory);
  p->rotation = stream_rotation(p->st);

  const AVCodecParameters* cp = p->st->codecpar;
  int w = 0, h = 0;
  fit(cp->width, cp->height, cp->sample_aspect_ratio, 1u << 30, w, h);
  if (p->rotation == 90 || p->rotation == 270) std::swap(w, h);
  p->facts.width = static_cast<std::uint32_t>(w);
  p->facts.height = static_cast<std::uint32_t>(h);
  p->facts.duration_ms = p->src.duration_ns / 1'000'000;
  p->facts.hdr = is_hdr(cp);
  if (p->opt.start_ms > 0) seek_before(p->src, p->opt.start_ms * 1'000'000);
  return std::unique_ptr<frame_sampler>(new frame_sampler(std::move(p)));
}

const sample_facts& frame_sampler::facts() const noexcept { return p_->facts; }

result<sampled> frame_sampler::next() {
  while (p_->ready.empty()) {
    if (p_->eof) {
      sampled end;
      end.end = true;
      return end;
    }
    MV_TRY_VOID(p_->step());
  }
  sampled out = std::move(p_->ready.front());
  p_->ready.pop_front();
  return out;
}

// ---- the soundtrack -------------------------------------------------------------

struct audio_reader::impl {
  std::atomic<bool> never{false};
  const std::atomic<bool>* cancel = nullptr;
  source src;
  AVStream* st = nullptr;
  codec_ptr dec;
  SwrContext* swr = nullptr;
  std::uint32_t rate = 16000;
  std::int64_t start_ms = 0;
  std::int64_t base_ms = -1;       // the time of the first sample kept
  std::uint64_t emitted = 0;       // samples handed out
  std::vector<float> pending;      // decoded, not yet handed out
  bool eof = false;
  audio_facts facts;
  packet_ptr pkt;
  frame_ptr frame;
  ~impl() {
    if (swr) swr_free(&swr);
  }

  [[nodiscard]] bool convert(const AVFrame* f) {
    const int max_out = swr_get_out_samples(swr, f ? f->nb_samples : 0);
    if (max_out <= 0) return true;
    std::vector<float> out(static_cast<std::size_t>(max_out));
    std::uint8_t* planes[1] = {reinterpret_cast<std::uint8_t*>(out.data())};
    const int got = swr_convert(swr, planes, max_out,
                                f ? const_cast<const std::uint8_t**>(f->extended_data) : nullptr,
                                f ? f->nb_samples : 0);
    if (got < 0) return false;
    out.resize(static_cast<std::size_t>(got));
    std::int64_t t_ms = -1;
    if (f && base_ms < 0) {
      const std::int64_t ts = frame_ts(f);
      t_ms = ts == AV_NOPTS_VALUE ? 0 : to_timeline(src, st, ts) / 1'000'000;
    }
    // Drop what precedes the resume point (seeking lands on a packet before it).
    std::size_t skip = 0;
    if (base_ms < 0) {
      if (t_ms < 0) t_ms = 0;
      if (t_ms < start_ms) {
        skip = std::min<std::size_t>(out.size(),
                                     static_cast<std::size_t>((start_ms - t_ms) * rate / 1000));
        t_ms += static_cast<std::int64_t>(skip) * 1000 / rate;
      }
      if (skip == out.size()) return true;
      base_ms = t_ms;
    }
    pending.insert(pending.end(), out.begin() + static_cast<std::ptrdiff_t>(skip), out.end());
    return true;
  }

  [[nodiscard]] expected step() {
    if (cancelled(cancel)) return err(status::cancelled);
    const int rc = av_read_frame(src.format.get(), pkt.get());
    if (rc < 0) {
      if (rc == AVERROR_EXIT) return err(status::cancelled);
      (void)avcodec_send_packet(dec.get(), nullptr);
      while (avcodec_receive_frame(dec.get(), frame.get()) == 0) {
        if (!convert(frame.get())) return err(status::corrupt);
        av_frame_unref(frame.get());
      }
      if (!convert(nullptr)) return err(status::corrupt);  // the resampler's tail
      eof = true;
      return {};
    }
    if (pkt->stream_index == src.audio) {
      (void)avcodec_send_packet(dec.get(), pkt.get());
      while (avcodec_receive_frame(dec.get(), frame.get()) == 0) {
        const bool ok = convert(frame.get());
        av_frame_unref(frame.get());
        if (!ok) {
          av_packet_unref(pkt.get());
          return err(status::corrupt);
        }
      }
    }
    av_packet_unref(pkt.get());
    return {};
  }
};

audio_reader::audio_reader(std::unique_ptr<impl> p) : p_(std::move(p)) {}
audio_reader::~audio_reader() = default;
const audio_facts& audio_reader::facts() const noexcept { return p_->facts; }

result<std::unique_ptr<audio_reader>> audio_reader::open(std::string_view utf8_path,
                                                         std::uint32_t sample_rate,
                                                         std::int64_t start_ms,
                                                         const std::atomic<bool>* cancel) {
  auto p = std::make_unique<impl>();
  p->cancel = cancel ? cancel : &p->never;
  p->rate = sample_rate ? sample_rate : 16000;
  p->start_ms = std::max<std::int64_t>(0, start_ms);
  MV_TRY(source s, open_source(utf8_path, p->cancel));
  if (s.audio < 0) return err(status::unsupported_format);
  p->src = std::move(s);
  p->st = p->src.format->streams[p->src.audio];
  for (unsigned i = 0; i < p->src.format->nb_streams; ++i) {
    if (static_cast<int>(i) != p->src.audio) p->src.format->streams[i]->discard = AVDISCARD_ALL;
  }
  MV_TRY(codec_ptr dec, open_decoder(p->st, 1));
  p->dec = std::move(dec);
  AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
  if (swr_alloc_set_opts2(&p->swr, &mono, AV_SAMPLE_FMT_FLT, static_cast<int>(p->rate), &p->dec->ch_layout,
                          p->dec->sample_fmt, p->dec->sample_rate, 0, nullptr) < 0 ||
      !p->swr || swr_init(p->swr) < 0) {
    return err(status::unsupported_format);
  }
  p->pkt.reset(av_packet_alloc());
  p->frame.reset(av_frame_alloc());
  if (!p->pkt || !p->frame) return err(status::out_of_memory);
  p->facts.has_audio = true;
  p->facts.duration_ms = p->src.duration_ns / 1'000'000;
  if (p->start_ms > 0) {
    // Seek the audio stream itself (the source's helper seeks video first).
    const std::int64_t ts = from_timeline(p->src, p->st, p->start_ms * 1'000'000);
    (void)av_seek_frame(p->src.format.get(), p->src.audio, ts, AVSEEK_FLAG_BACKWARD);
  }
  return std::unique_ptr<audio_reader>(new audio_reader(std::move(p)));
}

result<std::vector<float>> audio_reader::read(std::size_t max_samples, std::int64_t& start_ms) {
  while (p_->pending.size() < max_samples && !p_->eof) MV_TRY_VOID(p_->step());
  const std::size_t n = std::min(max_samples, p_->pending.size());
  start_ms = (p_->base_ms < 0 ? p_->start_ms : p_->base_ms) +
             static_cast<std::int64_t>(p_->emitted * 1000 / p_->rate);
  std::vector<float> out(p_->pending.begin(), p_->pending.begin() + static_cast<std::ptrdiff_t>(n));
  p_->pending.erase(p_->pending.begin(), p_->pending.begin() + static_cast<std::ptrdiff_t>(n));
  p_->emitted += n;
  return out;
}

result<sample_rgb> frame_rgb_at(std::string_view utf8_path, std::int64_t at_ms,
                                std::uint32_t max_long_edge, const std::atomic<bool>* cancel) {
  std::atomic<bool> never{false};
  if (!cancel) cancel = &never;
  MV_TRY(source s, open_source(utf8_path, cancel));
  if (s.video < 0) return err(status::unsupported_format);
  AVStream* st = s.format->streams[s.video];
  for (unsigned i = 0; i < s.format->nb_streams; ++i) {
    if (static_cast<int>(i) != s.video) s.format->streams[i]->discard = AVDISCARD_ALL;
  }
  MV_TRY(codec_ptr dec, open_decoder(st, 2));
  const std::int64_t at_ns =
      std::clamp<std::int64_t>(at_ms * 1'000'000, 0, std::max<std::int64_t>(0, s.duration_ns));
  seek_before(s, at_ns);
  packet_ptr pkt(av_packet_alloc());
  frame_ptr frame(av_frame_alloc());
  frame_ptr held(av_frame_alloc());
  if (!pkt || !frame || !held) return err(status::out_of_memory);
  bool have = false;
  bool done = false;
  constexpr std::int64_t kTolNs = 500'000;
  const auto drain = [&] {
    while (!done) {
      const int rc = avcodec_receive_frame(dec.get(), frame.get());
      if (rc < 0) return;
      const std::int64_t ts = frame_ts(frame.get());
      if (ts != AV_NOPTS_VALUE) {
        const std::int64_t t = to_timeline(s, st, ts);
        if (t > at_ns + kTolNs && have) {
          done = true;
        } else {
          av_frame_unref(held.get());
          if (av_frame_ref(held.get(), frame.get()) == 0) have = true;
          if (t > at_ns + kTolNs) done = true;
        }
      }
      av_frame_unref(frame.get());
    }
  };
  while (!done) {
    if (cancelled(cancel)) return err(status::cancelled);
    const int rc = av_read_frame(s.format.get(), pkt.get());
    if (rc < 0) {
      if (rc == AVERROR_EXIT) return err(status::cancelled);
      (void)avcodec_send_packet(dec.get(), nullptr);
      drain();
      break;
    }
    if (pkt->stream_index == s.video) (void)avcodec_send_packet(dec.get(), pkt.get());
    av_packet_unref(pkt.get());
    drain();
  }
  if (!have) return err(status::corrupt);
  int w = 0, h = 0;
  fit(held->width, held->height,
      held->sample_aspect_ratio.num > 0 ? held->sample_aspect_ratio : st->codecpar->sample_aspect_ratio,
      max_long_edge, w, h);
  rgba_converter conv;
  std::vector<std::uint8_t> rgba;
  if (!conv.convert(held.get(), st->codecpar, w, h, rgba)) return err(status::internal);
  rotate_rgba(rgba, w, h, stream_rotation(st));
  sample_rgb out;
  out.width = static_cast<std::uint32_t>(w);
  out.height = static_cast<std::uint32_t>(h);
  to_rgb(rgba, out);
  return out;
}

}  // namespace mv::edit::clip
