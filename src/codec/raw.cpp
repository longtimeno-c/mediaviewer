// SPDX-License-Identifier: GPL-2.0-or-later
// RAW via LibRaw (LGPL, dynamic, libraw::raw_r). Embedded JPEG is first
// pixel; full dcraw output replaces it. Never writes the original.
//
// Read-only by construction: LibRaw is only ever handed the caller's buffer
// through open_buffer(). It has no file name, so it has nothing to write to,
// and none of the writer entry points (dcraw_ppm_tiff_writer, thumb writer)
// are called from this file (rule 5).
//
// Colour and brightness choices, measured in tests/test_raw.cpp:
// - Output is LibRaw's 8-bit sRGB (output_color=1) with the sRGB tone curve
//   (gamm = 1/2.4, 12.92), camera white balance, camera matrix. That is
//   display-referred sRGB already, so the raster is `display_referred`,
//   tagged sRGB, no ICC: the colour stage copies it through with no tone map
//   (D6). "Untagged RAW -> camera matrix from LibRaw" (plan/04) happens here.
// - Highlights clip (highlight=0), matching what the embedded JPEG shows.
// - Auto-bright stays on (dcraw default, 1 % clip). A fixed white point is
//   visibly darker than the camera's own JPEG, which is the pop the verify
//   line forbids; auto-bright lands much closer (still somewhat brighter).
// - PR 11: decode_raw_linear is the same develop at 16 bits with a linear
//   curve (the edit working space, D6), so the adjust pane's pixels are the
//   viewer's pixels before the sRGB curve and the 8-bit rounding.
// - Demosaic defaults to PPG — see default_options() for the numbers.
#include "codec/raw_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#include "codec/decode.h"

#include <libraw/libraw.h>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
// LibRaw refuses (LIBRAW_TOO_BIG) a raw buffer over this. 2 GB covers a
// 150 MP 16-bit frame with its 4-channel working image.
constexpr unsigned kMaxRawMemoryMb = 2048;

// ---------------------------------------------------------------------------
// TIFF-container sniffing. Bounds-checked, allocation-free, noexcept.
// ---------------------------------------------------------------------------

struct tiff_reader {
  std::span<const std::uint8_t> b;
  bool le = true;

  [[nodiscard]] bool has(std::uint64_t off, std::uint64_t n) const noexcept {
    return off <= b.size() && n <= b.size() - off;
  }
  [[nodiscard]] std::uint16_t u16(std::size_t off) const noexcept {
    return le ? static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8))
              : static_cast<std::uint16_t>((b[off] << 8) | b[off + 1]);
  }
  [[nodiscard]] std::uint32_t u32(std::size_t off) const noexcept {
    return le ? (static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
                 (static_cast<std::uint32_t>(b[off + 2]) << 16) |
                 (static_cast<std::uint32_t>(b[off + 3]) << 24))
              : ((static_cast<std::uint32_t>(b[off]) << 24) |
                 (static_cast<std::uint32_t>(b[off + 1]) << 16) |
                 (static_cast<std::uint32_t>(b[off + 2]) << 8) |
                 static_cast<std::uint32_t>(b[off + 3]));
  }
};

// Makers whose TIFF-container files are camera RAW when the IFD also carries
// raw structure. A maker name alone is not enough: cameras (and Photoshop
// keeping EXIF) write plain RGB TIFFs with a Make tag too.
bool raw_maker(std::span<const std::uint8_t> make) noexcept {
  static constexpr const char* kMakers[] = {
      "canon",   "nikon",   "sony",       "pentax",  "ricoh",   "samsung",     "olympus",
      "om digital", "panasonic", "leica", "fujifilm", "hasselblad", "phase one", "leaf",
      "mamiya",  "kodak",   "eastman kodak", "seiko epson", "sigma", "minolta", "konica minolta",
      "dji",     "gopro",   "apple",      "google",  "xiaomi",  "huawei",      "oneplus",
      "yuneec",  "parrot",  "blackmagic", "zeiss",   "nokia",   "motorola",    "lg",
  };
  std::size_t n = make.size();
  while (n > 0 && (make[n - 1] == 0 || make[n - 1] == ' ')) --n;
  std::size_t start = 0;
  while (start < n && make[start] == ' ') ++start;
  if (start >= n) return false;
  for (const char* m : kMakers) {
    const std::size_t len = std::strlen(m);
    if (n - start < len) continue;
    bool eq = true;
    for (std::size_t i = 0; i < len && eq; ++i) {
      std::uint8_t c = make[start + i];
      if (c >= 'A' && c <= 'Z') c = static_cast<std::uint8_t>(c - 'A' + 'a');
      eq = c == static_cast<std::uint8_t>(m[i]);
    }
    if (eq) return true;
  }
  return false;
}

