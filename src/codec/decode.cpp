// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/decode.h"
#include "codec/crash_test_hook.h"

#include <new>

namespace mv::codec {

result<raster> decode(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  // D3 (policy in codec/os_decode.h): the OS codec is offered HEIC stills only —
  // the one camera-dump format where the OS path can be hardware-backed and
  // still match the bundled colour and orientation. Every other format goes
  // straight to its bundled decoder. Any OS failure except cancellation falls
  // through silently: on a clean VM WIC's HEIF decoder exists but has no HEVC
  // codec, and a corrupt HEIC is then judged by libheif, not by WIC.
  if (probe(bytes) == format_family::heic) {
    if (auto os = try_os_decode(bytes, ctx)) return os;
    else if (os.error() == status::cancelled) return err(status::cancelled);
  }

  // PR 7 crash reporting: annotate the worker slot; MV_CRASH_TEST verify hook.
  const decode_crash_scope crash_scope(bytes, ctx);
  switch (probe(bytes)) {
    case format_family::jpeg: return decode_jpeg(bytes, ctx);
    case format_family::png:  return decode_png(bytes, ctx);
    case format_family::bmp:  return decode_bmp(bytes, ctx);
    case format_family::gif:  return decode_gif(bytes, ctx);
    case format_family::webp: return decode_webp(bytes, ctx);
    case format_family::tiff:
      // CR2/NEF/ARW/DNG share the TIFF magic. LibRaw wins so a camera file is
      // never walked as a generic TIFF (and never rewritten).
      if (looks_like_raw(bytes)) return decode_raw(bytes, ctx);
      return decode_tiff(bytes, ctx);
    case format_family::ico:  return decode_ico(bytes, ctx);
    case format_family::heic: return decode_heic(bytes, ctx);
    case format_family::avif: return decode_avif(bytes, ctx);
    case format_family::raw:  return decode_raw(bytes, ctx);
    case format_family::unknown:
      if (looks_like_raw(bytes)) return decode_raw(bytes, ctx);
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
    case format_family::heic: return open_heic_animation(std::move(bytes));
    case format_family::avif: return open_avif_animation(std::move(bytes));
    case format_family::jpeg:
    case format_family::bmp:
    case format_family::tiff:
    case format_family::ico:
    case format_family::raw:
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
