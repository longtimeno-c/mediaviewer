// SPDX-License-Identifier: GPL-2.0-or-later
#include "codec/exif.h"

#include <cstring>

namespace mv::codec {
namespace {

constexpr std::uint16_t kTagOrientation = 0x0112;
constexpr std::uint16_t kTagExifIfd = 0x8769;
constexpr std::uint16_t kTagGpsIfd = 0x8825;
constexpr std::uint16_t kTagPixelX = 0xA002;
constexpr std::uint16_t kTagPixelY = 0xA003;

constexpr std::uint16_t kTypeShort = 3;
constexpr std::uint16_t kTypeLong = 4;

constexpr char kXmpNamespace[] = "http://ns.adobe.com/xap/1.0/";  // + NUL

// Bounds-checked TIFF reads and writes. `le` is the byte order from the header.
struct tiff_view {
  std::uint8_t* data;
  std::size_t size;
  bool le;

  [[nodiscard]] bool has(std::uint64_t off, std::uint64_t n) const noexcept {
    return off <= size && n <= size - off;
  }
  [[nodiscard]] std::uint16_t u16(std::size_t off) const noexcept {
    return le ? static_cast<std::uint16_t>(data[off] | data[off + 1] << 8)
              : static_cast<std::uint16_t>(data[off] << 8 | data[off + 1]);
  }
  [[nodiscard]] std::uint32_t u32(std::size_t off) const noexcept {
    const std::uint32_t b0 = data[off], b1 = data[off + 1], b2 = data[off + 2], b3 = data[off + 3];
    return le ? (b0 | b1 << 8 | b2 << 16 | b3 << 24) : (b0 << 24 | b1 << 16 | b2 << 8 | b3);
  }
  void put16(std::size_t off, std::uint16_t v) const noexcept {
    if (le) {
      data[off] = static_cast<std::uint8_t>(v);
      data[off + 1] = static_cast<std::uint8_t>(v >> 8);
    } else {
      data[off] = static_cast<std::uint8_t>(v >> 8);
      data[off + 1] = static_cast<std::uint8_t>(v);
    }
  }
  void put32(std::size_t off, std::uint32_t v) const noexcept {
    for (int i = 0; i < 4; ++i) {
      const int shift = le ? 8 * i : 8 * (3 - i);
      data[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> shift);
    }
  }
};

std::optional<tiff_view> open_tiff(std::span<const std::uint8_t> tiff) noexcept {
  if (tiff.size() < 8) return std::nullopt;
  bool le = false;
  if (tiff[0] == 'I' && tiff[1] == 'I') {
    le = true;
  } else if (!(tiff[0] == 'M' && tiff[1] == 'M')) {
    return std::nullopt;
  }
  // The views are read-only unless the caller holds a mutable span; the
  // const_cast is confined to this file and every write goes through a
  // function that took std::span<std::uint8_t>.
  tiff_view v{const_cast<std::uint8_t*>(tiff.data()), tiff.size(), le};
  if (v.u16(2) != 42) return std::nullopt;
  return v;
}

// Offset of the 12-byte entry for `tag` in the IFD at `ifd`, if present.
std::optional<std::size_t> find_entry(const tiff_view& v, std::uint32_t ifd,
                                      std::uint16_t tag) noexcept {
  if (!v.has(ifd, 2)) return std::nullopt;
  const std::uint32_t count = v.u16(ifd);
  if (!v.has(ifd + 2ull, 12ull * count)) return std::nullopt;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t e = ifd + 2 + 12 * static_cast<std::size_t>(i);
    if (v.u16(e) == tag) return e;
  }
  return std::nullopt;
}

std::size_t type_size(std::uint16_t type) noexcept {
  switch (type) {
    case 1: case 2: case 6: case 7: return 1;  // BYTE ASCII SBYTE UNDEFINED
    case 3: case 8: return 2;                   // SHORT SSHORT
    case 4: case 9: case 11: case 13: return 4; // LONG SLONG FLOAT IFD
    case 5: case 10: case 12: return 8;         // RATIONAL SRATIONAL DOUBLE
    default: return 0;
  }
}

// Writes an unsigned value into a SHORT or LONG entry holding one value.
bool put_scalar(const tiff_view& v, std::size_t entry, std::uint32_t value) noexcept {
  const std::uint16_t type = v.u16(entry + 2);
  if (v.u32(entry + 4) != 1) return false;
  if (type == kTypeShort) {
    if (value > 0xFFFF) return false;
    v.put16(entry + 8, static_cast<std::uint16_t>(value));
    return true;
  }
  if (type == kTypeLong) {
    v.put32(entry + 8, value);
    return true;
  }
  return false;
}

std::optional<byte_range> find_app1(std::span<const std::uint8_t> jpeg, const char* ns,
                                    std::size_t ns_len) noexcept {
  if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return std::nullopt;
  std::size_t p = 2;
  while (p + 4 <= jpeg.size()) {
    if (jpeg[p] != 0xFF) return std::nullopt;
    const std::uint8_t marker = jpeg[p + 1];
    if (marker == 0xFF) {  // fill byte
      ++p;
      continue;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
      p += 2;
      continue;
    }
    if (marker == 0xDA || marker == 0xD9) return std::nullopt;  // scan: no more headers
    const std::size_t len = static_cast<std::size_t>(jpeg[p + 2] << 8 | jpeg[p + 3]);
    if (len < 2 || p + 2 + len > jpeg.size()) return std::nullopt;
    if (marker == 0xE1 && len >= 2 + ns_len &&
        std::memcmp(jpeg.data() + p + 4, ns, ns_len) == 0) {
      return byte_range{p + 4 + ns_len, len - 2 - ns_len};
    }
    p += 2 + len;
  }
  return std::nullopt;
}

}  // namespace

