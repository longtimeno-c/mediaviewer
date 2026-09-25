// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// AVIF via libavif + dav1d (BSD). Still and animated (`avis`) sequences
// (plan/04).
//
// libavif does NOT apply the container transforms, so clap → irot → imir are
// applied here, in that order (ISO/IEC 23008-12), to match libheif's HEIC
// output. Colour follows codec/cicp.h: ICC wins for SDR, nclx/AV1 CICP P3 gets
// a synthesised ICC, PQ/HLG are tone-mapped to SDR. Gain maps are ignored.
#include "codec/decode.h"

#include <avif/avif.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include "codec/cicp.h"

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;

status map_result(avifResult r) noexcept {
  switch (r) {
    case AVIF_RESULT_OK: return status::ok;
    case AVIF_RESULT_OUT_OF_MEMORY: return status::out_of_memory;
    case AVIF_RESULT_NO_CODEC_AVAILABLE:
    case AVIF_RESULT_NOT_IMPLEMENTED:
    case AVIF_RESULT_INVALID_FTYP:
      return status::unsupported_format;
    default: return status::corrupt;
  }
}

struct decoder_deleter {
  void operator()(avifDecoder* d) const noexcept { avifDecoderDestroy(d); }
};
using decoder_ptr = std::unique_ptr<avifDecoder, decoder_deleter>;

// --- container transforms on packed RGBA8 ------------------------------------

void crop(std::vector<std::uint8_t>& rgba, std::uint32_t& w, std::uint32_t& h,
          const avifCropRect& r) {
  if (r.x == 0 && r.y == 0 && r.width == w && r.height == h) return;
  std::vector<std::uint8_t> out(static_cast<std::size_t>(r.width) * r.height * 4);
  for (std::uint32_t y = 0; y < r.height; ++y) {
    std::memcpy(out.data() + static_cast<std::size_t>(y) * r.width * 4,
                rgba.data() + (static_cast<std::size_t>(r.y + y) * w + r.x) * 4,
                static_cast<std::size_t>(r.width) * 4);
  }
  rgba.swap(out);
  w = r.width;
  h = r.height;
}

// irot: `angle` quarter turns anti-clockwise.
void rotate_ccw(std::vector<std::uint8_t>& rgba, std::uint32_t& w, std::uint32_t& h,
                std::uint8_t angle) {
  angle &= 3;
  if (angle == 0) return;
  const std::uint32_t nw = angle == 2 ? w : h;
  const std::uint32_t nh = angle == 2 ? h : w;
  std::vector<std::uint8_t> out(rgba.size());
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint32_t dx = 0, dy = 0;
      switch (angle) {
        case 1: dx = y; dy = w - 1 - x; break;          // 90° ccw
        case 2: dx = w - 1 - x; dy = h - 1 - y; break;  // 180°
        default: dx = h - 1 - y; dy = x; break;         // 270° ccw
      }
      std::memcpy(out.data() + (static_cast<std::size_t>(dy) * nw + dx) * 4,
                  rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4, 4);
    }
  }
  rgba.swap(out);
  w = nw;
  h = nh;
}

// imir: axis 0 exchanges top and bottom, axis 1 left and right (23008-12:2022).
void mirror(std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h, std::uint8_t axis) {
  const std::size_t stride = static_cast<std::size_t>(w) * 4;
  if (axis == 0) {
    std::vector<std::uint8_t> row(stride);
    for (std::uint32_t y = 0; y < h / 2; ++y) {
      std::uint8_t* a = rgba.data() + y * stride;
      std::uint8_t* b = rgba.data() + (h - 1 - y) * stride;
      std::memcpy(row.data(), a, stride);
      std::memcpy(a, b, stride);
      std::memcpy(b, row.data(), stride);
    }
  } else {
    for (std::uint32_t y = 0; y < h; ++y) {
      std::uint8_t* r = rgba.data() + y * stride;
      for (std::uint32_t x = 0; x < w / 2; ++x) {
        std::uint8_t px[4];
        std::memcpy(px, r + x * 4, 4);
        std::memcpy(r + x * 4, r + (w - 1 - x) * 4, 4);
        std::memcpy(r + (w - 1 - x) * 4, px, 4);
      }
    }
  }
}

struct frame_out {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;
};

