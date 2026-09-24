// SPDX-License-Identifier: GPL-2.0-or-later
#include "edit/encode.h"

#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <jpeglib.h>
#include <spng.h>

namespace mv::edit {
namespace {

constexpr std::size_t kMaxSegment = 65533;  // APPn payload limit
constexpr char kXmpNs[] = "http://ns.adobe.com/xap/1.0/";  // + NUL

struct error_trap {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void error_exit(j_common_ptr cinfo) {
  auto* trap = reinterpret_cast<error_trap*>(cinfo->err);
  longjmp(trap->jump, 1);
}

// Everything the JPEG path allocates before its setjmp, so the jump handler
// can free it without running a destructor.
struct jpeg_scratch {
  unsigned char* out = nullptr;
  unsigned long out_size = 0;
  unsigned char* row = nullptr;
  unsigned char* app = nullptr;
};

result<std::vector<std::uint8_t>> encode_jpeg(const codec::raster& img, const encode_options& opt,
                                              const metadata_blobs& meta) {
  jpeg_compress_struct cinfo{};
  error_trap trap{};
  jpeg_scratch s{};
  cinfo.err = jpeg_std_error(&trap.pub);
  trap.pub.error_exit = error_exit;
  trap.pub.output_message = [](j_common_ptr) {};

  s.row = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(img.width) * 3));
  const std::size_t app_cap = 6 + std::max(meta.exif.size(), meta.xmp.size() + sizeof(kXmpNs));
  s.app = static_cast<unsigned char*>(std::malloc(app_cap));
  if (!s.row || !s.app) {
    std::free(s.row);
    std::free(s.app);
    return err(status::out_of_memory);
  }

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
  if (setjmp(trap.jump)) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    jpeg_destroy_compress(&cinfo);
    std::free(s.out);
    std::free(s.row);
    std::free(s.app);
    return err(status::internal);
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &s.out, &s.out_size);
  cinfo.image_width = img.width;
  cinfo.image_height = img.height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, std::clamp(opt.quality, 1, 100), TRUE);
  cinfo.optimize_coding = TRUE;
  const int h = opt.subsampling == chroma::s444 ? 1 : 2;
  const int v = opt.subsampling == chroma::s420 ? 2 : 1;
  cinfo.comp_info[0].h_samp_factor = h;
  cinfo.comp_info[0].v_samp_factor = v;
  for (int c = 1; c < 3; ++c) {
    cinfo.comp_info[c].h_samp_factor = 1;
    cinfo.comp_info[c].v_samp_factor = 1;
  }
  jpeg_start_compress(&cinfo, TRUE);

  if (!meta.exif.empty() && 6 + meta.exif.size() <= kMaxSegment) {
    std::memcpy(s.app, "Exif\0\0", 6);
    std::memcpy(s.app + 6, meta.exif.data(), meta.exif.size());
    jpeg_write_marker(&cinfo, JPEG_APP0 + 1, s.app, static_cast<unsigned>(6 + meta.exif.size()));
  }
  if (!meta.xmp.empty() && sizeof(kXmpNs) + meta.xmp.size() <= kMaxSegment) {
    std::memcpy(s.app, kXmpNs, sizeof(kXmpNs));
    std::memcpy(s.app + sizeof(kXmpNs), meta.xmp.data(), meta.xmp.size());
    jpeg_write_marker(&cinfo, JPEG_APP0 + 1, s.app,
                      static_cast<unsigned>(sizeof(kXmpNs) + meta.xmp.size()));
  }
  if (!img.icc.empty()) {
    jpeg_write_icc_profile(&cinfo, img.icc.data(), static_cast<unsigned>(img.icc.size()));
  }

  JSAMPROW rows[1] = {s.row};
  while (cinfo.next_scanline < cinfo.image_height) {
    const std::uint8_t* src =
        img.rgba.data() + static_cast<std::size_t>(cinfo.next_scanline) * img.width * 4;
    for (std::uint32_t x = 0; x < img.width; ++x) {
      const unsigned a = src[x * 4 + 3];
      for (int c = 0; c < 3; ++c) {
        const unsigned v8 = src[x * 4 + static_cast<std::uint32_t>(c)];
        // Over white: v*a + 255*(1-a), rounded.
        s.row[x * 3 + static_cast<std::uint32_t>(c)] =
            static_cast<unsigned char>((v8 * a + 255u * (255u - a) + 127u) / 255u);
      }
    }
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  std::free(s.row);
  std::free(s.app);

  std::vector<std::uint8_t> out;
  try {
    out.assign(s.out, s.out + s.out_size);
  } catch (const std::bad_alloc&) {
    std::free(s.out);
    return err(status::out_of_memory);
  }
  std::free(s.out);
  return out;
}

