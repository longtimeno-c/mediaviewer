// SPDX-License-Identifier: GPL-2.0-or-later
// PR 12: the three fields the writer owns, read back out of Exiv2's parsed
// EXIF / XMP. Shared by the read model (the summary card), the writer (the
// snapshot of what was there) and the sidecar overlay, so all three agree on
// what a file "says".
//
// Exiv2 reports failure by throwing; that stops here.
#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "meta/internal.h"

namespace mv::meta::detail {
namespace {

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

std::string trim_ws_nul(std::string s) {
  const auto junk = [](char c) { return c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (!s.empty() && junk(s.back())) s.pop_back();
  std::size_t lead = 0;
  while (lead < s.size() && junk(s[lead])) ++lead;
  s.erase(0, lead);
  return s;
}

// XMP writes ratings as text ("3", "-1", sometimes "3.0" from other tools).
std::optional<int> parse_rating_text(const std::string& text) {
  if (text.empty()) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) return std::nullopt;
  const long r = std::lround(v);
  if (r < -1 || r > 5) return std::nullopt;
  return static_cast<int>(r);
}

// Windows' RatingPercent buckets: 1, 25, 50, 75, 99 are 1..5 stars.
int stars_of_percent(long p) {
  if (p <= 0) return 0;
  if (p <= 12) return 1;
  if (p <= 37) return 2;
  if (p <= 62) return 3;
  if (p <= 87) return 4;
  return 5;
}

std::optional<std::string> exif_comment(const Exiv2::ExifData& e, comment_order order) {
  const auto* d = find(e, "Exif.Photo.UserComment");
  if (!d) return std::nullopt;
  try {
    std::vector<std::uint8_t> raw(d->size());
    if (!raw.empty()) d->copy(reinterpret_cast<Exiv2::byte*>(raw.data()), Exiv2::littleEndian);
    std::string text = trim_ws_nul(decode_user_comment(raw, order));
    if (text.empty()) return std::nullopt;
    return text;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::string> xmp_comment(const Exiv2::XmpData& x) {
  const auto* d = find(x, "Xmp.exif.UserComment");
  if (!d) return std::nullopt;
  try {
    std::string text;
    if (d->typeId() == Exiv2::langAlt) {
      const auto& la = static_cast<const Exiv2::LangAltValue&>(d->value());
      auto it = la.value_.find("x-default");
      if (it == la.value_.end()) it = la.value_.begin();
      if (it != la.value_.end()) text = it->second;
    } else {
      text = d->toString();
    }
    text = trim_ws_nul(sanitise_utf8(std::move(text)));
    if (text.empty()) return std::nullopt;
    return text;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

std::string sanitise_utf8(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
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


namespace {

void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}  // namespace

comment_order comment_order_of(const Exiv2::Image& image) noexcept {
  switch (image.byteOrder()) {
    case Exiv2::littleEndian: return comment_order::little;
    case Exiv2::bigEndian: return comment_order::big;
    default: return comment_order::unknown;
  }
}

std::string decode_user_comment(std::span<const std::uint8_t> raw, comment_order order) {
  if (raw.size() < 8) return {};
  const std::string code(reinterpret_cast<const char*>(raw.data()), 8);
  const std::span<const std::uint8_t> text = raw.subspan(8);
  if (code == std::string("ASCII\0\0\0", 8)) {
    return sanitise_utf8(std::string(reinterpret_cast<const char*>(text.data()), text.size()));
  }
  if (code == std::string("UNICODE\0", 8)) {
    if (order == comment_order::unknown && text.size() >= 2) {
      // ASCII-range text has one zero byte per unit: which one says the order.
      if (text[1] == 0 && text[0] != 0) order = comment_order::little;
      else if (text[0] == 0 && text[1] != 0) order = comment_order::big;
    }
    if (order == comment_order::unknown) order = comment_order::big;  // the format's default
    std::string out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
      char32_t u = order == comment_order::little ? static_cast<char32_t>(text[i] | (text[i + 1] << 8))
                                                  : static_cast<char32_t>((text[i] << 8) | text[i + 1]);
      if (u == 0) break;
      if (u >= 0xD800 && u <= 0xDBFF && i + 3 < text.size()) {
        const char32_t lo = order == comment_order::little ? static_cast<char32_t>(text[i + 2] | (text[i + 3] << 8))
                                                           : static_cast<char32_t>((text[i + 2] << 8) | text[i + 3]);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      if (u >= 0xD800 && u <= 0xDFFF) u = '?';  // an unpaired surrogate
      append_utf8(out, u);
    }
    return sanitise_utf8(std::move(out));
  }
  if (code == std::string(8, '\0')) {
    // "Undefined": in practice UTF-8 or Latin-1 that a tool did not label.
    return sanitise_utf8(std::string(reinterpret_cast<const char*>(text.data()), text.size()));
  }
  return {};  // JIS and anything else: not decodable here
}

std::vector<std::uint8_t> encode_user_comment(const std::string& utf8, comment_order order) {
  std::vector<std::uint8_t> out;
  const bool ascii = std::all_of(utf8.begin(), utf8.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
  if (ascii) {
    const char code[8] = {'A', 'S', 'C', 'I', 'I', 0, 0, 0};
    out.assign(code, code + 8);
    out.insert(out.end(), utf8.begin(), utf8.end());
    return out;
  }
  const char code[8] = {'U', 'N', 'I', 'C', 'O', 'D', 'E', 0};
  out.assign(code, code + 8);
  const auto put = [&](std::uint32_t unit) {
    const auto hi = static_cast<std::uint8_t>(unit >> 8), lo = static_cast<std::uint8_t>(unit & 0xFF);
    if (order == comment_order::little) { out.push_back(lo); out.push_back(hi); }
    else { out.push_back(hi); out.push_back(lo); }
  };
  for (std::size_t i = 0; i < utf8.size();) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
    for (std::size_t k = 1; k < len && i + k < utf8.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put(0xD800 + static_cast<std::uint32_t>(cp >> 10));
      put(0xDC00 + static_cast<std::uint32_t>(cp & 0x3FF));
    } else {
      put(static_cast<std::uint32_t>(cp));
    }
  }
  return out;
}

field_state read_fields(const Exiv2::ExifData& exif, const Exiv2::XmpData& xmp,
                        comment_order order) noexcept {
  field_state f;
  try {
    if (const auto* d = find(xmp, "Xmp.xmp.Rating")) f.rating = parse_rating_text(d->toString());
    if (!f.rating) {
      if (const auto* d = find(exif, "Exif.Image.Rating"); d && d->count() > 0) {
        const long v = static_cast<long>(d->toInt64(0));
        if (v >= 0 && v <= 5) f.rating = static_cast<int>(v);
      }
    }
    if (!f.rating) {
      if (const auto* d = find(exif, "Exif.Image.RatingPercent"); d && d->count() > 0) {
        f.rating = stars_of_percent(static_cast<long>(d->toInt64(0)));
      }
    }

    if (const auto* d = find(exif, "Exif.Image.Orientation"); d && d->count() > 0) {
      const auto v = d->toInt64(0);
      if (v >= 1 && v <= 8) f.orientation = static_cast<int>(v);
    }
    if (!f.orientation) {
      if (const auto* d = find(xmp, "Xmp.tiff.Orientation")) {
        const auto v = parse_rating_text(d->toString());  // a small integer, same parse
        if (v && *v >= 1 && *v <= 8) f.orientation = *v;
      }
    }

    f.comment = exif_comment(exif, order);
    if (!f.comment) f.comment = xmp_comment(xmp);
  } catch (...) {
    // Whatever was read before the damaged tag stands.
  }
  return f;
}

}  // namespace mv::meta::detail
