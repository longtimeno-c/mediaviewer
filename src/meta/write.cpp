// SPDX-License-Identifier: GPL-2.0-or-later
// PR 12 metadata writer (meta/write.h). The rules it exists to keep:
//   * never open an original for writing (rule 5): a JPEG is rewritten as new
//     bytes and swapped in by the io replace port; everything else gets a
//     sidecar;
//   * what was not asked for is not changed: the rewrite is checked against
//     the original before it replaces anything;
//   * a snapshot of the three fields exists before the first write.
//
// Exiv2 reports failure by throwing. That is confined to this file (and
// fields.cpp): every entry point catches at the boundary.
#include "meta/write.h"

#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "codec/format.h"
#include "io/content_hash.h"
#include "io/file.h"
#include "io/replace.h"
#include "meta/internal.h"

namespace mv::meta {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxJpegBytes = 256u * 1024 * 1024;  // beyond this, a sidecar
constexpr std::size_t kMaxSidecarBytes = 4u * 1024 * 1024;

fs::path to_path(std::string_view utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// A file's identity for "did it change under us".
struct stamp {
  std::uintmax_t size = 0;
  fs::file_time_type mtime{};
  bool operator==(const stamp&) const = default;
};

bool stamp_of(const fs::path& p, stamp& out) {
  std::error_code ec;
  out.size = fs::file_size(p, ec);
  if (ec) return false;
  out.mtime = fs::last_write_time(p, ec);
  return !ec;
}

// ---- JPEG structure --------------------------------------------------------

struct jpeg_scan {
  bool valid = false;        // SOI .. EOI walked cleanly
  bool mpf = false;          // a Multi-Picture Format APP2: offsets a resized header would break
  bool trailer = false;      // bytes after the final EOI (a motion photo's video, a gain map)
  bool xmp_extended = false; // XMP split across APP1 segments; Exiv2 keeps only the main packet
  std::vector<std::uint8_t> digest_input;  // see payload_digest()
};

bool starts_with(std::span<const std::uint8_t> b, std::size_t at, const char* s, std::size_t n) {
  return at + n <= b.size() && std::memcmp(b.data() + at, s, n) == 0;
}

// Walks a JPEG's markers and returns what the writer needs to know about it.
// `digest` is filled with the bytes that must survive any metadata rewrite:
// every segment except Exif / XMP APP1, the Photoshop APP13 (IPTC) and COM,
// with an ICC profile reduced to its payload (Exiv2 re-splits it into APP2
// chunks, which is not a change to the profile), then the whole entropy-coded
// part from the first SOS on.
jpeg_scan scan_jpeg(std::span<const std::uint8_t> b, bool want_digest) {
  jpeg_scan out;
  if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) return out;
  std::size_t i = 2;
  if (want_digest) out.digest_input.assign(b.begin(), b.begin() + 2);

  const auto push = [&](std::size_t from, std::size_t to) {
    if (want_digest) out.digest_input.insert(out.digest_input.end(), b.begin() + from, b.begin() + to);
  };

  bool in_scan = false;
  const std::size_t scan_start_unset = static_cast<std::size_t>(-1);
  std::size_t scan_start = scan_start_unset;
  while (i + 1 < b.size()) {
    if (in_scan) {
      // Entropy-coded data: 0xFF is followed by 0x00 (stuffing), RSTn, or a marker.
      if (b[i] != 0xFF) { ++i; continue; }
      const std::uint8_t m = b[i + 1];
      if (m == 0x00 || (m >= 0xD0 && m <= 0xD7) || m == 0xFF) { i += (m == 0xFF ? 1 : 2); continue; }
      in_scan = false;  // a real marker: fall through to the segment walk below
      continue;
    }
    if (b[i] != 0xFF) return out;  // garbage between segments
    const std::uint8_t marker = b[i + 1];
    if (marker == 0xFF) { ++i; continue; }  // fill byte
    if (marker == 0xD9) {                  // EOI
      const std::size_t end = i + 2;
      out.trailer = end != b.size();
      if (scan_start != scan_start_unset) push(scan_start, end);
      out.valid = scan_start != scan_start_unset;
      return out;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {  // no length
      if (scan_start == scan_start_unset) push(i, i + 2);
      i += 2;
      continue;
    }
    if (i + 4 > b.size()) return out;
    const std::size_t len = (static_cast<std::size_t>(b[i + 2]) << 8) | b[i + 3];
    if (len < 2 || i + 2 + len > b.size()) return out;
    const std::size_t seg = i + 2 + len;
    const std::size_t body = i + 4;

    if (marker == 0xDA) {  // SOS: the header is image data; so is what follows
      if (scan_start == scan_start_unset) scan_start = i;
      in_scan = true;
      i = seg;
      continue;
    }
    if (scan_start != scan_start_unset) {  // a DHT / DRI between progressive scans
      i = seg;
      continue;
    }

    bool metadata_segment = false;
    if (marker == 0xE1) {
      if (starts_with(b, body, "Exif\0\0", 6)) metadata_segment = true;
      else if (starts_with(b, body, "http://ns.adobe.com/xap/1.0/\0", 29)) metadata_segment = true;
      else if (starts_with(b, body, "http://ns.adobe.com/xmp/extension/\0", 35)) {
        metadata_segment = true;
        out.xmp_extended = true;
      }
    } else if (marker == 0xED && starts_with(b, body, "Photoshop 3.0\0", 14)) {
      metadata_segment = true;
    } else if (marker == 0xFE) {
      metadata_segment = true;
    } else if (marker == 0xE2) {
      if (starts_with(b, body, "MPF\0", 4)) out.mpf = true;
      if (starts_with(b, body, "ICC_PROFILE\0", 12) && len >= 16) {
        push(body + 14, seg);  // payload only, past the sequence number and count
        i = seg;
        continue;
      }
    }
    if (!metadata_segment) push(i, seg);
    i = seg;
  }
  return out;
}

io::content_hash digest_of(const std::vector<std::uint8_t>& bytes) {
  io::hasher h;
  h.update(bytes);
  return h.finish();
}

// ---- What a write is allowed to change ---------------------------------------

constexpr const char* kTouchedExif[] = {
    "Exif.Image.Orientation", "Exif.Photo.UserComment", "Exif.Image.Rating",
    "Exif.Image.RatingPercent",
};
constexpr const char* kTouchedXmp[] = {
    "Xmp.xmp.Rating", "Xmp.MicrosoftPhoto.Rating", "Xmp.tiff.Orientation", "Xmp.exif.UserComment",
};

bool in_list(const std::string& key, const char* const* list, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    if (key == list[i]) return true;
  }
  return false;
}
bool touched_exif(const std::string& k) { return in_list(k, kTouchedExif, std::size(kTouchedExif)); }
bool touched_xmp(const std::string& k) { return in_list(k, kTouchedXmp, std::size(kTouchedXmp)); }

// Tags whose value is a byte offset into the file or block. They legitimately
// move when the block is laid out again, so the untouched-tags check skips
// them; the data they point at is checked another way (the thumbnail below).
bool offset_valued(const Exiv2::Exifdatum& d) {
  const std::string n = d.tagName();
  if (n.find("Offset") != std::string::npos) return true;
  if (n.find("ImageStart") != std::string::npos) return true;
  return n == "JPEGInterchangeFormat" || n == "Preview" || n == "PreviewIFD" || n == "NikonPreview" ||
         n == "ExifTag" || n == "GPSTag" || n == "InteroperabilityTag" || n == "SubIFDs";
}

// A maker note Exiv2 understands is compared entry by entry (Exif.Canon.* ...);
// its raw blob moves, and its internal offsets move with it, whenever the
// block is laid out again. One it does not understand is only its blob, and
// that must come back byte for byte.
bool decoded_maker_note(const Exiv2::ExifData& e) {
  for (const auto& d : e) {
    const std::string g = d.groupName();
    if (g != "Image" && g != "Photo" && g != "GPSInfo" && g != "Iop" && g != "Thumbnail" &&
        g.rfind("SubImage", 0) != 0) {
      return true;
    }
  }
  return false;
}

struct exif_row {
  int type = 0;
  std::size_t count = 0;
  std::vector<std::uint8_t> bytes;
  auto operator<=>(const exif_row&) const = default;
};

// Tag key -> every value the file holds for it, in a canonical order (a key
// can repeat, and Exiv2 does not promise to list repeats in the same order
// after a rewrite).
using exif_rows_t = std::map<std::string, std::vector<exif_row>>;
using text_rows_t = std::map<std::string, std::vector<std::string>>;

exif_rows_t exif_rows(const Exiv2::ExifData& e) {
  exif_rows_t rows;
  const bool note_decoded = decoded_maker_note(e);
  for (const auto& d : e) {
    const std::string key = d.key();
    if (touched_exif(key) || offset_valued(d)) continue;
    if (note_decoded && key == "Exif.Photo.MakerNote") continue;
    exif_row r;
    r.type = static_cast<int>(d.typeId());
    r.count = d.count();
    r.bytes.resize(d.size());
    if (!r.bytes.empty()) d.copy(reinterpret_cast<Exiv2::byte*>(r.bytes.data()), Exiv2::littleEndian);
    rows[key].push_back(std::move(r));
  }
  for (auto& kv : rows) std::sort(kv.second.begin(), kv.second.end());
  return rows;
}

text_rows_t xmp_rows(const Exiv2::XmpData& x) {
  text_rows_t rows;
  for (const auto& d : x) {
    const std::string key = d.key();
    if (touched_xmp(key)) continue;
    rows[key].push_back(d.toString());
  }
  for (auto& kv : rows) std::sort(kv.second.begin(), kv.second.end());
  return rows;
}

text_rows_t iptc_rows(const Exiv2::IptcData& x) {
  text_rows_t rows;
  for (const auto& d : x) rows[d.key()].push_back(d.toString());
  for (auto& kv : rows) std::sort(kv.second.begin(), kv.second.end());
  return rows;
}

std::vector<std::uint8_t> thumbnail_of(const Exiv2::ExifData& e) {
  const Exiv2::DataBuf t = Exiv2::ExifThumbC(e).copy();
  return std::vector<std::uint8_t>(t.c_data(), t.c_data() + t.size());
}

// ---- Applying fields to Exiv2's structures ---------------------------------

// XMP Rating in Windows' percent scale, for the tags that use it.
int percent_of(int stars) {
  static constexpr int kPercent[] = {0, 1, 25, 50, 75, 99};
  return kPercent[std::clamp(stars, 0, 5)];
}

bool valid_utf8(const std::string& s) {
  if (detail::sanitise_utf8(s) != s) return false;
  return s.find('\0') == std::string::npos;
}

std::optional<int> wanted_rating(const write_fields& f) {
  if (!f.rating.touches()) return std::nullopt;
  if (f.rating.k == change<int>::kind::clear) return 0;
  return f.rating.value;
}

void erase_key(Exiv2::ExifData& e, const char* key) {
  const auto it = e.findKey(Exiv2::ExifKey(key));
  if (it != e.end()) e.erase(it);
}
void erase_key(Exiv2::XmpData& x, const char* key) {
  const auto it = x.findKey(Exiv2::XmpKey(key));
  if (it != x.end()) x.erase(it);
}
bool has_key(const Exiv2::ExifData& e, const char* key) { return e.findKey(Exiv2::ExifKey(key)) != e.end(); }
bool has_key(const Exiv2::XmpData& x, const char* key) { return x.findKey(Exiv2::XmpKey(key)) != x.end(); }

void set_xmp_comment(Exiv2::XmpData& x, const std::string& text) {
  erase_key(x, "Xmp.exif.UserComment");
  Exiv2::LangAltValue v;
  v.read("lang=x-default " + text);
  x.add(Exiv2::XmpKey("Xmp.exif.UserComment"), &v);
}

// Rating and comment into an XMP packet's data (a sidecar, or a JPEG's XMP).
// `create` false = only touch keys already there (used for the mirrors).
void apply_xmp(Exiv2::XmpData& x, const write_fields& f, bool sidecar) {
  if (const auto r = wanted_rating(f)) {
    if (*r == 0) {
      erase_key(x, "Xmp.xmp.Rating");
      erase_key(x, "Xmp.MicrosoftPhoto.Rating");
    } else {
      x["Xmp.xmp.Rating"] = std::to_string(*r);
      if (has_key(x, "Xmp.MicrosoftPhoto.Rating")) {
        if (*r > 0) x["Xmp.MicrosoftPhoto.Rating"] = std::to_string(percent_of(*r));
        else erase_key(x, "Xmp.MicrosoftPhoto.Rating");
      }
    }
  }
  if (f.orientation.touches()) {
    // A sidecar always records it; a JPEG's XMP only stays in agreement with
    // the EXIF value it already mirrors.
    if (f.orientation.k == change<int>::kind::clear) {
      erase_key(x, "Xmp.tiff.Orientation");
    } else if (sidecar || has_key(x, "Xmp.tiff.Orientation")) {
      x["Xmp.tiff.Orientation"] = std::to_string(f.orientation.value);
    }
  }
  if (f.comment.touches()) {
    if (f.comment.k == change<std::string>::kind::clear || f.comment.value.empty()) {
      erase_key(x, "Xmp.exif.UserComment");
    } else {
      set_xmp_comment(x, f.comment.value);
    }
  }
}

void apply_exif(Exiv2::ExifData& e, const write_fields& f, detail::comment_order order) {
  if (const auto r = wanted_rating(f)) {
    // XMP carries the rating. The EXIF tags Windows reads are only kept in
    // agreement when the file already had them, so a rating never grows IFD0.
    const bool exact = *r >= 1;  // EXIF cannot say "rejected"
    if (has_key(e, "Exif.Image.Rating")) {
      if (exact) e["Exif.Image.Rating"] = static_cast<uint16_t>(*r);
      else erase_key(e, "Exif.Image.Rating");
    }
    if (has_key(e, "Exif.Image.RatingPercent")) {
      if (exact) e["Exif.Image.RatingPercent"] = static_cast<uint16_t>(percent_of(*r));
      else erase_key(e, "Exif.Image.RatingPercent");
    }
  }
  if (f.orientation.touches()) {
    if (f.orientation.k == change<int>::kind::clear) erase_key(e, "Exif.Image.Orientation");
    else e["Exif.Image.Orientation"] = static_cast<uint16_t>(f.orientation.value);
  }
  if (f.comment.touches()) {
    if (f.comment.k == change<std::string>::kind::clear || f.comment.value.empty()) {
      erase_key(e, "Exif.Photo.UserComment");
    } else {
      // Written as the raw bytes the tag holds (charset code + text); Exiv2's
      // own comment type needs iconv and reads back as opaque data anyway.
      const std::vector<std::uint8_t> raw = detail::encode_user_comment(f.comment.value, order);
      Exiv2::DataValue value(reinterpret_cast<const Exiv2::byte*>(raw.data()), raw.size(),
                             Exiv2::littleEndian, Exiv2::undefined);
      e["Exif.Photo.UserComment"].setValue(&value);
    }
  }
}

bool validate(const write_fields& f) {
  if (f.rating.k == change<int>::kind::set && (f.rating.value < -1 || f.rating.value > kMaxRating)) return false;
  if (f.orientation.k == change<int>::kind::set && (f.orientation.value < 1 || f.orientation.value > 8)) return false;
  if (f.comment.k == change<std::string>::kind::set) {
    if (f.comment.value.size() > kMaxCommentBytes || !valid_utf8(f.comment.value)) return false;
  }
  return true;
}

// ---- Snapshot store --------------------------------------------------------

struct state_pair {
  write_fields file;     // what the file itself said
  write_fields sidecar;  // what its sidecar said
};

struct snapshot {
  write_target target = write_target::in_file;
  bool sidecar_existed = false;
  state_pair state;
};

std::mutex g_snapshot_mutex;
std::unordered_set<std::string> g_snapshotted;  // snapshot files written this session

std::string snapshot_file(std::string_view dir, std::string_view media_path) {
  io::hasher h;
  h.update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(media_path.data()),
                                         media_path.size()));
  std::string p(dir);
  if (!p.empty() && p.back() != '/' && p.back() != '\\') p += '/';
  return p + h.finish().hex() + ".mvsnap";
}

