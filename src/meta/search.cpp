// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/search.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/fold.h"

namespace mv::meta {
namespace {

bool space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  while (b < e && space(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && space(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

// Exiv2 prints an XMP LangAlt as `lang="x-default" Title`, several of them
// joined by ", ". Keep the text of the first.
std::string strip_lang(std::string_view v) {
  std::string s = trim(v);
  if (s.rfind("lang=", 0) != 0) return s;
  std::size_t at = 5;
  if (at < s.size() && s[at] == '"') {
    const std::size_t close = s.find('"', at + 1);
    if (close == std::string::npos) return {};
    at = close + 1;
  } else {
    while (at < s.size() && !space(static_cast<unsigned char>(s[at]))) ++at;
  }
  s = trim(std::string_view(s).substr(at));
  if (const std::size_t next = s.find(", lang="); next != std::string::npos) s.resize(next);
  return s;
}

// Text that is not worth a field: empty, or a placeholder cameras write.
bool junk(const std::string& s) {
  if (s.empty()) return true;
  if (s.find_first_not_of(" ?.-_0") == std::string::npos) return true;
  static constexpr const char* kPlaceholders[] = {"OLYMPUS DIGITAL CAMERA", "DIGITAL CAMERA", "SONY DSC", "default",
                                                  "Untitled", "Unknown", "charset=Ascii"};
  for (const char* p : kPlaceholders) {
    if (s == p) return true;
  }
  return false;
}

struct builder {
  search_facts& f;
  std::vector<std::string> keys;  // folded, for repeats

  void add(std::string_view value, std::size_t cap = 512) {
    std::string v = trim(value);
    if (v.size() > cap) {
      std::size_t cut = cap;
      while (cut > 0 && (static_cast<unsigned char>(v[cut]) & 0xC0) == 0x80) --cut;  // a whole code point
      v.resize(cut);
    }
    if (junk(v)) return;
    for (char& c : v) {
      if (c == kFieldSeparator || c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    std::string key = text::fold(v);
    // A repeat, or already inside a field shown earlier ("Canon" in "Canon EOS R5").
    for (const std::string& k : keys) {
      if (k.find(key) != std::string::npos) return;
    }
    keys.push_back(std::move(key));
    f.fields.push_back(std::move(v));
  }
  // An XMP bag / IPTC repeat printed as "a, b, c".
  void add_list(std::string_view value) {
    std::string_view s = value;
    while (!s.empty()) {
      const std::size_t comma = s.find(',');
      add(strip_lang(s.substr(0, comma)), 128);
      if (comma == std::string_view::npos) break;
      s.remove_prefix(comma + 1);
    }
  }
};

const property* find_tag(const metadata& m, std::string_view raw_tag) {
  for (const property& p : m.properties) {
    if (p.raw_tag == raw_tag) return &p;
  }
  return nullptr;
}

// The raw (untranslated) value, else the shown one.
std::string value_of(const property& p) { return p.raw.empty() ? p.value : p.raw; }

// A decimal number at the start of `s` (after spaces), or nullopt; `used`
// is how many bytes it took.
std::optional<double> leading_number(std::string_view s, std::size_t& used) {
  std::size_t i = 0;
  while (i < s.size() && space(static_cast<unsigned char>(s[i]))) ++i;
  const std::size_t b = i;
  bool digits = false;
  while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) {
    digits = digits || s[i] != '.';
    ++i;
  }
  if (!digits) return std::nullopt;
  const std::string n(s.substr(b, i - b));
  char* end = nullptr;
  const double v = std::strtod(n.c_str(), &end);
  if (!end || end == n.c_str() || !std::isfinite(v)) return std::nullopt;
  used = i;
  return v;
}

std::int32_t clamp_i32(double v) {
  if (!(v > 0)) return 0;
  return static_cast<std::int32_t>(std::min(v, 2.0e9));
}

}  // namespace

std::int32_t iso_number(std::string_view printed) {
  std::size_t at = 0;
  while (at < printed.size() && !(printed[at] >= '0' && printed[at] <= '9')) ++at;
  std::size_t used = 0;
  const auto v = leading_number(printed.substr(at), used);
  return v ? clamp_i32(std::round(*v)) : 0;
}

std::int32_t aperture_x100(std::string_view printed) {
  std::string_view s = printed;
  if (const std::size_t slash = s.find('/'); slash != std::string_view::npos) s.remove_prefix(slash + 1);
  std::size_t used = 0;
  const auto v = leading_number(s, used);
  return v ? clamp_i32(std::round(*v * 100.0)) : 0;
}

std::int32_t focal_mm(std::string_view printed) {
  std::size_t used = 0;
  const auto v = leading_number(printed, used);
  return v ? clamp_i32(std::floor(*v + 0.5)) : 0;
}

std::int64_t shutter_us(std::string_view printed, std::int32_t* den) {
  if (den) *den = 0;
  std::size_t used = 0;
  const auto a = leading_number(printed, used);
  if (!a || *a <= 0) return 0;
  double seconds = *a;
  std::string_view rest = printed.substr(used);
  std::size_t i = 0;
  while (i < rest.size() && space(static_cast<unsigned char>(rest[i]))) ++i;
  if (i < rest.size() && rest[i] == '/') {
    std::size_t used_b = 0;
    const auto b = leading_number(rest.substr(i + 1), used_b);
    if (!b || *b <= 0) return 0;
    seconds = *a / *b;
  }
  if (!(seconds > 0) || seconds > 100000.0) return 0;
  // A unit fraction: 1/250, also written 0.004 or 10/2500.
  if (den && seconds < 1.0) {
    const double n = 1.0 / seconds;
    const double r = std::round(n);
    if (r >= 2 && std::fabs(n - r) <= r * 0.01) *den = clamp_i32(r);
  }
  return static_cast<std::int64_t>(std::llround(seconds * 1e6));
}

search_facts search_facts_of(const metadata& m) {
  search_facts f;
  f.camera = m.s.camera;
  f.lens = m.s.lens;
  f.made_with = m.s.made_with;
  f.ai_generated = m.s.ai_generated;
  f.date_taken_key = m.s.date_taken_key;
  f.where = m.s.where;
  f.rating = std::clamp(m.s.rating, -1, 5);
  f.label = trim(m.s.label);
  f.iso = iso_number(m.s.iso);
  f.aperture_x100 = aperture_x100(m.s.aperture);
  f.focal_mm = focal_mm(m.s.focal_length);
  f.shutter_us = shutter_us(m.s.exposure, &f.shutter_den);

  builder b{f, {}};
  b.add(m.s.camera);
  // A make the camera line did not already carry ("NIKON CORPORATION").
  for (const char* t : {"Exif.Image.Make", "Container.com.apple.quicktime.make"}) {
    if (const property* p = find_tag(m, t)) b.add(value_of(*p));
  }
  b.add(m.s.lens);
  b.add(m.s.made_with);
  for (const char* t : {"Exif.Image.Software", "Xmp.xmp.CreatorTool", "Container.com.apple.quicktime.software"}) {
    if (const property* p = find_tag(m, t)) b.add(value_of(*p));
  }
  for (const char* t : {"Xmp.dc.title", "Iptc.Application2.ObjectName", "Xmp.photoshop.Headline",
                        "Iptc.Application2.Headline", "Container.title", "Container.com.apple.quicktime.title"}) {
    if (const property* p = find_tag(m, t)) b.add(strip_lang(value_of(*p)));
  }
  for (const char* t : {"Xmp.dc.description", "Iptc.Application2.Caption", "Exif.Image.ImageDescription",
                        "Container.description", "Container.comment", "Container.com.apple.quicktime.description"}) {
    if (const property* p = find_tag(m, t)) b.add(strip_lang(value_of(*p)));
  }
  b.add(m.s.comment);
  // Keywords: an XMP bag prints as one row "a, b"; IPTC repeats the tag.
  for (const property& p : m.properties) {
    if (p.raw_tag == "Xmp.dc.subject" || p.raw_tag == "Container.keywords" ||
        p.raw_tag == "Container.com.apple.quicktime.keywords") {
      b.add_list(value_of(p));
    } else if (p.raw_tag == "Iptc.Application2.Keywords") {
      b.add(value_of(p), 128);
    }
  }
  for (const char* t : {"Exif.Image.Artist", "Xmp.dc.creator", "Iptc.Application2.Byline", "Container.artist",
                        "Container.com.apple.quicktime.author", "Container.com.apple.quicktime.artist"}) {
    if (const property* p = find_tag(m, t)) b.add_list(value_of(*p));
  }
  return f;
}

std::string folded_text(std::span<const std::string> fields) {
  std::string out;
  std::size_t n = 0;
  for (const std::string& s : fields) n += s.size() + 1;
  out.reserve(n);
  for (std::size_t i = 0; i < fields.size(); ++i) {
    if (i) out.push_back(kFieldSeparator);
    text::fold_into(out, fields[i]);
  }
  return out;
}

std::vector<std::string> search_words(std::string_view typed) {
  std::vector<std::string> out;
  const std::string folded = text::fold(typed);
  // The separator splits too: a word never spans two fields.
  const auto gap = [](char c) { return c == kFieldSeparator || space(static_cast<unsigned char>(c)); };
  std::size_t i = 0;
  while (i < folded.size()) {
    while (i < folded.size() && gap(folded[i])) ++i;
    std::size_t j = i;
    while (j < folded.size() && !gap(folded[j])) ++j;
    if (j > i) out.emplace_back(folded.substr(i, j - i));
    i = j;
  }
  return out;
}

int match_fields(std::string_view folded, std::span<const std::string> words) noexcept {
  if (words.empty() || folded.empty()) return -1;
  int first = -1;
  for (std::size_t w = 0; w < words.size(); ++w) {
    const std::string& word = words[w];
    std::size_t at = folded.find(word);
    // A hit that spans a separator cannot happen: words hold no separator.
    if (at == std::string_view::npos) return -1;
    if (w == 0) first = static_cast<int>(std::count(folded.begin(), folded.begin() + static_cast<std::ptrdiff_t>(at),
                                                    kFieldSeparator));
  }
  return first;
}

std::string why_of(std::span<const std::string> fields, int i, std::size_t max_chars) {
  if (i < 0 || static_cast<std::size_t>(i) >= fields.size()) return {};
  const std::string& s = fields[static_cast<std::size_t>(i)];
  std::size_t chars = 0;
  for (std::size_t b = 0; b < s.size(); ++b) {
    if ((static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) continue;
    if (chars == max_chars) return s.substr(0, b) + "\xE2\x80\xA6";
    ++chars;
  }
  return s;
}

}  // namespace mv::meta