bool tiff_container_is_raw(std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.size() < 16) return false;
  tiff_reader r{bytes, bytes[0] == 'I'};
  // CR2: "CR" + major version right after the TIFF header.
  if (bytes[8] == 'C' && bytes[9] == 'R') return true;
  // Phase One IIQ.
  if (std::memcmp(bytes.data() + 8, "IIII", 4) == 0) return true;

  const std::uint32_t ifd = r.u32(4);
  if (!r.has(ifd, 2)) return false;
  const std::uint32_t count = r.u16(ifd);
  if (count == 0 || count > 1024 || !r.has(ifd + 2ull, 12ull * count)) return false;

  bool vendor = false;
  bool structure = false;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t e = ifd + 2 + 12 * static_cast<std::size_t>(i);
    const std::uint16_t tag = r.u16(e);
    const std::uint16_t type = r.u16(e + 2);
    const std::uint32_t n = r.u32(e + 4);
    // SHORT or LONG scalar in the value field.
    const std::uint32_t scalar = type == 3 ? r.u16(e + 8) : r.u32(e + 8);
    switch (tag) {
      case 50706:  // DNGVersion
        return true;
      case 262:    // PhotometricInterpretation: CFA / LinearRaw
        if (scalar == 32803 || scalar == 34892) return true;
        break;
      case 259:    // Compression: vendor RAW codecs (NEF, Sony, Samsung, Kodak, Pentax)
        if (scalar == 34713 || scalar == 32767 || scalar == 32769 || scalar == 32770 ||
            scalar == 32772 || scalar == 32773 || scalar == 65000 || scalar == 65535 ||
            scalar == 262 || scalar == 99) {
          structure = true;
        }
        break;
      case 254:    // NewSubFileType: IFD0 is a reduced-resolution preview
        if (scalar == 1) structure = true;
        break;
      case 330:    // SubIFDs: the raw frame lives below IFD0
      case 50740:  // DNGPrivateData / Sony SR2Private
        structure = true;
        break;
      case 271: {  // Make
        if (type != 2 || n == 0) break;
        const std::uint32_t len = std::min<std::uint32_t>(n, 64);
        if (n <= 4) {
          vendor = raw_maker(bytes.subspan(e + 8, len));
        } else {
          const std::uint32_t off = r.u32(e + 8);
          if (r.has(off, len)) vendor = raw_maker(bytes.subspan(off, len));
        }
        break;
      }
      default:
        break;
    }
  }
  return vendor && structure;
}

// ---------------------------------------------------------------------------
// LibRaw plumbing
// ---------------------------------------------------------------------------

// LibRaw's default prints "unexpected end of file" to stderr. The count is
// still kept (LibRaw::error_count()); we read it instead of printing.
void silent_data_error(void*, const char*, const INT64) {}

int progress_cb(void* data, enum LibRaw_progress, int, int) {
  const auto* ctx = static_cast<const job_context*>(data);
  return (ctx != nullptr && ctx->cancelled()) ? 1 : 0;
}