std::string hex_of(const std::string& s) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out += kDigits[c >> 4];
    out += kDigits[c & 15];
  }
  return out;
}

bool unhex(std::string_view h, std::string& out) {
  if (h.size() % 2) return false;
  out.clear();
  const auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < h.size(); i += 2) {
    const int a = nib(h[i]), b = nib(h[i + 1]);
    if (a < 0 || b < 0) return false;
    out += static_cast<char>((a << 4) | b);
  }
  return true;
}

void put_fields(std::string& out, const char* tag, const write_fields& f) {
  out += tag;
  out += " rating ";
  out += f.rating.k == change<int>::kind::set ? "set " + std::to_string(f.rating.value) : "clear";
  out += "\n";
  out += tag;
  out += " orientation ";
  out += f.orientation.k == change<int>::kind::set ? "set " + std::to_string(f.orientation.value) : "clear";
  out += "\n";
  out += tag;
  out += " comment ";
  out += f.comment.k == change<std::string>::kind::set ? "set " + hex_of(f.comment.value) : "clear";
  out += "\n";
}

std::string serialise(const snapshot& s) {
  std::string out = "mvsnap 1\n";
  out += s.target == write_target::in_file ? "target in_file\n" : "target sidecar\n";
  out += s.sidecar_existed ? "sidecar_existed 1\n" : "sidecar_existed 0\n";
  put_fields(out, "file", s.state.file);
  put_fields(out, "sidecar", s.state.sidecar);
  return out;
}

