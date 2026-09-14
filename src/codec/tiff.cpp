// SPDX-License-Identifier: GPL-2.0-or-later
// TIFF via libtiff, decoded from memory. Page 0 is the still; pages are
// Ctrl+PageUp/PageDown (plan/04), not extra filmstrip stops — decode_page()
// already takes a page index so that feature does not need a rewrite.
//
// What the raster holds (RGBA8, source-encoded, stored pixel order):
//  - uint 1/2/4/8/16/32-bit grey (min-is-black / min-is-white), palette, RGB,
//    CMYK; strips or tiles; contiguous or (>= 8-bit) separate planes; any
//    compression the linked libtiff decodes (none/LZW/ZIP/PackBits/JPEG/CCITT).
//  - 16/32-bit samples round to 8 bits: the raster is RGBA8.
//  - Unassociated alpha passes straight; associated alpha is un-premultiplied.
//  - 32/64-bit float grey/RGB is clamped to [0,1]. Untagged float is taken as
//    linear with sRGB primaries and sRGB-encoded; a tagged float keeps its
//    profile and is not re-encoded (the profile's TRC applies, D6).
//  - CMYK is naïve (1-C)(1-K); its CMYK profile is dropped.
//  - YCbCr without JPEG, OJPEG, CIELab, LogLuv: libtiff's TIFFRGBAImage.
//  - ICC is attached only when it is an RGB profile over RGB-like pixels. Grey
//    and CMYK profiles are dropped: the display stage builds RGBA transforms.
//  - The Orientation tag is NOT applied (plan/04: orientation belongs on the
//    display path, which does not exist yet).
// The original bytes are read through a const span; the handle is opened "rm"
// (read, no mapping) and its write proc refuses.
#include "codec/decode.h"

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
constexpr std::uint16_t kMaxSamples = 64;
// One decoded strip/tile (and all planes of one block together).
constexpr std::uint64_t kMaxBlockBytes = 1ull << 30;
constexpr std::uint64_t kMaxPlanesBytes = 2ull << 30;
constexpr tmsize_t kMaxSingleAlloc = static_cast<tmsize_t>(1) << 30;
constexpr tmsize_t kMaxCumulatedAlloc = static_cast<tmsize_t>(3) << 30;
constexpr std::uint32_t kCancelRows = 64;
constexpr std::uint64_t kMaxRgbaBandBytes = 256ull << 20;

template <typename T>
bool try_resize(std::vector<T>& v, std::uint64_t n) noexcept {
  if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) return false;
  try {
    v.resize(static_cast<std::size_t>(n));
  } catch (...) {
    return false;
  }
  return true;
}

// ---- read-only memory stream -------------------------------------------

struct mem_stream {
  const std::uint8_t* data = nullptr;
  std::uint64_t size = 0;
  std::uint64_t pos = 0;
};

tmsize_t mem_read(thandle_t h, tdata_t buf, tmsize_t n) {
  auto* s = static_cast<mem_stream*>(h);
  if (n <= 0 || s->pos >= s->size) return 0;
  const std::uint64_t take =
      std::min<std::uint64_t>(s->size - s->pos, static_cast<std::uint64_t>(n));
  std::memcpy(buf, s->data + s->pos, static_cast<std::size_t>(take));
  s->pos += take;
  return static_cast<tmsize_t>(take);
}

tmsize_t mem_write(thandle_t, tdata_t, tmsize_t) { return -1; }

toff_t mem_seek(thandle_t h, toff_t off, int whence) {
  auto* s = static_cast<mem_stream*>(h);
  std::uint64_t base = 0;
  switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = s->pos; break;
    case SEEK_END: base = s->size; break;
    default: return static_cast<toff_t>(-1);
  }
  if (off > std::numeric_limits<std::uint64_t>::max() - base) return static_cast<toff_t>(-1);
  s->pos = base + off;
  return s->pos;
}

int mem_close(thandle_t) { return 0; }
toff_t mem_size(thandle_t h) { return static_cast<mem_stream*>(h)->size; }
int mem_map(thandle_t, tdata_t*, toff_t*) { return 0; }
void mem_unmap(thandle_t, tdata_t, toff_t) {}

