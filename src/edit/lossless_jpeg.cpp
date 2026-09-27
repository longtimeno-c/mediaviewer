// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/lossless_jpeg.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <jpeglib.h>

#include "codec/exif.h"

namespace mv::edit {
namespace {

constexpr char kExifNs[] = "Exif\0\0";  // 6 bytes
constexpr char kXmpNs[] = "http://ns.adobe.com/xap/1.0/";  // + NUL
constexpr char kIccNs[] = "ICC_PROFILE";  // + NUL
constexpr std::uint32_t kMaxDim = 65535;

struct error_trap {
  jpeg_error_mgr pub;
  jmp_buf jump;
};

void error_exit(j_common_ptr cinfo) {
  auto* trap = reinterpret_cast<error_trap*>(cinfo->err);
  longjmp(trap->jump, 1);
}

bool starts_with(const jpeg_saved_marker_ptr m, const char* ns, std::size_t n) noexcept {
  return m->data_length >= n && std::memcmp(m->data, ns, n) == 0;
}

// Which stored axes the transform reverses: those must be whole iMCUs.
void reversed_axes(codec::d4 g, bool& x, bool& y) noexcept {
  if (g.transposes()) {
    x = g.flip_y();
    y = g.flip_x();
  } else {
    x = g.flip_x();
    y = g.flip_y();
  }
}

std::uint32_t round_up(std::uint32_t v, std::uint32_t m) noexcept { return (v + m - 1) / m * m; }
std::uint32_t div_up(std::uint64_t v, std::uint64_t d) noexcept {
  return static_cast<std::uint32_t>((v + d - 1) / d);
}

// Markers into the output, under `policy`. EXIF/XMP are patched in place in
// the source's saved copy (it is ours; the decompressor is done with it).
void write_markers(j_decompress_ptr src, j_compress_ptr dst, metadata_policy policy,
                   bool rotated, std::uint32_t out_w, std::uint32_t out_h) {
  for (jpeg_saved_marker_ptr m = src->marker_list; m != nullptr; m = m->next) {
    const bool is_icc = m->marker == JPEG_APP0 + 2 && starts_with(m, kIccNs, sizeof(kIccNs));
    if (m->marker == JPEG_APP0 && dst->write_JFIF_header && starts_with(m, "JFIF", 5)) continue;
    if (m->marker == JPEG_APP0 + 14 && dst->write_Adobe_marker && starts_with(m, "Adobe", 5)) continue;
    if (policy == metadata_policy::none && !is_icc) continue;

    if (m->marker == JPEG_APP0 + 1 && starts_with(m, kExifNs, 6)) {
      std::span<std::uint8_t> tiff(m->data + 6, m->data_length - 6);
      (void)codec::exif_set_orientation(tiff, 1);
      (void)codec::exif_set_pixel_dimensions(tiff, out_w, out_h);
      if (rotated) (void)codec::exif_drop_thumbnail(tiff);
      if (policy == metadata_policy::minus_gps && !codec::exif_strip_gps(tiff)) continue;
    } else if (m->marker == JPEG_APP0 + 1 && starts_with(m, kXmpNs, sizeof(kXmpNs))) {
      std::span<std::uint8_t> xmp(m->data + sizeof(kXmpNs), m->data_length - sizeof(kXmpNs));
      if (policy == metadata_policy::minus_gps && codec::xmp_has_gps(xmp)) continue;
      (void)codec::xmp_set_orientation(xmp, 1);
    }
    jpeg_write_marker(dst, m->marker, m->data, m->data_length);
  }
}

void transform_block(const JCOEF* in, JCOEF* out, bool t, bool fx, bool fy) noexcept {
  for (int v = 0; v < DCTSIZE; ++v) {
    for (int u = 0; u < DCTSIZE; ++u) {
      JCOEF c = t ? in[u * DCTSIZE + v] : in[v * DCTSIZE + u];
      if ((fx && (u & 1)) != (fy && (v & 1))) c = static_cast<JCOEF>(-c);
      out[v * DCTSIZE + u] = c;
    }
  }
}

}  // namespace

result<jpeg_layout> read_layout(std::span<const std::uint8_t> jpeg) {
  if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return err(status::unsupported_format);
  jpeg_decompress_struct cinfo{};
  error_trap trap{};
  cinfo.err = jpeg_std_error(&trap.pub);
  trap.pub.error_exit = error_exit;
  trap.pub.output_message = [](j_common_ptr) {};
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
  if (setjmp(trap.jump)) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    jpeg_destroy_decompress(&cinfo);
    return err(status::corrupt);
  }
  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(jpeg.data()), static_cast<unsigned long>(jpeg.size()));
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return err(status::corrupt);
  }
  jpeg_layout out;
  out.width = cinfo.image_width;
  out.height = cinfo.image_height;
  out.components = cinfo.num_components;
  out.progressive = cinfo.progressive_mode != 0;
  int max_h = 1, max_v = 1;
  for (int i = 0; i < cinfo.num_components; ++i) {
    if (cinfo.comp_info[i].h_samp_factor > max_h) max_h = cinfo.comp_info[i].h_samp_factor;
    if (cinfo.comp_info[i].v_samp_factor > max_v) max_v = cinfo.comp_info[i].v_samp_factor;
  }
  out.mcu_w = static_cast<std::uint32_t>(max_h * DCTSIZE);
  out.mcu_h = static_cast<std::uint32_t>(max_v * DCTSIZE);
  jpeg_destroy_decompress(&cinfo);
  const int o = codec::jpeg_orientation(jpeg);
  out.orientation = o == 0 ? 1 : o;
  return out;
}