bool parse_snapshot(const std::string& text, snapshot& out) {
  snapshot s;
  std::size_t pos = 0;
  bool header = false;
  int seen = 0;
  while (pos < text.size()) {
    const std::size_t nl = text.find('\n', pos);
    const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? text.size() : nl + 1;
    if (!header) {
      if (line != "mvsnap 1") return false;
      header = true;
      continue;
    }
    if (line == "target in_file") { s.target = write_target::in_file; ++seen; continue; }
    if (line == "target sidecar") { s.target = write_target::sidecar; ++seen; continue; }
    if (line == "sidecar_existed 0") { s.sidecar_existed = false; ++seen; continue; }
    if (line == "sidecar_existed 1") { s.sidecar_existed = true; ++seen; continue; }
    write_fields* f = nullptr;
    std::string rest;
    if (line.rfind("file ", 0) == 0) { f = &s.state.file; rest = line.substr(5); }
    else if (line.rfind("sidecar ", 0) == 0) { f = &s.state.sidecar; rest = line.substr(8); }
    else return false;
    if (rest.rfind("rating ", 0) == 0) {
      const std::string v = rest.substr(7);
      if (v == "clear") f->rating = change<int>::remove();
      else if (v.rfind("set ", 0) == 0) f->rating = change<int>::to(std::atoi(v.c_str() + 4));
      else return false;
    } else if (rest.rfind("orientation ", 0) == 0) {
      const std::string v = rest.substr(12);
      if (v == "clear") f->orientation = change<int>::remove();
      else if (v.rfind("set ", 0) == 0) f->orientation = change<int>::to(std::atoi(v.c_str() + 4));
      else return false;
    } else if (rest.rfind("comment ", 0) == 0) {
      const std::string v = rest.substr(8);
      if (v == "clear") f->comment = change<std::string>::remove();
      else if (v.rfind("set ", 0) == 0) {
        std::string text_value;
        if (!unhex(std::string_view(v).substr(4), text_value)) return false;
        f->comment = change<std::string>::to(std::move(text_value));
      } else return false;
    } else return false;
    ++seen;
  }
  if (!header || seen != 2 + 6) return false;
  out = std::move(s);
  return true;
}

