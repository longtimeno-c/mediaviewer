// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include <hb.h>
#include <hb-ot.h>

#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "codec/fonts.h"
#include "io/file.h"

namespace mv::codec::text {
namespace {

// Font files are read through io/ (UTF-8 paths on every platform) and kept for
// a while: every page turn of a document opens the same few. Eight files is
// a document's body, heading and fallback faces with room to spare.
constexpr std::size_t kFontCacheFiles = 8;

std::shared_ptr<const std::vector<std::uint8_t>> font_bytes(const std::string& path) {
  static std::mutex mutex;
  static std::list<std::pair<std::string, std::shared_ptr<const std::vector<std::uint8_t>>>> cache;
  {
    std::lock_guard lock(mutex);
    for (auto it = cache.begin(); it != cache.end(); ++it) {
      if (it->first == path) {
        cache.splice(cache.begin(), cache, it);
        return it->second;
      }
    }
  }
  auto read = io::read_all(path);  // outside the lock: this is the I/O
  if (!read) return nullptr;
  auto bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(read).value());
  std::lock_guard lock(mutex);
  cache.emplace_front(path, bytes);
  while (cache.size() > kFontCacheFiles) cache.pop_back();
  return bytes;
}

// Families Office names that are often not installed, and the metric-
// compatible or nearest family to try next. The family itself is always
// tried first.
std::vector<std::string_view> substitutes(std::string_view family) {
  struct sub {
    std::string_view from;
    std::string_view to[3];
  };
  static constexpr sub kSubs[] = {
      {"Calibri", {"Carlito", "Helvetica Neue", "Arial"}},
      {"Calibri Light", {"Calibri", "Carlito", "Helvetica Neue"}},
      {"Aptos", {"Calibri", "Helvetica Neue", "Arial"}},
      {"Cambria", {"Caladea", "Georgia", "Times New Roman"}},
      {"Times New Roman", {"Times", "Liberation Serif", "Georgia"}},
      {"Arial", {"Helvetica", "Liberation Sans", "Helvetica Neue"}},
      {"Courier New", {"Courier", "Menlo", "Consolas"}},
      {"Consolas", {"Menlo", "Courier New", "Courier"}},
      {"Segoe UI", {"Helvetica Neue", "Arial", "Helvetica"}},
  };
  std::vector<std::string_view> out{family};
  for (const sub& s : kSubs) {
    if (s.from != family) continue;
    for (std::string_view t : s.to) out.push_back(t);
  }
  return out;
}

// Next UTF-8 code point; invalid bytes read as U+FFFD one at a time.
std::uint32_t next_cp(std::string_view s, std::size_t& i) noexcept {
  const auto b = static_cast<unsigned char>(s[i]);
  int extra = 0;
  std::uint32_t cp = 0;
  if (b < 0x80) {
    ++i;
    return b;
  } else if ((b & 0xE0) == 0xC0) {
    extra = 1;
    cp = b & 0x1F;
  } else if ((b & 0xF0) == 0xE0) {
    extra = 2;
    cp = b & 0x0F;
  } else if ((b & 0xF8) == 0xF0) {
    extra = 3;
    cp = b & 0x07;
  } else {
    ++i;
    return 0xFFFD;
  }
  if (i + static_cast<std::size_t>(extra) >= s.size()) {  // truncated sequence
    ++i;
    return 0xFFFD;
  }
  for (int k = 1; k <= extra; ++k) {
    const auto c = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
    if ((c & 0xC0) != 0x80) {
      ++i;
      return 0xFFFD;
    }
    cp = (cp << 6) | (c & 0x3F);
  }
  i += static_cast<std::size_t>(extra) + 1;
  return cp;
}

bool needs_glyph(std::uint32_t cp) noexcept {
  return cp > 0x20 && cp != 0x7F && cp != 0xA0 && !(cp >= 0x200B && cp <= 0x200F) && cp != 0xFEFF;
}

}  // namespace

struct engine::impl {
  struct font {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    hb_blob_t* blob = nullptr;
    hb_face_t* face = nullptr;
    hb_font_t* shaper = nullptr;  // not `font`: MSVC reads that as the struct's constructor
    FT_Face ft = nullptr;
    double upem = 1000.0;
    double ascent = 800.0, descent = 200.0, gap = 0.0;  // font units
    bool bold = false, italic = false;
  };

