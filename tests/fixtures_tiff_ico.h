// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// In-memory TIFF / ICO writers for tests. Not shipped.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <lcms2.h>
#include <tiffio.h>

namespace fixtures {

// ---- TIFF ----------------------------------------------------------------

struct tiff_sink {
  std::vector<std::uint8_t> data;
  std::uint64_t pos = 0;
};

inline tmsize_t tiff_sink_read(thandle_t h, tdata_t buf, tmsize_t n) {
  auto* s = static_cast<tiff_sink*>(h);
  if (n <= 0 || s->pos >= s->data.size()) return 0;
  const std::uint64_t take = std::min<std::uint64_t>(s->data.size() - s->pos, static_cast<std::uint64_t>(n));
  std::memcpy(buf, s->data.data() + s->pos, static_cast<std::size_t>(take));
  s->pos += take;
  return static_cast<tmsize_t>(take);
}

inline tmsize_t tiff_sink_write(thandle_t h, tdata_t buf, tmsize_t n) {
  auto* s = static_cast<tiff_sink*>(h);
  if (n < 0) return -1;
  const std::uint64_t end = s->pos + static_cast<std::uint64_t>(n);
  if (end > s->data.size()) s->data.resize(static_cast<std::size_t>(end), 0);
  std::memcpy(s->data.data() + s->pos, buf, static_cast<std::size_t>(n));
  s->pos = end;
  return n;
}

inline toff_t tiff_sink_seek(thandle_t h, toff_t off, int whence) {
  auto* s = static_cast<tiff_sink*>(h);
  std::uint64_t base = 0;
  if (whence == SEEK_CUR) base = s->pos;
  if (whence == SEEK_END) base = s->data.size();
  s->pos = base + off;
  return s->pos;
}

inline int tiff_sink_close(thandle_t) { return 0; }
inline toff_t tiff_sink_size(thandle_t h) { return static_cast<tiff_sink*>(h)->data.size(); }
inline int tiff_sink_map(thandle_t, tdata_t*, toff_t*) { return 0; }
inline void tiff_sink_unmap(thandle_t, tdata_t, toff_t) {}

struct tiff_spec {
  std::uint32_t width = 1;
  std::uint32_t height = 1;
  std::uint16_t spp = 3;
  std::uint16_t bps = 8;
  std::uint16_t photometric = PHOTOMETRIC_RGB;
  std::uint16_t compression = COMPRESSION_NONE;
  std::uint16_t sample_format = SAMPLEFORMAT_UINT;
  std::uint16_t planar = PLANARCONFIG_CONTIG;
  std::uint16_t orientation = ORIENTATION_TOPLEFT;
  int extra = -1;                    // EXTRASAMPLE_* for one extra sample; -1: no tag
  std::uint32_t rows_per_strip = 0;  // 0: one strip per 2 rows
  std::uint32_t tile = 0;            // > 0: tiled, tile x tile (>= 8-bit only)
  std::vector<std::uint8_t> icc;
  std::vector<std::uint16_t> cmap;   // r[2^bps], g[2^bps], b[2^bps]
  bool jpeg_ycbcr = false;           // COMPRESSION_JPEG + PHOTOMETRIC_YCBCR, RGB input
  bool ycbcr_1x1 = false;            // raw YCbCr, no subsampling (samples are Y,Cb,Cr)
};

// `samples` is contiguous, native byte order, each row padded to a byte.
inline std::vector<std::uint8_t> tiff_write(const tiff_spec& s, const std::uint8_t* samples) {
  tiff_sink sink;
  TIFF* tif = TIFFClientOpen("fixture", "w", &sink, tiff_sink_read, tiff_sink_write, tiff_sink_seek,
                             tiff_sink_close, tiff_sink_size, tiff_sink_map, tiff_sink_unmap);
  if (!tif) return {};
  bool ok = true;
  ok &= TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, s.width) == 1;
  ok &= TIFFSetField(tif, TIFFTAG_IMAGELENGTH, s.height) == 1;
  ok &= TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, s.bps) == 1;
  ok &= TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, s.spp) == 1;
  ok &= TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, s.sample_format) == 1;
  ok &= TIFFSetField(tif, TIFFTAG_PLANARCONFIG, s.planar) == 1;
  ok &= TIFFSetField(tif, TIFFTAG_ORIENTATION, s.orientation) == 1;
  if (s.jpeg_ycbcr) {
    ok &= TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_JPEGQUALITY, 95) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB) == 1;
  } else {
    ok &= TIFFSetField(tif, TIFFTAG_COMPRESSION, s.compression) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, s.photometric) == 1;
  }
  if (s.ycbcr_1x1) ok &= TIFFSetField(tif, TIFFTAG_YCBCRSUBSAMPLING, 1, 1) == 1;
  if (s.extra >= 0) {
    std::uint16_t info[1] = {static_cast<std::uint16_t>(s.extra)};
    ok &= TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, static_cast<std::uint16_t>(1), info) == 1;
  }
  if (!s.icc.empty()) {
    ok &= TIFFSetField(tif, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(s.icc.size()),
                       s.icc.data()) == 1;
  }
  std::vector<std::uint16_t> cmap = s.cmap;
  if (!cmap.empty()) {
    const std::size_t n = cmap.size() / 3;
    ok &= TIFFSetField(tif, TIFFTAG_COLORMAP, cmap.data(), cmap.data() + n, cmap.data() + 2 * n) == 1;
  }

  const bool separate = s.planar == PLANARCONFIG_SEPARATE;
  const std::size_t row_bytes = (static_cast<std::size_t>(s.width) * s.spp * s.bps + 7) / 8;
  const std::size_t sample_bytes = s.bps / 8u;

  if (s.tile > 0) {
    ok &= TIFFSetField(tif, TIFFTAG_TILEWIDTH, s.tile) == 1;
    ok &= TIFFSetField(tif, TIFFTAG_TILELENGTH, s.tile) == 1;
    const std::uint16_t planes = separate ? s.spp : 1;
    const std::size_t per = separate ? sample_bytes : sample_bytes * s.spp;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(s.tile) * s.tile * per);
    for (std::uint16_t p = 0; p < planes && ok; ++p) {
      for (std::uint32_t ty = 0; ty < s.height; ty += s.tile) {
        for (std::uint32_t tx = 0; tx < s.width; tx += s.tile) {
          std::fill(buf.begin(), buf.end(), std::uint8_t{0});
          for (std::uint32_t y = ty; y < std::min(ty + s.tile, s.height); ++y) {
            for (std::uint32_t x = tx; x < std::min(tx + s.tile, s.width); ++x) {
              const std::size_t src = (static_cast<std::size_t>(y) * s.width + x) * s.spp * sample_bytes;
              const std::size_t dst = (static_cast<std::size_t>(y - ty) * s.tile + (x - tx)) * per;
              if (separate) {
                std::memcpy(buf.data() + dst, samples + src + p * sample_bytes, sample_bytes);
              } else {
                std::memcpy(buf.data() + dst, samples + src, per);
              }
            }
          }
          ok &= TIFFWriteEncodedTile(tif, TIFFComputeTile(tif, tx, ty, 0, p), buf.data(),
                                     static_cast<tmsize_t>(buf.size())) >= 0;
        }
      }
    }
  } else {
    const std::uint32_t rps = s.rows_per_strip ? s.rows_per_strip : 2;
    ok &= TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rps) == 1;
    if (separate) {
      std::vector<std::uint8_t> row(static_cast<std::size_t>(s.width) * sample_bytes);
      for (std::uint16_t p = 0; p < s.spp && ok; ++p) {
        for (std::uint32_t y = 0; y < s.height && ok; ++y) {
          for (std::uint32_t x = 0; x < s.width; ++x) {
            const std::size_t src = (static_cast<std::size_t>(y) * s.width + x) * s.spp * sample_bytes;
            std::memcpy(row.data() + x * sample_bytes, samples + src + p * sample_bytes, sample_bytes);
          }
          ok &= TIFFWriteScanline(tif, row.data(), y, p) == 1;
        }
      }
    } else {
      std::vector<std::uint8_t> row(row_bytes);
      for (std::uint32_t y = 0; y < s.height && ok; ++y) {
        std::memcpy(row.data(), samples + y * row_bytes, row_bytes);
        ok &= TIFFWriteScanline(tif, row.data(), y, 0) == 1;
      }
    }
  }
  TIFFClose(tif);
  if (!ok) return {};
  return std::move(sink.data);
}

