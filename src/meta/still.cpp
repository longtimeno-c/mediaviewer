// SPDX-License-Identifier: GPL-2.0-or-later
// Still-image metadata through Exiv2: EXIF (with maker notes), IPTC and XMP.
//
// Exiv2 reports failure by throwing. That is confined to this file: every
// entry point catches at the boundary and turns it into "fewer fields"
// (CLAUDE.md: no exceptions across the line; plan/06: missing or damaged
// metadata is an empty field, never an error).
#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

#include "meta/af.h"
#include "meta/internal.h"

namespace mv::meta::detail {
namespace {

constexpr std::size_t kMaxValueChars = 256;
constexpr std::size_t kMaxProperties = 4000;  // a hostile file must not make a 100 MB tree

std::string cap(std::string s) {
  if (s.size() <= kMaxValueChars) return s;
  std::size_t cut = kMaxValueChars;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;  // UTF-8 boundary
  s.resize(cut);
  s += "\xE2\x80\xA6";  // …
  return s;
}

std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

// Exiv2 hands back arbitrary bytes for ASCII tags; only valid UTF-8 may reach
// the UI (Swift's String(cString:) would replace, C#'s would too, but the
// tree search should not see the mess).
std::string sanitise(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    bool ok = len != 0 && i + len <= s.size();
    for (std::size_t k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    if (c < 0x20 && c != '\t' && c != '\n') ok = false;
    if (ok) {
      out.append(s, i, len);
      i += len;
    } else {
      out += '?';
      ++i;
    }
  }
  return out;
}

// Every Exiv2 datum type prints through the EXIF table (a few Exif print
// functions read sibling tags), so the context is always the ExifData.
template <typename Datum>
void add_property(metadata& out, origin space, const Datum& d, const Exiv2::ExifData* data) {
  if (out.properties.size() >= kMaxProperties) return;
  property p;
  p.space = space;
  p.raw_tag = d.key();
  p.name = d.tagName();
  const std::string label = d.tagLabel();
  p.label = label.empty() ? p.name : label;
  p.group = std::string(space == origin::exif ? "Exif." : space == origin::iptc ? "Iptc." : "Xmp.") +
            d.groupName();
  p.raw = sanitise(cap(trim(d.toString())));
  std::string shown;
  try {
    shown = d.print(data);
  } catch (...) {
    shown.clear();
  }
  p.value = shown.empty() ? p.raw : sanitise(cap(trim(std::move(shown))));
  out.properties.push_back(std::move(p));
}

// A key Exiv2 does not know (an XMP namespace nobody registered) throws from
// the key constructor. That must cost one lookup, not the rest of the summary.
const Exiv2::Exifdatum* find(const Exiv2::ExifData& e, const char* key) {
  try {
    const auto it = e.findKey(Exiv2::ExifKey(key));
    return it == e.end() ? nullptr : &*it;
  } catch (...) {
    return nullptr;
  }
}
const Exiv2::Xmpdatum* find(const Exiv2::XmpData& x, const char* key) {
  try {
    const auto it = x.findKey(Exiv2::XmpKey(key));
    return it == x.end() ? nullptr : &*it;
  } catch (...) {
    return nullptr;
  }
}

std::string print_of(const Exiv2::ExifData& e, const char* key) {
  const auto* d = find(e, key);
  if (!d) return {};
  try {
    return sanitise(trim(d->print(&e)));
  } catch (...) {
    return {};
  }
}

std::int64_t int_of(const Exiv2::ExifData& e, const char* key, std::int64_t fallback) {
  const auto* d = find(e, key);
  if (!d || d->count() == 0) return fallback;
  try {
    return d->toInt64(0);
  } catch (...) {
    return fallback;
  }
}

std::vector<std::int64_t> ints_of(const Exiv2::ExifData& e, const char* key) {
  std::vector<std::int64_t> v;
  const auto* d = find(e, key);
  if (!d) return v;
  const std::size_t n = std::min<std::size_t>(d->count(), 4096);
  v.reserve(n);
  try {
    for (std::size_t i = 0; i < n; ++i) v.push_back(d->toInt64(i));
  } catch (...) {
    v.clear();
  }
  return v;
}

std::string ascii_of(const Exiv2::ExifData& e, const char* key) {
  const auto* d = find(e, key);
  if (!d) return {};
  try {
    return sanitise(trim(d->toString()));
  } catch (...) {
    return {};
  }
}

bool starts_with_nocase(const std::string& s, const std::string& prefix) {
  if (prefix.empty() || s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

// "2024:05:01 14:03:22" / "2024-05-01T14:03:22+02:00" → "2024-05-01 14:03:22".
std::string pretty_date(const std::string& stamp) {
  if (stamp.size() < 10) return stamp;
  std::string s = stamp.substr(0, std::min<std::size_t>(stamp.size(), 19));
  s[4] = '-';
  s[7] = '-';
  if (s.size() > 10 && s[10] == 'T') s[10] = ' ';
  return s;
}

// The date-taken candidates, most specific first.
std::string date_stamp(const Exiv2::ExifData& e, const Exiv2::XmpData& x) {
  for (const char* key : {"Exif.Photo.DateTimeOriginal", "Exif.Photo.DateTimeDigitized",
                          "Exif.Image.DateTime"}) {
    std::string v = ascii_of(e, key);
    if (parse_date_key(v)) return v;
  }
  for (const char* key : {"Xmp.exif.DateTimeOriginal", "Xmp.photoshop.DateCreated",
                          "Xmp.xmp.CreateDate"}) {
    if (const auto* d = find(x, key)) {
      try {
        std::string v = d->toString();
        if (parse_date_key(v)) return v;
      } catch (...) {
      }
    }
  }
  return {};
}

double rational_at(const Exiv2::Exifdatum& d, std::size_t i, bool& ok) {
  const Exiv2::Rational r = d.toRational(i);
  if (r.second == 0) {
    ok = false;
    return 0;
  }
  return static_cast<double>(r.first) / static_cast<double>(r.second);
}

std::string gps_of(const Exiv2::ExifData& e) {
  const auto* lat = find(e, "Exif.GPSInfo.GPSLatitude");
  const auto* lon = find(e, "Exif.GPSInfo.GPSLongitude");
  if (!lat || !lon || lat->count() < 3 || lon->count() < 3) return {};
  try {
    bool ok = true;
    const double la = rational_at(*lat, 0, ok) + rational_at(*lat, 1, ok) / 60 +
                      rational_at(*lat, 2, ok) / 3600;
    const double lo = rational_at(*lon, 0, ok) + rational_at(*lon, 1, ok) / 60 +
                      rational_at(*lon, 2, ok) / 3600;
    if (!ok || la > 90.0 || lo > 180.0) return {};
    const std::string lar = ascii_of(e, "Exif.GPSInfo.GPSLatitudeRef");
    const std::string lor = ascii_of(e, "Exif.GPSInfo.GPSLongitudeRef");
    return format_coordinate(la, lar == "S" ? 'S' : 'N') + ", " +
           format_coordinate(lo, lor == "W" ? 'W' : 'E');
  } catch (...) {
    return {};
  }
}

std::string number(double v, const char* unit) {
  char buf[48];
  if (std::fabs(v - std::round(v)) < 0.05) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", v, unit);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", v, unit);
  }
  return buf;
}

void fill_summary(const Exiv2::ExifData& e, const Exiv2::XmpData& x, Exiv2::Image& image,
                  metadata& out) {
  summary& s = out.s;
  s.width = image.pixelWidth();
  s.height = image.pixelHeight();
  if (s.width == 0 || s.height == 0) {
    s.width = static_cast<std::uint32_t>(std::max<std::int64_t>(0, int_of(e, "Exif.Photo.PixelXDimension", 0)));
    s.height = static_cast<std::uint32_t>(std::max<std::int64_t>(0, int_of(e, "Exif.Photo.PixelYDimension", 0)));
  }

  const std::string make = ascii_of(e, "Exif.Image.Make");
  const std::string model = ascii_of(e, "Exif.Image.Model");
  s.camera = starts_with_nocase(model, make) || make.empty() ? model
             : model.empty()                                 ? make
                                                             : make + " " + model;

  s.lens = ascii_of(e, "Exif.Photo.LensModel");
  if (s.lens.empty()) s.lens = print_of(e, "Exif.CanonCs.LensType");
  if (s.lens.empty()) s.lens = print_of(e, "Exif.Photo.LensSpecification");

  s.exposure = print_of(e, "Exif.Photo.ExposureTime");
  if (const auto* f = find(e, "Exif.Photo.FNumber"); f && f->count() > 0) {
    try {
      const float v = f->toFloat(0);
      if (v > 0) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "f/%.1f", static_cast<double>(v));
        s.aperture = buf;
      }
    } catch (...) {
    }
  }
  std::int64_t iso = int_of(e, "Exif.Photo.ISOSpeedRatings", 0);
  if (iso <= 0) iso = int_of(e, "Exif.Photo.PhotographicSensitivity", 0);
  if (iso > 0) s.iso = "ISO " + std::to_string(iso);
  if (const auto* f = find(e, "Exif.Photo.FocalLength"); f && f->count() > 0) {
    try {
      const float v = f->toFloat(0);
      if (v > 0) s.focal_length = number(static_cast<double>(v), "mm");
    } catch (...) {
    }
  }