status map_libraw(int ec) noexcept {
  switch (ec) {
    case LIBRAW_SUCCESS: return status::ok;
    case LIBRAW_FILE_UNSUPPORTED:
    case LIBRAW_NOT_IMPLEMENTED:
    case LIBRAW_NO_THUMBNAIL:
    case LIBRAW_UNSUPPORTED_THUMBNAIL:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_THUMBNAIL:
    case LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE:
    case LIBRAW_TOO_BIG:  // over max_raw_memory_mb: a limit, like kMaxPixels
      return status::unsupported_format;
    case LIBRAW_UNSUFFICIENT_MEMORY:
    case LIBRAW_MEMPOOL_OVERFLOW:
      return status::out_of_memory;
    case LIBRAW_CANCELLED_BY_CALLBACK:
      return status::cancelled;
    case LIBRAW_OUT_OF_ORDER_CALL:
      return status::internal;
    default:  // DATA_ERROR, IO_ERROR (a truncated buffer), BAD_CROP, unspecified
      return status::corrupt;
  }
}

struct processed_image_deleter {
  void operator()(libraw_processed_image_t* p) const noexcept { LibRaw::dcraw_clear_mem(p); }
};
using processed_image = std::unique_ptr<libraw_processed_image_t, processed_image_deleter>;

// Output-frame size before flip, including LibRaw's pixel-aspect stretch.
struct frame_size {
  double width = 0;
  double height = 0;
};

frame_size unflipped_size(const libraw_image_sizes_t& s) noexcept {
  frame_size f{static_cast<double>(s.width), static_cast<double>(s.height)};
  if (s.pixel_aspect > 0.0 && s.pixel_aspect < 1.0) f.height /= s.pixel_aspect;
  if (s.pixel_aspect > 1.0) f.width *= s.pixel_aspect;
  return f;
}

// LibRaw normalises a degree-valued flip exactly this way before writing the
// processed image (dcraw_make_mem_image); the preview must match it.
int normalized_flip(int flip) noexcept {
  switch ((flip + 3600) % 360) {
    case 270: return 5;
    case 180: return 3;
    case 90:  return 6;
    default:  return flip & 7;
  }
}

result<std::unique_ptr<LibRaw>> open_raw(std::span<const std::uint8_t> bytes,
                                         const job_context* ctx) {
  auto lr = std::unique_ptr<LibRaw>(new (std::nothrow) LibRaw(LIBRAW_OPTIONS_NONE));
  if (!lr) return err(status::out_of_memory);
  lr->imgdata.rawparams.max_raw_memory_mb = kMaxRawMemoryMb;
  lr->set_dataerror_handler(&silent_data_error, nullptr);
  if (ctx != nullptr) {
    // LibRaw's callback data is void*; progress_cb only reads it back as const.
    lr->set_progress_handler(&progress_cb, const_cast<void*>(static_cast<const void*>(ctx)));
  }
  const int ec = lr->open_buffer(bytes.data(), bytes.size());
  if (ec != LIBRAW_SUCCESS) return err(map_libraw(ec));

  const auto& s = lr->imgdata.sizes;
  if (s.raw_width == 0 || s.raw_height == 0 || s.width == 0 || s.height == 0) {
    return err(status::corrupt);
  }
  const frame_size f = unflipped_size(s);
  if (static_cast<std::uint64_t>(s.raw_width) * s.raw_height > kMaxPixels ||
      f.width > kMaxDim || f.height > kMaxDim || f.width * f.height > static_cast<double>(kMaxPixels)) {
    return err(status::unsupported_format);
  }
  if (ctx != nullptr && ctx->cancelled()) return err(status::cancelled);
  return lr;
}