// A hand-built little-endian TIFF: one 8-bit grey strip that claims w x h
// but carries 16 bytes of pixel data. For hostile-header tests.
inline std::vector<std::uint8_t> tiff_header_only(std::uint32_t w, std::uint32_t h) {
  constexpr std::uint16_t kEntries = 9;
  const std::uint32_t data_off = 8 + 2 + kEntries * 12 + 4;
  std::vector<std::uint8_t> out(data_off + 16, 0x80);
  auto u16 = [&](std::size_t o, std::uint16_t v) {
    out[o] = static_cast<std::uint8_t>(v);
    out[o + 1] = static_cast<std::uint8_t>(v >> 8);
  };
  auto u32 = [&](std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
  };
  out[0] = 'I';
  out[1] = 'I';
  u16(2, 42);
  u32(4, 8);
  u16(8, kEntries);
  std::size_t e = 10;
  auto entry = [&](std::uint16_t tag, std::uint16_t type, std::uint32_t value) {
    u16(e, tag);
    u16(e + 2, type);
    u32(e + 4, 1);
    u32(e + 8, 0);
    if (type == 3) {
      u16(e + 8, static_cast<std::uint16_t>(value));
    } else {
      u32(e + 8, value);
    }
    e += 12;
  };
  entry(256, 4, w);         // ImageWidth
  entry(257, 4, h);         // ImageLength
  entry(258, 3, 8);         // BitsPerSample
  entry(259, 3, 1);         // Compression: none
  entry(262, 3, 1);         // Photometric: min-is-black
  entry(273, 4, data_off);  // StripOffsets
  entry(277, 3, 1);         // SamplesPerPixel
  entry(278, 4, h);         // RowsPerStrip
  entry(279, 4, 16);        // StripByteCounts
  u32(e, 0);                // no next IFD
  return out;
}