// Per-handle: libtiff's default handlers print to stderr, and a message can
// carry the (client-supplied) name. Returning 1 stops the global handler.
int quiet_handler(TIFF*, void*, const char*, const char*, va_list) { return 1; }

struct open_options {
  TIFFOpenOptions* opts = TIFFOpenOptionsAlloc();
  open_options() = default;
  open_options(const open_options&) = delete;
  open_options& operator=(const open_options&) = delete;
  ~open_options() {
    if (opts) TIFFOpenOptionsFree(opts);
  }
};

struct tiff_handle {
  TIFF* tif = nullptr;
  tiff_handle() = default;
  tiff_handle(const tiff_handle&) = delete;
  tiff_handle& operator=(const tiff_handle&) = delete;
  ~tiff_handle() {
    if (tif) TIFFClose(tif);
  }
};

// ---- layout ----------------------------------------------------------------

enum class model : std::uint8_t { grey, palette, rgb, cmyk };
enum class alpha_kind : std::uint8_t { none, straight, associated };
enum class route : std::uint8_t { native, rgba_image };

struct layout {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint16_t spp = 1;
  std::uint16_t bps = 1;
  std::uint16_t planar = PLANARCONFIG_CONTIG;
  std::uint16_t photometric = PHOTOMETRIC_MINISBLACK;
  std::uint16_t colour = 1;  // colour samples; the first extra sample follows
  model m = model::grey;
  alpha_kind alpha = alpha_kind::none;
  bool min_is_white = false;
  bool is_float = false;
  bool encode_srgb = false;  // untagged float
  bool cmap_8bit = false;
  const std::uint16_t* cmap[3] = {nullptr, nullptr, nullptr};
};

// ---- sample conversion -----------------------------------------------------

inline std::uint32_t fetch_uint(const std::uint8_t* row, std::size_t i, std::uint16_t bps) noexcept {
  switch (bps) {
    case 8: return row[i];
    case 16: {
      std::uint16_t v = 0;
      std::memcpy(&v, row + i * 2, 2);  // libtiff hands back native byte order
      return v;
    }
    case 32: {
      std::uint32_t v = 0;
      std::memcpy(&v, row + i * 4, 4);
      return v;
    }
    default: {  // 1, 2, 4: MSB-first packed (FillOrder is undone by libtiff)
      const std::size_t bit = i * bps;
      const unsigned shift = 8u - bps - static_cast<unsigned>(bit & 7u);
      return (static_cast<unsigned>(row[bit >> 3]) >> shift) & ((1u << bps) - 1u);
    }
  }
}

inline std::uint8_t to8(std::uint32_t v, std::uint16_t bps) noexcept {
  switch (bps) {
    case 8: return static_cast<std::uint8_t>(v);
    case 16: return static_cast<std::uint8_t>((v * 255u + 32767u) / 65535u);
    case 32:
      return static_cast<std::uint8_t>((static_cast<std::uint64_t>(v) * 255u + 0x7FFFFFFFull) /
                                       0xFFFFFFFFull);
    default: {
      const std::uint32_t max = (1u << bps) - 1u;
      return static_cast<std::uint8_t>((v * 255u + max / 2u) / max);
    }
  }
}

inline float fetch_float(const std::uint8_t* row, std::size_t i, std::uint16_t bps) noexcept {
  if (bps == 64) {
    double d = 0;
    std::memcpy(&d, row + i * 8, 8);
    return static_cast<float>(d);
  }
  float f = 0;
  std::memcpy(&f, row + i * 4, 4);
  return f;
}

inline float clamp01(float v) noexcept {
  if (!(v > 0.0f)) return 0.0f;  // also NaN
  return v < 1.0f ? v : 1.0f;
}

struct srgb_table {
  std::uint8_t v[65536];
  srgb_table() noexcept {
    for (std::size_t i = 0; i < 65536; ++i) {
      const double lin = static_cast<double>(i) / 65535.0;
      const double enc = lin <= 0.0031308 ? 12.92 * lin : 1.055 * std::pow(lin, 1.0 / 2.4) - 0.055;
      v[i] = static_cast<std::uint8_t>(std::lround(std::clamp(enc, 0.0, 1.0) * 255.0));
    }
  }
};

const std::uint8_t* srgb_lut() noexcept {
  static const srgb_table table;
  return table.v;
}

