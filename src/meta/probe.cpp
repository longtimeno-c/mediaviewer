// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/probe.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "codec/format.h"
#include "meta/parse.h"
#include "meta/parse_util.h"

namespace mv::meta {
namespace {

using bytes = std::span<const std::uint8_t>;
using buffer = std::vector<std::uint8_t>;
namespace util = parse::util;

// Bounds. Head-resident metadata is small; the caps only stop a hostile file
// from turning a probe into a full read. Every read below is one of these.
constexpr std::size_t kHeadBytes = 64u * 1024;
constexpr std::size_t kMaxSegmentBytes = 0xFFFF;         // a JPEG segment's own limit
constexpr std::size_t kMaxJpegHeaderBytes = 16u << 20;   // APP11 segments kept for read_jpeg
constexpr std::size_t kMaxPngChunkBytes = 8u << 20;      // one non-image chunk
constexpr std::size_t kMaxPngKeptBytes = 16u << 20;      // all of them
constexpr std::size_t kMaxXmpBytes = 1u << 20;           // one XMP packet
constexpr std::size_t kMaxExifBytes = 256u * 1024;       // one EXIF block (IFD0 lives at its start)
constexpr std::size_t kMaxMetaBoxBytes = 1u << 20;       // a HEIF `meta` box, read whole
constexpr std::size_t kMaxUdtaBytes = 1u << 20;          // `udta` / `meta` of a larger `moov`
constexpr std::size_t kMaxCanonBoxBytes = 1u << 20;      // CR3's Canon uuid box
constexpr std::uint64_t kMaxFileBytes = kProbeMaxFileBytes;
constexpr int kMaxSegments = 1024;
constexpr int kMaxChunks = 4096;
constexpr int kMaxBoxes = 4096;

// IPTC / Adobe's XMP uuid (ISO-BMFF), Canon's CR3 metadata uuid.
constexpr std::uint8_t kXmpUuid[16] = {0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                                       0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC};
constexpr std::uint8_t kCanonUuid[16] = {0x85, 0xC0, 0xB6, 0x87, 0x82, 0x0F, 0x11, 0xE0,
                                         0x81, 0x11, 0xF4, 0xCE, 0x46, 0x2B, 0x6A, 0x48};

// Every probe read goes through here: the first 64 KB are read once and served
// from memory, and the bytes read in total are capped (kMaxFileBytes, on top of
// file_source's own cap, so a memory source in the fuzzer is bounded too).
class reader {
 public:
  explicit reader(parse::byte_source& src) : src_(src), size_(src.size()) {
    head_.resize(static_cast<std::size_t>(std::min<std::uint64_t>(size_, kHeadBytes)));
    if (!head_.empty() && src_.read_at(0, head_) != head_.size()) head_.clear();
  }
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] bytes head() const noexcept { return head_; }

  // `n` bytes at `at`; false when they are not all there or the budget is spent.
  bool read(std::uint64_t at, std::size_t n, buffer& out) {
    if (at > size_ || n > size_ - at) return false;
    out.resize(n);
    if (n == 0) return true;
    if (at + n <= head_.size()) {
      std::memcpy(out.data(), head_.data() + at, n);
      return true;
    }
    if (n > kMaxFileBytes - spent_) return false;
    spent_ += n;
    return src_.read_at(at, out) == n;
  }

 private:
  parse::byte_source& src_;
  std::uint64_t size_;
  buffer head_;
  std::uint64_t spent_ = 0;
};

// A window [base, base + len) of another source (RAF's embedded JPEG).
class window_source final : public parse::byte_source {
 public:
  window_source(reader& r, std::uint64_t base, std::uint64_t len) : r_(r), base_(base), len_(len) {}
  [[nodiscard]] std::uint64_t size() const noexcept override { return len_; }
  [[nodiscard]] std::size_t read_at(std::uint64_t offset, std::span<std::uint8_t> into) noexcept override {
    if (offset > len_) return 0;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(into.size(), len_ - offset));
    buffer b;
    if (!r_.read(base_ + offset, n, b)) return 0;
    std::memcpy(into.data(), b.data(), n);
    return n;
  }

 private:
  reader& r_;
  std::uint64_t base_;
  std::uint64_t len_;
};

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