// EXIF Orientation (0x0112) from a JPEG's APP1, or 0 when absent/unreadable.
int jpeg_exif_orientation(std::span<const std::uint8_t> jpeg) noexcept {
  std::size_t p = 2;
  while (p + 4 <= jpeg.size()) {
    if (jpeg[p] != 0xFF) return 0;
    const std::uint8_t marker = jpeg[p + 1];
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      p += 2;
      continue;
    }
    if (marker == 0xDA || marker == 0xD9) return 0;  // scan data: no more headers
    const std::size_t len = static_cast<std::size_t>(jpeg[p + 2] << 8 | jpeg[p + 3]);
    if (len < 2 || p + 2 + len > jpeg.size()) return 0;
    if (marker == 0xE1 && len >= 16 && std::memcmp(jpeg.data() + p + 4, "Exif\0\0", 6) == 0) {
      const auto tiff = jpeg.subspan(p + 10, len - 8);
      if (tiff.size() < 8 || !(tiff[0] == 'I' || tiff[0] == 'M')) return 0;
      tiff_reader r{tiff, tiff[0] == 'I'};
      const std::uint32_t ifd = r.u32(4);
      if (!r.has(ifd, 2)) return 0;
      const std::uint32_t count = r.u16(ifd);
      if (!r.has(ifd + 2ull, 12ull * count)) return 0;
      for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t e = ifd + 2 + 12 * static_cast<std::size_t>(i);
        if (r.u16(e) == 0x0112) {
          const int v = r.u16(e + 8);
          return (v >= 1 && v <= 8) ? v : 0;
        }
      }
      return 0;
    }
    p += 2 + len;
  }
  return 0;
}

// Rotate/mirror `src` (w x h RGBA) by a LibRaw flip code, with the same
// output->source mapping as LibRaw's flip_index(), so preview and full decode
// land pixel-for-pixel in the same orientation.
bool apply_flip(raster& img, int flip) {
  flip &= 7;
  if (flip == 0) return true;
  const std::uint32_t iw = img.width;
  const std::uint32_t ih = img.height;
  const std::uint32_t ow = (flip & 4) ? ih : iw;
  const std::uint32_t oh = (flip & 4) ? iw : ih;
  std::vector<std::uint8_t> out;
  try {
    out.resize(static_cast<std::size_t>(ow) * oh * 4);
  } catch (const std::bad_alloc&) {
    return false;
  }
  for (std::uint32_t row = 0; row < oh; ++row) {
    std::uint8_t* dst = out.data() + static_cast<std::size_t>(row) * ow * 4;
    for (std::uint32_t col = 0; col < ow; ++col) {
      std::uint32_t sr = row;
      std::uint32_t sc = col;
      if (flip & 4) std::swap(sr, sc);
      if (flip & 2) sr = ih - 1 - sr;
      if (flip & 1) sc = iw - 1 - sc;
      std::memcpy(dst + col * 4, img.rgba.data() + (static_cast<std::size_t>(sr) * iw + sc) * 4, 4);
    }
  }
  img.rgba = std::move(out);
  img.width = ow;
  img.height = oh;
  return true;
}

int preview_scale(std::uint32_t w, std::uint32_t h, std::uint32_t min_long_side) noexcept {
  if (min_long_side == 0) return 1;
  const std::uint32_t long_side = std::max(w, h);
  int scale = 1;
  for (int s : {2, 4, 8}) {
    if (long_side / static_cast<std::uint32_t>(s) >= min_long_side) scale = s;
  }
  return scale;
}

result<raster> bitmap_thumb(const libraw_thumbnail_t& t) {
  const bool wide = t.tformat == LIBRAW_THUMBNAIL_BITMAP16;
  const std::uint32_t w = t.twidth;
  const std::uint32_t h = t.theight;
  const int colors = t.tcolors;
  if (w == 0 || h == 0 || (colors != 1 && colors != 3) || t.thumb == nullptr) {
    return err(status::unsupported_format);
  }
  const std::uint64_t need = static_cast<std::uint64_t>(w) * h * static_cast<unsigned>(colors) *
                             (wide ? 2u : 1u);
  if (t.tlength < need) return err(status::corrupt);
  raster out;
  try {
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  const auto* src = reinterpret_cast<const std::uint8_t*>(t.thumb);
  const std::size_t step = static_cast<std::size_t>(colors) * (wide ? 2 : 1);
  for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
    const std::uint8_t* s = src + i * step;
    std::uint8_t* d = out.rgba.data() + i * 4;
    for (int c = 0; c < 3; ++c) {
      const std::size_t ci = colors == 3 ? static_cast<std::size_t>(c) : 0;
      if (wide) {
        std::uint16_t v = 0;
        std::memcpy(&v, s + ci * 2, 2);  // host-endian, per LibRaw
        d[c] = static_cast<std::uint8_t>(v >> 8);
      } else {
        d[c] = s[ci];
      }
    }
    d[3] = 255;
  }
  out.width = w;
  out.height = h;
  return out;
}

