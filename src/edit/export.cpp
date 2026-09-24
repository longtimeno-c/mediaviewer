// SPDX-License-Identifier: GPL-2.0-or-later
#include "edit/export.h"

#include <cstring>
#include <new>

#include "codec/decode.h"
#include "codec/exif.h"
#include "codec/format.h"
#include "edit/bake.h"
#include "edit/geometry.h"
#include "edit/lossless_jpeg.h"
#include "image/linear.h"

namespace mv::edit {
namespace {

bool assign(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> in) noexcept {
  try {
    out.assign(in.begin(), in.end());
    return true;
  } catch (const std::bad_alloc&) {
    out.clear();
    return false;
  }
}

std::uint32_t be32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
         static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

// eXIf and an uncompressed iTXt XML:com.adobe.xmp, from a PNG's chunks.
void png_metadata(std::span<const std::uint8_t> png, metadata_blobs& out) {
  std::size_t p = 8;
  while (p + 12 <= png.size()) {
    const std::uint32_t len = be32(png.data() + p);
    if (len > png.size() - p - 12) return;
    const char* type = reinterpret_cast<const char*>(png.data() + p + 4);
    const auto data = png.subspan(p + 8, len);
    if (std::memcmp(type, "eXIf", 4) == 0 && out.exif.empty()) {
      (void)assign(out.exif, data);
    } else if (std::memcmp(type, "iTXt", 4) == 0 && out.xmp.empty()) {
      constexpr char kKey[] = "XML:com.adobe.xmp";
      if (len > sizeof(kKey) + 2 && std::memcmp(data.data(), kKey, sizeof(kKey)) == 0 &&
          data[sizeof(kKey)] == 0) {  // compression flag: uncompressed only
        std::size_t q = sizeof(kKey) + 2;
        for (int field = 0; field < 2 && q < data.size(); ++q) {  // language, translated keyword
          if (data[q] == 0) ++field;
        }
        if (q <= data.size()) (void)assign(out.xmp, data.subspan(q));
      }
    } else if (std::memcmp(type, "IDAT", 4) == 0 || std::memcmp(type, "IEND", 4) == 0) {
      // Metadata after the image data is allowed but rare; stop at IEND.
      if (std::memcmp(type, "IEND", 4) == 0) return;
    }
    p += 12 + static_cast<std::size_t>(len);
  }
}

// Orientation and size of what is actually written, and a thumbnail that
// would disagree with it unlinked.
void patch_for_output(metadata_blobs& m, std::uint32_t w, std::uint32_t h, bool changed) {
  if (!m.exif.empty()) {
    std::span<std::uint8_t> tiff(m.exif);
    (void)codec::exif_set_orientation(tiff, 1);
    (void)codec::exif_set_pixel_dimensions(tiff, w, h);
    if (changed) (void)codec::exif_drop_thumbnail(tiff);
  }
  if (!m.xmp.empty()) (void)codec::xmp_set_orientation(m.xmp, 1);
}

}  // namespace

metadata_blobs source_metadata(std::span<const std::uint8_t> source, metadata_policy policy) {
  metadata_blobs m;
  if (policy == metadata_policy::none) return m;
  switch (codec::probe(source)) {
    case codec::format_family::jpeg:
      if (const auto r = codec::find_jpeg_exif(source)) (void)assign(m.exif, source.subspan(r->offset, r->size));
      if (const auto r = codec::find_jpeg_xmp(source)) (void)assign(m.xmp, source.subspan(r->offset, r->size));
      break;
    case codec::format_family::png:
      png_metadata(source, m);
      break;
    default:
      break;
  }
  if (policy == metadata_policy::minus_gps) {
    if (!m.exif.empty() && !codec::exif_strip_gps(m.exif)) m.exif.clear();  // unwalkable: drop, don't leak
    if (!m.xmp.empty() && codec::xmp_has_gps(m.xmp)) m.xmp.clear();
  }
  return m;
}

result<export_result> export_image(std::span<const std::uint8_t> source, const geometry& stack,
                                   const export_options& opt, const job_context* ctx) {
  return export_image(source, stack, colour{}, opt, ctx);
}

result<export_result> export_image(std::span<const std::uint8_t> source, const geometry& stack,
                                   const colour& c, const export_options& opt,
                                   const job_context* ctx) {
  geometry g = stack;
  if (opt.long_edge != 0) g.resize = resize_spec{resize_mode::long_edge, opt.long_edge, 0, 100.0f};

  // PR 11 bake: colour changes every pixel, so there is no lossless path.
  if (!c.identity()) {
    MV_TRY(image::linear_image working, image::decode_linear(source, ctx));
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    const placement p = place(g, size2{working.width, working.height});
    MV_TRY(codec::raster out, bake(working, p, uniforms_of(c), ctx));
    working = image::linear_image{};  // release the full frame before encoding
    metadata_blobs meta = source_metadata(source, opt.policy);
    patch_for_output(meta, out.width, out.height, true);
    MV_TRY(std::vector<std::uint8_t> bytes, encode(out, opt.encode, meta));
    export_result r;
    r.bytes = std::move(bytes);
    r.lossless = false;
    r.width = out.width;
    r.height = out.height;
    return r;
  }
  const bool is_jpeg = codec::probe(source) == codec::format_family::jpeg;

  // Lossless: JPEG → JPEG with no straighten and no resize.
  if (is_jpeg && opt.prefer_lossless && opt.encode.format == image_format::jpeg &&
      g.straighten == 0.0f && g.resize.mode == resize_mode::none) {
    if (auto layout = read_layout(source)) {
      const placement p = place(g, size2{layout->width, layout->height},
                                codec::from_exif(layout->orientation));
      lossless_request req;
      req.transform = p.total;
      req.policy = opt.policy;
      if (!(g.crop == rect{})) {
        req.crop_x = p.crop_x;
        req.crop_y = p.crop_y;
        req.crop_w = p.cropped.w;
        req.crop_h = p.cropped.h;
      }
      if (lossless_possible(*layout, req)) {
        MV_TRY(std::vector<std::uint8_t> bytes, transform(source, req));
        export_result r;
        r.bytes = std::move(bytes);
        r.lossless = true;
        r.width = p.cropped.w;
        r.height = p.cropped.h;
        return r;
      }
    }
  }

  // Re-encode. The decoded raster is what the viewer shows (already
  // oriented), so the geometry applies with no base rotation.
  MV_TRY(codec::raster decoded, codec::decode(source, ctx));
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  const placement p = place(g, size2{decoded.width, decoded.height});
  MV_TRY(codec::raster out, render(decoded, p, ctx));
  decoded = codec::raster{};  // release the full frame before encoding

  metadata_blobs meta = source_metadata(source, opt.policy);
  patch_for_output(meta, out.width, out.height, !g.identity());
  MV_TRY(std::vector<std::uint8_t> bytes, encode(out, opt.encode, meta));
  export_result r;
  r.bytes = std::move(bytes);
  r.lossless = false;
  r.width = out.width;
  r.height = out.height;
  return r;
}

std::string export_file_name(std::string_view source_name, image_format format) {
  std::size_t dot = source_name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) dot = source_name.size();
  std::string out(source_name.substr(0, dot));
  out += "-edit";
  out += format == image_format::png ? ".png" : ".jpg";
  return out;
}

}  // namespace mv::edit