void set_software(parse::sink& out, std::string_view s) {
  if (out.ev.exif_software.empty()) out.ev.exif_software = trimmed(s);
}

// ---- TIFF IFD0: Software (0x0131) and XMP (700) ---------------------------------
// The two fields of a TIFF structure that can carry provenance, from an EXIF
// block or a TIFF-based file (TIFF, DNG, CR2, NEF, ARW, PEF, and ORF / RW2 with
// their own magic). Nothing else is walked.
void read_ifd0(reader& r, std::uint64_t base, parse::sink& out) {
  buffer b;
  if (!r.read(base, 8, b)) return;
  const bool le = b[0] == 'I' && b[1] == 'I';
  if (!le && !(b[0] == 'M' && b[1] == 'M')) return;
  const auto u16 = [le](bytes v, std::size_t at) { return le ? util::le16(v, at) : util::be16(v, at); };
  const auto u32 = [le](bytes v, std::size_t at) { return le ? util::le32(v, at) : util::be32(v, at); };
  const std::uint16_t magic = u16(b, 2);
  // 42 TIFF, 0x4F52 / 0x5352 Olympus ORF ("IIRO" / "IIRS"), 0x55 Panasonic RW2.
  if (magic != 42 && magic != 0x4F52 && magic != 0x5352 && magic != 0x55) return;
  const std::uint64_t ifd = base + u32(b, 4);
  buffer cnt;
  if (!r.read(ifd, 2, cnt)) return;
  const std::size_t n = std::min<std::size_t>(u16(cnt, 0), 512);
  buffer entries;
  if (!r.read(ifd + 2, n * 12, entries)) return;
  bool software_done = false;
  bool xmp_done = false;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t e = i * 12;
    const std::uint16_t tag = u16(entries, e);
    const std::uint16_t type = u16(entries, e + 2);
    const std::uint32_t count = u32(entries, e + 4);
    // The first of each only, as Exiv2 keeps: a hostile IFD can repeat tag 700
    // in all 512 entries, each pointing at the same packet.
    const bool software = tag == 0x0131 && type == 2 && !software_done;
    const bool xmp = tag == 700 && (type == 1 || type == 7) && !xmp_done;
    if (!software && !xmp) continue;
    (software ? software_done : xmp_done) = true;
    const std::size_t cap = software ? 256 : kMaxXmpBytes;
    if (count == 0 || count > cap) continue;
    buffer v;
    if (count <= 4) {
      v.assign(entries.begin() + static_cast<std::ptrdiff_t>(e + 8),
               entries.begin() + static_cast<std::ptrdiff_t>(e + 8 + count));
    } else if (!r.read(base + u32(entries, e + 8), count, v)) {
      continue;
    }
    if (software) set_software(out, util::chars(v));
    if (xmp) read_xmp_evidence(util::chars(v), out);
  }
}

// An EXIF block in memory: TIFF header first, or JPEG's "Exif\0\0" before it.
void read_exif_block(bytes block, parse::sink& out) {
  if (block.size() > 6 && std::memcmp(block.data(), "Exif\0\0", 6) == 0) block = block.subspan(6);
  parse::memory_source m(block);
  reader r(m);
  read_ifd0(r, 0, out);
}

// ---- JPEG: the segments before the first scan ------------------------------------
void probe_jpeg(reader& r, parse::sink& out) {
  // APP11 segments are kept as SOI + segments + EOI, so read_jpeg reassembles
  // the C2PA store exactly as the full read does, without the image data.
  buffer header{0xFF, 0xD8};
  std::uint64_t at = 2;
  buffer marker;
  buffer seg;
  bool exif_done = false;
  bool xmp_done = false;
  for (int i = 0; i < kMaxSegments && at + 4 <= r.size(); ++i) {
    if (!r.read(at, 4, marker) || marker[0] != 0xFF) break;
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
    if (len < 2 || len > kMaxSegmentBytes) break;
    const bool app11 = m == 0xEB;
    const bool app1 = m == 0xE1 && (!exif_done || !xmp_done);
    if (app11 || app1) {
      if (!r.read(at + 4, len - 2, seg)) break;
      if (app11 && header.size() + len + 2 <= kMaxJpegHeaderBytes) {
        header.insert(header.end(), marker.begin(), marker.end());
        header.insert(header.end(), seg.begin(), seg.end());
      } else if (app1 && !exif_done && seg.size() > 6 && std::memcmp(seg.data(), "Exif\0\0", 6) == 0) {
        exif_done = true;
        read_exif_block(seg, out);
      } else if (app1 && !xmp_done && seg.size() > 29 &&
                 std::memcmp(seg.data(), "http://ns.adobe.com/xap/1.0/\0", 29) == 0) {
        xmp_done = true;
        read_xmp_evidence(util::chars(bytes(seg).subspan(29)), out);
      }
    }
    at += 2 + len;  // every other segment is skipped by seeking, never read
  }
  header.push_back(0xFF);
  header.push_back(0xD9);
  if (header.size() > 4) parse::read_jpeg(header, out);
}

