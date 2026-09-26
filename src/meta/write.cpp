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
  // PR 29: where each metadata segment (as digest_input leaves out) sits, as
  // [begin, end) byte offsets, and where new ones go: after SOI and any
  // leading APP0 (JFIF must stay first).
  std::vector<std::pair<std::size_t, std::size_t>> meta_ranges;
  std::size_t insert_at = 2;
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
  bool leading_app0 = true;
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
    else out.meta_ranges.emplace_back(i, seg);
    if (leading_app0 && marker == 0xE0) out.insert_at = seg;
    else leading_app0 = false;
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

using key_set = std::set<std::string>;
const key_set& no_keys() {
  static const key_set empty;
  return empty;
}

// `skip`: keys a PR 29 write changes, compared separately (`only`).
// `only` non-null: the rows of exactly those keys, nothing else.
exif_rows_t exif_rows(const Exiv2::ExifData& e, const key_set& skip = no_keys(),
                      const key_set* only = nullptr) {
  exif_rows_t rows;
  const bool note_decoded = decoded_maker_note(e);
  for (const auto& d : e) {
    const std::string key = d.key();
    if (only != nullptr) {
      if (only->count(key) == 0) continue;
    } else if (touched_exif(key) || offset_valued(d) || skip.count(key) != 0) {
      continue;
    }
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

text_rows_t xmp_rows(const Exiv2::XmpData& x, const key_set& skip = no_keys(),
                     const key_set* only = nullptr) {
  text_rows_t rows;
  for (const auto& d : x) {
    const std::string key = d.key();
    if (only != nullptr) {
      if (only->count(key) == 0) continue;
    } else if (touched_xmp(key) || skip.count(key) != 0) {
      continue;
    }
    rows[key].push_back(d.toString());
  }
  for (auto& kv : rows) std::sort(kv.second.begin(), kv.second.end());
  return rows;
}

text_rows_t iptc_rows(const Exiv2::IptcData& x, const key_set& skip = no_keys(),
                      const key_set* only = nullptr) {
  text_rows_t rows;
  for (const auto& d : x) {
    const std::string key = d.key();
    if (only != nullptr ? only->count(key) == 0 : skip.count(key) != 0) continue;
    rows[key].push_back(d.toString());
  }
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

// ---- PR 29: any tag, and the capture date ----------------------------------

constexpr const char* kDateExif[] = {"Exif.Photo.DateTimeOriginal", "Exif.Photo.DateTimeDigitized",
                                     "Exif.Image.DateTimeOriginal"};
constexpr const char* kDateXmp[] = {"Xmp.exif.DateTimeOriginal", "Xmp.xmp.CreateDate",
                                    "Xmp.photoshop.DateCreated"};

// Tags that describe the file's own layout or pixels, or live inside a maker
// note: a value there that disagrees with the bytes breaks the file for every
// reader. Shown, never written. Orientation is the viewer's rotate (PR 10).
bool exif_read_only(std::string_view key) {
  const std::size_t a = key.find('.');
  const std::size_t b = key.find('.', a + 1);
  if (a == std::string_view::npos || b == std::string_view::npos) return true;
  const std::string_view group = key.substr(a + 1, b - a - 1);
  const std::string_view name = key.substr(b + 1);
  if (group != "Image" && group != "Photo" && group != "GPSInfo" && group != "Iop") return true;
  if (name.find("Offset") != std::string_view::npos || name.find("ByteCount") != std::string_view::npos) return true;
  static constexpr std::string_view kLayout[] = {
      "ImageWidth", "ImageLength", "BitsPerSample", "Compression", "PhotometricInterpretation",
      "SamplesPerPixel", "RowsPerStrip", "PlanarConfiguration", "TileWidth", "TileLength", "SubIFDs",
      "JPEGInterchangeFormat", "JPEGInterchangeFormatLength", "YCbCrSubSampling", "YCbCrPositioning",
      "ExifTag", "GPSTag", "InteroperabilityTag", "NewSubfileType", "SubfileType", "MakerNote",
      "PixelXDimension", "PixelYDimension", "Orientation", "PrintImageMatching", "DNGPrivateData"};
  for (std::string_view n : kLayout) {
    if (name == n) return true;
  }
  return false;
}

// An EXIF / IPTC value as the XMP a sidecar holds for it (Exiv2's own
// conversion table). Empty when XMP has no name for that tag.
Exiv2::XmpData as_xmp(const std::string& key, const std::string& value) {
  Exiv2::XmpData out;
  if (key.rfind("Exif.", 0) == 0) {
    Exiv2::ExifData tmp;
    if (tmp[key].setValue(value) != 0) return out;
    Exiv2::copyExifToXmp(tmp, out);
  } else if (key.rfind("Iptc.", 0) == 0) {
    Exiv2::IptcData tmp;
    if (tmp[key].setValue(value) != 0) return out;
    Exiv2::copyIptcToXmp(tmp, out);
  }
  return out;
}

// A plausible value to ask the conversion table whether a key maps at all.
std::string sample_value(std::string_view key) {
  return key.find("Date") != std::string_view::npos ? "2000:01:01 00:00:00" : "1";
}

void erase_all(Exiv2::ExifData& e, const std::string& key) {
  const Exiv2::ExifKey k(key);
  for (auto it = e.findKey(k); it != e.end(); it = e.findKey(k)) e.erase(it);
}
void erase_all(Exiv2::IptcData& x, const std::string& key) {
  const Exiv2::IptcKey k(key);
  for (auto it = x.findKey(k); it != x.end(); it = x.findKey(k)) x.erase(it);
}
void erase_all(Exiv2::XmpData& x, const std::string& key) {
  const Exiv2::XmpKey k(key);
  for (auto it = x.findKey(k); it != x.end(); it = x.findKey(k)) x.erase(it);
}

struct bad_value {};  // a value Exiv2 will not take for that tag: invalid_arg

void set_xmp(Exiv2::XmpData& x, const std::string& key, const std::string& value) {
  const Exiv2::XmpKey k(key);
  const auto it = x.findKey(k);
  const Exiv2::TypeId type = it != x.end() ? it->typeId() : Exiv2::XmpProperties::propertyType(k);
  erase_all(x, key);
  const auto v = Exiv2::Value::create(type);
  if (type == Exiv2::xmpBag || type == Exiv2::xmpSeq || type == Exiv2::xmpAlt) {
    // The tree shows an array as "a, b, c"; each item is read back in.
    std::size_t pos = 0;
    while (pos <= value.size()) {
      std::size_t cut = value.find(", ", pos);
      if (cut == std::string::npos) cut = value.size();
      if (cut > pos && v->read(value.substr(pos, cut - pos)) != 0) throw bad_value{};
      pos = cut + 2;
    }
  } else if (v->read(value) != 0) {
    throw bad_value{};
  }
  x.add(k, v.get());
}

// The tag edits into a JPEG's own blocks (`sidecar` false) or into a
// sidecar's XMP (`sidecar` true: EXIF / IPTC keys go under their XMP names).
void apply_tags(Exiv2::ExifData* e, Exiv2::IptcData* iptc, Exiv2::XmpData& x, const write_fields& f,
                bool sidecar) {
  for (const tag_edit& t : f.tags) {
    const bool set = t.value.k == change<std::string>::kind::set;
    if (t.key.rfind("Xmp.", 0) == 0) {
      if (set) set_xmp(x, t.key, t.value.value);
      else erase_all(x, t.key);
    } else if (sidecar) {
      // Only a set reaches here (apply() refuses a remove): the file keeps its
      // own value, the sidecar -- which the viewer and Lightroom read over it
      // -- says the new one.
      const Exiv2::XmpData mapped = as_xmp(t.key, t.value.value);
      if (mapped.empty()) throw bad_value{};
      for (const auto& d : mapped) {
        erase_all(x, d.key());
        x.add(d);
      }
    } else if (t.key.rfind("Exif.", 0) == 0 && e != nullptr) {
      if (set) {
        if ((*e)[t.key].setValue(t.value.value) != 0) throw bad_value{};
      } else {
        erase_all(*e, t.key);
      }
    } else if (t.key.rfind("Iptc.", 0) == 0 && iptc != nullptr) {
      erase_all(*iptc, t.key);
      if (set) {
        Exiv2::Iptcdatum d{Exiv2::IptcKey(t.key)};
        if (d.setValue(t.value.value) != 0) throw bad_value{};
        iptc->add(d);
      }
    } else {
      throw bad_value{};
    }
  }
}

void apply_date(Exiv2::ExifData* e, Exiv2::XmpData& x, const change<std::string>& d, bool sidecar) {
  if (!d.touches()) return;
  std::string exif_form, xmp_form;
  const bool set = d.k == change<std::string>::kind::set;
  if (set && !exif_date_of(d.value, exif_form, xmp_form)) throw bad_value{};
  if (e != nullptr) {
    for (const char* key : kDateExif) {
      const bool primary = std::strcmp(key, "Exif.Photo.DateTimeOriginal") == 0;
      if (!set) erase_all(*e, key);
      else if (primary || has_key(*e, key)) (*e)[key] = exif_form;
    }
  }
  for (const char* key : kDateXmp) {
    // A sidecar states the date where Lightroom and Photos look for it; a
    // JPEG's own XMP only keeps the copies it already has in agreement.
    const bool create = sidecar && std::strcmp(key, "Xmp.photoshop.DateCreated") != 0;
    if (!set) erase_all(x, key);
    else if (create || has_key(x, key)) x[key] = xmp_form;
  }
}

// Every key a write's tag and date edits may change, for the checks.
key_set touched_keys(const write_fields& f) {
  key_set keys;
  for (const tag_edit& t : f.tags) {
    keys.insert(t.key);
    if (t.key.rfind("Xmp.", 0) != 0 && t.value.k == change<std::string>::kind::set) {
      try {
        for (const auto& d : as_xmp(t.key, t.value.value)) keys.insert(d.key());
      } catch (...) {
      }
    }
  }
  if (f.date_taken.touches()) {
    for (const char* k : kDateExif) keys.insert(k);
    for (const char* k : kDateXmp) keys.insert(k);
  }
  return keys;
}

bool validate(const write_fields& f) {
  if (f.tags.size() > kMaxTagEdits) return false;
  for (const tag_edit& t : f.tags) {
    if (t.key.rfind("Exif.", 0) != 0 && t.key.rfind("Iptc.", 0) != 0 && t.key.rfind("Xmp.", 0) != 0) return false;
    if (t.value.k == change<std::string>::kind::keep) return false;
    if (t.value.value.size() > kMaxTagValueBytes || !valid_utf8(t.value.value)) return false;
  }
  if (f.date_taken.k == change<std::string>::kind::set) {
    std::string a, b;
    if (!exif_date_of(f.date_taken.value, a, b)) return false;
  }
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

// ---- PR 29: the whole-metadata snapshot ---------------------------------------
//
// Beside the three-field snapshot: a JPEG's metadata segments exactly as they
// were (Exif / XMP APP1, IPTC APP13, COM) and the sidecar's bytes (or its
// absence). `revert` splices the segments back and restores the sidecar, so
// every tag -- not only rating, orientation and comment -- returns, byte for
// byte. The image data is checked unchanged before anything is replaced.

struct blob_snapshot {
  write_target target = write_target::sidecar;
  bool sidecar_existed = false;
  std::string sidecar;                 // the packet's bytes, when it existed
  std::vector<std::string> segments;   // in file order, marker included
};

constexpr std::size_t kMaxBlobSnapshotBytes = 16u * 1024 * 1024;

std::string blob_file(std::string_view dir, std::string_view media_path) {
  return snapshot_file(dir, media_path) + "2";  // "<hash>.mvsnap2"
}

bool save_blob_snapshot(const std::string& file, const blob_snapshot& b) {
  std::string out = "mvsnap 2\n";
  out += b.target == write_target::in_file ? "target in_file\n" : "target sidecar\n";
  out += b.sidecar_existed ? "sidecar " + hex_of(b.sidecar) + "\n" : "sidecar -\n";
  for (const std::string& seg : b.segments) out += "seg " + hex_of(seg) + "\n";
  std::error_code ec;
  fs::create_directories(to_path(file).parent_path(), ec);
  return static_cast<bool>(io::write_all(
      file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(out.data()), out.size())));
}

bool load_blob_snapshot(const std::string& file, blob_snapshot& out) {
  auto bytes = io::read_prefix(file, kMaxBlobSnapshotBytes);
  if (!bytes) return false;
  const std::string text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  blob_snapshot b;
  std::size_t pos = 0;
  int line_no = 0;
  bool have_target = false, have_sidecar = false;
  while (pos < text.size()) {
    const std::size_t nl = text.find('\n', pos);
    if (nl == std::string::npos) return false;  // a torn write
    const std::string_view line(text.data() + pos, nl - pos);
    pos = nl + 1;
    if (line_no++ == 0) {
      if (line != "mvsnap 2") return false;
      continue;
    }
    if (line == "target in_file") { b.target = write_target::in_file; have_target = true; }
    else if (line == "target sidecar") { b.target = write_target::sidecar; have_target = true; }
    else if (line == "sidecar -") { b.sidecar_existed = false; have_sidecar = true; }
    else if (line.rfind("sidecar ", 0) == 0) {
      if (!unhex(line.substr(8), b.sidecar)) return false;
      b.sidecar_existed = true;
      have_sidecar = true;
    } else if (line.rfind("seg ", 0) == 0) {
      std::string seg;
      if (!unhex(line.substr(4), seg)) return false;
      b.segments.push_back(std::move(seg));
    } else {
      return false;
    }
  }
  if (!have_target || !have_sidecar) return false;
  out = std::move(b);
  return true;
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

    // Copies of the state that has to survive: every tag the write does not
    // name. The ones it does name are checked against a rehearsal below.
    const key_set touched = touched_keys(f);
    const auto exif_before = exif_rows(image->exifData(), touched);
    const auto xmp_before = xmp_rows(image->xmpData(), touched);
    const auto iptc_before = iptc_rows(image->iptcData(), touched);
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
    apply_tags(&image->exifData(), &image->iptcData(), image->xmpData(), f, /*sidecar=*/false);
    apply_date(&image->exifData(), image->xmpData(), f.date_taken, /*sidecar=*/false);
    // PR 29: what the named tags must read back as -- Exiv2's own encoding of
    // the edit, taken before it is written.
    const auto exif_want = exif_rows(image->exifData(), no_keys(), &touched);
    const auto xmp_want = xmp_rows(image->xmpData(), no_keys(), &touched);
    const auto iptc_want = iptc_rows(image->iptcData(), no_keys(), &touched);
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
    if (exif_rows(check->exifData(), touched) != exif_before) return err(status::internal);
    if (xmp_rows(check->xmpData(), touched) != xmp_before) return err(status::internal);
    if (iptc_rows(check->iptcData(), touched) != iptc_before) return err(status::internal);
    if (exif_rows(check->exifData(), no_keys(), &touched) != exif_want) return err(status::internal);
    if (xmp_rows(check->xmpData(), no_keys(), &touched) != xmp_want) return err(status::internal);
    if (iptc_rows(check->iptcData(), no_keys(), &touched) != iptc_want) return err(status::internal);
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
  } catch (const bad_value&) {
    return err(status::invalid_arg);
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
    const key_set touched = touched_keys(f);
    const auto before = xmp_rows(xmp, touched);
    apply_xmp(xmp, f, /*sidecar=*/true);
    apply_tags(nullptr, nullptr, xmp, f, /*sidecar=*/true);
    apply_date(nullptr, xmp, f.date_taken, /*sidecar=*/true);
    const auto want_rows = xmp_rows(xmp, no_keys(), &touched);

    sidecar_result r;
    if (xmp.empty()) {
      r.empty = true;
      return r;
    }
    if (Exiv2::XmpParser::encode(r.packet, xmp) != 0) return err(status::internal);

    Exiv2::XmpData check;
    if (Exiv2::XmpParser::decode(check, r.packet) != 0) return err(status::internal);
    if (xmp_rows(check, touched) != before) return err(status::internal);
    if (xmp_rows(check, no_keys(), &touched) != want_rows) return err(status::internal);
    const detail::field_state got = detail::read_fields(Exiv2::ExifData{}, check);
    if (const auto w = wanted_rating(f)) {
      if (got.rating.value_or(0) != *w) return err(status::internal);
    }
    if (f.comment.touches()) {
      const bool want = f.comment.k == change<std::string>::kind::set && !f.comment.value.empty();
      if (want ? got.comment != f.comment.value : got.comment.has_value()) return err(status::internal);
    }
    return r;
  } catch (const bad_value&) {
    return err(status::invalid_arg);
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

  // PR 29: a tag the file cannot take is refused before anything is written.
  // A JPEG that Exiv2 then declines falls back to its sidecar only when every
  // tag edit can live there (an EXIF / IPTC remove cannot).
  bool sidecar_ok = !file_fields.orientation.touches();
  for (const tag_edit& t : file_fields.tags) {
    const tag_access here = access_of(t.key, target);
    const bool removes = t.value.k == change<std::string>::kind::clear;
    if (here == tag_access::read_only || (here == tag_access::via_sidecar && removes)) {
      return err(status::invalid_arg);
    }
    const tag_access there = access_of(t.key, write_target::sidecar);
    if (there == tag_access::read_only || (there == tag_access::via_sidecar && removes)) sidecar_ok = false;
  }

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
      // PR 29: and every tag, as bytes.
      blob_snapshot b;
      b.target = target;
      b.sidecar_existed = sidecar_existed;
      if (sidecar_existed) {
        auto side = io::read_prefix(side_path, kMaxSidecarBytes + 1);
        if (!side || side->size() > kMaxSidecarBytes) return err(status::io);
        b.sidecar.assign(reinterpret_cast<const char*>(side->data()), side->size());
      }
      if (target == write_target::in_file) {
        for (const auto& [from, to] : l.scan.meta_ranges) {
          b.segments.emplace_back(reinterpret_cast<const char*>(l.bytes.data() + from), to - from);
        }
      }
      if (!save_snapshot(snap_file, s)) return err(status::io);
      if (!save_blob_snapshot(blob_file(snapshot_dir, utf8_path), b)) return err(status::io);
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
    } else if (sidecar_ok &&
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
  if (blob_snapshot b; load_blob_snapshot(blob_file(snapshot_dir, utf8_path), b)) return true;
  snapshot s;
  return load_snapshot(snapshot_file(snapshot_dir, utf8_path), s);
}

namespace {

// Puts back what a blob snapshot holds (see blob_snapshot).
result<write_outcome> revert_blobs(std::string_view utf8_path, const blob_snapshot& b) {
  write_outcome out;
  out.target = b.target;
  out.sidecar_path = sidecar_path_for(utf8_path);
  if (b.target == write_target::in_file) {
    auto plan = load_and_plan(utf8_path);
    if (!plan) return err(plan.error());
    loaded& l = *plan;
    if (l.target != write_target::in_file) return err(status::io);  // no longer the JPEG it was
    // The file with its metadata segments taken out, the snapshot's put in.
    std::vector<std::uint8_t> fresh(l.bytes.begin(), l.bytes.begin() + static_cast<std::ptrdiff_t>(l.scan.insert_at));
    for (const std::string& seg : b.segments) fresh.insert(fresh.end(), seg.begin(), seg.end());
    std::size_t at = l.scan.insert_at;
    for (const auto& [from, to] : l.scan.meta_ranges) {
      if (from < at) return err(status::internal);
      fresh.insert(fresh.end(), l.bytes.begin() + static_cast<std::ptrdiff_t>(at),
                   l.bytes.begin() + static_cast<std::ptrdiff_t>(from));
      at = to;
    }
    fresh.insert(fresh.end(), l.bytes.begin() + static_cast<std::ptrdiff_t>(at), l.bytes.end());
    // The picture, and every other segment, must be exactly what is there now.
    const jpeg_scan after = scan_jpeg(fresh, true);
    if (!after.valid || digest_of(after.digest_input) != digest_of(l.scan.digest_input)) {
      return err(status::internal);
    }
    if (after.meta_ranges.size() != b.segments.size()) return err(status::internal);
    stamp now;
    if (!stamp_of(l.real, now) || !(now == l.id)) return err(status::io);  // changed under us
    const expected swapped = io::replace_atomic(to_utf8(l.real), fresh);
    if (!swapped) return err(swapped.error());
  }
  std::error_code ec;
  const bool exists = fs::exists(to_path(out.sidecar_path), ec);
  if (b.sidecar_existed) {
    const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(b.sidecar.data()),
                                              b.sidecar.size());
    const expected done = exists ? io::replace_atomic(out.sidecar_path, bytes)
                                 : io::write_new_atomic(out.sidecar_path, bytes);
    if (!done) return err(done.error());
    out.sidecar_touched = true;
  } else if (exists) {
    if (!fs::remove(to_path(out.sidecar_path), ec)) return err(status::io);
    out.sidecar_touched = true;
  }
  return out;
}

}  // namespace

tag_access access_of(std::string_view key, write_target target) noexcept {
  try {
    if (key.rfind("Xmp.", 0) == 0) {
      detail::ensure_exiv2();
      (void)Exiv2::XmpKey(std::string(key));  // throws on an unknown namespace
      return tag_access::editable;
    }
    const bool exif = key.rfind("Exif.", 0) == 0;
    if (!exif && key.rfind("Iptc.", 0) != 0) return tag_access::read_only;  // container, computed
    if (exif && exif_read_only(key)) return tag_access::read_only;
    if (target == write_target::in_file) return tag_access::editable;
    detail::ensure_exiv2();
    const std::string k(key);
    return as_xmp(k, sample_value(k)).empty() ? tag_access::read_only : tag_access::via_sidecar;
  } catch (...) {
    return tag_access::read_only;
  }
}

bool exif_date_of(std::string_view stamp, std::string& exif_out, std::string& xmp_out) {
  // "YYYY-MM-DD HH:MM:SS", with '-' or ':' in the date and ' ' or 'T' between.
  if (stamp.size() != 19) return false;
  int v[6] = {};
  const std::size_t at[6] = {0, 5, 8, 11, 14, 17};
  const std::size_t len[6] = {4, 2, 2, 2, 2, 2};
  for (int i = 0; i < 6; ++i) {
    for (std::size_t j = 0; j < len[i]; ++j) {
      const char c = stamp[at[i] + j];
      if (c < '0' || c > '9') return false;
      v[i] = v[i] * 10 + (c - '0');
    }
  }
  const auto sep = [&](std::size_t i, const char* ok) { return std::strchr(ok, stamp[i]) != nullptr; };
  if (!sep(4, "-:") || stamp[7] != stamp[4] || !sep(10, " T") || stamp[13] != ':' || stamp[16] != ':') return false;
  static constexpr int kDays[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (v[0] % 4 == 0 && v[0] % 100 != 0) || v[0] % 400 == 0;
  if (v[0] < 1800 || v[1] < 1 || v[1] > 12 || v[2] < 1) return false;
  if (v[2] > kDays[v[1] - 1] || (v[1] == 2 && v[2] == 29 && !leap)) return false;
  if (v[3] > 23 || v[4] > 59 || v[5] > 59) return false;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d:%02d:%02d %02d:%02d:%02d", v[0], v[1], v[2], v[3], v[4], v[5]);
  exif_out = buf;
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", v[0], v[1], v[2], v[3], v[4], v[5]);
  xmp_out = buf;
  return true;
}

result<write_outcome> revert(std::string_view utf8_path, std::string_view snapshot_dir) {
  if (snapshot_dir.empty()) return err(status::io);
  // PR 29: every tag, when this session's snapshot holds them.
  if (blob_snapshot b; load_blob_snapshot(blob_file(snapshot_dir, utf8_path), b)) {
    return revert_blobs(utf8_path, b);
  }
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