// What a field_state says, as a write that puts exactly that back.
write_fields fields_of(const detail::field_state& s) {
  write_fields f;
  f.rating = s.rating && *s.rating != 0 ? change<int>::to(*s.rating) : change<int>::remove();
  f.orientation = s.orientation ? change<int>::to(*s.orientation) : change<int>::remove();
  f.comment = s.comment ? change<std::string>::to(*s.comment) : change<std::string>::remove();
  return f;
}

bool save_snapshot(const std::string& file, const snapshot& s) {
  std::error_code ec;
  fs::create_directories(to_path(file).parent_path(), ec);
  const std::string text = serialise(s);
  return static_cast<bool>(io::write_all(
      file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
}

bool load_snapshot(const std::string& file, snapshot& out) {
  auto bytes = io::read_prefix(file, 64 * 1024);
  if (!bytes) return false;
  return parse_snapshot(std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size()), out);
}

// ---- The two writers -------------------------------------------------------

std::vector<std::uint8_t> bytes_of(Exiv2::Image& image) {
  Exiv2::BasicIo& io = image.io();
  io.open();
  const Exiv2::DataBuf buf = io.read(io.size());
  return std::vector<std::uint8_t>(buf.c_data(), buf.c_data() + buf.size());
}

// A JPEG, rewritten with Exiv2 and checked. Returns the new bytes; the caller
// swaps them in. `orig_state` is the file's own fields, captured on the way.
result<std::vector<std::uint8_t>> rewrite_jpeg(std::span<const std::uint8_t> orig,
                                               const write_fields& f, const jpeg_scan& before) {
  try {
    detail::ensure_exiv2();
    auto image = Exiv2::ImageFactory::open(orig.data(), orig.size());
    if (!image) return err(status::unsupported_format);
    image->readMetadata();

    // Copies of the state that has to survive.
    const auto exif_before = exif_rows(image->exifData());
    const auto xmp_before = xmp_rows(image->xmpData());
    const auto iptc_before = iptc_rows(image->iptcData());
    const auto thumb_before = thumbnail_of(image->exifData());
    const std::uint32_t w = image->pixelWidth(), h = image->pixelHeight();
    std::vector<std::uint8_t> icc_before;
    if (image->iccProfileDefined()) {
      const Exiv2::DataBuf& icc = image->iccProfile();
      icc_before.assign(icc.c_data(), icc.c_data() + icc.size());
    }

    detail::comment_order order = detail::comment_order_of(*image);
    if (order == detail::comment_order::unknown) {  // no EXIF yet: a new block, little-endian
      image->setByteOrder(Exiv2::littleEndian);
      order = detail::comment_order::little;
    }
    apply_exif(image->exifData(), f, order);
    apply_xmp(image->xmpData(), f, /*sidecar=*/false);
    image->writeMetadata();
    std::vector<std::uint8_t> out = bytes_of(*image);

    // ---- Verify before anything is replaced -------------------------------
    const jpeg_scan after = scan_jpeg(out, true);
    if (!after.valid || after.trailer || after.mpf) return err(status::internal);
    if (digest_of(before.digest_input) != digest_of(after.digest_input)) return err(status::internal);

    auto check = Exiv2::ImageFactory::open(out.data(), out.size());
    if (!check) return err(status::internal);
    check->readMetadata();
    if (check->pixelWidth() != w || check->pixelHeight() != h) return err(status::internal);
    if (exif_rows(check->exifData()) != exif_before) return err(status::internal);
    if (xmp_rows(check->xmpData()) != xmp_before) return err(status::internal);
    if (iptc_rows(check->iptcData()) != iptc_before) return err(status::internal);
    if (thumbnail_of(check->exifData()) != thumb_before) return err(status::internal);
    std::vector<std::uint8_t> icc_after;
    if (check->iccProfileDefined()) {
      const Exiv2::DataBuf& icc = check->iccProfile();
      icc_after.assign(icc.c_data(), icc.c_data() + icc.size());
    }
    if (icc_after != icc_before) return err(status::internal);

    // What was asked for is what is there.
    const detail::field_state got =
        detail::read_fields(check->exifData(), check->xmpData(), detail::comment_order_of(*check));
    if (const auto r = wanted_rating(f)) {
      if (got.rating.value_or(0) != *r) return err(status::internal);
    }
    if (f.orientation.k == change<int>::kind::set && got.orientation != f.orientation.value) {
      return err(status::internal);
    }
    if (f.comment.touches()) {
      const bool want = f.comment.k == change<std::string>::kind::set && !f.comment.value.empty();
      if (want ? got.comment != f.comment.value : got.comment.has_value()) return err(status::internal);
    }
    return out;
  } catch (const Exiv2::Error&) {
    return err(status::unsupported_format);
  } catch (...) {
    return err(status::internal);
  }
}

// The sidecar at `path` with `f` applied. `existed` says whether there was
// one. An empty result means "no sidecar needed": the caller removes the file.
struct sidecar_result {
  std::string packet;
  bool empty = false;
};

result<sidecar_result> rewrite_sidecar(const std::string& path, const write_fields& f) {
  try {
    detail::ensure_exiv2();
    Exiv2::XmpData xmp;
    (void)detail::load_sidecar(path, xmp);
    const auto before = xmp_rows(xmp);
    apply_xmp(xmp, f, /*sidecar=*/true);

    sidecar_result r;
    if (xmp.empty()) {
      r.empty = true;
      return r;
    }
    if (Exiv2::XmpParser::encode(r.packet, xmp) != 0) return err(status::internal);

    Exiv2::XmpData check;
    if (Exiv2::XmpParser::decode(check, r.packet) != 0) return err(status::internal);
    if (xmp_rows(check) != before) return err(status::internal);
    const detail::field_state got = detail::read_fields(Exiv2::ExifData{}, check);
    if (const auto w = wanted_rating(f)) {
      if (got.rating.value_or(0) != *w) return err(status::internal);
    }
    if (f.comment.touches()) {
      const bool want = f.comment.k == change<std::string>::kind::set && !f.comment.value.empty();
      if (want ? got.comment != f.comment.value : got.comment.has_value()) return err(status::internal);
    }
    return r;
  } catch (const Exiv2::Error&) {
    return err(status::unsupported_format);
  } catch (...) {
    return err(status::internal);
  }
}

expected commit_sidecar(const std::string& path, const sidecar_result& r) {
  std::error_code ec;
  const bool exists = fs::exists(to_path(path), ec);
  if (r.empty) {
    if (exists && !fs::remove(to_path(path), ec)) return err(status::io);
    return {};
  }
  const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(r.packet.data()),
                                            r.packet.size());
  if (bytes.size() > kMaxSidecarBytes) return err(status::invalid_arg);
  return exists ? io::replace_atomic(path, bytes) : io::write_new_atomic(path, bytes);
}