// ---- PNG: every chunk but the image data ------------------------------------------
void probe_png(reader& r, parse::sink& out) {
  static constexpr std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  buffer kept(kSig, kSig + 8);
  std::uint64_t at = 8;
  buffer h;
  buffer data;
  bool xmp_done = false;
  for (int i = 0; i < kMaxChunks && at + 12 <= r.size(); ++i) {
    if (!r.read(at, 8, h)) break;
    const std::uint32_t len = util::be32(h, 0);
    if (len > 0x7FFFFFFF) break;
    if (util::fourcc_is(h, 4, "IEND")) break;
    const bool text = util::fourcc_is(h, 4, "tEXt") || util::fourcc_is(h, 4, "zTXt") ||
                      util::fourcc_is(h, 4, "iTXt");
    const bool cabx = util::fourcc_is(h, 4, "caBX");
    const bool exif = util::fourcc_is(h, 4, "eXIf");
    if ((text || cabx || exif) && len <= kMaxPngChunkBytes && kept.size() + len + 12 <= kMaxPngKeptBytes) {
      if (!r.read(at + 8, len, data)) break;
      if (exif) {
        read_exif_block(data, out);
      } else {
        // The XMP packet rides in an iTXt that read_png leaves to Exiv2.
        // The first packet only: each can inflate to 1 MB from a few kilobytes.
        if (!xmp_done && util::fourcc_is(h, 4, "iTXt") && len >= 22 &&
            std::memcmp(data.data(), "XML:com.adobe.xmp\0", 18) == 0) {
          xmp_done = true;
          const bool compressed = data[18] != 0;
          std::size_t next = 0;
          (void)util::cstr(data, 20, next);    // language
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
    at += 12ull + len;  // IDAT and the rest: skipped by seeking
  }
  static constexpr std::uint8_t kEnd[12] = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
  kept.insert(kept.end(), kEnd, kEnd + 12);
  parse::read_png(kept, out);
}

// ---- WebP: the EXIF and XMP chunks -------------------------------------------------
void probe_webp(reader& r, parse::sink& out) {
  std::uint64_t at = 12;
  buffer h;
  buffer data;
  bool exif_done = false;
  bool xmp_done = false;
  for (int i = 0; i < kMaxChunks && at + 8 <= r.size() && !(exif_done && xmp_done); ++i) {
    if (!r.read(at, 8, h)) break;
    const std::uint32_t len = util::le32(h, 4);
    // The first of each, as the full read keeps.
    const bool exif = util::fourcc_is(h, 0, "EXIF") && !exif_done;
    const bool xmp = util::fourcc_is(h, 0, "XMP ") && !xmp_done;
    if (exif) exif_done = true;
    if (xmp) xmp_done = true;
    if ((exif && len <= kMaxExifBytes) || (xmp && len <= kMaxXmpBytes)) {
      if (r.read(at + 8, len, data)) {
        if (exif) read_exif_block(data, out);
        else read_xmp_evidence(util::chars(data), out);
      }
    }
    at += 8ull + len + (len & 1u);
  }
}

// ---- ISO-BMFF ------------------------------------------------------------------------

struct box {
  std::uint64_t start = 0;  // header
  std::uint64_t body = 0;   // payload (after a uuid box's 16-byte usertype)
  std::uint64_t end = 0;
  char type[5]{};
  std::uint8_t uuid[16]{};

  [[nodiscard]] bool is(const char* t) const noexcept { return std::memcmp(type, t, 4) == 0; }
  [[nodiscard]] std::uint64_t size() const noexcept { return end - body; }
};

// The box whose header is at `at`, inside [at, limit). False on damage.
bool box_at(reader& r, std::uint64_t at, std::uint64_t limit, box& b) {
  if (at + 8 > limit) return false;
  buffer h;
  if (!r.read(at, std::min<std::uint64_t>(limit - at, 32) >= 24 ? 24 : 8, h)) return false;
  std::uint64_t len = util::be32(h, 0);
  std::uint64_t header = 8;
  if (len == 1) {
    if (h.size() < 16) return false;
    len = util::be64(h, 8);
    header = 16;
  } else if (len == 0) {
    len = limit - at;
  }
  if (len < header || len > limit - at) return false;
  b.start = at;
  b.end = at + len;
  std::memcpy(b.type, h.data() + 4, 4);
  b.type[4] = '\0';
  b.body = at + header;
  if (b.is("uuid")) {
    buffer u;
    if (len < header + 16 || !r.read(b.body, 16, u)) return false;
    std::memcpy(b.uuid, u.data(), 16);
    b.body += 16;
  }
  return true;
}

// Calls `fn(box)` for each child box of [begin, end); stops when it returns false.
template <class F>
void for_each_box(reader& r, std::uint64_t begin, std::uint64_t end, F fn) {
  std::uint64_t at = begin;
  box b;
  for (int i = 0; i < kMaxBoxes && box_at(r, at, end, b); ++i) {
    if (!fn(b)) return;
    at = b.end;
  }
}

// HEIF / AVIF: the Exif item and the `application/rdf+xml` mime item of the
// top-level `meta` box, read through `iloc` (file offsets or `idat`).
void probe_heif_meta(reader& r, const box& meta, parse::sink& out) {
  if (meta.size() < 4 || meta.size() > kMaxMetaBoxBytes) return;
  buffer m;
  if (!r.read(meta.body, static_cast<std::size_t>(meta.size()), m)) return;
  parse::memory_source ms(m);
  reader mr(ms);
  std::uint32_t exif_id = 0;
  std::uint32_t xmp_id = 0;
  box iloc;
  box idat;
  bool have_iloc = false;
  bool have_idat = false;
  for_each_box(mr, 4, m.size(), [&](const box& c) {  // meta is a FullBox
    if (c.is("iloc")) {
      iloc = c;
      have_iloc = true;
    } else if (c.is("idat")) {
      idat = c;
      have_idat = true;
    } else if (c.is("iinf") && c.size() >= 6) {
      const bytes p = bytes(m).subspan(static_cast<std::size_t>(c.body), static_cast<std::size_t>(c.size()));
      const std::size_t first = p[0] == 0 ? 6 : 8;  // version 0: u16 count, else u32
      for_each_box(mr, c.body + first, c.end, [&](const box& e) {
        if (!e.is("infe") || e.size() < 12) return true;
        const bytes q = bytes(m).subspan(static_cast<std::size_t>(e.body), static_cast<std::size_t>(e.size()));
        const std::uint8_t v = q[0];
        if (v < 2) return true;
        std::size_t at = 4;
        const std::uint32_t id = v == 2 ? util::be16(q, at) : util::be32(q, at);
        at += v == 2 ? 2 : 4;
        at += 2;  // protection index
        if (!util::has(q, at, 4)) return true;
        if (util::fourcc_is(q, at, "Exif") && exif_id == 0) {  // the first of each kind
          exif_id = id;
        } else if (util::fourcc_is(q, at, "mime")) {
          std::size_t next = 0;
          (void)util::cstr(q, at + 4, next);  // item name
          if (xmp_id == 0 && util::cstr(q, next, next) == "application/rdf+xml") xmp_id = id;
        }
        return true;
      });
    }
    return true;
  });
  if ((!exif_id && !xmp_id) || !have_iloc || iloc.size() < 8) return;

  // iloc: find each wanted item's extents and read them, capped.
  const bytes p = bytes(m).subspan(static_cast<std::size_t>(iloc.body), static_cast<std::size_t>(iloc.size()));
  const std::uint8_t v = p[0];
  const unsigned off_size = p[4] >> 4, len_size = p[4] & 15, base_size = p[5] >> 4;
  const unsigned idx_size = v >= 1 ? (p[5] & 15) : 0;
  // A zero length_size gives every extent "the rest of the file", which the
  // probe never reads; it would also make each extent consume no bytes, so a
  // hostile box could spin 4096 items x 65535 extents without progress.
  if (len_size == 0) return;
  std::size_t at = 6;
  const auto take = [&](unsigned n, std::uint64_t& value) {
    if (n != 0 && n != 4 && n != 8) return false;
    if (!util::has(p, at, n)) return false;
    value = n == 8 ? util::be64(p, at) : n == 4 ? util::be32(p, at) : 0;
    at += n;
    return true;
  };
  std::uint64_t count = 0;
  if (v < 2) {
    if (!util::has(p, at, 2)) return;
    count = util::be16(p, at);
    at += 2;
  } else if (!take(4, count)) {
    return;
  }
  for (std::uint64_t i = 0; i < std::min<std::uint64_t>(count, 4096); ++i) {
    std::uint64_t id = 0;
    if (v < 2) {
      if (!util::has(p, at, 2)) return;
      id = util::be16(p, at);
      at += 2;
    } else if (!take(4, id)) {
      return;
    }
    unsigned method = 0;
    if (v >= 1) {
      if (!util::has(p, at, 2)) return;
      method = util::be16(p, at) & 15;
      at += 2;
    }
    at += 2;  // data reference index
    std::uint64_t base = 0;
    if (!take(base_size, base) || !util::has(p, at, 2)) return;
    const unsigned extents = util::be16(p, at);
    at += 2;
    const bool wanted = id != 0 && (id == exif_id || id == xmp_id);
    const std::size_t cap = id == exif_id ? kMaxExifBytes : kMaxXmpBytes;
    buffer data;
    bool ok = wanted && method <= 1 && (method == 0 || have_idat);
    for (unsigned k = 0; k < extents; ++k) {
      std::uint64_t index = 0, off = 0, len = 0;
      if (!take(idx_size, index) || !take(off_size, off) || !take(len_size, len)) return;
      if (!ok) continue;
      if (len == 0 || len > cap - std::min<std::size_t>(data.size(), cap)) {
        ok = false;
        continue;
      }
      buffer piece;
      if (method == 0) {
        ok = r.read(base + off, static_cast<std::size_t>(len), piece);
      } else {
        ok = idat.body + base + off <= idat.end && len <= idat.end - (idat.body + base + off) &&
             mr.read(idat.body + base + off, static_cast<std::size_t>(len), piece);
      }
      data.insert(data.end(), piece.begin(), piece.end());
    }
    if (!ok || data.empty()) continue;
    if (id == exif_id && data.size() > 4) {
      // HEIF Exif item: a 4-byte offset to the TIFF header, then the block.
      const std::uint32_t skip = util::be32(data, 0);
      if (skip <= data.size() - 4) read_exif_block(bytes(data).subspan(4 + skip), out);
    } else if (id == xmp_id) {
      read_xmp_evidence(util::chars(data), out);
    }
  }
}

// QuickTime / MP4 `udta` and `meta`: ©swr / ©too (QuickTime strings or iTunes
// `data` atoms), the `com.apple.quicktime.software` key, an `XMP_` packet.
// `ms` holds the parent box's bytes; `r` reads from them.
std::string data_atom_text(reader& r, const box& item) {
  std::string text;
  for_each_box(r, item.body, item.end, [&](const box& d) {
    if (!d.is("data") || d.size() < 8 || d.size() > 4096) return true;
    buffer v;
    if (r.read(d.body + 8, static_cast<std::size_t>(d.size() - 8), v)) text = util::chars(v);
    return false;
  });
  return text;
}

void probe_qt_meta(reader& r, const box& meta, parse::sink& out) {
  // MP4's udta/meta is a FullBox; QuickTime's moov/meta is a plain box.
  std::uint64_t first = meta.body;
  buffer peek;
  if (r.read(meta.body, std::min<std::uint64_t>(meta.size(), 12), peek) && peek.size() >= 12 &&
      util::be32(peek, 0) == 0 && util::fourcc_is(peek, 8, "hdlr")) {
    first += 4;
  }
  std::vector<std::string> keys;
  for_each_box(r, first, meta.end, [&](const box& c) {
    if (c.is("keys") && c.size() >= 8) {
      buffer k;
      if (!r.read(c.body, static_cast<std::size_t>(std::min<std::uint64_t>(c.size(), 64 * 1024)), k)) return true;
      const std::uint32_t n = util::be32(k, 4);
      std::size_t at = 8;
      for (std::uint32_t i = 0; i < n && i < 1024 && util::has(k, at, 8); ++i) {
        const std::uint32_t len = util::be32(k, at);
        if (len < 8 || !util::has(k, at, len)) break;
        keys.emplace_back(util::chars(bytes(k).subspan(at + 8, len - 8)));
        at += len;
      }
    } else if (c.is("ilst")) {
      for_each_box(r, c.body, c.end, [&](const box& item) {
        std::string name(item.type, 4);
        const std::uint32_t index = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(item.type[0])) << 24) |
                                    (static_cast<std::uint32_t>(static_cast<std::uint8_t>(item.type[1])) << 16) |
                                    (static_cast<std::uint32_t>(static_cast<std::uint8_t>(item.type[2])) << 8) |
                                    static_cast<std::uint32_t>(static_cast<std::uint8_t>(item.type[3]));
        if (index >= 1 && index <= keys.size()) name = keys[index - 1];
        if (name == "com.apple.quicktime.software") {
          // The key wins over ©swr / ©too, as in the full read.
          out.ev.exif_software = trimmed(data_atom_text(r, item));
        } else if (name == "\xA9too" || name == "\xA9swr") {
          set_software(out, data_atom_text(r, item));
        }
        return true;
      });
    }
    return true;
  });
}

void probe_udta(reader& r, const box& udta, parse::sink& out) {
  bool xmp_done = false;
  for_each_box(r, udta.body, udta.end, [&](const box& c) {
    if ((c.is("\xA9swr") || c.is("\xA9too")) && c.size() >= 4 && c.size() <= 4096) {
      buffer v;
      if (!r.read(c.body, static_cast<std::size_t>(c.size()), v)) return true;
      const std::size_t len = util::be16(v, 0);
      if (util::has(v, 4, len) && len > 0) set_software(out, util::chars(bytes(v).subspan(4, len)));
      else set_software(out, data_atom_text(r, c));  // iTunes style
    } else if (c.is("meta")) {
      probe_qt_meta(r, c, out);
    } else if (c.is("XMP_") && !xmp_done && c.size() <= kMaxXmpBytes) {
      xmp_done = true;
      buffer v;
      if (r.read(c.body, static_cast<std::size_t>(c.size()), v)) read_xmp_evidence(util::chars(v), out);
    }
    return true;
  });
}

// CR3: Canon's uuid box in moov, whose CMT1 is a TIFF IFD0 (Software).
void probe_canon_uuid(reader& r, const box& canon, parse::sink& out) {
  if (canon.size() > kMaxCanonBoxBytes) return;
  for_each_box(r, canon.body, canon.end, [&](const box& c) {
    if (c.is("CMT1")) {
      read_ifd0(r, c.body, out);
      return false;
    }
    return true;
  });
}

void probe_moov(reader& r, const box& moov, parse::sink& out) {
  // Only moov's child headers are read (a few bytes each, seeking past trak and
  // its sample tables), then udta / meta / Canon's box alone, each capped.
  if (moov.size() > (std::uint64_t{1} << 40)) return;
  for_each_box(r, moov.body, moov.end, [&](const box& c) {
    const bool canon = c.is("uuid") && std::memcmp(c.uuid, kCanonUuid, 16) == 0;
    if (!c.is("udta") && !c.is("meta") && !canon) return true;
    if (c.size() > (canon ? kMaxCanonBoxBytes : kMaxUdtaBytes)) return true;
    buffer part;
    if (!r.read(c.start, static_cast<std::size_t>(c.end - c.start), part)) return true;
    parse::memory_source ms(part);
    reader pr(ms);
    box local;
    if (!box_at(pr, 0, part.size(), local)) return true;
    if (c.is("udta")) probe_udta(pr, local, out);
    else if (c.is("meta")) probe_qt_meta(pr, local, out);
    else probe_canon_uuid(pr, local, out);
    return true;
  });
}

// Top level of any ISO-BMFF file: C2PA (read_bmff), an XMP uuid, the HEIF
// `meta` box (stills), `moov` (clips, CR3) wherever it sits.
void probe_bmff(reader& r, parse::byte_source& src, bool stills, parse::sink& out) {
  parse::read_bmff(src, false, out);  // the top-level C2PA box, as the full read finds it
  bool xmp_done = false;
  bool moov_done = false;
  bool meta_done = false;
  for_each_box(r, 0, r.size(), [&](const box& b) {
    // The first of each box kind only.
    if (b.is("meta") && stills && !meta_done) {
      meta_done = true;
      probe_heif_meta(r, b, out);
    } else if (b.is("moov") && !moov_done) {
      moov_done = true;
      probe_moov(r, b, out);
    } else if (b.is("uuid") && !xmp_done && std::memcmp(b.uuid, kXmpUuid, 16) == 0 && b.size() <= kMaxXmpBytes) {
      xmp_done = true;
      buffer v;
      if (r.read(b.body, static_cast<std::size_t>(b.size()), v)) read_xmp_evidence(util::chars(v), out);
    }
    return true;
  });
}

// Fujifilm RAF: the embedded JPEG (offset and length at 84 / 88, big-endian)
// carries the EXIF; only its header segments are read.
void probe_raf(reader& r, parse::sink& out) {
  buffer h;
  if (!r.read(84, 8, h)) return;
  const std::uint64_t off = util::be32(h, 0);
  const std::uint64_t len = util::be32(h, 4);
  if (off < 92 || len < 4 || off > r.size() || len > r.size() - off) return;
  window_source w(r, off, len);
  reader jr(w);
  if (jr.head().size() >= 2 && jr.head()[0] == 0xFF && jr.head()[1] == 0xD8) probe_jpeg(jr, out);
}

bool is_bmff(bytes head) noexcept {
  return head.size() >= 12 && head[4] == 'f' && head[5] == 't' && head[6] == 'y' && head[7] == 'p';
}

bool is_tiff_like(bytes head) noexcept {
  if (head.size() < 8) return false;
  const bool le = head[0] == 'I' && head[1] == 'I';
  const bool be = head[0] == 'M' && head[1] == 'M';
  if (!le && !be) return false;
  const std::uint16_t magic = le ? util::le16(head, 2) : util::be16(head, 2);
  return magic == 42 || magic == 0x4F52 || magic == 0x5352 || magic == 0x55;
}

}  // namespace