inline std::uint8_t float_to8(float unit, bool encode) noexcept {
  if (encode) return srgb_lut()[static_cast<std::size_t>(std::lround(unit * 65535.0f))];
  return static_cast<std::uint8_t>(std::lround(unit * 255.0f));
}

inline std::uint8_t unpremultiply(std::uint8_t c, std::uint8_t a) noexcept {
  if (a == 0) return 0;
  const unsigned v = (static_cast<unsigned>(c) * 255u + a / 2u) / a;
  return static_cast<std::uint8_t>(v > 255u ? 255u : v);
}

inline std::uint8_t cmap_to8(const layout& L, std::uint16_t v) noexcept {
  if (L.cmap_8bit) return static_cast<std::uint8_t>(v);
  return static_cast<std::uint8_t>((static_cast<unsigned>(v) * 255u + 32767u) / 65535u);
}

// `src` holds `count` pixels of contiguous samples starting at a byte boundary.
void convert_row(const layout& L, const std::uint8_t* src, std::uint32_t count,
                 std::uint8_t* dst) noexcept {
  const std::uint16_t spp = L.spp;
  const std::uint16_t bps = L.bps;
  const std::size_t ai = L.colour;
  for (std::uint32_t x = 0; x < count; ++x) {
    const std::size_t s = static_cast<std::size_t>(x) * spp;
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    if (L.is_float) {
      float fr = fetch_float(src, s, bps);
      float fg = fr, fb = fr;
      if (L.m == model::rgb) {
        fg = fetch_float(src, s + 1, bps);
        fb = fetch_float(src, s + 2, bps);
      }
      fr = clamp01(fr);
      fg = clamp01(fg);
      fb = clamp01(fb);
      if (L.m == model::grey && L.min_is_white) fr = fg = fb = 1.0f - fr;
      if (L.alpha != alpha_kind::none) {
        const float fa = clamp01(fetch_float(src, s + ai, bps));
        if (L.alpha == alpha_kind::associated) {
          if (fa > 0.0f) {
            fr = clamp01(fr / fa);
            fg = clamp01(fg / fa);
            fb = clamp01(fb / fa);
          } else {
            fr = fg = fb = 0.0f;
          }
        }
        a = static_cast<std::uint8_t>(std::lround(fa * 255.0f));
      }
      r = float_to8(fr, L.encode_srgb);
      g = float_to8(fg, L.encode_srgb);
      b = float_to8(fb, L.encode_srgb);
    } else {
      switch (L.m) {
        case model::grey: {
          std::uint8_t v = to8(fetch_uint(src, s, bps), bps);
          if (L.min_is_white) v = static_cast<std::uint8_t>(255u - v);
          r = g = b = v;
          break;
        }
        case model::palette: {
          const std::uint32_t i = fetch_uint(src, s, bps);  // < 2^bps == colormap size
          r = cmap_to8(L, L.cmap[0][i]);
          g = cmap_to8(L, L.cmap[1][i]);
          b = cmap_to8(L, L.cmap[2][i]);
          break;
        }
        case model::rgb:
          r = to8(fetch_uint(src, s, bps), bps);
          g = to8(fetch_uint(src, s + 1, bps), bps);
          b = to8(fetch_uint(src, s + 2, bps), bps);
          break;
        case model::cmyk: {
          const unsigned k = 255u - to8(fetch_uint(src, s + 3, bps), bps);
          const auto ink = [&](std::size_t o) {
            const unsigned c = 255u - to8(fetch_uint(src, s + o, bps), bps);
            return static_cast<std::uint8_t>((c * k + 127u) / 255u);
          };
          r = ink(0);
          g = ink(1);
          b = ink(2);
          break;
        }
      }
      if (L.alpha != alpha_kind::none) {
        a = to8(fetch_uint(src, s + ai, bps), bps);
        if (L.alpha == alpha_kind::associated) {
          r = unpremultiply(r, a);
          g = unpremultiply(g, a);
          b = unpremultiply(b, a);
        }
      }
    }
    dst[x * 4 + 0] = r;
    dst[x * 4 + 1] = g;
    dst[x * 4 + 2] = b;
    dst[x * 4 + 3] = a;
  }
}