struct loaded {
  std::vector<std::uint8_t> bytes;  // whole file (JPEG only)
  write_target target = write_target::sidecar;
  jpeg_scan scan;
  stamp id;
  fs::path real;  // symlinks resolved: what the swap replaces
};

result<loaded> load_and_plan(std::string_view utf8_path) {
  if (utf8_path.empty()) return err(status::invalid_arg);
  loaded l;
  auto head = io::read_prefix(utf8_path, 1024);
  if (!head) return err(head.error() == status::corrupt ? status::corrupt : status::io);
  const codec::format_family fam = codec::probe(*head);
  if (fam != codec::format_family::jpeg) return l;  // sidecar; the original is not even read further

  std::error_code ec;
  l.real = fs::canonical(to_path(utf8_path), ec);
  if (ec) l.real = to_path(utf8_path);
  if (!stamp_of(l.real, l.id)) return err(status::io);
  if (l.id.size > kMaxJpegBytes) return l;  // too big to hold and check: sidecar
  auto all = io::read_prefix(to_utf8(l.real), kMaxJpegBytes);
  if (!all) return err(status::io);
  l.bytes = std::move(*all);
  l.scan = scan_jpeg(l.bytes, true);
  if (l.scan.valid && !l.scan.mpf && !l.scan.trailer && !l.scan.xmp_extended) l.target = write_target::in_file;
  return l;
}