std::optional<byte_range> find_jpeg_exif(std::span<const std::uint8_t> jpeg) noexcept {
  return find_app1(jpeg, "Exif\0\0", 6);
}

std::optional<byte_range> find_jpeg_xmp(std::span<const std::uint8_t> jpeg) noexcept {
  return find_app1(jpeg, kXmpNamespace, sizeof(kXmpNamespace));
}

int exif_orientation(std::span<const std::uint8_t> tiff) noexcept {
  const auto v = open_tiff(tiff);
  if (!v) return 0;
  const auto e = find_entry(*v, v->u32(4), kTagOrientation);
  if (!e || v->u16(*e + 2) != kTypeShort) return 0;
  const int o = v->u16(*e + 8);
  return (o >= 1 && o <= 8) ? o : 0;
}

int jpeg_orientation(std::span<const std::uint8_t> jpeg) noexcept {
  const auto r = find_jpeg_exif(jpeg);
  if (!r) return 0;
  return exif_orientation(jpeg.subspan(r->offset, r->size));
}

bool exif_set_orientation(std::span<std::uint8_t> tiff, int orientation) noexcept {
  if (orientation < 1 || orientation > 8) return false;
  const auto v = open_tiff(tiff);
  if (!v) return false;
  const auto e = find_entry(*v, v->u32(4), kTagOrientation);
  if (!e) return false;
  return put_scalar(*v, *e, static_cast<std::uint32_t>(orientation));
}

bool exif_set_pixel_dimensions(std::span<std::uint8_t> tiff, std::uint32_t width,
                               std::uint32_t height) noexcept {
  const auto v = open_tiff(tiff);
  if (!v) return false;
  const auto exif_ptr = find_entry(*v, v->u32(4), kTagExifIfd);
  if (!exif_ptr) return true;
  const std::uint32_t exif_ifd = v->u32(*exif_ptr + 8);
  bool ok = true;
  if (const auto x = find_entry(*v, exif_ifd, kTagPixelX)) ok = put_scalar(*v, *x, width) && ok;
  if (const auto y = find_entry(*v, exif_ifd, kTagPixelY)) ok = put_scalar(*v, *y, height) && ok;
  return ok;
}

bool exif_has_gps(std::span<const std::uint8_t> tiff) noexcept {
  const auto v = open_tiff(tiff);
  return v && find_entry(*v, v->u32(4), kTagGpsIfd).has_value();
}

