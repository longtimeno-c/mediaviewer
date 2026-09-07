// SPDX-License-Identifier: GPL-2.0-or-later
// In-memory JPEG/PNG/BMP writers for tests. Not shipped.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <jpeglib.h>
#include <lcms2.h>
#include <spng.h>

namespace fixtures {

inline std::vector<std::uint8_t> bmp_rgba(std::uint32_t w, std::uint32_t h,
                                          const std::uint8_t* rgba) {
  const std::uint32_t row_unpadded = w * 3;
  const std::uint32_t stride = (row_unpadded + 3u) & ~3u;
  const std::uint32_t pixel_off = 14 + 40;
  const std::uint32_t size = pixel_off + stride * h;
  std::vector<std::uint8_t> out(size, 0);
  out[0] = 'B';
  out[1] = 'M';
  auto u32 = [&](std::size_t o, std::uint32_t v) {
    out[o] = static_cast<std::uint8_t>(v);
    out[o + 1] = static_cast<std::uint8_t>(v >> 8);
    out[o + 2] = static_cast<std::uint8_t>(v >> 16);
    out[o + 3] = static_cast<std::uint8_t>(v >> 24);
  };
  auto u16 = [&](std::size_t o, std::uint16_t v) {
    out[o] = static_cast<std::uint8_t>(v);
    out[o + 1] = static_cast<std::uint8_t>(v >> 8);
  };
  u32(2, size);
  u32(10, pixel_off);
  u32(14, 40);
  u32(18, w);
  u32(22, static_cast<std::uint32_t>(h));  // bottom-up
  u16(26, 1);
  u16(28, 24);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint32_t src_y = h - 1 - y;
    std::uint8_t* dst = out.data() + pixel_off + static_cast<std::size_t>(y) * stride;
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint8_t* s = rgba + (static_cast<std::size_t>(src_y) * w + x) * 4;
      dst[x * 3 + 0] = s[2];
      dst[x * 3 + 1] = s[1];
      dst[x * 3 + 2] = s[0];
    }
  }
  return out;
}

inline std::vector<std::uint8_t> png_rgba(std::uint32_t w, std::uint32_t h, const std::uint8_t* rgba,
                                          const std::vector<std::uint8_t>& icc = {}) {
  spng_ctx* ctx = spng_ctx_new(SPNG_CTX_ENCODER);
  if (!ctx) return {};
  spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1);
  spng_ihdr ihdr{};
  ihdr.width = w;
  ihdr.height = h;
  ihdr.bit_depth = 8;
  ihdr.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
  spng_set_ihdr(ctx, &ihdr);
  if (!icc.empty()) {
    spng_iccp iccp{};
    std::strncpy(iccp.profile_name, "embedded", sizeof(iccp.profile_name) - 1);
    iccp.profile = const_cast<char*>(reinterpret_cast<const char*>(icc.data()));
    iccp.profile_len = icc.size();
    spng_set_iccp(ctx, &iccp);
  }
  if (spng_encode_image(ctx, rgba, static_cast<std::size_t>(w) * h * 4, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE) !=
      0) {
    spng_ctx_free(ctx);
    return {};
  }
  std::size_t len = 0;
  int err = 0;
  void* buf = spng_get_png_buffer(ctx, &len, &err);
  std::vector<std::uint8_t> out;
  if (buf && err == 0) out.assign(static_cast<std::uint8_t*>(buf), static_cast<std::uint8_t*>(buf) + len);
  std::free(buf);
  spng_ctx_free(ctx);
  return out;
}

struct jpeg_dest {
  jpeg_destination_mgr pub;
  std::vector<std::uint8_t>* out = nullptr;
  std::array<std::uint8_t, 4096> buf{};
};

inline std::vector<std::uint8_t> jpeg_rgb(std::uint32_t w, std::uint32_t h, const std::uint8_t* rgb,
                                          const std::vector<std::uint8_t>& icc = {}) {
  std::vector<std::uint8_t> out;
  jpeg_compress_struct cinfo{};
  jpeg_error_mgr err{};
  cinfo.err = jpeg_std_error(&err);
  jpeg_create_compress(&cinfo);

  jpeg_dest dest{};
  dest.out = &out;
  dest.pub.init_destination = [](j_compress_ptr c) {
    auto* d = reinterpret_cast<jpeg_dest*>(c->dest);
    c->dest->next_output_byte = d->buf.data();
    c->dest->free_in_buffer = d->buf.size();
  };
  dest.pub.empty_output_buffer = [](j_compress_ptr c) -> boolean {
    auto* d = reinterpret_cast<jpeg_dest*>(c->dest);
    d->out->insert(d->out->end(), d->buf.begin(), d->buf.end());
    c->dest->next_output_byte = d->buf.data();
    c->dest->free_in_buffer = d->buf.size();
    return TRUE;
  };
  dest.pub.term_destination = [](j_compress_ptr c) {
    auto* d = reinterpret_cast<jpeg_dest*>(c->dest);
    const std::size_t used = d->buf.size() - c->dest->free_in_buffer;
    d->out->insert(d->out->end(), d->buf.begin(), d->buf.begin() + static_cast<std::ptrdiff_t>(used));
  };
  cinfo.dest = &dest.pub;

  cinfo.image_width = w;
  cinfo.image_height = h;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 100, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  if (!icc.empty()) {
    // APP2 ICC_PROFILE chunks, 16-byte header + payload, max ~64k per marker.
    constexpr std::size_t kChunk = 65519;
    const int count = static_cast<int>((icc.size() + kChunk - 1) / kChunk);
    std::size_t off = 0;
    for (int seq = 1; seq <= count; ++seq) {
      const std::size_t n = std::min(kChunk, icc.size() - off);
      std::vector<std::uint8_t> marker(14 + n);
      std::memcpy(marker.data(), "ICC_PROFILE\0", 12);
      marker[12] = static_cast<std::uint8_t>(seq);
      marker[13] = static_cast<std::uint8_t>(count);
      std::memcpy(marker.data() + 14, icc.data() + off, n);
      jpeg_write_marker(&cinfo, JPEG_APP0 + 2, marker.data(),
                        static_cast<unsigned int>(marker.size()));
      off += n;
    }
  }

  std::vector<JSAMPLE> row(static_cast<std::size_t>(w) * 3);
  while (cinfo.next_scanline < cinfo.image_height) {
    const std::uint8_t* src = rgb + static_cast<std::size_t>(cinfo.next_scanline) * w * 3;
    std::memcpy(row.data(), src, row.size());
    JSAMPROW rows[1] = {row.data()};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  return out;
}

inline std::vector<std::uint8_t> adobe_rgb_icc() {
  cmsCIExyY d65 = {0.3127, 0.3290, 1.0};
  cmsCIExyYTRIPLE primaries = {
      {0.6400, 0.3300, 1.0},
      {0.2100, 0.7100, 1.0},
      {0.1500, 0.0600, 1.0},
  };
  cmsToneCurve* g = cmsBuildGamma(nullptr, 2.2);
  cmsToneCurve* curves[3] = {g, g, g};
  cmsHPROFILE p = cmsCreateRGBProfile(&d65, &primaries, curves);
  cmsFreeToneCurve(g);
  if (!p) return {};
  cmsUInt32Number bytes = 0;
  cmsSaveProfileToMem(p, nullptr, &bytes);
  std::vector<std::uint8_t> out(bytes);
  cmsSaveProfileToMem(p, out.data(), &bytes);
  out.resize(bytes);
  cmsCloseProfile(p);
  return out;
}

}  // namespace fixtures