inline std::vector<std::uint8_t> grey_icc() {
  cmsToneCurve* g = cmsBuildGamma(nullptr, 2.2);
  cmsHPROFILE p = cmsCreateGrayProfile(cmsD50_xyY(), g);
  cmsFreeToneCurve(g);
  std::vector<std::uint8_t> out;
  if (!p) return out;
  cmsUInt32Number bytes = 0;
  cmsSaveProfileToMem(p, nullptr, &bytes);
  out.resize(bytes);
  cmsSaveProfileToMem(p, out.data(), &bytes);
  cmsCloseProfile(p);
  return out;
}

// ---- ICO ------------------------------------------------------------------

struct ico_entry {
  std::vector<std::uint8_t> payload;
  std::uint8_t w_byte = 0;  // 0 means 256
  std::uint8_t h_byte = 0;
  std::uint16_t bpp = 32;
};

inline std::vector<std::uint8_t> ico_file(const std::vector<ico_entry>& entries) {
  std::vector<std::uint8_t> out(6 + 16 * entries.size(), 0);
  out[2] = 1;
  out[4] = static_cast<std::uint8_t>(entries.size());
  out[5] = static_cast<std::uint8_t>(entries.size() >> 8);
  std::uint32_t off = static_cast<std::uint32_t>(out.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    std::uint8_t* d = out.data() + 6 + 16 * i;
    d[0] = entries[i].w_byte;
    d[1] = entries[i].h_byte;
    d[4] = 1;
    d[6] = static_cast<std::uint8_t>(entries[i].bpp);
    d[7] = static_cast<std::uint8_t>(entries[i].bpp >> 8);
    const auto size = static_cast<std::uint32_t>(entries[i].payload.size());
    for (int b = 0; b < 4; ++b) {
      d[8 + b] = static_cast<std::uint8_t>(size >> (8 * b));
      d[12 + b] = static_cast<std::uint8_t>(off >> (8 * b));
    }
    off += size;
  }
  for (const auto& e : entries) out.insert(out.end(), e.payload.begin(), e.payload.end());
  return out;
}