bool lossless_possible(const jpeg_layout& l, const lossless_request& req) noexcept {
  if (l.width == 0 || l.height == 0 || l.components < 1 || l.components > 4) return false;
  bool rx = false, ry = false;
  reversed_axes(req.transform, rx, ry);
  if (rx && l.width % l.mcu_w != 0) return false;
  if (ry && l.height % l.mcu_h != 0) return false;
  if (req.crop_w == 0) return true;
  const bool t = req.transform.transposes();
  const std::uint32_t ow = t ? l.height : l.width;
  const std::uint32_t oh = t ? l.width : l.height;
  const std::uint32_t omcu_w = t ? l.mcu_h : l.mcu_w;
  const std::uint32_t omcu_h = t ? l.mcu_w : l.mcu_h;
  if (req.crop_h == 0) return false;
  if (req.crop_x % omcu_w != 0 || req.crop_y % omcu_h != 0) return false;
  if (req.crop_x >= ow || req.crop_y >= oh) return false;
  return req.crop_w <= ow - req.crop_x && req.crop_h <= oh - req.crop_y;
}

result<std::vector<std::uint8_t>> transform(std::span<const std::uint8_t> jpeg,
                                            const lossless_request& req) {
  MV_TRY(const jpeg_layout layout, read_layout(jpeg));
  if (!lossless_possible(layout, req)) return err(status::unsupported_format);
  if (layout.width > kMaxDim || layout.height > kMaxDim) return err(status::unsupported_format);

  const bool t = req.transform.transposes();
  const bool fx = req.transform.flip_x();
  const bool fy = req.transform.flip_y();
  const std::uint32_t full_w = t ? layout.height : layout.width;
  const std::uint32_t full_h = t ? layout.width : layout.height;
  const std::uint32_t out_w = req.crop_w ? req.crop_w : full_w;
  const std::uint32_t out_h = req.crop_w ? req.crop_h : full_h;
  const std::uint32_t crop_x = req.crop_w ? req.crop_x : 0;
  const std::uint32_t crop_y = req.crop_w ? req.crop_y : 0;

  jpeg_decompress_struct src{};
  jpeg_compress_struct dst{};
  // One error manager for both objects (only message state lives in it), so
  // one setjmp catches either side.
  error_trap trap{};
  src.err = jpeg_std_error(&trap.pub);
  dst.err = &trap.pub;
  trap.pub.error_exit = error_exit;
  trap.pub.output_message = [](j_common_ptr) {};
  // Everything live across the setjmps is POD or libjpeg-owned; the output
  // buffer is malloc'd by jpeg_mem_dest and freed on both paths.
  unsigned char* out_buf = nullptr;
  unsigned long out_size = 0;
  volatile bool dst_created = false;
  jvirt_barray_ptr dst_arrays[MAX_COMPONENTS] = {};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
  if (setjmp(trap.jump)) {
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (dst_created) jpeg_destroy_compress(&dst);
    jpeg_destroy_decompress(&src);
    std::free(out_buf);
    return err(status::corrupt);
  }

  jpeg_create_decompress(&src);
  jpeg_mem_src(&src, const_cast<unsigned char*>(jpeg.data()), static_cast<unsigned long>(jpeg.size()));
  jpeg_save_markers(&src, JPEG_COM, 0xFFFF);
  for (int i = 0; i < 16; ++i) jpeg_save_markers(&src, JPEG_APP0 + i, 0xFFFF);
  if (jpeg_read_header(&src, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&src);
    return err(status::corrupt);
  }

  const int ncomp = src.num_components;
  int max_h = 1, max_v = 1;
  for (int ci = 0; ci < ncomp; ++ci) {
    if (src.comp_info[ci].h_samp_factor > max_h) max_h = src.comp_info[ci].h_samp_factor;
    if (src.comp_info[ci].v_samp_factor > max_v) max_v = src.comp_info[ci].v_samp_factor;
  }
  // Output sampling: a transpose swaps each component's factors.
  const int out_max_h = t ? max_v : max_h;
  const int out_max_v = t ? max_h : max_v;

  // Destination coefficient arrays, requested before jpeg_read_coefficients
  // realises the pool (jpegtran's jtransform_request_workspace order).
  std::uint32_t dst_wb[MAX_COMPONENTS] = {}, dst_hb[MAX_COMPONENTS] = {};
  for (int ci = 0; ci < ncomp; ++ci) {
    const jpeg_component_info& c = src.comp_info[ci];
    const int oh_s = t ? c.v_samp_factor : c.h_samp_factor;
    const int ov_s = t ? c.h_samp_factor : c.v_samp_factor;
    const std::uint32_t wb = div_up(static_cast<std::uint64_t>(out_w) * oh_s,
                                    static_cast<std::uint64_t>(out_max_h) * DCTSIZE);
    const std::uint32_t hb = div_up(static_cast<std::uint64_t>(out_h) * ov_s,
                                    static_cast<std::uint64_t>(out_max_v) * DCTSIZE);
    dst_wb[ci] = round_up(wb, static_cast<std::uint32_t>(oh_s));
    dst_hb[ci] = round_up(hb, static_cast<std::uint32_t>(ov_s));
    dst_arrays[ci] = (*src.mem->request_virt_barray)(
        reinterpret_cast<j_common_ptr>(&src), JPOOL_IMAGE, TRUE, dst_wb[ci], dst_hb[ci],
        static_cast<JDIMENSION>(ov_s));
  }

  jvirt_barray_ptr* src_arrays = jpeg_read_coefficients(&src);
  if (src_arrays == nullptr) {
    jpeg_destroy_decompress(&src);
    return err(status::corrupt);
  }

  jpeg_create_compress(&dst);
  dst_created = true;
  jpeg_mem_dest(&dst, &out_buf, &out_size);
  jpeg_copy_critical_parameters(&src, &dst);
  dst.image_width = out_w;
  dst.image_height = out_h;
#if JPEG_LIB_VERSION >= 70
  // The libjpeg 7/8 API (a system libjpeg-turbo built that way) writes the
  // frame header from jpeg_width/height, which the copy set to the source's.
  dst.jpeg_width = out_w;
  dst.jpeg_height = out_h;
#endif
  if (t) {
    for (int ci = 0; ci < ncomp; ++ci) {
      jpeg_component_info& c = dst.comp_info[ci];
      const int h = c.h_samp_factor;
      c.h_samp_factor = c.v_samp_factor;
      c.v_samp_factor = h;
    }
    // A transposed block needs the transposed quantisation table.
    for (int q = 0; q < NUM_QUANT_TBLS; ++q) {
      JQUANT_TBL* tbl = dst.quant_tbl_ptrs[q];
      if (tbl == nullptr) continue;
      for (int i = 0; i < DCTSIZE; ++i) {
        for (int j = i + 1; j < DCTSIZE; ++j) {
          const UINT16 tmp = tbl->quantval[i * DCTSIZE + j];
          tbl->quantval[i * DCTSIZE + j] = tbl->quantval[j * DCTSIZE + i];
          tbl->quantval[j * DCTSIZE + i] = tmp;
        }
      }
    }
  }
  dst.optimize_coding = TRUE;
  if (src.progressive_mode) jpeg_simple_progression(&dst);

  // Rearrange: output block (ox, oy) of the crop → block of the full
  // transformed frame → source block, then the in-block transform.
  for (int ci = 0; ci < ncomp; ++ci) {
    const jpeg_component_info& sc = src.comp_info[ci];
    const std::uint32_t src_wb = sc.width_in_blocks;
    const std::uint32_t src_hb = sc.height_in_blocks;
    const std::uint32_t src_wb_alloc = round_up(src_wb, static_cast<std::uint32_t>(sc.h_samp_factor));
    const std::uint32_t src_hb_alloc = round_up(src_hb, static_cast<std::uint32_t>(sc.v_samp_factor));
    const std::uint32_t full_wb = t ? src_hb : src_wb;
    const std::uint32_t full_hb = t ? src_wb : src_hb;
    const int oh_s = t ? sc.v_samp_factor : sc.h_samp_factor;
    const int ov_s = t ? sc.h_samp_factor : sc.v_samp_factor;
    const std::uint32_t off_x = crop_x / static_cast<std::uint32_t>(out_max_h * DCTSIZE) * static_cast<std::uint32_t>(oh_s);
    const std::uint32_t off_y = crop_y / static_cast<std::uint32_t>(out_max_v * DCTSIZE) * static_cast<std::uint32_t>(ov_s);

    for (std::uint32_t oy = 0; oy < dst_hb[ci]; ++oy) {
      JBLOCKARRAY drow = (*src.mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(&src),
                                                         dst_arrays[ci], oy, 1, TRUE);
      const std::uint32_t Y = oy + off_y;
      for (std::uint32_t ox = 0; ox < dst_wb[ci]; ++ox) {
        const std::uint32_t X = ox + off_x;
        JCOEF* out = drow[0][ox];
        // Mirrored axes are whole iMCUs (lossless_possible), so a reversed
        // index only leaves the frame in the rounding pad: those blocks are
        // left zero (pre_zero), exactly as an encoder pads.
        if ((fx && X >= full_wb) || (fy && Y >= full_hb)) continue;
        const std::uint32_t px = fx ? full_wb - 1 - X : X;
        const std::uint32_t py = fy ? full_hb - 1 - Y : Y;
        const std::uint32_t sx = t ? py : px;
        const std::uint32_t sy = t ? px : py;
        if (sx >= src_wb_alloc || sy >= src_hb_alloc) continue;
        JBLOCKARRAY srow = (*src.mem->access_virt_barray)(reinterpret_cast<j_common_ptr>(&src),
                                                           src_arrays[ci], sy, 1, FALSE);
        transform_block(srow[0][sx], out, t, fx, fy);
      }
    }
  }

  jpeg_write_coefficients(&dst, dst_arrays);
  const bool rotated = !req.transform.identity() || req.crop_w != 0;
  write_markers(&src, &dst, req.policy, rotated, out_w, out_h);
  jpeg_finish_compress(&dst);
  jpeg_destroy_compress(&dst);
  dst_created = false;
  (void)jpeg_finish_decompress(&src);
  jpeg_destroy_decompress(&src);

  std::vector<std::uint8_t> bytes;
  try {
    bytes.assign(out_buf, out_buf + out_size);
  } catch (const std::bad_alloc&) {
    std::free(out_buf);
    return err(status::out_of_memory);
  }
  std::free(out_buf);
  return bytes;
}

