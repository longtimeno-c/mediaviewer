// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/decode.h"

#include <csetjmp>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#include <jpeglib.h>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
// libjpeg's own working memory, separate from the raster we allocate. It is
// sized from the *source* dimensions inside jpeg_start_decompress, before our
// output-size check can run, so a 292-byte header declaring 56528x18759 cost
// 2.1 GB at 1:1 and 6.9 GB at 1/4 (fuzz_jpeg, CI run 36159301248). With a
// budget libjpeg fails the decode instead. 512 MB covers a 160 MP progressive
// 4:2:0 file at every scale (measured); baseline files need far less.
constexpr long kMaxLibjpegMemory = 512L * 1024 * 1024;
constexpr int kMaxIccChunks = 256;

struct jpeg_error_trap {
  jpeg_error_mgr pub;
  jmp_buf jump;
  unsigned char* icc;
  void* rows;  // the slow path's sample rows; freed on the longjmp path too
};

void jpeg_error_exit(j_common_ptr cinfo) {
  auto* trap = reinterpret_cast<jpeg_error_trap*>(cinfo->err);
  longjmp(trap->jump, 1);
}

// APP2 "ICC_PROFILE\0" + seq + count + payload. Assembled into a malloc'd
// buffer so the setjmp path can free it without running C++ destructors.
bool extract_icc(j_decompress_ptr cinfo, unsigned char** out, unsigned* out_len) {
  constexpr char kTag[] = "ICC_PROFILE";
  struct chunk {
    int seq;
    const JOCTET* data;
    unsigned length;
  };
  chunk chunks[kMaxIccChunks];
  int n = 0;
  int declared = 0;

  for (jpeg_saved_marker_ptr m = cinfo->marker_list; m != nullptr; m = m->next) {
    if (m->marker != JPEG_APP0 + 2) continue;
    if (m->data_length < 14) continue;
    if (std::memcmp(m->data, kTag, 12) != 0) continue;
    const int seq = m->data[12];
    const int count = m->data[13];
    if (seq < 1 || count < 1) continue;
    if (declared == 0) declared = count;
    if (n >= kMaxIccChunks) return false;
    chunks[n++] = chunk{seq, m->data + 14, m->data_length - 14};
  }
  if (n == 0 || declared <= 0 || n != declared) return false;

  for (int i = 0; i < n; ++i) {
    for (int j = i + 1; j < n; ++j) {
      if (chunks[j].seq < chunks[i].seq) {
        const chunk tmp = chunks[i];
        chunks[i] = chunks[j];
        chunks[j] = tmp;
      }
    }
  }

  unsigned total = 0;
  for (int i = 0; i < n; ++i) {
    if (chunks[i].seq != i + 1) return false;
    total += chunks[i].length;
  }
  unsigned char* icc = static_cast<unsigned char*>(std::malloc(total));
  if (!icc) return false;
  unsigned off = 0;
  for (int i = 0; i < n; ++i) {
    std::memcpy(icc + off, chunks[i].data, chunks[i].length);
    off += chunks[i].length;
  }
  *out = icc;
  *out_len = total;
  return true;
}

// What the slow path's sample rows hold. Lossy files get libjpeg-turbo's own
// YCbCr → RGB and YCCK → CMYK; lossless files are read in their own space.
enum class jpeg_px : std::uint8_t { grey, rgb, cmyk };

inline std::uint8_t sample_to8(unsigned v, unsigned max) noexcept {
  if (max == 255u) return static_cast<std::uint8_t>(v);
  return static_cast<std::uint8_t>((v * 255u + max / 2u) / max);
}

// Naive CMYK → RGB, as tiff.cpp does; the CMYK profile is not applied.
// Photoshop (any file with an Adobe APP14 marker) stores the inks inverted:
// 255 is no ink. Without the marker, 255 is full ink.
inline std::uint8_t ink_to_rgb(unsigned c, unsigned k, bool adobe) noexcept {
  if (!adobe) {
    c = 255u - c;
    k = 255u - k;
  }
  return static_cast<std::uint8_t>((c * k + 127u) / 255u);
}