  FT_Library lib = nullptr;
  bool lib_failed = false;
  std::vector<font> fonts;
  std::unordered_map<std::string, int> by_file;    // "path#index"
  std::unordered_map<std::string, int> by_family;  // "family|b|i" -> slot or -1

  ~impl() {
    for (font& f : fonts) {
      if (f.shaper) hb_font_destroy(f.shaper);
      if (f.face) hb_face_destroy(f.face);
      if (f.blob) hb_blob_destroy(f.blob);
      if (f.ft) FT_Done_Face(f.ft);
    }
    if (lib) FT_Done_FreeType(lib);
  }

  bool library() {
    if (lib) return true;
    if (lib_failed) return false;
    if (FT_Init_FreeType(&lib) != 0) {
      lib = nullptr;
      lib_failed = true;
    }
    return lib != nullptr;
  }

  int open_file(const fonts::face_ref& ref) {
    if (!library()) return -1;
    auto bytes = font_bytes(ref.path);
    if (!bytes || bytes->empty()) return -1;
    const auto* data = reinterpret_cast<const FT_Byte*>(bytes->data());
    const auto size = static_cast<FT_Long>(bytes->size());

    // A collection: find the face by PostScript name when the OS gave one.
    FT_Long index = static_cast<FT_Long>(ref.index);
    if (!ref.postscript.empty()) {
      FT_Face probe = nullptr;
      if (FT_New_Memory_Face(lib, data, size, -1, &probe) == 0) {
        const FT_Long count = probe->num_faces;
        FT_Done_Face(probe);
        for (FT_Long i = 0; i < count && i < 64; ++i) {
          FT_Face f = nullptr;
          if (FT_New_Memory_Face(lib, data, size, i, &f) != 0) continue;
          const char* ps = FT_Get_Postscript_Name(f);
          const bool hit = ps && ref.postscript == ps;
          FT_Done_Face(f);
          if (hit) {
            index = i;
            break;
          }
        }
      }
    }
    const std::string key = ref.path + "#" + std::to_string(index);
    if (auto it = by_file.find(key); it != by_file.end()) return it->second;

    font f;
    f.bytes = bytes;
    if (FT_New_Memory_Face(lib, data, size, index, &f.ft) != 0) {
      f.ft = nullptr;
      by_file[key] = -1;
      return -1;
    }
    // HarfBuzz reads the same bytes; the blob borrows them for as long as the
    // engine holds `bytes`.
    f.blob = hb_blob_create(reinterpret_cast<const char*>(data), static_cast<unsigned>(size),
                            HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    f.face = hb_face_create(f.blob, static_cast<unsigned>(index));
    f.shaper = hb_font_create(f.face);
    f.upem = std::max(16u, hb_face_get_upem(f.face));
    hb_font_set_scale(f.shaper, static_cast<int>(f.upem), static_cast<int>(f.upem));
    hb_font_extents_t ext{};
    if (hb_font_get_h_extents(f.shaper, &ext)) {
      f.ascent = ext.ascender;
      f.descent = -ext.descender;
      f.gap = std::max<hb_position_t>(0, ext.line_gap);
    }
    if (f.ascent + f.descent <= 0) {
      f.ascent = f.upem * 0.8;
      f.descent = f.upem * 0.2;
    }
    f.bold = ref.bold || (f.ft->style_flags & FT_STYLE_FLAG_BOLD) != 0;
    f.italic = ref.italic || (f.ft->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    fonts.push_back(std::move(f));
    const int slot = static_cast<int>(fonts.size()) - 1;
    by_file[key] = slot;
    return slot;
  }

  // The face for exactly this family, or -1.
  int family_slot(std::string_view family, bool bold, bool italic) {
    std::string key(family);
    key += bold ? "|b" : "|-";
    key += italic ? "i" : "-";
    if (auto it = by_family.find(key); it != by_family.end()) return it->second;
    int slot = -1;
    if (auto ref = fonts::find(family, bold, italic)) slot = open_file(*ref);
    by_family[key] = slot;
    return slot;
  }

  // The style's family, its substitutes, then the platform fallbacks.
  int primary(const style& s) {
    for (std::string_view name : substitutes(s.family)) {
      const int slot = family_slot(name, s.bold, s.italic);
      if (slot >= 0) return slot;
    }
    for (std::string_view name : fonts::fallbacks()) {
      const int slot = family_slot(name, s.bold, s.italic);
      if (slot >= 0) return slot;
    }
    return -1;
  }

  bool covers(int slot, std::string_view utf8) {
    hb_font_t* f = fonts[static_cast<std::size_t>(slot)].shaper;
    for (std::size_t i = 0; i < utf8.size();) {
      const std::uint32_t cp = next_cp(utf8, i);
      if (!needs_glyph(cp)) continue;
      hb_codepoint_t g = 0;
      if (!hb_font_get_nominal_glyph(f, cp, &g)) return false;
    }
    return true;
  }
};

engine::engine() : d_(std::make_unique<impl>()) {}
engine::~engine() = default;

bool engine::usable() {
  return d_->primary(style{}) >= 0;
}

shaped engine::metrics(const style& s) {
  shaped out;
  out.font = d_->primary(s);
  if (out.font < 0) {
    out.ascent = s.size_pt * 0.8;
    out.descent = s.size_pt * 0.2;
    return out;
  }
  const impl::font& f = d_->fonts[static_cast<std::size_t>(out.font)];
  const double k = s.size_pt / f.upem;
  out.ascent = f.ascent * k;
  out.descent = f.descent * k;
  out.line_gap = f.gap * k;
  return out;
}

shaped engine::shape(std::string_view utf8, const style& s) {
  shaped out = metrics(s);
  if (out.font < 0) return out;
  int slot = out.font;
  if (!d_->covers(slot, utf8)) {
    for (std::string_view name : fonts::fallbacks()) {
      const int other = d_->family_slot(name, s.bold, s.italic);
      if (other >= 0 && d_->covers(other, utf8)) {
        slot = other;
        break;
      }
    }
  }
  const impl::font& f = d_->fonts[static_cast<std::size_t>(slot)];
  out.font = slot;
  const double k = s.size_pt / f.upem;
  out.ascent = std::max(out.ascent, f.ascent * k);
  out.descent = std::max(out.descent, f.descent * k);
  out.synthetic_bold = s.bold && !f.bold;
  out.synthetic_italic = s.italic && !f.italic;

  hb_buffer_t* buf = hb_buffer_create();
  hb_buffer_add_utf8(buf, utf8.data(), static_cast<int>(utf8.size()), 0, static_cast<int>(utf8.size()));
  hb_buffer_guess_segment_properties(buf);
  hb_shape(f.shaper, buf, nullptr, 0);
  unsigned count = 0;
  const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(buf, &count);
  const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &count);
  double pen = 0.0;
  out.glyphs.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    shaped::glyph g;
    g.id = info[i].codepoint;
    g.x = pen + pos[i].x_offset * k;
    g.dy = pos[i].y_offset * k;
    pen += pos[i].x_advance * k;
    out.glyphs.push_back(g);
  }
  hb_buffer_destroy(buf);
  out.advance = pen + (out.synthetic_bold ? s.size_pt * 0.02 : 0.0);
  return out;
}

