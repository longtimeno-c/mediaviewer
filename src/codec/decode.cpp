// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/decode.h"

#include <new>

namespace mv::codec {

result<raster> decode(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  switch (probe(bytes)) {
    case format_family::jpeg: return decode_jpeg(bytes, ctx);
    case format_family::png:  return decode_png(bytes, ctx);
    case format_family::bmp:  return decode_bmp(bytes, ctx);
    case format_family::gif:  return decode_gif(bytes, ctx);
    case format_family::webp: return decode_webp(bytes, ctx);
    case format_family::unknown:
      return err(status::unsupported_format);
  }
  return err(status::unsupported_format);
}

result<std::unique_ptr<animation_source>> open_animation(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
  if (!bytes) return err(status::invalid_arg);
  switch (probe(*bytes)) {
    case format_family::gif:  return open_gif_animation(std::move(bytes));
    case format_family::webp: return open_webp_animation(std::move(bytes));
    case format_family::png:  return open_apng_animation(std::move(bytes));
    case format_family::jpeg:
    case format_family::bmp:
    case format_family::unknown:
      return err(status::unsupported_format);
  }
  return err(status::unsupported_format);
}

result<animation_frames> decode_animation(std::span<const std::uint8_t> bytes,
                                          const job_context* ctx, std::size_t max_bytes) {
  try {
    auto shared = std::make_shared<const std::vector<std::uint8_t>>(bytes.begin(), bytes.end());
    auto opened = open_animation(std::move(shared));
    if (!opened) return err(opened.error());
    animation_source& source = *opened.value();

    animation_frames out;
    canvas_frame frame;
    for (;;) {
      auto more = source.next(frame, ctx);
      if (!more) return err(more.error());
      if (!more.value()) break;
      if ((out.frames.size() + 1) * frame.rgba.size() > max_bytes) {
        return err(status::unsupported_format);
      }
      out.frames.push_back(frame.rgba);
      out.delays_ms.push_back(frame.delay_ms);
    }
    if (out.frames.size() < 2) return err(status::unsupported_format);
    const animation_info& info = source.info();
    out.width = info.width;
    out.height = info.height;
    out.loops = info.loops;
    out.format = info.format;
    out.icc = info.icc;
    out.tagged_srgb = info.tagged_srgb;
    return out;
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
}

}  // namespace mv::codec