void probe_evidence(parse::byte_source& src, parse::evidence& ev) noexcept {
  try {
    reader r(src);
    const bytes head = r.head();
    if (head.empty()) return;
    std::vector<property> rows;  // the readers' rows; a probe keeps only the evidence
    parse::sink out{rows, ev, 4000};
    const auto family = codec::probe(head.first(std::min<std::size_t>(head.size(), 1024)));
    switch (family) {
      case codec::format_family::jpeg: probe_jpeg(r, out); break;
      case codec::format_family::png: probe_png(r, out); break;
      case codec::format_family::webp: probe_webp(r, out); break;
      case codec::format_family::tiff: read_ifd0(r, 0, out); break;
      case codec::format_family::heic:
      case codec::format_family::avif: probe_bmff(r, src, true, out); break;
      case codec::format_family::raw:
        if (is_bmff(head)) probe_bmff(r, src, false, out);  // CR3
        else if (is_tiff_like(head)) read_ifd0(r, 0, out);  // DNG, CR2, NEF, ARW, ORF, RW2, PEF...
        else if (head.size() >= 16 && std::memcmp(head.data(), "FUJIFILMCCD-RAW ", 16) == 0) probe_raf(r, out);
        break;
      case codec::format_family::unknown:
        if (is_bmff(head)) probe_bmff(r, src, false, out);  // a clip
        break;
      default: break;  // GIF, BMP, ICO, documents: nothing a generator writes
    }
  } catch (...) {
  }
}

provenance probe_provenance(parse::byte_source& src) noexcept {
  try {
    parse::evidence ev;
    probe_evidence(src, ev);
    return derive_provenance(ev);
  } catch (...) {
    return {};
  }
}

}  // namespace mv::meta