void engine::draw(const shaped& word, const style& s, raster& page, double x_pt, double baseline_pt,
                  double scale) {
  if (word.font < 0 || word.glyphs.empty() || page.rgba.empty()) {
    if (s.underline && word.advance > 0) {
      fill_rect(page, x_pt, baseline_pt + s.size_pt * 0.12, word.advance,
                std::max(0.5, s.size_pt * 0.06), scale, s.rgb);
    }
    return;
  }
  impl::font& f = d_->fonts[static_cast<std::size_t>(word.font)];
  const double px = s.size_pt * scale;
  if (px < 1.0 || px > 4000.0) return;
  if (FT_Set_Char_Size(f.ft, 0, static_cast<FT_F26Dot6>(std::lround(px * 64.0)), 72, 72) != 0) return;

  const int w = static_cast<int>(page.width);
  const int h = static_cast<int>(page.height);
  const double r = (s.rgb >> 16) & 0xFF, gr = (s.rgb >> 8) & 0xFF, b = s.rgb & 0xFF;
  for (const shaped::glyph& g : word.glyphs) {
    const double pen_x = (x_pt + g.x) * scale;
    const double pen_y = (baseline_pt - g.dy) * scale;
    const double fx = std::floor(pen_x);
    FT_Vector delta{static_cast<FT_Pos>(std::lround((pen_x - fx) * 64.0)), 0};
    FT_Matrix shear{0x10000, word.synthetic_italic ? 0x0366A : 0, 0, 0x10000};  // ~12° slant
    FT_Set_Transform(f.ft, word.synthetic_italic ? &shear : nullptr, &delta);
    if (FT_Load_Glyph(f.ft, g.id, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) continue;
    if (word.synthetic_bold) FT_GlyphSlot_Embolden(f.ft->glyph);
    if (FT_Render_Glyph(f.ft->glyph, FT_RENDER_MODE_NORMAL) != 0) continue;
    const FT_Bitmap& bm = f.ft->glyph->bitmap;
    if (bm.pixel_mode != FT_PIXEL_MODE_GRAY && bm.pixel_mode != FT_PIXEL_MODE_MONO) continue;
    const int left = static_cast<int>(fx) + f.ft->glyph->bitmap_left;
    const int top = static_cast<int>(std::lround(pen_y)) - f.ft->glyph->bitmap_top;
    for (unsigned row = 0; row < bm.rows; ++row) {
      const int y = top + static_cast<int>(row);
      if (y < 0 || y >= h) continue;
      const unsigned char* src = bm.buffer + static_cast<std::ptrdiff_t>(row) * bm.pitch;
      std::uint8_t* dst = page.rgba.data() + static_cast<std::size_t>(y) * page.width * 4u;
      for (unsigned col = 0; col < bm.width; ++col) {
        const int x = left + static_cast<int>(col);
        if (x < 0 || x >= w) continue;
        const unsigned cov = bm.pixel_mode == FT_PIXEL_MODE_GRAY
                                 ? src[col]
                                 : ((src[col >> 3] >> (7 - (col & 7))) & 1u) * 255u;
        if (cov == 0) continue;
        const double a = cov / 255.0;
        std::uint8_t* p = dst + static_cast<std::size_t>(x) * 4u;
        p[0] = static_cast<std::uint8_t>(std::lround(p[0] + (r - p[0]) * a));
        p[1] = static_cast<std::uint8_t>(std::lround(p[1] + (gr - p[1]) * a));
        p[2] = static_cast<std::uint8_t>(std::lround(p[2] + (b - p[2]) * a));
      }
    }
  }
  FT_Set_Transform(f.ft, nullptr, nullptr);
  const double rule = std::max(0.5, s.size_pt * 0.06);
  if (s.underline) fill_rect(page, x_pt, baseline_pt + s.size_pt * 0.12, word.advance, rule, scale, s.rgb);
  if (s.strike) fill_rect(page, x_pt, baseline_pt - s.size_pt * 0.3, word.advance, rule, scale, s.rgb);
}

void fill_rect(raster& page, double x_pt, double y_pt, double w_pt, double h_pt, double scale,
               std::uint32_t rgb) {
  const auto clampi = [](double v, std::uint32_t hi) {
    return static_cast<std::uint32_t>(std::clamp(v, 0.0, static_cast<double>(hi)));
  };
  const std::uint32_t x0 = clampi(std::floor(x_pt * scale), page.width);
  const std::uint32_t y0 = clampi(std::floor(y_pt * scale), page.height);
  // At least one pixel, so a hairline rule is never lost at a small scale.
  const std::uint32_t x1 = clampi(std::max(std::ceil((x_pt + w_pt) * scale), x0 + 1.0), page.width);
  const std::uint32_t y1 = clampi(std::max(std::ceil((y_pt + h_pt) * scale), y0 + 1.0), page.height);
  const auto r = static_cast<std::uint8_t>((rgb >> 16) & 0xFF);
  const auto g = static_cast<std::uint8_t>((rgb >> 8) & 0xFF);
  const auto b = static_cast<std::uint8_t>(rgb & 0xFF);
  for (std::uint32_t y = y0; y < y1; ++y) {
    std::uint8_t* p = page.rgba.data() + (static_cast<std::size_t>(y) * page.width + x0) * 4u;
    for (std::uint32_t x = x0; x < x1; ++x, p += 4) {
      p[0] = r;
      p[1] = g;
      p[2] = b;
    }
  }
}

}  // namespace mv::codec::text