// `src` holds `width` pixels of JSAMPLE / J12SAMPLE / J16SAMPLE in the layout
// `px` names; `max` is the data precision's largest sample.
template <typename T>
void jpeg_row_to_rgba(const T* src, std::uint32_t width, jpeg_px px, unsigned max, bool adobe,
                      std::uint8_t* dst) noexcept {
  const auto get = [max](T v) noexcept {
    const long s = static_cast<long>(v);  // J12SAMPLE is signed
    return s <= 0 ? 0u : s >= static_cast<long>(max) ? max : static_cast<unsigned>(s);
  };
  for (std::uint32_t x = 0; x < width; ++x) {
    std::uint8_t* d = dst + static_cast<std::size_t>(x) * 4;
    d[3] = 255;
    switch (px) {
      case jpeg_px::grey:
        d[0] = d[1] = d[2] = sample_to8(get(src[x]), max);
        break;
      case jpeg_px::rgb: {
        const T* s = src + static_cast<std::size_t>(x) * 3;
        for (int c = 0; c < 3; ++c) d[c] = sample_to8(get(s[c]), max);
        break;
      }
      case jpeg_px::cmyk: {
        const T* s = src + static_cast<std::size_t>(x) * 4;
        const unsigned k = sample_to8(get(s[3]), max);
        for (int c = 0; c < 3; ++c) d[c] = ink_to_rgb(sample_to8(get(s[c]), max), k, adobe);
        break;
      }
    }
  }
}

}  // namespace

