// SPDX-License-Identifier: GPL-2.0-or-later
// Decoded frames -> 8-bit sRGB RGBA, shared by the clip jobs (clip_encode.cpp)
// and the index sampler (clip_sample.cpp, Milestone H). FFmpeg types appear
// here and in the clip .cpp files only; clip.h names none.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
}

#include "codec/cicp.h"
#include "edit/clip_internal.h"

namespace mv::edit::clip::detail {

struct sws_deleter {
  void operator()(SwsContext* c) const noexcept { sws_freeContext(c); }
};
using sws_ptr = std::unique_ptr<SwsContext, sws_deleter>;

[[nodiscard]] inline result<codec_ptr> open_decoder(const AVStream* st, int threads = 0) {
  const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (dec == nullptr) return err(status::unsupported_format);
  codec_ptr ctx(avcodec_alloc_context3(dec));
  if (!ctx) return err(status::out_of_memory);
  if (avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0) return err(status::corrupt);
  ctx->pkt_timebase = st->time_base;
  ctx->thread_count = threads;  // 0: the job's own thread pool, sized by FFmpeg
  if (avcodec_open2(ctx.get(), dec, nullptr) < 0) return err(status::unsupported_format);
  return ctx;
}

[[nodiscard]] inline std::int64_t frame_ts(const AVFrame* f) noexcept {
  return f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
}

// ---- pixels -> 8-bit sRGB ------------------------------------------------------------

// The frame's colour, falling back to the stream's, then to what an untagged
// camera clip is in practice (BT.709 at HD sizes, BT.601 below).
struct colour {
  AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic transfer = AVCOL_TRC_UNSPECIFIED;
  AVColorSpace matrix = AVCOL_SPC_UNSPECIFIED;
  AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
};

[[nodiscard]] inline colour colour_of(const AVFrame* f, const AVCodecParameters* p) noexcept {
  colour c;
  c.primaries = f->color_primaries != AVCOL_PRI_UNSPECIFIED ? f->color_primaries : p->color_primaries;
  c.transfer = f->color_trc != AVCOL_TRC_UNSPECIFIED ? f->color_trc : p->color_trc;
  c.matrix = f->colorspace != AVCOL_SPC_UNSPECIFIED ? f->colorspace : p->color_space;
  c.range = f->color_range != AVCOL_RANGE_UNSPECIFIED ? f->color_range : p->color_range;
  if (c.matrix == AVCOL_SPC_UNSPECIFIED || c.matrix == AVCOL_SPC_RESERVED) {
    c.matrix = f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
  }
  return c;
}

// Converts decoded frames to RGBA8 in sRGB at a fixed output size. SDR with
// BT.709 / BT.601 / unspecified primaries is taken as sRGB-encoded, the same
// convention the still path applies to CICP (codec/cicp.h). Other SDR
// primaries (P3, BT.2020) are converted to BT.709 in linear light. PQ / HLG go
// through the CPU twin of the video shader's tone map (cicp::hdr_to_sdr), so a
// frame saved from an iPhone HDR clip looks like the frame on the canvas (D6).
class rgba_converter {
 public:
  [[nodiscard]] bool convert(const AVFrame* f, const AVCodecParameters* p, int out_w, int out_h,
                             std::vector<std::uint8_t>& rgba) {
    const colour c = colour_of(f, p);
    const bool hdr = c.transfer == AVCOL_TRC_SMPTE2084 || c.transfer == AVCOL_TRC_ARIB_STD_B67;
    const AVPixelFormat dst = hdr ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_RGBA;
    sws_ = sws_ptr(sws_getCachedContext(sws_.release(), f->width, f->height,
                                        static_cast<AVPixelFormat>(f->format), out_w, out_h, dst,
                                        SWS_BICUBIC | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND,
                                        nullptr, nullptr, nullptr));
    if (!sws_) return false;
    const int* coeffs = sws_getCoefficients(static_cast<int>(c.matrix));
    const int src_full = c.range == AVCOL_RANGE_JPEG ? 1 : 0;
    // sws_setColorspaceDetails fails for RGB inputs; that is fine, RGB needs none.
    (void)sws_setColorspaceDetails(sws_.get(), coeffs, src_full, sws_getCoefficients(SWS_CS_DEFAULT),
                                   1, 0, 1 << 16, 1 << 16);
    const std::size_t px = static_cast<std::size_t>(out_w) * static_cast<std::size_t>(out_h);
    rgba.resize(px * 4);
    if (hdr) {
      wide_.resize(px * 4);
      std::uint8_t* dst_planes[4] = {reinterpret_cast<std::uint8_t*>(wide_.data()), nullptr, nullptr, nullptr};
      int dst_stride[4] = {out_w * 8, 0, 0, 0};
      if (sws_scale(sws_.get(), f->data, f->linesize, 0, f->height, dst_planes, dst_stride) <= 0) return false;
      const std::uint16_t prim = static_cast<std::uint16_t>(c.primaries);
      const std::uint16_t trc = static_cast<std::uint16_t>(c.transfer);
      if (!tone_ready_ || prim != tone_prim_ || trc != tone_trc_) {
        if (!tone_.init(prim, trc, 16)) return false;
        tone_ready_ = true;
        tone_prim_ = prim;
        tone_trc_ = trc;
      }
      for (int y = 0; y < out_h; ++y) {
        tone_.row(wide_.data() + static_cast<std::size_t>(y) * out_w * 4, static_cast<std::size_t>(out_w),
                  rgba.data() + static_cast<std::size_t>(y) * out_w * 4);
      }
      return true;
    }
    std::uint8_t* dst_planes[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {out_w * 4, 0, 0, 0};
    if (sws_scale(sws_.get(), f->data, f->linesize, 0, f->height, dst_planes, dst_stride) <= 0) return false;
    to_srgb_primaries(c.primaries, rgba);
    return true;
  }

 private:
  void to_srgb_primaries(AVColorPrimaries primaries, std::vector<std::uint8_t>& rgba) {
    namespace cicp = codec::cicp;
    if (primaries == AVCOL_PRI_BT709 || primaries == AVCOL_PRI_UNSPECIFIED ||
        primaries == AVCOL_PRI_BT470BG || primaries == AVCOL_PRI_SMPTE170M ||
        primaries == AVCOL_PRI_SMPTE240M || primaries == AVCOL_PRI_RESERVED0 ||
        primaries == AVCOL_PRI_RESERVED) {
      return;
    }
    if (matrix_prim_ != static_cast<int>(primaries)) {
      cicp::chromaticities src{}, dst{};
      cicp::mat3 sx{}, dx{}, dinv{};
      if (!cicp::primaries_of(static_cast<std::uint16_t>(primaries), src) ||
          !cicp::primaries_of(cicp::kPrimariesBt709, dst) || !cicp::rgb_to_xyz(src, sx) ||
          !cicp::rgb_to_xyz(dst, dx) || !cicp::invert(dx, dinv)) {
        return;  // an RGB space cicp cannot describe: leave the samples as they are
      }
      const cicp::mat3 adapt =
          cicp::bradford(cicp::xy_to_xyz(src.wx, src.wy), cicp::xy_to_xyz(dst.wx, dst.wy));
      const cicp::mat3 m = cicp::mul(dinv, cicp::mul(adapt, sx));
      for (std::size_t i = 0; i < 9; ++i) matrix_[i] = static_cast<float>(m[i]);
      for (int i = 0; i < 256; ++i) {
        const double v = i / 255.0;
        lin_[static_cast<std::size_t>(i)] =
            static_cast<float>(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
      }
      for (std::size_t i = 0; i < enc_.size(); ++i) {
        const double v = static_cast<double>(i) / static_cast<double>(enc_.size() - 1);
        const double s = v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
        enc_[i] = static_cast<std::uint8_t>(std::clamp(std::lround(s * 255.0), 0L, 255L));
      }
      matrix_prim_ = static_cast<int>(primaries);
    }
    const auto encode = [this](float v) {
      v = std::clamp(v, 0.0f, 1.0f);
      return enc_[static_cast<std::size_t>(v * static_cast<float>(enc_.size() - 1) + 0.5f)];
    };
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
      const float r = lin_[rgba[i]], g = lin_[rgba[i + 1]], b = lin_[rgba[i + 2]];
      rgba[i] = encode(matrix_[0] * r + matrix_[1] * g + matrix_[2] * b);
      rgba[i + 1] = encode(matrix_[3] * r + matrix_[4] * g + matrix_[5] * b);
      rgba[i + 2] = encode(matrix_[6] * r + matrix_[7] * g + matrix_[8] * b);
    }
  }

  sws_ptr sws_;
  std::vector<std::uint16_t> wide_;
  codec::cicp::hdr_to_sdr tone_;
  bool tone_ready_ = false;
  std::uint16_t tone_prim_ = 0, tone_trc_ = 0;
  int matrix_prim_ = -1;
  std::array<float, 9> matrix_{};
  std::array<float, 256> lin_{};
  std::array<std::uint8_t, 4096> enc_{};
};

// Turns an RGBA8 image clockwise by 90/180/270 (the display matrix, applied to
// pixels, because PNG / GIF / WebP carry no orientation).
inline void rotate_rgba(std::vector<std::uint8_t>& px, int& w, int& h, int clockwise) {
  if (clockwise == 0) return;
  std::vector<std::uint8_t> out(px.size());
  const int nw = clockwise == 180 ? w : h;
  const int nh = clockwise == 180 ? h : w;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int nx = 0, ny = 0;
      if (clockwise == 90) {
        nx = h - 1 - y;
        ny = x;
      } else if (clockwise == 180) {
        nx = w - 1 - x;
        ny = h - 1 - y;
      } else {
        nx = y;
        ny = w - 1 - x;
      }
      std::memcpy(&out[(static_cast<std::size_t>(ny) * nw + nx) * 4],
                  &px[(static_cast<std::size_t>(y) * w + x) * 4], 4);
    }
  }
  px.swap(out);
  w = nw;
  h = nh;
}

}  // namespace mv::edit::clip::detail