  const std::string stamp = date_stamp(e, x);
  if (!stamp.empty()) {
    s.date_taken = pretty_date(stamp);
    s.date_taken_key = parse_date_key(stamp).value_or(0);
  }
  s.gps = gps_of(e);

  s.colour_space = print_of(e, "Exif.Photo.ColorSpace");
  if (s.colour_space.empty()) {
    try {
      if (image.iccProfileDefined()) s.colour_space = "Embedded ICC profile";
    } catch (...) {
    }
  }
  const std::int64_t o = int_of(e, "Exif.Image.Orientation", 1);
  s.orientation = (o >= 1 && o <= 8) ? static_cast<std::uint8_t>(o) : 1;
}

// SubjectArea is the one standard AF-ish tag (EXIF 2.3); vendor notes below are
// preferred when present because they carry the real focus points.
std::vector<af_point> find_af_points(const Exiv2::ExifData& e, const summary& s) {
  // Canon AFInfo2, split by Exiv2 into Exif.Canon.AF* pseudo-tags.
  const std::int64_t canon_n = int_of(e, "Exif.Canon.AFNumPoints", 0);
  if (canon_n > 0) {
    const auto gw = static_cast<std::uint32_t>(std::max<std::int64_t>(0, int_of(e, "Exif.Canon.AFImageWidth", 0)));
    const auto gh = static_cast<std::uint32_t>(std::max<std::int64_t>(0, int_of(e, "Exif.Canon.AFImageHeight", 0)));
    auto pts = canon_af_points(gw, gh, ints_of(e, "Exif.Canon.AFAreaWidths"),
                               ints_of(e, "Exif.Canon.AFAreaHeights"),
                               ints_of(e, "Exif.Canon.AFXPositions"),
                               ints_of(e, "Exif.Canon.AFYPositions"),
                               ints_of(e, "Exif.Canon.AFPointsInFocus"),
                               ints_of(e, "Exif.Canon.AFPointsSelected"));
    if (!pts.empty()) return pts;
  }

  // Nikon AFInfo2: one area, centre + size, on the AF grid it names.
  for (const char* g : {"NikonAf22", "NikonAf2"}) {
    const std::string p = std::string("Exif.") + g + ".";
    const std::int64_t gw = int_of(e, (p + "AFImageWidth").c_str(), 0);
    const std::int64_t gh = int_of(e, (p + "AFImageHeight").c_str(), 0);
    if (gw <= 0 || gh <= 0) continue;
    auto pts = centre_af_point(static_cast<double>(int_of(e, (p + "AFAreaXPosition").c_str(), 0)),
                               static_cast<double>(int_of(e, (p + "AFAreaYPosition").c_str(), 0)),
                               static_cast<double>(int_of(e, (p + "AFAreaWidth").c_str(), 0)),
                               static_cast<double>(int_of(e, (p + "AFAreaHeight").c_str(), 0)),
                               static_cast<std::uint32_t>(gw), static_cast<std::uint32_t>(gh), true);
    if (!pts.empty()) return pts;
  }

  // Sony FocusLocation: [grid width, grid height, x, y].
  for (const char* key : {"Exif.Sony1.FocusLocation", "Exif.Sony2.FocusLocation"}) {
    const auto v = ints_of(e, key);
    if (v.size() >= 4 && v[0] > 0 && v[1] > 0) {
      auto pts = centre_af_point(static_cast<double>(v[2]), static_cast<double>(v[3]), 0, 0,
                                 static_cast<std::uint32_t>(v[0]), static_cast<std::uint32_t>(v[1]), true);
      if (!pts.empty()) return pts;
    }
  }

  // Fujifilm FocusPoint: [x, y] on the EXIF image grid.
  if (const auto v = ints_of(e, "Exif.Fujifilm.FocusPoint"); v.size() >= 2 && s.width && s.height) {
    auto pts = centre_af_point(static_cast<double>(v[0]), static_cast<double>(v[1]), 0, 0,
                               s.width, s.height, true);
    if (!pts.empty()) return pts;
  }

  // EXIF SubjectArea: 2 = centre, 3 = circle (x,y,d), 4 = rectangle (x,y,w,h).
  const auto area = ints_of(e, "Exif.Photo.SubjectArea");
  if (area.size() >= 2 && s.width && s.height) {
    const double w = area.size() >= 3 ? static_cast<double>(area[2]) : 0;
    const double h = area.size() >= 4 ? static_cast<double>(area[3]) : w;
    return centre_af_point(static_cast<double>(area[0]), static_cast<double>(area[1]), w, h,
                           s.width, s.height, true);
  }
  return {};
}

}  // namespace