// The two field sets, and whether to take a snapshot on the way.
result<write_outcome> apply(std::string_view utf8_path, const write_fields& file_fields,
                            const write_fields& sidecar_fields, std::string_view snapshot_dir,
                            bool take_snapshot) {
  if (!validate(file_fields) || !validate(sidecar_fields)) return err(status::invalid_arg);
  if (file_fields.empty() && sidecar_fields.empty()) return err(status::invalid_arg);

  auto plan = load_and_plan(utf8_path);
  if (!plan) return err(plan.error());
  loaded& l = *plan;
  write_target target = l.target;

  const std::string side_path = sidecar_path_for(utf8_path);
  Exiv2::XmpData existing_sidecar;
  const bool sidecar_existed = detail::load_sidecar(side_path, existing_sidecar);

  // ---- Snapshot (before any change) -------------------------------------
  if (take_snapshot && !snapshot_dir.empty()) {
    const std::string snap_file = snapshot_file(snapshot_dir, utf8_path);
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (g_snapshotted.count(snap_file) == 0) {
      snapshot s;
      s.target = target;
      s.sidecar_existed = sidecar_existed;
      if (target == write_target::in_file) {
        try {
          detail::ensure_exiv2();
          auto image = Exiv2::ImageFactory::open(l.bytes.data(), l.bytes.size());
          if (!image) return err(status::corrupt);
          image->readMetadata();
          s.state.file = fields_of(detail::read_fields(image->exifData(), image->xmpData(),
                                                       detail::comment_order_of(*image)));
        } catch (...) {
          return err(status::corrupt);
        }
      }
      s.state.sidecar = fields_of(detail::read_fields(Exiv2::ExifData{}, existing_sidecar));
      if (!save_snapshot(snap_file, s)) return err(status::io);
      g_snapshotted.insert(snap_file);
    }
  }

  write_outcome out;
  out.sidecar_path = side_path;

  // ---- In place, when it is safe ----------------------------------------
  if (target == write_target::in_file && !file_fields.empty()) {
    auto fresh = rewrite_jpeg(l.bytes, file_fields, l.scan);
    if (fresh) {
      stamp now;
      if (!stamp_of(l.real, now) || !(now == l.id)) return err(status::io);  // changed under us
      const expected swapped = io::replace_atomic(to_utf8(l.real), *fresh);
      if (!swapped) return err(swapped.error());
    } else if (!file_fields.orientation.touches() &&
               (fresh.error() == status::internal || fresh.error() == status::unsupported_format)) {
      // Exiv2 will not rewrite this JPEG cleanly, or its rewrite did not
      // check out. The file is untouched; the rating / comment go to the
      // sidecar instead, which the read model puts over the file.
      target = write_target::sidecar;
    } else {
      return err(fresh.error());
    }
  }
  out.target = target;

  // ---- Sidecar ------------------------------------------------------------
  // Always for a sidecar target; for an in-file write only to keep an
  // existing sidecar in agreement (it would otherwise shadow the new value).
  // In a normal write both field sets are the same; a revert carries what the
  // sidecar said, separately from what the file said.
  if ((target == write_target::sidecar || sidecar_existed) && !sidecar_fields.empty()) {
    std::error_code ec;
    if (!sidecar_existed && fs::exists(to_path(side_path), ec)) {
      return err(status::corrupt);  // something is there that is not XMP we can read: never overwrite it
    }
    auto side = rewrite_sidecar(side_path, sidecar_fields);
    if (!side) return err(side.error());
    const expected done = commit_sidecar(side_path, *side);
    if (!done) return err(done.error());
    out.sidecar_touched = true;
  }
  return out;
}

}  // namespace