result<raster> decode_jpeg(std::span<const std::uint8_t> bytes, const job_context* ctx,
                           int scale_denom) {
  if (probe(bytes) != format_family::jpeg) return err(status::unsupported_format);
  if (bytes.size() < 4) return err(status::corrupt);

  // The result lives on the heap and is created before setjmp: libjpeg writes
  // straight into its pixel vector, and the unique_ptr itself is never changed
  // after setjmp, so the longjmp path can still destroy it normally.
  std::unique_ptr<raster> out;
  try {
    out = std::make_unique<raster>();
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }

  jpeg_decompress_struct cinfo{};
  jpeg_error_trap jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  jerr.pub.output_message = [](j_common_ptr) {};

  // C4611: longjmp skips C++ destructors. Everything created after this setjmp
  // is POD or a malloc the jump handler frees.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
  if (setjmp(jerr.jump)) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.icc);
    std::free(jerr.rows);
    return err(status::corrupt);
  }

  jpeg_create_decompress(&cinfo);
  cinfo.mem->max_memory_to_use = kMaxLibjpegMemory;
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(bytes.data()),
               static_cast<unsigned long>(bytes.size()));
  jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return err(status::corrupt);
  }

  unsigned icc_len = 0;
  (void)extract_icc(&cinfo, &jerr.icc, &icc_len);

  // jpeg_read_header stops after the first SOS. A lossless (SOF3) scan is not
  // progressive and has Se = 0; a sequential DCT scan always has Se = 63.
  const bool lossless = !cinfo.progressive_mode && cinfo.Se == 0;
  const J_COLOR_SPACE jcs = cinfo.jpeg_color_space;
  // Every camera JPEG: 8-bit lossy YCbCr, grey or RGB. libjpeg-turbo's SIMD
  // colour converter writes RGBX with alpha 255 straight into the raster,
  // instead of RGB into a row buffer and a scalar expand.
  const bool fast = cinfo.data_precision == 8 && !lossless &&
                    (jcs == JCS_YCbCr || jcs == JCS_GRAYSCALE || jcs == JCS_RGB);
  if (fast) {
    cinfo.out_color_space = JCS_EXT_RGBA;
  } else if (!lossless) {
    // CMYK / YCCK (Photoshop), 12-bit, or an unlabelled component set: the
    // library converts what it can, rows are expanded to RGBA8 below.
    cinfo.out_color_space = jcs == JCS_YCCK ? JCS_CMYK : jcs == JCS_YCbCr ? JCS_RGB : jcs;
  } else if (jcs == JCS_YCbCr || jcs == JCS_YCCK) {
    // libjpeg-turbo allows no colour conversion in lossless mode, and its own
    // encoder does not round-trip lossless YCbCr, so there is nothing to check
    // a converter here against. Refused rather than shown wrong; lossless
    // files in practice are grey or RGB.
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.icc);
    return err(status::unsupported_format);
  } else {
    cinfo.out_color_space = jcs;  // lossless: null conversion only
  }
  if (scale_denom != 2 && scale_denom != 4 && scale_denom != 8) scale_denom = 1;
  cinfo.scale_num = 1;
  cinfo.scale_denom = scale_denom;  // lossless decodes at 1:1 whatever is asked
  jpeg_start_decompress(&cinfo);

  const auto width = static_cast<std::uint32_t>(cinfo.output_width);
  const auto height = static_cast<std::uint32_t>(cinfo.output_height);
  if (width == 0 || height == 0 || width > kMaxDim || height > kMaxDim ||
      static_cast<std::uint64_t>(width) * height > kMaxPixels) {
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.icc);
    return err(status::unsupported_format);
  }

  jpeg_px px = jpeg_px::rgb;
  const int comps = cinfo.output_components;
  bool layout_ok = fast ? comps == 4 : false;
  if (!fast) {
    switch (cinfo.out_color_space) {
      case JCS_GRAYSCALE: px = jpeg_px::grey; layout_ok = comps == 1; break;
      case JCS_RGB: px = jpeg_px::rgb; layout_ok = comps == 3; break;
      case JCS_CMYK: px = jpeg_px::cmyk; layout_ok = comps == 4; break;
      default:  // JCS_UNKNOWN: read by component count
        px = comps == 1 ? jpeg_px::grey : comps == 3 ? jpeg_px::rgb : jpeg_px::cmyk;
        layout_ok = comps == 1 || comps == 3 || comps == 4;
        break;
    }
  }
  if (!layout_ok) {
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.icc);
    return err(status::unsupported_format);
  }
  // A CMYK profile describes the inks, not the RGB we hand out (tiff.cpp
  // drops it the same way): the display stage builds RGBA transforms.
  const bool keep_icc = fast || px != jpeg_px::cmyk;

  const std::size_t stride = static_cast<std::size_t>(width) * 4;
  try {
    out->rgba.resize(stride * height);
    if (keep_icc && jerr.icc && icc_len > 0) out->icc.assign(jerr.icc, jerr.icc + icc_len);
  } catch (const std::bad_alloc&) {
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.icc);
    return err(status::out_of_memory);
  }
  std::free(jerr.icc);
  jerr.icc = nullptr;

  // Several rows a call: libjpeg-turbo emits up to rec_outbuf_height rows
  // (2 for 4:2:0) per call, and fancy upsampling otherwise goes through its
  // spare row buffer one row at a time.
  constexpr int kRowsPerCall = 16;
  unsigned char* const pixels = out->rgba.data();
  if (fast) {
    JSAMPROW rows[kRowsPerCall];
    while (cinfo.output_scanline < cinfo.output_height) {
      if (ctx && ctx->cancelled()) {
        jpeg_destroy_decompress(&cinfo);
        return err(status::cancelled);
      }
      const JDIMENSION first = cinfo.output_scanline;
      const int n = static_cast<int>(std::min<JDIMENSION>(kRowsPerCall, cinfo.output_height - first));
      for (int r = 0; r < n; ++r) rows[r] = pixels + static_cast<std::size_t>(first + r) * stride;
      if (jpeg_read_scanlines(&cinfo, rows, static_cast<JDIMENSION>(n)) == 0) break;
    }
  } else {
    // Samples are JSAMPLE (2-8 bits), J12SAMPLE (9-12) or J16SAMPLE (13-16),
    // each read through its own libjpeg-turbo entry point.
    const int precision = cinfo.data_precision;
    const unsigned max = (1u << precision) - 1u;
    const std::size_t sample_size = precision <= 8 ? 1 : 2;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * comps * sample_size;
    jerr.rows = std::malloc(row_bytes * kRowsPerCall);
    if (!jerr.rows) {
      jpeg_destroy_decompress(&cinfo);
      return err(status::out_of_memory);
    }
    auto* const buf = static_cast<unsigned char*>(jerr.rows);
    const bool adobe = cinfo.saw_Adobe_marker != 0;
    while (cinfo.output_scanline < cinfo.output_height) {
      if (ctx && ctx->cancelled()) {
        jpeg_destroy_decompress(&cinfo);
        std::free(jerr.rows);
        return err(status::cancelled);
      }
      const JDIMENSION first = cinfo.output_scanline;
      const int n = static_cast<int>(std::min<JDIMENSION>(kRowsPerCall, cinfo.output_height - first));
      JDIMENSION got = 0;
      if (precision <= 8) {
        JSAMPROW rows[kRowsPerCall];
        for (int r = 0; r < n; ++r) rows[r] = buf + r * row_bytes;
        got = jpeg_read_scanlines(&cinfo, rows, static_cast<JDIMENSION>(n));
      } else if (precision <= 12) {
        J12SAMPROW rows[kRowsPerCall];
        for (int r = 0; r < n; ++r) rows[r] = reinterpret_cast<J12SAMPROW>(buf + r * row_bytes);
        got = jpeg12_read_scanlines(&cinfo, rows, static_cast<JDIMENSION>(n));
      } else {
        J16SAMPROW rows[kRowsPerCall];
        for (int r = 0; r < n; ++r) rows[r] = reinterpret_cast<J16SAMPROW>(buf + r * row_bytes);
        got = jpeg16_read_scanlines(&cinfo, rows, static_cast<JDIMENSION>(n));
      }
      for (JDIMENSION r = 0; r < got; ++r) {
        const unsigned char* src = buf + r * row_bytes;
        std::uint8_t* dst = pixels + static_cast<std::size_t>(first + r) * stride;
        if (precision <= 8) {
          jpeg_row_to_rgba(src, width, px, max, adobe, dst);
        } else if (precision <= 12) {
          jpeg_row_to_rgba(reinterpret_cast<const J12SAMPLE*>(src), width, px, max, adobe, dst);
        } else {
          jpeg_row_to_rgba(reinterpret_cast<const J16SAMPLE*>(src), width, px, max, adobe, dst);
        }
      }
      if (got == 0) break;
    }
    std::free(jerr.rows);
    jerr.rows = nullptr;
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  out->format = format_family::jpeg;
  out->intent = transfer_intent::display_referred;
  out->width = width;
  out->height = height;
  return std::move(*out);
}