std::unique_ptr<Exiv2::Image> open_image(std::span<const std::uint8_t> bytes) {
  // Exiv2 initialises its XMP parser lazily and that is not safe to race; the
  // metadata jobs run on the pool, so do it exactly once, up front. Never
  // terminated: the process owns it until exit.
  static std::once_flag once;
  std::call_once(once, [] {
    Exiv2::XmpParser::initialize();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);  // its messages can name paths (rule 6)
  });
  auto image = Exiv2::ImageFactory::open(bytes.data(), bytes.size());
  if (!image) return nullptr;
  image->readMetadata();
  return image;
}

namespace {
std::unique_ptr<Exiv2::Image> open(std::span<const std::uint8_t> bytes) {
  return open_image(bytes);
}
}  // namespace

void read_still(std::span<const std::uint8_t> bytes, bool decoder_orients, metadata& out) noexcept {
  try {
    auto image = open(bytes);
    if (!image) return;
    const Exiv2::ExifData& exif = image->exifData();
    const Exiv2::XmpData& xmp = image->xmpData();
    const Exiv2::IptcData& iptc = image->iptcData();

    for (const auto& d : exif) add_property(out, origin::exif, d, &exif);
    for (const auto& d : iptc) add_property(out, origin::iptc, d, &exif);
    for (const auto& d : xmp) add_property(out, origin::xmp, d, &exif);

    fill_summary(exif, xmp, *image, out);
    out.display_orientation = decoder_orients ? out.s.orientation : 1;
    out.af_points = find_af_points(exif, out.s);
  } catch (...) {
    // A damaged tag table: keep whatever was read before it.
  }
}

std::optional<std::int64_t> still_date_key(std::span<const std::uint8_t> bytes) noexcept {
  try {
    auto image = open(bytes);
    if (!image) return std::nullopt;
    const std::string stamp = date_stamp(image->exifData(), image->xmpData());
    return parse_date_key(stamp);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace mv::meta::detail