result<raster> preview_impl(std::span<const std::uint8_t> bytes, const job_context* ctx,
                            const raw_detail::raw_options& opt) {
  if (ctx != nullptr && ctx->cancelled()) return err(status::cancelled);
  if (!looks_like_raw(bytes)) return err(status::unsupported_format);

  auto opened = open_raw(bytes, ctx);
  if (!opened) return err(opened.error());
  LibRaw& lr = *opened.value();

  // Largest embedded preview. LibRaw >= 0.21 lists every one it found; the
  // plain unpack_thumb() pick is not always the biggest (ARW, CR3).
  int ec = LIBRAW_NO_THUMBNAIL;
#if LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0, 21)
  {
    const auto& list = lr.imgdata.thumbs_list;
    const int n = std::clamp(list.thumbcount, 0, LIBRAW_THUMBNAIL_MAXCOUNT);
    int best = -1;
    std::uint64_t best_area = 0;
    bool best_jpeg = false;
    for (int i = 0; i < n; ++i) {
      const auto& item = list.thumblist[i];
      const std::uint64_t area = static_cast<std::uint64_t>(item.twidth) * item.theight;
      const bool jpeg = item.tformat == LIBRAW_INTERNAL_THUMBNAIL_JPEG;
      if (best < 0 || area > best_area || (area == best_area && jpeg && !best_jpeg)) {
        best = i;
        best_area = area;
        best_jpeg = jpeg;
      }
    }
    if (best >= 0) ec = lr.unpack_thumb_ex(best);
  }
#endif
  if (ec != LIBRAW_SUCCESS && ec != LIBRAW_CANCELLED_BY_CALLBACK &&
      !LIBRAW_FATAL_ERROR(ec)) {
    ec = lr.unpack_thumb();
  }
  if (ec != LIBRAW_SUCCESS) return err(map_libraw(ec));
  if (ctx != nullptr && ctx->cancelled()) return err(status::cancelled);

  const libraw_thumbnail_t& t = lr.imgdata.thumbnail;
  result<raster> decoded = err(status::unsupported_format);
  int jpeg_orientation = 0;
  if (t.tformat == LIBRAW_THUMBNAIL_JPEG) {
    if (t.thumb == nullptr || t.tlength < 4) return err(status::unsupported_format);
    const std::span<const std::uint8_t> jpeg(reinterpret_cast<const std::uint8_t*>(t.thumb),
                                             t.tlength);
    if (probe(jpeg) != format_family::jpeg) return err(status::unsupported_format);
    auto dims = jpeg_dimensions(jpeg);
    if (!dims) return err(dims.error());
    jpeg_orientation = jpeg_exif_orientation(jpeg);
    decoded = decode_jpeg(jpeg, ctx,
                          preview_scale(dims->width, dims->height, opt.preview_min_long_side));
  } else if (t.tformat == LIBRAW_THUMBNAIL_BITMAP || t.tformat == LIBRAW_THUMBNAIL_BITMAP16) {
    decoded = bitmap_thumb(t);
  }
  if (!decoded) {
    // A bad embedded preview is not a bad RAW: the full decode may still work.
    if (decoded.error() == status::cancelled || decoded.error() == status::out_of_memory) {
      return err(decoded.error());
    }
    return err(status::unsupported_format);
  }
  raster out = std::move(decoded).value();

  // Orientation: the full decode is rotated by sizes.flip. Embedded previews
  // are stored in sensor orientation, so they get the same flip — unless the
  // preview is visibly already rotated. A JPEG that carries its own non-1
  // EXIF orientation is by definition unrotated. Otherwise, for a 90° flip the
  // aspect ratio tells: a preview whose aspect matches the *rotated* frame
  // (and not the sensor frame) was rotated in-camera and is left alone. A
  // 180° or mirror flip cannot be told apart that way and is applied.
  const int flip = normalized_flip(lr.imgdata.sizes.flip);
  bool rotate = flip != 0;
  if (rotate && (flip & 4) && jpeg_orientation <= 1) {
    const frame_size f = unflipped_size(lr.imgdata.sizes);
    const double sensor = std::log(f.width / f.height);
    const double preview = std::log(static_cast<double>(out.width) / out.height);
    constexpr double kSquareish = 0.05;
    if (std::abs(sensor) > kSquareish &&
        std::abs(preview + sensor) < std::abs(preview - sensor)) {
      rotate = false;
    }
  }
  if (rotate && !apply_flip(out, flip)) return err(status::out_of_memory);

  out.format = format_family::raw;
  out.intent = transfer_intent::display_referred;
  // Camera previews are sRGB unless they embed a profile; keep one if present.
  out.tagged_srgb = out.icc.empty();
  return out;
}