namespace {

// The Orientation tag alone, pixels untouched: patch it in place, or insert
// a minimal EXIF APP1 when the file has no EXIF at all.
result<std::vector<std::uint8_t>> rewrite_orientation(std::span<const std::uint8_t> jpeg, int orientation) {
  std::vector<std::uint8_t> out;
  try {
    out.assign(jpeg.begin(), jpeg.end());
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  if (const auto exif = codec::find_jpeg_exif(out)) {
    std::span<std::uint8_t> tiff(out.data() + exif->offset, exif->size);
    if (!codec::exif_set_orientation(tiff, orientation)) {
      // EXIF without an Orientation entry: adding one means growing IFD0 and
      // moving every offset after it. Not done blind (plan/06: that is the
      // PR 12 writer's job, with maker-note preservation tests).
      return err(status::unsupported_format);
    }
    if (const auto xmp = codec::find_jpeg_xmp(out)) {
      (void)codec::xmp_set_orientation(std::span<std::uint8_t>(out.data() + xmp->offset, xmp->size),
                                       orientation);
    }
    return out;
  }
  // No EXIF: a new APP1 after SOI and any JFIF APP0.
  std::size_t at = 2;
  if (out.size() > 6 && out[2] == 0xFF && out[3] == 0xE0) {
    at = 4 + static_cast<std::size_t>(out[4] << 8 | out[5]);
    if (at > out.size()) return err(status::corrupt);
  }
  const std::vector<std::uint8_t> tiff = codec::exif_minimal(orientation);
  const std::size_t len = 2 + 6 + tiff.size();
  std::vector<std::uint8_t> seg{0xFF, 0xE1, static_cast<std::uint8_t>(len >> 8),
                                static_cast<std::uint8_t>(len & 0xFF), 'E', 'x', 'i', 'f', 0, 0};
  seg.insert(seg.end(), tiff.begin(), tiff.end());
  out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), seg.begin(), seg.end());
  return out;
}

}  // namespace

result<std::vector<std::uint8_t>> rotate_in_viewer(std::span<const std::uint8_t> jpeg, codec::d4 op,
                                                   bool* used_tag) {
  if (used_tag) *used_tag = false;
  MV_TRY(const jpeg_layout layout, read_layout(jpeg));
  const codec::d4 total = codec::compose(codec::from_exif(layout.orientation), op);
  lossless_request req;
  req.transform = total;
  req.policy = metadata_policy::all;
  if (lossless_possible(layout, req)) return transform(jpeg, req);
  if (used_tag) *used_tag = true;
  return rewrite_orientation(jpeg, codec::to_exif(total));
}

}  // namespace mv::edit