// decoder->image (just decoded) → displayed RGBA8, transforms applied.
result<frame_out> convert(const avifDecoder* decoder, const job_context* job) {
  const avifImage* image = decoder->image;
  if (!image || image->width == 0 || image->height == 0) return err(status::corrupt);
  if (image->width > kMaxDim || image->height > kMaxDim ||
      static_cast<std::uint64_t>(image->width) * image->height > kMaxPixels) {
    return err(status::unsupported_format);
  }
  const bool hdr = image->icc.size == 0 && cicp::is_hdr(image->transferCharacteristics);
  const bool high_bit = image->depth > 8 || hdr;

  avifRGBImage rgb;
  avifRGBImageSetDefaults(&rgb, image);
  rgb.format = AVIF_RGB_FORMAT_RGBA;
  rgb.depth = high_bit ? (image->depth > 8 ? image->depth : 16) : 8;
  rgb.maxThreads = 1;
  const std::size_t w = image->width;
  const std::size_t h = image->height;
  const std::size_t bytes_per_channel = rgb.depth > 8 ? 2 : 1;
  rgb.rowBytes = static_cast<std::uint32_t>(w * 4 * bytes_per_channel);

  frame_out out;
  out.width = image->width;
  out.height = image->height;
  out.rgba.resize(w * h * 4);
  std::vector<std::uint16_t> wide;
  if (bytes_per_channel == 2) {
    wide.resize(w * h * 4);
    rgb.pixels = reinterpret_cast<std::uint8_t*>(wide.data());
  } else {
    rgb.pixels = out.rgba.data();
  }
  const avifResult r = avifImageYUVToRGB(image, &rgb);
  if (r != AVIF_RESULT_OK) return err(map_result(r));
  if (job && job->cancelled()) return err(status::cancelled);

  if (bytes_per_channel == 2) {
    if (hdr) {
      cicp::hdr_to_sdr map;
      if (!map.init(image->colorPrimaries, image->transferCharacteristics, rgb.depth)) {
        return err(status::unsupported_format);
      }
      for (std::size_t y = 0; y < h; ++y) {
        if ((y & 63) == 0 && job && job->cancelled()) return err(status::cancelled);
        map.row(wide.data() + y * w * 4, w, out.rgba.data() + y * w * 4);
      }
    } else {
      cicp::samples_to_8bit(wide.data(), wide.size(), rgb.depth, out.rgba.data());
    }
  }

  // clap is applied only when valid; an invalid clap is ignored, as browsers do.
  if (image->transformFlags & AVIF_TRANSFORM_CLAP) {
    avifCropRect rect{};
    avifDiagnostics diag{};
    if (avifCropRectFromCleanApertureBox(&rect, &image->clap, image->width, image->height, &diag) &&
        rect.width > 0 && rect.height > 0) {
      crop(out.rgba, out.width, out.height, rect);
    }
  }
  if (image->transformFlags & AVIF_TRANSFORM_IROT) {
    rotate_ccw(out.rgba, out.width, out.height, image->irot.angle);
  }
  if (image->transformFlags & AVIF_TRANSFORM_IMIR) {
    mirror(out.rgba, out.width, out.height, image->imir.axis);
  }
  return out;
}

struct avif_colour {
  std::vector<std::uint8_t> icc;
  bool tagged_srgb = false;
};

avif_colour colour_of(const avifImage* image) {
  avif_colour c;
  if (image->icc.size > 0) {
    c.icc.assign(image->icc.data, image->icc.data + image->icc.size);
  } else if (!cicp::is_hdr(image->transferCharacteristics)) {
    cicp::sdr_tag tag = cicp::tag_sdr(image->colorPrimaries, image->transferCharacteristics);
    c.icc = std::move(tag.icc);
    c.tagged_srgb = tag.tagged_srgb;
  }  // HDR: tone-mapped to sRGB in convert(), untagged.
  return c;
}

class avif_source final : public animation_source {
 public:
  avif_source(std::span<const std::uint8_t> bytes, std::shared_ptr<const void> keepalive) noexcept
      : bytes_(bytes), keepalive_(std::move(keepalive)) {}