// ---- directory inspection --------------------------------------------------

result<route> inspect(TIFF* tif, layout& L) {
  std::uint32_t w = 0, h = 0;
  if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w) != 1 ||
      TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h) != 1) {
    return err(status::corrupt);
  }
  if (w == 0 || h == 0) return err(status::corrupt);
  // Before any pixel allocation (same limits as jpeg.cpp).
  if (w > kMaxDim || h > kMaxDim || static_cast<std::uint64_t>(w) * h > kMaxPixels) {
    return err(status::unsupported_format);
  }
  L.width = w;
  L.height = h;

  std::uint16_t spp = 1, bps = 1, fmt = SAMPLEFORMAT_UINT, planar = PLANARCONFIG_CONTIG;
  std::uint16_t compression = COMPRESSION_NONE, photometric = 0;
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bps);
  TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &fmt);
  TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
  TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &compression);
  if (TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric) != 1) {
    photometric = spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK;
  }
  if (spp == 0 || spp > kMaxSamples || bps == 0) return err(status::unsupported_format);
  if (planar != PLANARCONFIG_CONTIG && planar != PLANARCONFIG_SEPARATE) {
    return err(status::corrupt);
  }
  L.spp = spp;
  L.bps = bps;
  L.planar = planar;
  L.photometric = photometric;

  if (compression == COMPRESSION_OJPEG) return route::rgba_image;
  if (compression == COMPRESSION_JPEG && photometric == PHOTOMETRIC_YCBCR &&
      planar == PLANARCONFIG_CONTIG) {
    // libjpeg does the YCbCr → RGB and the chroma upsampling.
    if (TIFFSetField(tif, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB) != 1) {
      return err(status::unsupported_format);
    }
    photometric = PHOTOMETRIC_RGB;
    L.photometric = photometric;
  }

  switch (photometric) {
    case PHOTOMETRIC_MINISWHITE:
      L.min_is_white = true;
      [[fallthrough]];
    case PHOTOMETRIC_MINISBLACK:
      L.m = model::grey;
      L.colour = 1;
      break;
    case PHOTOMETRIC_PALETTE:
      L.m = model::palette;
      L.colour = 1;
      break;
    case PHOTOMETRIC_RGB:
      L.m = model::rgb;
      L.colour = 3;
      break;
    case PHOTOMETRIC_SEPARATED: {
      std::uint16_t inkset = INKSET_CMYK;
      TIFFGetFieldDefaulted(tif, TIFFTAG_INKSET, &inkset);
      if (inkset != INKSET_CMYK) return route::rgba_image;
      L.m = model::cmyk;
      L.colour = 4;
      break;
    }
    default:
      return route::rgba_image;  // YCbCr, CIELab, LogL/LogLuv, ...
  }
  if (spp < L.colour) return err(status::corrupt);

  if (fmt == SAMPLEFORMAT_IEEEFP) {
    if (bps != 32 && bps != 64) return err(status::unsupported_format);
    if (L.m != model::grey && L.m != model::rgb) return err(status::unsupported_format);
    L.is_float = true;
  } else if (fmt == SAMPLEFORMAT_UINT || fmt == SAMPLEFORMAT_VOID) {
    if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 16 && bps != 32) {
      return err(status::unsupported_format);
    }
    if (L.m == model::palette && bps > 16) return err(status::unsupported_format);
  } else {
    return err(status::unsupported_format);  // signed integer, complex
  }
  if (planar == PLANARCONFIG_SEPARATE && bps < 8) return err(status::unsupported_format);

  if (L.m == model::palette) {
    std::uint16_t* r = nullptr;
    std::uint16_t* g = nullptr;
    std::uint16_t* b = nullptr;
    if (TIFFGetField(tif, TIFFTAG_COLORMAP, &r, &g, &b) != 1 || !r || !g || !b) {
      return err(status::corrupt);
    }
    L.cmap[0] = r;
    L.cmap[1] = g;
    L.cmap[2] = b;
    // Some writers store 8-bit colormaps; libtiff's RGBA reader makes the same call.
    const std::size_t n = std::size_t{1} << bps;
    bool small = true;
    for (std::size_t i = 0; i < n && small; ++i) {
      if (r[i] >= 256 || g[i] >= 256 || b[i] >= 256) small = false;
    }
    L.cmap_8bit = small;
  }

  if (spp > L.colour && L.m != model::palette) {
    std::uint16_t count = 0;
    std::uint16_t* info = nullptr;
    TIFFGetFieldDefaulted(tif, TIFFTAG_EXTRASAMPLES, &count, &info);
    if (count > 0 && info) {
      if (info[0] == EXTRASAMPLE_ASSOCALPHA) {
        L.alpha = alpha_kind::associated;
      } else if (info[0] == EXTRASAMPLE_UNASSALPHA) {
        L.alpha = alpha_kind::straight;
      }  // unspecified: not alpha
    } else if (L.m == model::rgb && spp == 4) {
      L.alpha = alpha_kind::associated;  // untagged 4th sample: libtiff's reading
    }
  }
  return route::native;
}

