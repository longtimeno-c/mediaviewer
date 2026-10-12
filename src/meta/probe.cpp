// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/probe.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "codec/format.h"
#include "io/file_port.h"
#include "meta/parse.h"
#include "meta/parse_util.h"

namespace mv::meta {
namespace {

using bytes = std::span<const std::uint8_t>;
namespace util = parse::util;

// Bounds. Head-resident metadata is small; the caps only stop a hostile file
// from turning a probe into a full read.
constexpr std::size_t kHeadBytes = 64u * 1024;
constexpr std::size_t kMaxSegmentBytes = 0xFFFF;         // a JPEG segment's own limit
constexpr std::size_t kMaxJpegHeaderBytes = 16u << 20;   // all segments before the scan
constexpr std::size_t kMaxPngChunkBytes = 8u << 20;      // one non-image chunk
constexpr std::size_t kMaxPngKeptBytes = 16u << 20;      // all of them
constexpr std::size_t kMaxXmpBytes = 4u << 20;
constexpr int kMaxSegments = 1024;
constexpr int kMaxChunks = 4096;

class file_source final : public parse::byte_source {
 public:
  bool open(std::string_view utf8_path) { return f_.open(utf8_path, io::read_mode::sequential).has_value(); }
  [[nodiscard]] std::uint64_t size() const noexcept override { return f_.size(); }
  [[nodiscard]] std::size_t read_at(std::uint64_t offset, std::span<std::uint8_t> into) noexcept override {
    auto n = f_.read_at(offset, into);
    return n ? *n : 0;
  }