// An icon DIB: BITMAPINFOHEADER (height doubled), palette, bottom-up XOR rows,
// then the AND mask. `pixels` is RGBA for 24/32 bpp and one index per pixel
// for 1/4/8 bpp. `transparent` (optional) is one byte per pixel, 1 = AND bit set.
inline std::vector<std::uint8_t> ico_dib(std::uint32_t w, std::uint32_t h, std::uint16_t bpp,
                                         const std::uint8_t* pixels,
                                         const std::vector<std::array<std::uint8_t, 3>>& palette = {},
                                         const std::vector<std::uint8_t>& transparent = {},
                                         bool write_mask = true) {
  const std::size_t xor_stride = (static_cast<std::size_t>(w) * bpp + 31) / 32 * 4;
  const std::size_t and_stride = (static_cast<std::size_t>(w) + 31) / 32 * 4;
  const std::size_t pal = bpp <= 8 ? palette.size() : 0;
  std::vector<std::uint8_t> out(40 + pal * 4 + xor_stride * h + (write_mask ? and_stride * h : 0), 0);
  auto u16 = [&](std::size_t o, std::uint16_t v) {
    out[o] = static_cast<std::uint8_t>(v);
    out[o + 1] = static_cast<std::uint8_t>(v >> 8);
  };
  auto u32 = [&](std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[o + i] = static_cast<std::uint8_t>(v >> (8 * i));
  };
  u32(0, 40);
  u32(4, w);
  u32(8, h * 2);
  u16(12, 1);
  u16(14, bpp);
  u32(32, static_cast<std::uint32_t>(pal));
  for (std::size_t i = 0; i < pal; ++i) {
    out[40 + i * 4] = palette[i][2];
    out[40 + i * 4 + 1] = palette[i][1];
    out[40 + i * 4 + 2] = palette[i][0];
  }
  const std::size_t xor_off = 40 + pal * 4;
  const std::size_t and_off = xor_off + xor_stride * h;
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint32_t file_row = h - 1 - y;  // bottom-up
    std::uint8_t* row = out.data() + xor_off + file_row * xor_stride;
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      if (bpp == 32 || bpp == 24) {
        const std::size_t n = bpp / 8u;
        row[x * n] = pixels[i * 4 + 2];
        row[x * n + 1] = pixels[i * 4 + 1];
        row[x * n + 2] = pixels[i * 4 + 0];
        if (bpp == 32) row[x * n + 3] = pixels[i * 4 + 3];
      } else if (bpp == 8) {
        row[x] = pixels[i];
      } else if (bpp == 4) {
        row[x >> 1] = static_cast<std::uint8_t>(row[x >> 1] | (pixels[i] << ((x & 1u) ? 0 : 4)));
      } else {
        row[x >> 3] = static_cast<std::uint8_t>(row[x >> 3] | ((pixels[i] & 1u) << (7 - (x & 7u))));
      }
      if (write_mask && !transparent.empty() && transparent[i]) {
        out[and_off + file_row * and_stride + (x >> 3)] |= static_cast<std::uint8_t>(0x80u >> (x & 7u));
      }
    }
  }
  return out;
}

}  // namespace fixtures