// dcraw_process with the viewer's settings, in one of two encodings: 8-bit
// with the sRGB tone curve (the viewer's full decode), or 16-bit linear
// (PR 11: the edit working space, D6). Everything else — white balance,
// matrix, demosaic, auto-bright, highlight clip, flip — is the same, so the
// two agree up to the transfer curve and the bit depth.
result<processed_image> develop(std::span<const std::uint8_t> bytes, const job_context* ctx,
                                const raw_detail::raw_options& opt, bool linear16) {
  if (ctx != nullptr && ctx->cancelled()) return err(status::cancelled);
  if (!looks_like_raw(bytes)) return err(status::unsupported_format);

  auto opened = open_raw(bytes, ctx);
  if (!opened) return err(opened.error());
  LibRaw& lr = *opened.value();

  auto& p = lr.imgdata.params;
  p.use_camera_wb = 1;
  p.use_camera_matrix = 1;
  p.output_color = 1;  // sRGB primaries
  p.output_bps = linear16 ? 16 : 8;
  if (linear16) {
    p.gamm[0] = 1.0;  // linear: dcraw's curve with power 1 and no toe
    p.gamm[1] = 1.0;
  } else {
    p.gamm[0] = 1.0 / 2.4;  // sRGB transfer (dcraw's default is BT.709 0.45/4.5)
    p.gamm[1] = 12.92;
  }
  p.highlight = 0;  // clip
  p.no_auto_bright = opt.auto_bright ? 0 : 1;
  p.user_qual = static_cast<int>(opt.quality);
  p.half_size = 0;
  p.user_flip = -1;  // honour the file's orientation

  // A raw strip that the file declares but does not contain. Sony's ARW
  // loaders read it in one go and never flag the short read (error_count()
  // below stays 0), then demosaic uninitialised memory into a "success".
  // data_size is 0 for formats that do not declare it; those rely on the
  // error count instead.
  if (const libraw_internal_data_t* internal = lr.get_internal_data_pointer()) {
    const INT64 off = internal->unpacker_data.data_offset;
    const INT64 size = internal->unpacker_data.data_size;
    if (off < 0 || size < 0 ||
        (size > 0 && static_cast<std::uint64_t>(off) + static_cast<std::uint64_t>(size) >
                         bytes.size())) {
      return err(status::corrupt);
    }
  }

  int ec = lr.unpack();
  if (ec != LIBRAW_SUCCESS) return err(map_libraw(ec));
  if (ctx != nullptr && ctx->cancelled()) return err(status::cancelled);
  // Several vendor decoders (Sony ARW among them) zero-fill past the end of a
  // truncated buffer and return success, flagging it only through the data
  // error count. A half-black "full" image replacing a good preview is worse
  // than keeping the preview, so a short read is corrupt — and caught here,
  // before the seconds-long dcraw_process.
  if (lr.error_count() > 0) return err(status::corrupt);
  ec = lr.dcraw_process();
  if (ec != LIBRAW_SUCCESS) return err(map_libraw(ec));
  if (ctx != nullptr && ctx->cancelled()) return err(status::cancelled);

  int mem_ec = LIBRAW_SUCCESS;
  processed_image img(lr.dcraw_make_mem_image(&mem_ec));
  if (!img) return err(mem_ec != LIBRAW_SUCCESS ? map_libraw(mem_ec) : status::out_of_memory);
  const int bits = linear16 ? 16 : 8;
  if (img->type != LIBRAW_IMAGE_BITMAP || img->bits != bits ||
      (img->colors != 3 && img->colors != 1)) {
    return err(status::unsupported_format);
  }
  const std::uint32_t w = img->width;
  const std::uint32_t h = img->height;
  const auto colors = static_cast<std::uint64_t>(img->colors);
  if (w == 0 || h == 0 || w > kMaxDim || h > kMaxDim ||
      static_cast<std::uint64_t>(w) * h > kMaxPixels ||
      img->data_size < static_cast<std::uint64_t>(w) * h * colors * (bits / 8)) {
    return err(status::corrupt);
  }
  lr.recycle();  // drop the 16-bit working image before the caller's allocation
  return img;
}