struct spng_guard {
  spng_ctx* ctx = nullptr;
  explicit spng_guard(spng_ctx* c) : ctx(c) {}
  ~spng_guard() {
    if (ctx) spng_ctx_free(ctx);
  }
  spng_guard(const spng_guard&) = delete;
  spng_guard& operator=(const spng_guard&) = delete;
};

result<std::vector<std::uint8_t>> encode_png(const codec::raster& img, const metadata_blobs& meta) {
  bool opaque = true;
  for (std::size_t i = 3; i < img.rgba.size(); i += 4) {
    if (img.rgba[i] != 255) {
      opaque = false;
      break;
    }
  }

  std::vector<std::uint8_t> rgb;
  const std::uint8_t* pixels = img.rgba.data();
  std::size_t pixel_bytes = img.rgba.size();
  if (opaque) {
    try {
      rgb.resize(static_cast<std::size_t>(img.width) * img.height * 3);
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
    for (std::size_t i = 0, j = 0; i < img.rgba.size(); i += 4, j += 3) {
      rgb[j] = img.rgba[i];
      rgb[j + 1] = img.rgba[i + 1];
      rgb[j + 2] = img.rgba[i + 2];
    }
    pixels = rgb.data();
    pixel_bytes = rgb.size();
  }

  spng_guard g(spng_ctx_new(SPNG_CTX_ENCODER));
  if (!g.ctx) return err(status::out_of_memory);
  if (spng_set_option(g.ctx, SPNG_ENCODE_TO_BUFFER, 1) != 0) return err(status::internal);

  spng_ihdr ihdr{};
  ihdr.width = img.width;
  ihdr.height = img.height;
  ihdr.bit_depth = 8;
  ihdr.color_type = opaque ? SPNG_COLOR_TYPE_TRUECOLOR : SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
  if (spng_set_ihdr(g.ctx, &ihdr) != 0) return err(status::invalid_arg);

  // libspng copies nothing: the buffers below must outlive the encode.
  std::vector<char> icc(img.icc.begin(), img.icc.end());
  if (!icc.empty()) {
    spng_iccp iccp{};
    std::memcpy(iccp.profile_name, "ICC Profile", 12);
    iccp.profile_len = icc.size();
    iccp.profile = icc.data();
    if (spng_set_iccp(g.ctx, &iccp) != 0) return err(status::internal);
  } else if (img.tagged_srgb) {
    if (spng_set_srgb(g.ctx, 0) != 0) return err(status::internal);
  }
  std::vector<char> exif(meta.exif.begin(), meta.exif.end());
  if (!exif.empty()) {
    spng_exif e{};
    e.length = exif.size();
    e.data = exif.data();
    // A block libspng judges malformed is left out, not fatal: the pixels
    // are the export.
    (void)spng_set_exif(g.ctx, &e);
  }
  // libspng measures the text with strlen: keep a terminator past `length`.
  std::vector<char> xmp(meta.xmp.begin(), meta.xmp.end());
  spng_text text{};
  if (!xmp.empty()) {
    xmp.push_back('\0');
    std::memcpy(text.keyword, "XML:com.adobe.xmp", 18);
    text.type = SPNG_ITXT;
    text.length = xmp.size() - 1;
    text.text = xmp.data();
    static char kEmpty[] = "";
    text.language_tag = kEmpty;
    text.translated_keyword = kEmpty;
    (void)spng_set_text(g.ctx, &text, 1);
  }

  if (spng_encode_image(g.ctx, pixels, pixel_bytes, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE) != 0) {
    return err(status::internal);
  }
  std::size_t size = 0;
  int error = 0;
  void* buf = spng_get_png_buffer(g.ctx, &size, &error);
  if (!buf || error != 0) {
    std::free(buf);
    return err(status::internal);
  }
  std::vector<std::uint8_t> out;
  try {
    out.assign(static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + size);
  } catch (const std::bad_alloc&) {
    std::free(buf);
    return err(status::out_of_memory);
  }
  std::free(buf);
  return out;
}

}  // namespace

result<std::vector<std::uint8_t>> encode(const codec::raster& img, const encode_options& opt,
                                         const metadata_blobs& meta) {
  if (img.width == 0 || img.height == 0 ||
      img.rgba.size() != static_cast<std::size_t>(img.width) * img.height * 4) {
    return err(status::invalid_arg);
  }
  if (img.width > 65500 || img.height > 65500) return err(status::unsupported_format);
  switch (opt.format) {
    case image_format::jpeg: return encode_jpeg(img, opt, meta);
    case image_format::png: return encode_png(img, meta);
  }
  return err(status::invalid_arg);
}

}  // namespace mv::edit