// ICC → raster.icc when it describes the pixels we hand out (RGB).
bool read_icc(TIFF* tif, std::vector<std::uint8_t>& out) noexcept {
  std::uint32_t len = 0;
  void* data = nullptr;
  if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &len, &data) != 1 || !data || len < 20) return true;
  const auto* p = static_cast<const std::uint8_t*>(data);
  if (!(p[16] == 'R' && p[17] == 'G' && p[18] == 'B' && p[19] == ' ')) return true;
  try {
    out.assign(p, p + len);
  } catch (...) {
    return false;
  }
  return true;
}

bool allocate(raster& out) noexcept {
  return try_resize(out.rgba, static_cast<std::uint64_t>(out.width) * out.height * 4);
}

// ---- native reader -----------------------------------------------------------

status read_native(TIFF* tif, const layout& L, raster& out, const job_context* ctx) {
  const std::uint64_t w = L.width;
  const std::uint64_t h = L.height;
  const std::uint64_t row_bytes = (w * L.spp * L.bps + 7) / 8;
  const bool tiled = TIFFIsTiled(tif) != 0;

  if (!tiled && L.planar == PLANARCONFIG_CONTIG) {
    // Sequential scanlines: libtiff decodes strip by strip without a
    // whole-strip buffer, so a single-strip 256 MP file stays cheap.
    const tmsize_t sl = TIFFScanlineSize(tif);
    if (sl <= 0 || static_cast<std::uint64_t>(sl) < row_bytes) return status::corrupt;
    std::vector<std::uint8_t> buf;
    if (!try_resize(buf, static_cast<std::uint64_t>(sl))) return status::out_of_memory;
    for (std::uint32_t y = 0; y < L.height; ++y) {
      if ((y % kCancelRows) == 0 && ctx && ctx->cancelled()) return status::cancelled;
      if (TIFFReadScanline(tif, buf.data(), y, 0) != 1) return status::corrupt;
      // Allocate the output only once real pixel data exists: a hostile
      // header with no data never costs the full raster.
      if (out.rgba.empty() && !allocate(out)) return status::out_of_memory;
      convert_row(L, buf.data(), L.width, out.rgba.data() + static_cast<std::size_t>(y) * w * 4);
    }
    return status::ok;
  }

  std::uint64_t bw = w, bh = h;
  if (tiled) {
    std::uint32_t tw = 0, th = 0;
    if (TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw) != 1 ||
        TIFFGetField(tif, TIFFTAG_TILELENGTH, &th) != 1 || tw == 0 || th == 0) {
      return status::corrupt;
    }
    bw = tw;
    bh = th;
  } else {
    std::uint32_t rps = L.height;
    TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rps);
    bh = (rps == 0 || rps > L.height) ? h : rps;
  }
  const bool separate = L.planar == PLANARCONFIG_SEPARATE;
  const std::uint16_t planes = separate ? L.spp : 1;
  const std::uint64_t block_spp = separate ? 1 : L.spp;
  const std::uint64_t block_row = (bw * block_spp * L.bps + 7) / 8;
  const std::uint64_t block_need = block_row * bh;
  const tmsize_t lib_size = tiled ? TIFFTileSize(tif) : TIFFStripSize(tif);
  if (lib_size <= 0) return status::corrupt;
  const std::uint64_t buf_size = std::max<std::uint64_t>(block_need, static_cast<std::uint64_t>(lib_size));
  if (buf_size > kMaxBlockBytes || buf_size * planes > kMaxPlanesBytes) {
    return status::unsupported_format;
  }

  std::vector<std::vector<std::uint8_t>> bufs;
  try {
    bufs.resize(planes);
  } catch (...) {
    return status::out_of_memory;
  }
  for (auto& b : bufs) {
    if (!try_resize(b, buf_size)) return status::out_of_memory;
  }
  const std::size_t sample_bytes = L.bps / 8u;  // separate planes are >= 8-bit
  std::vector<std::uint8_t> contig;
  if (separate && !try_resize(contig, std::min(bw, w) * L.spp * sample_bytes)) {
    return status::out_of_memory;
  }

  for (std::uint64_t by = 0; by < h; by += bh) {
    const std::uint64_t rows = std::min(bh, h - by);
    for (std::uint64_t bx = 0; bx < w; bx += bw) {
      if (ctx && ctx->cancelled()) return status::cancelled;
      const std::uint64_t cols = std::min(bw, w - bx);
      for (std::uint16_t p = 0; p < planes; ++p) {
        tmsize_t n = -1;
        if (tiled) {
          const std::uint32_t tile = TIFFComputeTile(tif, static_cast<std::uint32_t>(bx),
                                                     static_cast<std::uint32_t>(by), 0, p);
          n = TIFFReadEncodedTile(tif, tile, bufs[p].data(), static_cast<tmsize_t>(buf_size));
        } else {
          const std::uint32_t strip = TIFFComputeStrip(tif, static_cast<std::uint32_t>(by), p);
          n = TIFFReadEncodedStrip(tif, strip, bufs[p].data(), static_cast<tmsize_t>(buf_size));
        }
        if (n < 0 || static_cast<std::uint64_t>(n) < block_row * rows) return status::corrupt;
      }
      if (out.rgba.empty() && !allocate(out)) return status::out_of_memory;
      for (std::uint64_t r = 0; r < rows; ++r) {
        std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>(((by + r) * w + bx) * 4);
        const std::size_t src_off = static_cast<std::size_t>(r * block_row);
        if (!separate) {
          convert_row(L, bufs[0].data() + src_off, static_cast<std::uint32_t>(cols), dst);
          continue;
        }
        for (std::uint64_t x = 0; x < cols; ++x) {
          for (std::uint16_t p = 0; p < planes; ++p) {
            std::memcpy(contig.data() + static_cast<std::size_t>((x * L.spp + p) * sample_bytes),
                        bufs[p].data() + src_off + static_cast<std::size_t>(x * sample_bytes),
                        sample_bytes);
          }
        }
        convert_row(L, contig.data(), static_cast<std::uint32_t>(cols), dst);
      }
    }
  }
  return status::ok;
}