result<raster> full_impl(std::span<const std::uint8_t> bytes, const job_context* ctx,
                         const raw_detail::raw_options& opt) {
  MV_TRY(processed_image img, develop(bytes, ctx, opt, false));
  const std::uint32_t w = img->width;
  const std::uint32_t h = img->height;
  const auto colors = static_cast<std::size_t>(img->colors);

  raster out;
  try {
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  const std::uint8_t* src = img->data;
  for (std::uint32_t y = 0; y < h; ++y) {
    if ((y & 255) == 0 && ctx != nullptr && ctx->cancelled()) return err(status::cancelled);
    const std::uint8_t* s = src + static_cast<std::size_t>(y) * w * colors;
    std::uint8_t* d = out.rgba.data() + static_cast<std::size_t>(y) * w * 4;
    if (colors == 3) {
      for (std::uint32_t x = 0; x < w; ++x) {
        d[x * 4 + 0] = s[x * 3 + 0];
        d[x * 4 + 1] = s[x * 3 + 1];
        d[x * 4 + 2] = s[x * 3 + 2];
        d[x * 4 + 3] = 255;
      }
    } else {
      for (std::uint32_t x = 0; x < w; ++x) {
        d[x * 4 + 0] = d[x * 4 + 1] = d[x * 4 + 2] = s[x];
        d[x * 4 + 3] = 255;
      }
    }
  }
  out.width = w;
  out.height = h;
  out.format = format_family::raw;
  out.intent = transfer_intent::display_referred;
  out.tagged_srgb = true;
  return out;
}

result<raster16> linear_impl(std::span<const std::uint8_t> bytes, const job_context* ctx,
                             const raw_detail::raw_options& opt) {
  MV_TRY(processed_image img, develop(bytes, ctx, opt, true));
  const std::uint32_t w = img->width;
  const std::uint32_t h = img->height;
  const auto colors = static_cast<std::size_t>(img->colors);

  raster16 out;
  try {
    out.rgba.resize(static_cast<std::size_t>(w) * h * 4);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  // LibRaw writes host-endian 16-bit samples; the buffer is only byte-aligned.
  const std::uint8_t* src = img->data;
  for (std::uint32_t y = 0; y < h; ++y) {
    if ((y & 255) == 0 && ctx != nullptr && ctx->cancelled()) return err(status::cancelled);
    const std::uint8_t* s = src + static_cast<std::size_t>(y) * w * colors * 2;
    std::uint16_t* d = out.rgba.data() + static_cast<std::size_t>(y) * w * 4;
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint16_t v[3];
      if (colors == 3) {
        std::memcpy(v, s + static_cast<std::size_t>(x) * 6, 6);
      } else {
        std::memcpy(v, s + static_cast<std::size_t>(x) * 2, 2);
        v[1] = v[2] = v[0];
      }
      d[x * 4 + 0] = v[0];
      d[x * 4 + 1] = v[1];
      d[x * 4 + 2] = v[2];
      d[x * 4 + 3] = 65535;
    }
  }
  out.width = w;
  out.height = h;
  out.format = format_family::raw;
  return out;
}

}  // namespace