  [[nodiscard]] expected open() {
    decoder_.reset(avifDecoderCreate());
    if (!decoder_) return err(status::out_of_memory);
    avifDecoder* d = decoder_.get();
    d->maxThreads = 1;  // already on a decode worker
    d->imageSizeLimit = static_cast<std::uint32_t>(kMaxPixels);
    d->imageDimensionLimit = kMaxDim;
    // pixi is missing from many real encoders' output; browsers accept it.
    d->strictFlags = AVIF_STRICT_CLAP_VALID | AVIF_STRICT_ALPHA_ISPE_REQUIRED;
    d->ignoreExif = AVIF_TRUE;
    d->ignoreXMP = AVIF_TRUE;
    avifResult r = avifDecoderSetIOMemory(d, bytes_.data(), bytes_.size());
    if (r != AVIF_RESULT_OK) return err(map_result(r));
    r = avifDecoderParse(d);
    if (r != AVIF_RESULT_OK) {
      const status s = map_result(r);
      return err(s == status::unsupported_format ? s : status::corrupt);
    }
    if (d->imageCount < 1 || !d->image) return err(status::corrupt);
    if (d->image->width > kMaxDim || d->image->height > kMaxDim ||
        static_cast<std::uint64_t>(d->image->width) * d->image->height > kMaxPixels) {
      return err(status::unsupported_format);
    }
    // Displayed size: the transforms can swap or shrink it.
    std::uint32_t w = d->image->width, h = d->image->height;
    if (d->image->transformFlags & AVIF_TRANSFORM_CLAP) {
      avifCropRect rect{};
      avifDiagnostics diag{};
      if (avifCropRectFromCleanApertureBox(&rect, &d->image->clap, w, h, &diag) && rect.width &&
          rect.height) {
        w = rect.width;
        h = rect.height;
      }
    }
    if ((d->image->transformFlags & AVIF_TRANSFORM_IROT) && (d->image->irot.angle & 1)) {
      std::swap(w, h);
    }
    info_.width = w;
    info_.height = h;
    info_.format = format_family::avif;
    info_.frame_count = static_cast<std::uint32_t>(d->imageCount);
    // repetitionCount counts repeats after the first play. UNKNOWN (no edit
    // list) loops forever, as Chromium does for avis.
    if (d->repetitionCount == AVIF_REPETITION_COUNT_INFINITE ||
        d->repetitionCount == AVIF_REPETITION_COUNT_UNKNOWN || d->repetitionCount < 0) {
      info_.loops = 0;
    } else {
      info_.loops = static_cast<std::uint32_t>(d->repetitionCount) + 1u;
    }
    avif_colour c = colour_of(d->image);
    info_.icc = std::move(c.icc);
    info_.tagged_srgb = c.tagged_srgb;
    next_index_ = 0;
    return {};
  }

  [[nodiscard]] const animation_info& info() const noexcept override { return info_; }

  [[nodiscard]] result<bool> next(canvas_frame& out, const job_context* ctx) override {
    if (!decoder_) return err(status::internal);
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    if (next_index_ >= static_cast<std::uint32_t>(decoder_->imageCount)) return false;
    try {
      const avifResult r = avifDecoderNextImage(decoder_.get());
      if (r != AVIF_RESULT_OK) {
        if (next_index_ == 0) return err(map_result(r) == status::out_of_memory ? status::out_of_memory : status::corrupt);
        return false;  // truncated: the play ends at the last good frame
      }
      auto frame = convert(decoder_.get(), ctx);
      if (!frame) return err(frame.error());
      if (frame.value().width != info_.width || frame.value().height != info_.height) {
        if (next_index_ == 0) return err(status::corrupt);
        return false;
      }
      out.rgba = std::move(frame.value().rgba);
      const double ms = decoder_->imageTiming.duration * 1000.0;
      out.delay_ms = browser_frame_delay_ms(
          ms > 0 && ms < 4.0e9 ? static_cast<std::uint32_t>(std::lround(ms)) : 0u);
      out.index = next_index_++;
      return true;
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
  }

  [[nodiscard]] expected rewind() override {
    if (!decoder_) return err(status::internal);
    const avifResult r = avifDecoderReset(decoder_.get());
    if (r != AVIF_RESULT_OK) return err(map_result(r));
    next_index_ = 0;
    return {};
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::shared_ptr<const void> keepalive_;
  decoder_ptr decoder_;
  animation_info info_{};
  std::uint32_t next_index_ = 0;
};

}  // namespace

result<raster> decode_avif(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::avif) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  try {
    avif_source source(bytes, nullptr);
    if (auto opened = source.open(); !opened) return err(opened.error());
    canvas_frame frame;
    auto got = source.next(frame, ctx);
    if (!got) return err(got.error());
    if (!got.value()) return err(status::corrupt);
    raster out;
    out.width = source.info().width;
    out.height = source.info().height;
    out.format = format_family::avif;
    out.intent = transfer_intent::display_referred;
    out.icc = source.info().icc;
    out.tagged_srgb = source.info().tagged_srgb;
    out.rgba = std::move(frame.rgba);
    return out;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

result<std::unique_ptr<animation_source>> open_avif_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
  if (!bytes) return err(status::invalid_arg);
  if (probe(*bytes) != format_family::avif) return err(status::unsupported_format);
  try {
    const std::span<const std::uint8_t> view(*bytes);
    auto source = std::make_unique<avif_source>(view, std::move(bytes));
    if (auto opened = source->open(); !opened) return err(opened.error());
    if (source->info().frame_count < 2) return err(status::unsupported_format);  // a still
    return std::unique_ptr<animation_source>(std::move(source));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

}  // namespace mv::codec