// ---- TIFFRGBAImage fallback (exotic photometrics) ---------------------------

struct rgba_image_end {
  TIFFRGBAImage* img;
  ~rgba_image_end() { TIFFRGBAImageEnd(img); }
};

status read_rgba_image(TIFF* tif, const layout& L, raster& out, const job_context* ctx) {
  char emsg[1024] = {};
  if (TIFFRGBAImageOK(tif, emsg) != 1) return status::unsupported_format;
  TIFFRGBAImage img{};
  if (TIFFRGBAImageBegin(&img, tif, 1, emsg) != 1) return status::unsupported_format;
  rgba_image_end end{&img};
  if (img.width != L.width || img.height != L.height) return status::corrupt;
  // Same orientation in and out = no flips: stored pixel order, like the
  // native path.
  img.req_orientation = img.orientation;
  const bool has_alpha = img.alpha != 0;  // libtiff premultiplies both kinds

  std::uint32_t rps = L.height;
  TIFFGetFieldDefaulted(tif, TIFFTAG_ROWSPERSTRIP, &rps);
  std::uint64_t band = std::clamp<std::uint64_t>(rps, 64, L.height);
  const std::uint64_t cap = std::max<std::uint64_t>(1, kMaxRgbaBandBytes / (4ull * L.width));
  band = std::min(band, cap);

  std::vector<std::uint32_t> buf;
  if (!try_resize(buf, band * L.width)) return status::out_of_memory;
  const std::uint64_t w = L.width;
  for (std::uint64_t y = 0; y < L.height; y += band) {
    if (ctx && ctx->cancelled()) return status::cancelled;
    const std::uint64_t rows = std::min<std::uint64_t>(band, L.height - y);
    img.row_offset = static_cast<int>(y);
    img.col_offset = 0;
    if (TIFFRGBAImageGet(&img, buf.data(), L.width, static_cast<std::uint32_t>(rows)) != 1) {
      return status::corrupt;
    }
    if (out.rgba.empty() && !allocate(out)) return status::out_of_memory;
    for (std::uint64_t r = 0; r < rows; ++r) {
      std::uint8_t* dst = out.rgba.data() + static_cast<std::size_t>((y + r) * w * 4);
      const std::uint32_t* src = buf.data() + static_cast<std::size_t>(r * w);
      for (std::uint64_t x = 0; x < w; ++x) {
        const std::uint32_t px = src[x];
        std::uint8_t cr = static_cast<std::uint8_t>(TIFFGetR(px));
        std::uint8_t cg = static_cast<std::uint8_t>(TIFFGetG(px));
        std::uint8_t cb = static_cast<std::uint8_t>(TIFFGetB(px));
        const std::uint8_t ca = static_cast<std::uint8_t>(TIFFGetA(px));
        if (has_alpha) {
          cr = unpremultiply(cr, ca);
          cg = unpremultiply(cg, ca);
          cb = unpremultiply(cb, ca);
        }
        dst[x * 4 + 0] = cr;
        dst[x * 4 + 1] = cg;
        dst[x * 4 + 2] = cb;
        dst[x * 4 + 3] = ca;
      }
    }
  }
  return status::ok;
}