// ---- Public ------------------------------------------------------------------

std::string sidecar_path_for(std::string_view utf8_path) {
  std::string p(utf8_path);
  const std::size_t sep = p.find_last_of("/\\");
  const std::size_t name_start = sep == std::string::npos ? 0 : sep + 1;
  const std::size_t dot = p.find_last_of('.');
  // ".hidden" is a name, not an extension.
  if (dot != std::string::npos && dot > name_start) p.resize(dot);
  return p + ".xmp";
}

result<write_target> write_target_for(std::string_view utf8_path) {
  auto plan = load_and_plan(utf8_path);
  if (!plan) return err(plan.error());
  return plan->target;
}

result<write_outcome> write(std::string_view utf8_path, const write_fields& fields,
                            std::string_view snapshot_dir) {
  return apply(utf8_path, fields, fields, snapshot_dir, /*take_snapshot=*/true);
}

bool has_snapshot(std::string_view utf8_path, std::string_view snapshot_dir) {
  if (snapshot_dir.empty()) return false;
  snapshot s;
  return load_snapshot(snapshot_file(snapshot_dir, utf8_path), s);
}

result<write_outcome> revert(std::string_view utf8_path, std::string_view snapshot_dir) {
  if (snapshot_dir.empty()) return err(status::io);
  snapshot s;
  if (!load_snapshot(snapshot_file(snapshot_dir, utf8_path), s)) return err(status::io);
  // The sidecar may have been created by our write; if it did not exist
  // before, putting "nothing" back removes it once it is empty.
  return apply(utf8_path, s.state.file, s.state.sidecar, snapshot_dir, /*take_snapshot=*/false);
}

void reset_snapshot_session() {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  g_snapshotted.clear();
}

}  // namespace mv::meta