bool looks_like_raw(std::span<const std::uint8_t> bytes) noexcept {
  switch (probe(bytes)) {
    case format_family::raw:  return true;
    case format_family::tiff: return tiff_container_is_raw(bytes);
    case format_family::unknown:
      // Canon CR3: ISO BMFF with major brand "crx " (trailing space). The
      // scaffolding probe compares "crx" + NUL and misses it, so a real CR3
      // probes as unknown; decode() and decode_preview() fall back to here.
      return bytes.size() >= 12 && std::memcmp(bytes.data() + 4, "ftypcrx ", 8) == 0;
    default:                  return false;
  }
}

namespace raw_detail {

// Measured with `mv_tests "[.raw-bench]"` (Release, single-threaded LibRaw
// 0.22.2 — the vcpkg build has no OpenMP), 2026-09-14:
//
//   file (MP)          linear   VNG     PPG     AHD    | preview 1:1 -> scaled
//   CR2 7D II  (20)     943    2947    1025    2565 ms |  75 -> 32 ms (2736 px)
//   NEF D7500  (21)     904    2971     977    2464 ms | 100 -> 50 ms (2784 px)
//   ARW A7R III(42)    1360    6996    1594    4741 ms |  10 ms (1616 px embedded)
//   DNG K-50   (16)     739    2305     791    1983 ms |  68 -> 33 ms (2464 px)
//
// PPG: within ~10 % of linear, 2.5-3x faster than AHD, without linear's
// zipper aliasing on edges. None of them meets plan/09's < 500 ms on 45 MP on
// the CPU; the embedded preview is what keeps the viewer instant.
//
// Auto-bright vs the embedded JPEG (mean luma, 0-255, full / preview):
//   on:  CR2 207/166  NEF 109/102  ARW 157/137  DNG 132/119
//   off: CR2 120/166  NEF  70/102  ARW  92/137  DNG  99/119
// On is consistently closer (mean gap ~20 vs ~36), so it stays on.
//
// Preview: DCT 1/2 while the long side stays >= 2048 px keeps first pixel
// under plan/09's 60 ms; the full decode refines it (rule 3).
raw_options default_options() noexcept {
  raw_options o;
  o.quality = demosaic::ppg;
  o.auto_bright = true;
  o.preview_min_long_side = 2048;
  return o;
}

// LibRaw catches its own internal exceptions at every API call; this is the
// belt to that braces so nothing C++ crosses into the job system unexpectedly.
result<raster> decode_raw_with(std::span<const std::uint8_t> bytes, const job_context* ctx,
                               const raw_options& opt) {
  try {
    return full_impl(bytes, ctx, opt);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  } catch (...) {
    return err(status::internal);
  }
}

result<raster16> decode_raw_linear_with(std::span<const std::uint8_t> bytes,
                                        const job_context* ctx, const raw_options& opt) {
  try {
    return linear_impl(bytes, ctx, opt);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  } catch (...) {
    return err(status::internal);
  }
}

result<raster> decode_raw_preview_with(std::span<const std::uint8_t> bytes,
                                       const job_context* ctx, const raw_options& opt) {
  try {
    return preview_impl(bytes, ctx, opt);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  } catch (...) {
    return err(status::internal);
  }
}

}  // namespace raw_detail

result<raster> decode_raw_preview(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  return raw_detail::decode_raw_preview_with(bytes, ctx, raw_detail::default_options());
}

result<raster> decode_raw(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  return raw_detail::decode_raw_with(bytes, ctx, raw_detail::default_options());
}

result<raster16> decode_raw_linear(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  return raw_detail::decode_raw_linear_with(bytes, ctx, raw_detail::default_options());
}

}  // namespace mv::codec