result<jpeg_size> jpeg_dimensions(std::span<const std::uint8_t> bytes) {
  if (probe(bytes) != format_family::jpeg) return err(status::unsupported_format);
  if (bytes.size() < 4) return err(status::corrupt);

  jpeg_decompress_struct cinfo{};
  jpeg_error_trap jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  jerr.pub.output_message = [](j_common_ptr) {};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
  if (setjmp(jerr.jump)) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    jpeg_destroy_decompress(&cinfo);
    return err(status::corrupt);
  }

  jpeg_create_decompress(&cinfo);
  cinfo.mem->max_memory_to_use = kMaxLibjpegMemory;
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(bytes.data()),
               static_cast<unsigned long>(bytes.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return err(status::corrupt);
  }
  const jpeg_size size{static_cast<std::uint32_t>(cinfo.image_width),
                       static_cast<std::uint32_t>(cinfo.image_height)};
  jpeg_destroy_decompress(&cinfo);
  if (size.width == 0 || size.height == 0) return err(status::corrupt);
  return size;
}

struct jpeg_enc_trap {
  jpeg_error_mgr pub;
  jmp_buf jump;
  unsigned char* out = nullptr;
  std::size_t out_len = 0;
  std::size_t out_cap = 0;
  unsigned char chunk[4096]{};
  unsigned char* row = nullptr;
  bool oom = false;
};

void jpeg_enc_error_exit(j_common_ptr cinfo) {
  auto* trap = reinterpret_cast<jpeg_enc_trap*>(cinfo->err);
  longjmp(trap->jump, 1);
}

bool jpeg_enc_grow(jpeg_enc_trap* trap, std::size_t extra) {
  const std::size_t need = trap->out_len + extra;
  if (need <= trap->out_cap) return true;
  std::size_t cap = trap->out_cap ? trap->out_cap : 4096;
  while (cap < need) cap *= 2;
  auto* p = static_cast<unsigned char*>(std::realloc(trap->out, cap));
  if (!p) return false;
  trap->out = p;
  trap->out_cap = cap;
  return true;
}