result<raster> decode_page(std::span<const std::uint8_t> bytes, std::uint16_t page,
                           const job_context* ctx) {
  open_options o;
  if (!o.opts) return err(status::out_of_memory);
  TIFFOpenOptionsSetErrorHandlerExtR(o.opts, quiet_handler, nullptr);
  TIFFOpenOptionsSetWarningHandlerExtR(o.opts, quiet_handler, nullptr);
  TIFFOpenOptionsSetWarnAboutUnknownTags(o.opts, 0);
  TIFFOpenOptionsSetMaxSingleMemAlloc(o.opts, kMaxSingleAlloc);
  TIFFOpenOptionsSetMaxCumulatedMemAlloc(o.opts, kMaxCumulatedAlloc);

  mem_stream stream{bytes.data(), bytes.size(), 0};
  tiff_handle h;
  // No file name: libtiff never sees a path. "m" disables mapping; the write
  // proc refuses, so the original bytes cannot be touched.
  h.tif = TIFFClientOpenExt("tiff", "rm", &stream, mem_read, mem_write, mem_seek, mem_close,
                            mem_size, mem_map, mem_unmap, o.opts);
  if (!h.tif) return err(status::corrupt);
  if (page != 0 && TIFFSetDirectory(h.tif, page) != 1) return err(status::invalid_arg);

  layout L;
  auto kind = inspect(h.tif, L);
  if (!kind) return err(kind.error());

  raster out;
  out.format = format_family::tiff;
  out.intent = transfer_intent::display_referred;
  out.width = L.width;
  out.height = L.height;

  const bool rgb_like = kind.value() == route::native
                            ? (L.m == model::rgb || L.m == model::palette)
                            : (L.photometric == PHOTOMETRIC_YCBCR || L.photometric == PHOTOMETRIC_RGB);
  if (rgb_like && !read_icc(h.tif, out.icc)) return err(status::out_of_memory);
  L.encode_srgb = L.is_float && out.icc.empty();

  if (ctx && ctx->cancelled()) return err(status::cancelled);
  const status s = kind.value() == route::native ? read_native(h.tif, L, out, ctx)
                                                 : read_rgba_image(h.tif, L, out, ctx);
  if (s != status::ok) return err(s);
  if (out.rgba.empty()) return err(status::corrupt);
  return out;
}

}  // namespace

result<raster> decode_tiff(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::tiff) return err(status::unsupported_format);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  return decode_page(bytes, 0, ctx);
}

}  // namespace mv::codec