 private:
  io::file_reader f_;
};

// `n` bytes at `offset`, from the head already in memory when they lie inside it.
bool fetch(parse::byte_source& src, bytes head, std::uint64_t offset, std::size_t n, std::vector<std::uint8_t>& out) {
  out.resize(n);
  if (offset <= head.size() && n <= head.size() - offset) {
    if (n > 0) std::memcpy(out.data(), head.data() + offset, n);
    return true;
  }
  return src.read_at(offset, out) == n;
}

std::string trimmed(std::string_view s) {
  while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.remove_suffix(1);
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  return std::string(s.substr(0, 256));
}

// XMP is read for its evidence only (CreatorTool, DigitalSourceType): every
// property counts as listed, so no rows are built for a probe.
bool all_listed(void*, std::string_view, std::string_view) { return true; }

void read_xmp_evidence(std::string_view packet, parse::sink& out) {
  if (packet.empty() || packet.size() > kMaxXmpBytes) return;
  parse::read_xmp(packet, &all_listed, nullptr, out);
}

// ---- TIFF IFD0: Software (0x0131) and XMP (700) ---------------------------------
// The two fields of a TIFF structure that can carry provenance, from an EXIF
// block (JPEG APP1, PNG eXIf, WebP EXIF) or a TIFF file. Nothing else is walked.
void read_ifd0(parse::byte_source& src, bytes head, parse::sink& out) {
  std::vector<std::uint8_t> b;
  if (!fetch(src, head, 0, 8, b)) return;
  const bool le = b[0] == 'I' && b[1] == 'I';
  if (!le && !(b[0] == 'M' && b[1] == 'M')) return;
  const auto u16 = [le](bytes v, std::size_t at) { return le ? util::le16(v, at) : util::be16(v, at); };
  const auto u32 = [le](bytes v, std::size_t at) { return le ? util::le32(v, at) : util::be32(v, at); };
  if (u16(b, 2) != 42) return;
  const std::uint64_t ifd = u32(b, 4);
  std::vector<std::uint8_t> cnt;
  if (!fetch(src, head, ifd, 2, cnt)) return;
  const std::size_t n = std::min<std::size_t>(u16(cnt, 0), 512);
  std::vector<std::uint8_t> entries;
  if (!fetch(src, head, ifd + 2, n * 12, entries)) return;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t e = i * 12;
    const std::uint16_t tag = u16(entries, e);
    const std::uint16_t type = u16(entries, e + 2);
    const std::uint32_t count = u32(entries, e + 4);
    const bool software = tag == 0x0131 && type == 2;
    const bool xmp = tag == 700 && (type == 1 || type == 7);
    if (!software && !xmp) continue;
    const std::size_t cap = software ? 256 : kMaxXmpBytes;
    if (count == 0 || count > cap) continue;
    std::vector<std::uint8_t> v;
    if (count <= 4) {
      v.assign(entries.begin() + static_cast<std::ptrdiff_t>(e + 8),
               entries.begin() + static_cast<std::ptrdiff_t>(e + 8 + count));
    } else if (!fetch(src, head, u32(entries, e + 8), count, v)) {
      continue;
    }
    if (software && out.ev.exif_software.empty()) out.ev.exif_software = trimmed(util::chars(v));
    if (xmp) read_xmp_evidence(util::chars(v), out);
  }
}

void read_exif_block(bytes tiff, parse::sink& out) {
  parse::memory_source m(tiff);
  read_ifd0(m, tiff, out);
}

// ---- JPEG: the segments before the first scan ------------------------------------
void probe_jpeg(parse::byte_source& src, bytes head, parse::sink& out) {
  // The header rebuilt as SOI + segments + EOI, so read_jpeg sees what the full
  // read sees of it (APP11 reassembly included) without the entropy-coded data.
  std::vector<std::uint8_t> header{0xFF, 0xD8};
  std::uint64_t at = 2;
  const std::uint64_t size = src.size();
  std::vector<std::uint8_t> marker;
  std::vector<std::uint8_t> seg;
  bool exif_done = false;
  bool xmp_done = false;
  for (int i = 0; i < kMaxSegments && at + 4 <= size; ++i) {
    if (!fetch(src, head, at, 4, marker) || marker[0] != 0xFF) break;
    const std::uint8_t m = marker[1];
    if (m == 0xFF) {
      ++at;
      continue;
    }
    if (m == 0xDA || m == 0xD9) break;
    if ((m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      at += 2;
      continue;
    }
    const std::size_t len = util::be16(marker, 2);
    if (len < 2 || len > kMaxSegmentBytes || header.size() + len + 2 > kMaxJpegHeaderBytes) break;
    if (!fetch(src, head, at + 4, len - 2, seg)) break;
    at += 2 + len;
    // Only the segments that can hold provenance; a camera's maker-note-heavy
    // APP1 is read once, for IFD0.
    if (m == 0xEB) {
      header.insert(header.end(), marker.begin(), marker.end());
      header.insert(header.end(), seg.begin(), seg.end());
    } else if (m == 0xE1 && !exif_done && seg.size() > 6 && std::memcmp(seg.data(), "Exif\0\0", 6) == 0) {
      exif_done = true;
      read_exif_block(bytes(seg).subspan(6), out);
    } else if (m == 0xE1 && !xmp_done && seg.size() > 29 &&
               std::memcmp(seg.data(), "http://ns.adobe.com/xap/1.0/\0", 29) == 0) {
      xmp_done = true;
      read_xmp_evidence(util::chars(bytes(seg).subspan(29)), out);
    }
  }
  header.push_back(0xFF);
  header.push_back(0xD9);
  // Only APP11 went in: read_jpeg finds the C2PA store and adds its rows to the
  // probe's throwaway sink.
  if (header.size() > 4) parse::read_jpeg(header, out);
}

// ---- PNG: every chunk but the image data ------------------------------------------
void probe_png(parse::byte_source& src, bytes head, parse::sink& out) {
  static constexpr std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<std::uint8_t> kept(kSig, kSig + 8);
  std::uint64_t at = 8;
  const std::uint64_t size = src.size();
  std::vector<std::uint8_t> h;
  std::vector<std::uint8_t> data;
  for (int i = 0; i < kMaxChunks && at + 12 <= size; ++i) {
    if (!fetch(src, head, at, 8, h)) break;
    const std::uint32_t len = util::be32(h, 0);
    if (len > 0x7FFFFFFF) break;
    const bool iend = util::fourcc_is(h, 4, "IEND");
    const bool image = util::fourcc_is(h, 4, "IDAT") || util::fourcc_is(h, 4, "fdAT");
    const bool text = util::fourcc_is(h, 4, "tEXt") || util::fourcc_is(h, 4, "zTXt") ||
                      util::fourcc_is(h, 4, "iTXt");
    const bool cabx = util::fourcc_is(h, 4, "caBX");
    const bool exif = util::fourcc_is(h, 4, "eXIf");
    if (iend) break;
    if (!image && (text || cabx || exif) && len <= kMaxPngChunkBytes && kept.size() + len + 12 <= kMaxPngKeptBytes) {
      if (!fetch(src, head, at + 8, len, data)) break;
      if (exif) {
        read_exif_block(data, out);
      } else {
        // The XMP packet rides in an iTXt that read_png leaves to Exiv2.
        if (util::fourcc_is(h, 4, "iTXt") && len > 18 &&
            std::memcmp(data.data(), "XML:com.adobe.xmp\0", 18) == 0 && len >= 22) {
          std::size_t p = 18;
          const bool compressed = data[p] != 0;
          p += 2;
          std::size_t next = 0;
          (void)util::cstr(data, p, next);  // language
          (void)util::cstr(data, next, next);  // translated keyword
          const bytes body = bytes(data).subspan(std::min(next, data.size()));
          if (!compressed) {
            read_xmp_evidence(util::chars(body), out);
          } else {
            std::string packet;
            if (parse::inflate_bounded(body, kMaxXmpBytes, packet)) read_xmp_evidence(packet, out);
          }
        }
        kept.insert(kept.end(), h.begin(), h.end());
        kept.insert(kept.end(), data.begin(), data.end());
        kept.insert(kept.end(), {0, 0, 0, 0});  // CRC: read_png does not check it
      }
    }
    at += 12ull + len;
  }
  static constexpr std::uint8_t kEnd[12] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
  kept.insert(kept.end(), kEnd, kEnd + 12);
  parse::read_png(kept, out);
}

// ---- WebP: the EXIF and XMP chunks -------------------------------------------------
void probe_webp(parse::byte_source& src, bytes head, parse::sink& out) {
  std::uint64_t at = 12;
  const std::uint64_t size = src.size();
  std::vector<std::uint8_t> h;
  std::vector<std::uint8_t> data;
  for (int i = 0; i < kMaxChunks && at + 8 <= size; ++i) {
    if (!fetch(src, head, at, 8, h)) break;
    const std::uint32_t len = util::le32(h, 4);
    const bool exif = util::fourcc_is(h, 0, "EXIF");
    const bool xmp = util::fourcc_is(h, 0, "XMP ");
    if ((exif || xmp) && len <= kMaxXmpBytes && fetch(src, head, at + 8, len, data)) {
      if (exif) {
        // Some writers keep JPEG's "Exif\0\0" in front of the TIFF header.
        bytes tiff(data);
        if (tiff.size() > 6 && std::memcmp(tiff.data(), "Exif\0\0", 6) == 0) tiff = tiff.subspan(6);
        read_exif_block(tiff, out);
      } else {
        read_xmp_evidence(util::chars(data), out);
      }
    }
    at += 8ull + len + (len & 1u);
  }
}

bool is_bmff(bytes head) noexcept {
  return head.size() >= 12 && head[4] == 'f' && head[5] == 't' && head[6] == 'y' && head[7] == 'p';
}

}  // namespace

provenance probe_provenance(std::string_view utf8_path) noexcept {
  try {
    file_source src;
    if (!src.open(utf8_path)) return {};
    std::vector<std::uint8_t> head_buf(static_cast<std::size_t>(std::min<std::uint64_t>(src.size(), kHeadBytes)));
    if (head_buf.empty() || src.read_at(0, head_buf) != head_buf.size()) return {};
    const bytes head(head_buf);
    std::vector<property> rows;  // the readers' rows; a probe keeps only the evidence
    parse::evidence ev;
    parse::sink out{rows, ev, 4000};
    switch (codec::probe(head.first(std::min<std::size_t>(head.size(), 1024)))) {
      case codec::format_family::jpeg: probe_jpeg(src, head, out); break;
      case codec::format_family::png: probe_png(src, head, out); break;
      case codec::format_family::webp: probe_webp(src, head, out); break;
      case codec::format_family::tiff: read_ifd0(src, head, out); break;
      case codec::format_family::heic:
      case codec::format_family::avif: parse::read_bmff(src, false, out); break;
      case codec::format_family::unknown:
        if (is_bmff(head)) parse::read_bmff(src, false, out);  // a clip
        break;
      default: break;  // RAW, GIF, BMP, ICO, documents: nothing a generator writes
    }
    return derive_provenance(ev);
  } catch (...) {
    return {};
  }
}

}  // namespace mv::meta