bool exif_strip_gps(std::span<std::uint8_t> tiff) noexcept {
  const auto v = open_tiff(tiff);
  if (!v) return false;
  const std::uint32_t ifd0 = v->u32(4);
  const auto ptr = find_entry(*v, ifd0, kTagGpsIfd);
  if (!ptr) return true;

  // Zero the GPS IFD and the out-of-line values it references.
  const std::uint32_t gps = v->u32(*ptr + 8);
  if (v->has(gps, 2)) {
    const std::uint32_t n = v->u16(gps);
    if (v->has(gps + 2ull, 12ull * n + 4ull)) {
      for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t e = gps + 2 + 12 * static_cast<std::size_t>(i);
        const std::uint64_t bytes = static_cast<std::uint64_t>(type_size(v->u16(e + 2))) * v->u32(e + 4);
        if (bytes > 4) {
          const std::uint32_t off = v->u32(e + 8);
          if (v->has(off, bytes)) std::memset(v->data + off, 0, static_cast<std::size_t>(bytes));
        }
      }
      std::memset(v->data + gps, 0, 2 + 12 * static_cast<std::size_t>(n) + 4);
    }
  }

  // Drop the pointer entry from IFD0: later entries and the next-IFD offset
  // move up one slot, the freed slot is zeroed. The IFD only shrinks.
  const std::uint32_t count = v->u16(ifd0);
  const std::size_t end = ifd0 + 2 + 12 * static_cast<std::size_t>(count) + 4;
  if (!v->has(ifd0, end - ifd0)) return false;
  std::memmove(v->data + *ptr, v->data + *ptr + 12, end - (*ptr + 12));
  std::memset(v->data + end - 12, 0, 12);
  v->put16(ifd0, static_cast<std::uint16_t>(count - 1));
  return true;
}

bool exif_drop_thumbnail(std::span<std::uint8_t> tiff) noexcept {
  const auto v = open_tiff(tiff);
  if (!v) return false;
  const std::uint32_t ifd0 = v->u32(4);
  if (!v->has(ifd0, 2)) return false;
  const std::size_t next = ifd0 + 2 + 12 * static_cast<std::size_t>(v->u16(ifd0));
  if (!v->has(next, 4)) return false;
  v->put32(next, 0);
  return true;
}

int xmp_set_orientation(std::span<std::uint8_t> xmp, int orientation) noexcept {
  if (orientation < 1 || orientation > 8) return 0;
  constexpr char kName[] = "tiff:Orientation";
  constexpr std::size_t kLen = sizeof(kName) - 1;
  int rewritten = 0;
  for (std::size_t i = 0; i + kLen < xmp.size(); ++i) {
    if (std::memcmp(xmp.data() + i, kName, kLen) != 0) continue;
    // Attribute form: tiff:Orientation="6"; element form:
    // <tiff:Orientation>6</tiff:Orientation>. Skip to the first digit after
    // `=` + quote or `>`, and only rewrite a single digit.
    std::size_t p = i + kLen;
    while (p < xmp.size() && (xmp[p] == ' ' || xmp[p] == '\t' || xmp[p] == '\n' || xmp[p] == '\r')) ++p;
    if (p < xmp.size() && xmp[p] == '=') {
      ++p;
      while (p < xmp.size() && (xmp[p] == ' ' || xmp[p] == '\t')) ++p;
      if (p < xmp.size() && (xmp[p] == '"' || xmp[p] == '\'')) ++p;
    } else if (p < xmp.size() && xmp[p] == '>') {
      ++p;
    } else {
      continue;
    }
    if (p + 1 < xmp.size() && xmp[p] >= '1' && xmp[p] <= '8' &&
        !(xmp[p + 1] >= '0' && xmp[p + 1] <= '9')) {
      xmp[p] = static_cast<std::uint8_t>('0' + orientation);
      ++rewritten;
    }
    i = p;
  }
  return rewritten;
}

bool xmp_has_gps(std::span<const std::uint8_t> xmp) noexcept {
  constexpr char kName[] = "exif:GPS";
  constexpr std::size_t kLen = sizeof(kName) - 1;
  for (std::size_t i = 0; i + kLen <= xmp.size(); ++i) {
    if (std::memcmp(xmp.data() + i, kName, kLen) == 0) return true;
  }
  return false;
}

std::vector<std::uint8_t> exif_minimal(int orientation) {
  if (orientation < 1 || orientation > 8) orientation = 1;
  // "II", 42, IFD0 at 8; one entry (Orientation SHORT 1); next IFD 0.
  return std::vector<std::uint8_t>{
      'I', 'I', 42, 0, 8, 0, 0, 0,
      1, 0,
      0x12, 0x01, 3, 0, 1, 0, 0, 0, static_cast<std::uint8_t>(orientation), 0, 0, 0,
      0, 0, 0, 0};
}

}  // namespace mv::codec
