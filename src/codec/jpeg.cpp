// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/decode.h"

#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <new>

#include <jpeglib.h>

namespace mv::codec {
namespace {

constexpr std::uint32_t kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 256ull * 1000ull * 1000ull;
constexpr int kMaxIccChunks = 256;

struct jpeg_error_trap {
  jpeg_error_mgr pub;
  jmp_buf jump;
  unsigned char* rgba;
  unsigned char* row;
  unsigned char* icc;
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

}  // namespace

result<raster> decode_jpeg(std::span<const std::uint8_t> bytes, const job_context* ctx,
                           int scale_denom) {
  if (probe(bytes) != format_family::jpeg) return err(status::unsupported_format);
  if (bytes.size() < 4) return err(status::corrupt);

  jpeg_decompress_struct cinfo{};
  jpeg_error_trap jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  jerr.pub.output_message = [](j_common_ptr) {};

  // C4611: longjmp skips C++ destructors. Everything live across this setjmp
  // is POD or a malloc the jump handler frees.
#pragma warning(push)
#pragma warning(disable : 4611)
  if (setjmp(jerr.jump)) {
#pragma warning(pop)
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.rgba);
    std::free(jerr.row);
    std::free(jerr.icc);
    return err(status::corrupt);
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(bytes.data()),
               static_cast<unsigned long>(bytes.size()));
  jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return err(status::corrupt);
  }

  unsigned icc_len = 0;
  (void)extract_icc(&cinfo, &jerr.icc, &icc_len);

  cinfo.out_color_space = JCS_RGB;
  if (scale_denom != 2 && scale_denom != 4 && scale_denom != 8) scale_denom = 1;
  cinfo.scale_num = 1;
  cinfo.scale_denom = scale_denom;
  jpeg_start_decompress(&cinfo);

  const auto width = static_cast<std::uint32_t>(cinfo.output_width);
  const auto height = static_cast<std::uint32_t>(cinfo.output_height);
  if (width == 0 || height == 0 || width > kMaxDim || height > kMaxDim ||
      static_cast<std::uint64_t>(width) * height > kMaxPixels) {
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.icc);
    return err(status::unsupported_format);
  }
  if (cinfo.output_components != 3) {
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.icc);
    return err(status::unsupported_format);
  }

  const std::size_t rgba_bytes = static_cast<std::size_t>(width) * height * 4;
  jerr.rgba = static_cast<unsigned char*>(std::malloc(rgba_bytes));
  jerr.row = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(width) * 3));
  if (!jerr.rgba || !jerr.row) {
    jpeg_destroy_decompress(&cinfo);
    std::free(jerr.rgba);
    std::free(jerr.row);
    std::free(jerr.icc);
    return err(status::out_of_memory);
  }

  JSAMPROW rows[1] = {jerr.row};
  std::uint32_t y = 0;
  while (cinfo.output_scanline < cinfo.output_height) {
    if (ctx && ctx->cancelled()) {
      jpeg_destroy_decompress(&cinfo);
      std::free(jerr.rgba);
      std::free(jerr.row);
      std::free(jerr.icc);
      return err(status::cancelled);
    }
    jpeg_read_scanlines(&cinfo, rows, 1);
    unsigned char* dst = jerr.rgba + static_cast<std::size_t>(y) * width * 4;
    for (std::uint32_t x = 0; x < width; ++x) {
      dst[x * 4 + 0] = jerr.row[x * 3 + 0];
      dst[x * 4 + 1] = jerr.row[x * 3 + 1];
      dst[x * 4 + 2] = jerr.row[x * 3 + 2];
      dst[x * 4 + 3] = 255;
    }
    ++y;
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);

  raster out;
  out.format = format_family::jpeg;
  out.intent = transfer_intent::display_referred;
  out.width = width;
  out.height = height;
  try {
    out.rgba.assign(jerr.rgba, jerr.rgba + rgba_bytes);
    if (jerr.icc && icc_len > 0) out.icc.assign(jerr.icc, jerr.icc + icc_len);
  } catch (const std::bad_alloc&) {
    std::free(jerr.rgba);
    std::free(jerr.row);
    std::free(jerr.icc);
    return err(status::out_of_memory);
  }
  std::free(jerr.rgba);
  std::free(jerr.row);
  std::free(jerr.icc);
  return out;
}

result<jpeg_size> jpeg_dimensions(std::span<const std::uint8_t> bytes) {
  if (probe(bytes) != format_family::jpeg) return err(status::unsupported_format);
  if (bytes.size() < 4) return err(status::corrupt);

  jpeg_decompress_struct cinfo{};
  jpeg_error_trap jerr{};
  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpeg_error_exit;
  jerr.pub.output_message = [](j_common_ptr) {};

#pragma warning(push)
#pragma warning(disable : 4611)
  if (setjmp(jerr.jump)) {
#pragma warning(pop)
    jpeg_destroy_decompress(&cinfo);
    return err(status::corrupt);
  }

  jpeg_create_decompress(&cinfo);
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
#pragma warning(push)
#pragma warning(disable : 4611)
  if (setjmp(trap.jump)) {
#pragma warning(pop)
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

