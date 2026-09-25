// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// GIF (still and animated) via giflib (MIT). Decodes one frame at a time from
// memory — never DGifSlurp, which would hold every frame's index buffer at
// once — and composites onto the logical screen with codec::compositor.
//
// Browser behaviour where GIF leaves room: a frame that runs past the logical
// screen is clipped, "restore to background" clears to transparent (not the
// background colour), a missing NETSCAPE2.0 extension plays once, and delays
// follow browser_frame_delay_ms.
#include "codec/decode.h"

#include <gif_lib.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;

struct reader {
  std::span<const std::uint8_t> bytes;
  std::size_t pos = 0;
};

int read_bytes(GifFileType* gif, GifByteType* buf, int len) {
  auto* r = static_cast<reader*>(gif->UserData);
  if (!r || len <= 0) return 0;
  const std::size_t n =
      std::min<std::size_t>(static_cast<std::size_t>(len), r->bytes.size() - r->pos);
  std::memcpy(buf, r->bytes.data() + r->pos, n);
  r->pos += n;
  return static_cast<int>(n);
}

GraphicsControlBlock no_control() noexcept {
  GraphicsControlBlock gcb{};
  gcb.DisposalMode = DISPOSAL_UNSPECIFIED;
  gcb.DelayTime = 0;
  gcb.TransparentColor = NO_TRANSPARENT_COLOR;
  return gcb;
}

// One frame's colour indices (desc.Width x desc.Height), handling interlace.
bool read_indices(GifFileType* gif, const GifImageDesc& desc, std::vector<GifByteType>& indices) {
  const auto w = static_cast<std::size_t>(desc.Width);
  const auto h = static_cast<std::size_t>(desc.Height);
  indices.assign(w * h, 0);
  if (!desc.Interlace) {
    for (std::size_t y = 0; y < h; ++y) {
      if (DGifGetLine(gif, indices.data() + y * w, static_cast<int>(w)) == GIF_ERROR) return false;
    }
    return true;
  }
  static constexpr std::size_t kOffset[4] = {0, 4, 2, 1};
  static constexpr std::size_t kStep[4] = {8, 8, 4, 2};
  for (int pass = 0; pass < 4; ++pass) {
    for (std::size_t y = kOffset[pass]; y < h; y += kStep[pass]) {
      if (DGifGetLine(gif, indices.data() + y * w, static_cast<int>(w)) == GIF_ERROR) return false;
    }
  }
  return true;
}

class gif_source final : public animation_source {
 public:
  gif_source(std::span<const std::uint8_t> bytes, std::shared_ptr<const void> keepalive) noexcept
      : bytes_(bytes), keepalive_(std::move(keepalive)) {}
  ~gif_source() override { close(); }

  gif_source(const gif_source&) = delete;
  gif_source& operator=(const gif_source&) = delete;

  [[nodiscard]] expected open() {
    close();
    in_ = reader{bytes_, 0};
    int error = 0;
    gif_ = DGifOpen(&in_, read_bytes, &error);
    if (!gif_) return err(status::corrupt);
    const auto w = static_cast<std::uint32_t>(std::max(gif_->SWidth, 0));
    const auto h = static_cast<std::uint32_t>(std::max(gif_->SHeight, 0));
    if (w == 0 || h == 0) return err(status::corrupt);
    if (w > kMaxDim || h > kMaxDim || static_cast<std::uint64_t>(w) * h > kMaxPixels) {
      return err(status::unsupported_format);
    }
    info_.width = w;
    info_.height = h;
    info_.format = format_family::gif;
    info_.loops = 1;  // no NETSCAPE2.0 extension: play once; re-read every pass
    if (!canvas_.reset(w, h)) return err(status::out_of_memory);
    gcb_ = no_control();
    next_index_ = 0;
    return {};
  }

  [[nodiscard]] const animation_info& info() const noexcept override { return info_; }

