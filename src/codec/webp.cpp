// SPDX-License-Identifier: GPL-2.0-or-later
// WebP (still and animated) via libwebp (BSD-3). WebPAnimDecoder handles both —
// a still is a one-frame animation — composites onto the canvas itself, and
// hands back straight-alpha RGBA one frame at a time with cumulative
// timestamps. Rewind is WebPAnimDecoderReset.
#include "codec/decode.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <new>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 16383;  // the format's own limit
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;

class webp_source final : public animation_source {
 public:
  webp_source(std::span<const std::uint8_t> bytes, std::shared_ptr<const void> keepalive) noexcept
      : bytes_(bytes), keepalive_(std::move(keepalive)) {}
  ~webp_source() override {
    if (decoder_) WebPAnimDecoderDelete(decoder_);
  }

  webp_source(const webp_source&) = delete;
  webp_source& operator=(const webp_source&) = delete;

  [[nodiscard]] expected open() {
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) return err(status::internal);
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;  // already on a decode worker
    const WebPData data{bytes_.data(), bytes_.size()};
    decoder_ = WebPAnimDecoderNew(&data, &options);
    if (!decoder_) return err(status::corrupt);

    WebPAnimInfo anim{};
    if (!WebPAnimDecoderGetInfo(decoder_, &anim)) return err(status::corrupt);
    if (anim.canvas_width == 0 || anim.canvas_height == 0 || anim.frame_count == 0) {
      return err(status::corrupt);
    }
    if (anim.canvas_width > kMaxDim || anim.canvas_height > kMaxDim ||
        static_cast<std::uint64_t>(anim.canvas_width) * anim.canvas_height > kMaxPixels) {
      return err(status::unsupported_format);
    }
    info_.width = anim.canvas_width;
    info_.height = anim.canvas_height;
    info_.loops = anim.loop_count;
    info_.frame_count = anim.frame_count;
    info_.format = format_family::webp;
    if (const WebPDemuxer* demux = WebPAnimDecoderGetDemuxer(decoder_)) {
      WebPChunkIterator chunk;
      if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
        info_.icc.assign(chunk.chunk.bytes, chunk.chunk.bytes + chunk.chunk.size);
        WebPDemuxReleaseChunkIterator(&chunk);
      }
    }
    previous_ms_ = 0;
    next_index_ = 0;
    return {};
  }

  [[nodiscard]] const animation_info& info() const noexcept override { return info_; }

  [[nodiscard]] result<bool> next(canvas_frame& out, const job_context* ctx) override {
    if (!decoder_) return err(status::internal);
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    if (!WebPAnimDecoderHasMoreFrames(decoder_)) return false;
    try {
      std::uint8_t* pixels = nullptr;
      int timestamp_ms = 0;
      if (!WebPAnimDecoderGetNext(decoder_, &pixels, &timestamp_ms) || !pixels) {
        if (next_index_ == 0) return err(status::corrupt);
        return false;  // truncated: the play ends at the last good frame
      }
      const std::size_t frame_bytes = static_cast<std::size_t>(info_.width) * info_.height * 4;
      out.rgba.assign(pixels, pixels + frame_bytes);
      out.delay_ms = browser_frame_delay_ms(
          static_cast<std::uint32_t>(std::max(0, timestamp_ms - previous_ms_)));
      previous_ms_ = timestamp_ms;
      out.index = next_index_++;
      return true;
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
  }

  [[nodiscard]] expected rewind() override {
    if (!decoder_) return err(status::internal);
    WebPAnimDecoderReset(decoder_);
    previous_ms_ = 0;
    next_index_ = 0;
    return {};
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::shared_ptr<const void> keepalive_;
  WebPAnimDecoder* decoder_ = nullptr;
  animation_info info_{};
  int previous_ms_ = 0;
  std::uint32_t next_index_ = 0;
};

}  // namespace

result<raster> decode_webp(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::webp) return err(status::unsupported_format);
  try {
    webp_source source(bytes, nullptr);
    if (auto opened = source.open(); !opened) return err(opened.error());
    canvas_frame frame;
    auto got = source.next(frame, ctx);
    if (!got) return err(got.error());
    if (!got.value()) return err(status::corrupt);
    raster out;
    out.width = source.info().width;
    out.height = source.info().height;
    out.format = format_family::webp;
    out.intent = transfer_intent::display_referred;
    out.icc = source.info().icc;
    out.rgba = std::move(frame.rgba);
    return out;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

result<std::unique_ptr<animation_source>> open_webp_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
  if (!bytes) return err(status::invalid_arg);
  if (probe(*bytes) != format_family::webp) return err(status::unsupported_format);
  try {
    const std::span<const std::uint8_t> view(*bytes);
    auto source = std::make_unique<webp_source>(view, std::move(bytes));
    if (auto opened = source->open(); !opened) return err(opened.error());
    if (source->info().frame_count < 2) return err(status::unsupported_format);  // a still
    return std::unique_ptr<animation_source>(std::move(source));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

}  // namespace mv::codec