result<std::vector<std::uint8_t>> encode_jpeg_rgba(std::span<const std::uint8_t> rgba,
                                                   std::uint32_t width, std::uint32_t height,
                                                   int quality) {
  if (width == 0 || height == 0 || quality < 1 || quality > 100) {
    return err(status::invalid_arg);
  }
  const std::uint64_t need = static_cast<std::uint64_t>(width) * height * 4ull;
  if (rgba.size() < need) return err(status::invalid_arg);

  jpeg_compress_struct cinfo{};
  jpeg_enc_trap trap{};
  trap.pub.error_exit = jpeg_enc_error_exit;
  cinfo.err = jpeg_std_error(&trap.pub);
  trap.pub.error_exit = jpeg_enc_error_exit;

  // C4611: longjmp skips C++ destructors. Everything live across this setjmp
  // is POD or a malloc the jump handler frees.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
  if (setjmp(trap.jump)) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    const bool oom = trap.oom;
    jpeg_destroy_compress(&cinfo);
    std::free(trap.out);
    std::free(trap.row);
    return err(oom ? status::out_of_memory : status::internal);
  }

  jpeg_create_compress(&cinfo);

  jpeg_destination_mgr dest{};
  dest.init_destination = [](j_compress_ptr c) {
    auto* t = reinterpret_cast<jpeg_enc_trap*>(c->err);
    c->dest->next_output_byte = t->chunk;
    c->dest->free_in_buffer = sizeof(t->chunk);
  };
  // Returning FALSE from empty_output_buffer means "I/O suspended" to libjpeg,
  // which the compressor caller is not implementing — the result is undefined,
  // not an error. Go out through error_exit so the setjmp handler frees and
  // reports. Same for term_destination, where the old silent return produced a
  // truncated JPEG that then got cached as if it were valid.
  dest.empty_output_buffer = [](j_compress_ptr c) -> boolean {
    auto* t = reinterpret_cast<jpeg_enc_trap*>(c->err);
    if (!jpeg_enc_grow(t, sizeof(t->chunk))) {
      t->oom = true;
      (*c->err->error_exit)(reinterpret_cast<j_common_ptr>(c));
    }
    std::memcpy(t->out + t->out_len, t->chunk, sizeof(t->chunk));
    t->out_len += sizeof(t->chunk);
    c->dest->next_output_byte = t->chunk;
    c->dest->free_in_buffer = sizeof(t->chunk);
    return TRUE;
  };
  dest.term_destination = [](j_compress_ptr c) {
    auto* t = reinterpret_cast<jpeg_enc_trap*>(c->err);
    const std::size_t used = sizeof(t->chunk) - c->dest->free_in_buffer;
    if (used == 0) return;
    if (!jpeg_enc_grow(t, used)) {
      t->oom = true;
      (*c->err->error_exit)(reinterpret_cast<j_common_ptr>(c));
    }
    std::memcpy(t->out + t->out_len, t->chunk, used);
    t->out_len += used;
  };
  cinfo.dest = &dest;

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  trap.row = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(width) * 3u));
  if (!trap.row) {
    jpeg_destroy_compress(&cinfo);
    std::free(trap.out);
    return err(status::out_of_memory);
  }

  while (cinfo.next_scanline < cinfo.image_height) {
    const std::uint8_t* src =
        rgba.data() + static_cast<std::size_t>(cinfo.next_scanline) * width * 4u;
    for (std::uint32_t x = 0; x < width; ++x) {
      trap.row[x * 3u + 0] = src[x * 4u + 0];
      trap.row[x * 3u + 1] = src[x * 4u + 1];
      trap.row[x * 3u + 2] = src[x * 4u + 2];
    }
    JSAMPROW rows[1] = {trap.row};
    jpeg_write_scanlines(&cinfo, rows, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  std::free(trap.row);
  trap.row = nullptr;

  std::vector<std::uint8_t> out;
  try {
    out.assign(trap.out, trap.out + trap.out_len);
  } catch (const std::bad_alloc&) {
    std::free(trap.out);
    return err(status::out_of_memory);
  }
  std::free(trap.out);
  if (out.empty()) return err(status::internal);
  return out;
}

}  // namespace mv::codec