  [[nodiscard]] result<bool> next(canvas_frame& out, const job_context* ctx) override {
    if (!gif_) return err(status::internal);
    try {
      for (;;) {
        if (ctx && ctx->cancelled()) return err(status::cancelled);
        GifRecordType type = UNDEFINED_RECORD_TYPE;
        if (DGifGetRecordType(gif_, &type) == GIF_ERROR || type == TERMINATE_RECORD_TYPE) {
          return end_of_play();
        }

        if (type == EXTENSION_RECORD_TYPE) {
          int code = 0;
          GifByteType* ext = nullptr;
          if (DGifGetExtension(gif_, &code, &ext) == GIF_ERROR) return end_of_play();
          const bool looping = code == APPLICATION_EXT_FUNC_CODE && ext && ext[0] == 11 &&
                               (std::memcmp(ext + 1, "NETSCAPE2.0", 11) == 0 ||
                                std::memcmp(ext + 1, "ANIMEXTS1.0", 11) == 0);
          if (code == GRAPHICS_EXT_FUNC_CODE && ext && ext[0] >= 4) {
            if (DGifExtensionToGCB(ext[0], ext + 1, &gcb_) == GIF_ERROR) gcb_ = no_control();
          }
          while (ext) {
            if (DGifGetExtensionNext(gif_, &ext) == GIF_ERROR) return end_of_play();
            if (looping && ext && ext[0] >= 3 && ext[1] == 1) {
              info_.loops = static_cast<std::uint32_t>(ext[2] | (ext[3] << 8));  // 0 = forever
            }
          }
          continue;
        }

        if (type != IMAGE_DESC_RECORD_TYPE) continue;
        if (DGifGetImageDesc(gif_) == GIF_ERROR) return end_of_play();
        const GifImageDesc desc = gif_->Image;
        if (desc.Width <= 0 || desc.Height <= 0 ||
            static_cast<std::uint64_t>(desc.Width) * static_cast<std::uint64_t>(desc.Height) >
                kMaxPixels) {
          return end_of_play();
        }
        const ColorMapObject* map = desc.ColorMap ? desc.ColorMap : gif_->SColorMap;
        if (!read_indices(gif_, desc, indices_)) return end_of_play();
        if (!map) {
          gcb_ = no_control();
          continue;  // no palette: nothing drawable in this frame
        }

        // Clip the frame to the logical screen.
        const auto left = static_cast<std::uint32_t>(std::max(desc.Left, 0));
        const auto top = static_cast<std::uint32_t>(std::max(desc.Top, 0));
        const auto right = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(left) + static_cast<std::uint64_t>(desc.Width), info_.width));
        const auto bottom = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(top) + static_cast<std::uint64_t>(desc.Height), info_.height));
        if (left < right && top < bottom) {
          frame_region region;
          region.x = left;
          region.y = top;
          region.width = right - left;
          region.height = bottom - top;
          region.dispose = gcb_.DisposalMode == DISPOSE_BACKGROUND ? dispose_op::background
                           : gcb_.DisposalMode == DISPOSE_PREVIOUS ? dispose_op::previous
                                                                    : dispose_op::none;
          region.blend = blend_op::over;  // transparent pixels show what is underneath
          rgba_.assign(static_cast<std::size_t>(region.width) * region.height * 4, 0);
          for (std::uint32_t y = 0; y < region.height; ++y) {
            for (std::uint32_t x = 0; x < region.width; ++x) {
              const int index = indices_[static_cast<std::size_t>(y) * desc.Width + x];
              if (index == gcb_.TransparentColor || index >= map->ColorCount) continue;  // alpha 0
              std::uint8_t* px =
                  rgba_.data() + (static_cast<std::size_t>(y) * region.width + x) * 4;
              px[0] = map->Colors[index].Red;
              px[1] = map->Colors[index].Green;
              px[2] = map->Colors[index].Blue;
              px[3] = 255;
            }
          }
          if (!canvas_.draw(region, rgba_)) return err(status::corrupt);
        }

        out.rgba.assign(canvas_.pixels().begin(), canvas_.pixels().end());
        out.delay_ms = gif_delay_ms(static_cast<std::uint16_t>(std::max(gcb_.DelayTime, 0)));
        out.index = next_index_++;
        gcb_ = no_control();
        return true;
      }
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
  }

  [[nodiscard]] expected rewind() override { return open(); }

 private:
  [[nodiscard]] result<bool> end_of_play() noexcept {
    if (next_index_ == 0) return err(status::corrupt);
    info_.frame_count = next_index_;
    return false;
  }

  void close() noexcept {
    if (gif_) {
      int error = 0;
      (void)DGifCloseFile(gif_, &error);
      gif_ = nullptr;
    }
  }

  std::span<const std::uint8_t> bytes_;
  std::shared_ptr<const void> keepalive_;
  reader in_{};
  GifFileType* gif_ = nullptr;
  animation_info info_{};
  compositor canvas_;
  GraphicsControlBlock gcb_ = no_control();
  std::vector<GifByteType> indices_;
  std::vector<std::uint8_t> rgba_;
  std::uint32_t next_index_ = 0;
};

}  // namespace

result<raster> decode_gif(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::gif) return err(status::unsupported_format);
  try {
    gif_source source(bytes, nullptr);
    if (auto opened = source.open(); !opened) return err(opened.error());
    canvas_frame frame;
    auto got = source.next(frame, ctx);
    if (!got) return err(got.error());
    if (!got.value()) return err(status::corrupt);
    raster out;
    out.width = source.info().width;
    out.height = source.info().height;
    out.format = format_family::gif;
    out.intent = transfer_intent::display_referred;
    out.rgba = std::move(frame.rgba);
    return out;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

result<std::unique_ptr<animation_source>> open_gif_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
  if (!bytes) return err(status::invalid_arg);
  if (probe(*bytes) != format_family::gif) return err(status::unsupported_format);
  try {
    const std::span<const std::uint8_t> view(*bytes);
    auto source = std::make_unique<gif_source>(view, std::move(bytes));
    if (auto opened = source->open(); !opened) return err(opened.error());
    return std::unique_ptr<animation_source>(std::move(source));
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

}  // namespace mv::codec
