// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/doclayout.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace mv::codec::doclayout {
namespace {

// Next UTF-8 code point; a broken sequence reads as U+FFFD one byte at a time.
std::uint32_t next_cp(std::string_view s, std::size_t& i) noexcept {
  const auto b = static_cast<unsigned char>(s[i]);
  if (b < 0x80) {
    ++i;
    return b;
  }
  const int extra = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
  if (extra < 0 || i + static_cast<std::size_t>(extra) >= s.size()) {
    ++i;
    return 0xFFFD;
  }
  std::uint32_t cp = b & (extra == 1 ? 0x1F : extra == 2 ? 0x0F : 0x07);
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

// The bidi class of one code point, by block: enough for Hebrew, Arabic and
// their digits among Latin, Greek, Cyrillic and CJK text. Not the full UCD.
bidi_class cp_class(std::uint32_t c) noexcept {
  if (c == ' ' || c == '\t' || c == 0xA0 || c == 0x3000 || (c >= 0x2000 && c <= 0x200A)) return bidi_class::WS;
  if (c >= '0' && c <= '9') return bidi_class::EN;
  if (c < 0x80) return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') ? bidi_class::L : bidi_class::ON;
  if (c >= 0x06F0 && c <= 0x06F9) return bidi_class::EN;
  if ((c >= 0x0660 && c <= 0x0669) || c == 0x066B || c == 0x066C) return bidi_class::AN;
  if (c == 0x200E) return bidi_class::L;
  if (c == 0x200F) return bidi_class::R;
  if (c >= 0x0590 && c <= 0x05FF) return bidi_class::R;
  if (c >= 0x07C0 && c <= 0x085F) return bidi_class::R;
  if (c >= 0x0600 && c <= 0x07BF) return bidi_class::AL;
  if (c >= 0x0860 && c <= 0x08FF) return bidi_class::AL;
  if (c >= 0xFB1D && c <= 0xFB4F) return bidi_class::R;
  if (c >= 0xFB50 && c <= 0xFDFF) return bidi_class::AL;
  if (c >= 0xFE70 && c <= 0xFEFE) return bidi_class::AL;
  if (c < 0xC0 || c == 0xD7 || c == 0xF7) return bidi_class::ON;
  if (c >= 0x2000 && c <= 0x2BFF) return bidi_class::ON;
  if (c >= 0x3000 && c <= 0x303F) return bidi_class::ON;
  if (c >= 0xFE00 && c <= 0xFE6F) return bidi_class::ON;
  if (c >= 0xFF00 && c <= 0xFF20) return bidi_class::ON;
  return bidi_class::L;
}

bool is_cjk(std::uint32_t cp) noexcept {
  return (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x3000 && cp <= 0x303F);
}

// Brackets that mirror in a right-to-left run.
bool has_mirror(std::string_view s) noexcept {
  for (char c : s) {
    if (c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '<' || c == '>') return true;
  }
  return s.find("\xC2\xAB") != std::string_view::npos || s.find("\xC2\xBB") != std::string_view::npos;
}

// Bilinear, straight into the page; a missing picture is a grey box.
void blit(raster& page, const display::image& im, double scale) {
  const int x0 = static_cast<int>(std::floor(im.x * scale));
  const int y0 = static_cast<int>(std::floor(im.y * scale));
  const int w = std::max(1, static_cast<int>(std::lround(im.w * scale)));
  const int h = std::max(1, static_cast<int>(std::lround(im.h * scale)));
  if (!im.pixels || im.pixels->width == 0 || im.pixels->height == 0) {
    text::fill_rect(page, im.x, im.y, im.w, im.h, scale, 0xDDDDDD);
    return;
  }
  const raster& src = *im.pixels;
  for (int y = 0; y < h; ++y) {
    const int py = y0 + y;
    if (py < 0 || py >= static_cast<int>(page.height)) continue;
    const double sy = std::clamp((y + 0.5) * src.height / h - 0.5, 0.0, src.height - 1.0);
    const auto iy = static_cast<std::uint32_t>(sy);
    const std::uint32_t iy1 = std::min(iy + 1, src.height - 1);
    const double fy = sy - iy;
    for (int x = 0; x < w; ++x) {
      const int px = x0 + x;
      if (px < 0 || px >= static_cast<int>(page.width)) continue;
      const double sx = std::clamp((x + 0.5) * src.width / w - 0.5, 0.0, src.width - 1.0);
      const auto ix = static_cast<std::uint32_t>(sx);
      const std::uint32_t ix1 = std::min(ix + 1, src.width - 1);
      const double fx = sx - ix;
      const auto at = [&](std::uint32_t xx, std::uint32_t yy, int c) {
        return static_cast<double>(src.rgba[(static_cast<std::size_t>(yy) * src.width + xx) * 4 + static_cast<std::size_t>(c)]);
      };
      std::uint8_t* d = page.rgba.data() + (static_cast<std::size_t>(py) * page.width + static_cast<std::size_t>(px)) * 4;
      const double a = (at(ix, iy, 3) * (1 - fx) + at(ix1, iy, 3) * fx) * (1 - fy) +
                       (at(ix, iy1, 3) * (1 - fx) + at(ix1, iy1, 3) * fx) * fy;
      const double alpha = a / 255.0;
      for (int c = 0; c < 3; ++c) {
        const double v = (at(ix, iy, c) * (1 - fx) + at(ix1, iy, c) * fx) * (1 - fy) +
                         (at(ix, iy1, c) * (1 - fx) + at(ix1, iy1, c) * fx) * fy;
        d[c] = static_cast<std::uint8_t>(std::lround(d[c] + (v - d[c]) * alpha));
      }
    }
  }
}

void blend(std::uint8_t* p, std::uint32_t rgb, double a) noexcept {
  if (a <= 0.0) return;
  a = std::min(a, 1.0);
  const double c[3] = {static_cast<double>((rgb >> 16) & 0xFF), static_cast<double>((rgb >> 8) & 0xFF),
                       static_cast<double>(rgb & 0xFF)};
  for (int k = 0; k < 3; ++k) p[k] = static_cast<std::uint8_t>(std::lround(p[k] + (c[k] - p[k]) * a));
}

}  // namespace

// ---- display ------------------------------------------------------------------------

void display::add(word w) {
  order_.push_back({kind::word, current, z, static_cast<std::uint32_t>(words_.size())});
  words_.push_back(std::move(w));
}
void display::add(rect r) {
  order_.push_back({kind::rect, current, z, static_cast<std::uint32_t>(rects_.size())});
  rects_.push_back(r);
}
void display::add(image i) {
  order_.push_back({kind::image, current, z, static_cast<std::uint32_t>(images_.size())});
  images_.push_back(std::move(i));
}
void display::add(shape s) {
  order_.push_back({kind::shape, current, z, static_cast<std::uint32_t>(shapes_.size())});
  shapes_.push_back(s);
}

status paint(const display& d, text::engine& eng, raster& page, double scale, const std::string& page_label,
             const std::string& pages_label, const job_context* ctx) {
  std::vector<display::entry> order = d.order_;
  std::stable_sort(order.begin(), order.end(), [](const display::entry& a, const display::entry& b) {
    if (a.l != b.l) return a.l < b.l;
    return a.z < b.z;
  });
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (ctx && (i & 255) == 0 && ctx->cancelled()) return status::cancelled;
    const display::entry& e = order[i];
    switch (e.k) {
      case display::kind::image:
        blit(page, d.images_[e.index], scale);
        break;
      case display::kind::rect: {
        const display::rect& rc = d.rects_[e.index];
        text::fill_rect(page, rc.x, rc.y, rc.w, rc.h, scale, rc.rgb);
        break;
      }
      case display::kind::shape: {
        const display::shape& sh = d.shapes_[e.index];
        draw_shape(page, sh.s, sh.x, sh.y, sh.w, sh.h, scale);
        break;
      }
      case display::kind::word: {
        const display::word& w = d.words_[e.index];
        if (w.f == field::none) {
          eng.draw(w.shaped, w.style, page, w.x, w.baseline, scale);
        } else {
          // Laid out with a placeholder; drawn centred on it.
          const text::shaped real = eng.shape(w.f == field::page ? page_label : pages_label, w.style);
          eng.draw(real, w.style, page, w.x + (w.box_w - real.advance) / 2.0, w.baseline, scale);
        }
        break;
      }
    }
  }
  return status::ok;
}

void draw_shape(raster& page, const shape_style& s, double x, double y, double w, double h, double scale) {
  if (!(w >= 0.0) || !(h >= 0.0) || page.rgba.empty() || !(scale > 0.0)) return;
  if (!s.filled && !s.stroked) return;
  const double half = s.stroked ? std::max(s.stroke_w / 2.0, 0.5 / scale) : 0.0;
  const auto clampi = [](double v, std::uint32_t hi) {
    return static_cast<int>(std::clamp(v, 0.0, static_cast<double>(hi)));
  };
  const int x0 = clampi(std::floor((x - half - 1.0) * scale), page.width);
  const int y0 = clampi(std::floor((y - half - 1.0) * scale), page.height);
  const int x1 = clampi(std::ceil((x + w + half + 1.0) * scale), page.width);
  const int y1 = clampi(std::ceil((y + h + half + 1.0) * scale), page.height);
  const double cx = x + w / 2.0, cy = y + h / 2.0, hx = w / 2.0, hy = h / 2.0;
  const double r = s.geom == geometry::round_rect ? std::min(std::min(w, h) * std::clamp(s.radius, 0.0, 0.5), std::min(hx, hy)) : 0.0;
  // The line's ends.
  const double lx0 = s.flip_h ? x + w : x, lx1 = s.flip_h ? x : x + w;
  const double ly0 = s.flip_v ? y + h : y, ly1 = s.flip_v ? y : y + h;
  const double ldx = lx1 - lx0, ldy = ly1 - ly0, llen2 = ldx * ldx + ldy * ldy;
  for (int py = y0; py < y1; ++py) {
    const double fy = (py + 0.5) / scale;
    std::uint8_t* row = page.rgba.data() + static_cast<std::size_t>(py) * page.width * 4u;
    for (int px = x0; px < x1; ++px) {
      const double fx = (px + 0.5) / scale;
      double d = 0.0;  // signed distance to the outline, points; negative inside
      switch (s.geom) {
        case geometry::rect:
        case geometry::round_rect: {
          const double qx = std::fabs(fx - cx) - hx + r, qy = std::fabs(fy - cy) - hy + r;
          const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0);
          d = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0) - r;
          break;
        }
        case geometry::ellipse: {
          if (hx <= 0.0 || hy <= 0.0) continue;
          const double dx = fx - cx, dy = fy - cy;
          const double k = std::sqrt((dx * dx) / (hx * hx) + (dy * dy) / (hy * hy));
          const double gx = dx / (hx * hx), gy = dy / (hy * hy);
          const double g = std::sqrt(gx * gx + gy * gy);
          d = (k > 1e-6 && g > 1e-12) ? (k - 1.0) * k / g : -std::min(hx, hy);
          break;
        }
        case geometry::line: {
          const double t = llen2 > 0 ? std::clamp(((fx - lx0) * ldx + (fy - ly0) * ldy) / llen2, 0.0, 1.0) : 0.0;
          const double ex = fx - (lx0 + t * ldx), ey = fy - (ly0 + t * ldy);
          d = std::sqrt(ex * ex + ey * ey);
          break;
        }
      }
      std::uint8_t* p = row + static_cast<std::size_t>(px) * 4u;
      if (s.filled && s.geom != geometry::line) blend(p, s.fill, std::clamp(0.5 - d * scale, 0.0, 1.0) * s.fill_alpha);
      if (s.stroked) blend(p, s.stroke, std::clamp(half * scale + 0.5 - std::fabs(d) * scale, 0.0, 1.0));
    }
  }
}

// ---- frames ------------------------------------------------------------------------

slot box_frame::fit(double y, double, double inset_l, double inset_r, std::span<const int>) {
  slot s;
  s.y = y;
  s.seg[0].x0 = x0_ + inset_l;
  s.seg[0].x1 = std::max(s.seg[0].x0 + 1.0, x1_ - inset_r);
  return s;
}

void box_frame::placed(double, double, std::span<const int> refs) {
  for (int n : refs) notes.push_back(n);
}

// ---- bidi ---------------------------------------------------------------------------

bidi_class classify(std::string_view utf8) noexcept {
  bool digits = false, arabic_digits = false, all_space = true;
  for (std::size_t i = 0; i < utf8.size();) {
    const bidi_class c = cp_class(next_cp(utf8, i));
    if (c == bidi_class::L || c == bidi_class::R || c == bidi_class::AL) return c;
    if (c == bidi_class::EN) digits = true;
    if (c == bidi_class::AN) arabic_digits = true;
    if (c != bidi_class::WS) all_space = false;
  }
  if (arabic_digits) return bidi_class::AN;
  if (digits) return bidi_class::EN;
  return all_space ? bidi_class::WS : bidi_class::ON;
}

std::vector<std::size_t> visual_order(std::span<const bidi_class> cls, int base, std::vector<int>* levels) {
  using bc = bidi_class;
  const std::size_t n = cls.size();
  std::vector<bc> t(cls.begin(), cls.end());
  const bc sos = base ? bc::R : bc::L;
  // W2: digits after Arabic letters are Arabic numbers. W3: AL is R.
  bc last = sos;
  for (bc& c : t) {
    if (c == bc::EN && last == bc::AL) c = bc::AN;
    if (c == bc::L || c == bc::R || c == bc::AL) last = c;
  }
  for (bc& c : t) {
    if (c == bc::AL) c = bc::R;
  }
  // W7: digits after Latin are Latin.
  last = sos;
  for (bc& c : t) {
    if (c == bc::EN && last == bc::L) c = bc::L;
    if (c == bc::L || c == bc::R) last = c;
  }
  // N1/N2: a run of neutrals takes the direction around it when both sides
  // agree (numbers count as R), else the paragraph's.
  const auto dir = [](bc c) { return c == bc::L ? bc::L : bc::R; };
  for (std::size_t i = 0; i < n;) {
    if (t[i] != bc::WS && t[i] != bc::ON) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j < n && (t[j] == bc::WS || t[j] == bc::ON)) ++j;
    const bc before = i == 0 ? sos : dir(t[i - 1]);
    const bc after = j == n ? sos : dir(t[j]);
    const bc d = before == after ? before : sos;
    for (std::size_t k = i; k < j; ++k) t[k] = d;
    i = j;
  }
  // I1/I2.
  std::vector<int> lv(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (base == 0) lv[i] = t[i] == bc::L ? 0 : t[i] == bc::R ? 1 : 2;
    else lv[i] = t[i] == bc::R ? 1 : 2;
  }
  // L1: white space at the end of the line goes back to the paragraph level.
  for (std::size_t i = n; i > 0 && cls[i - 1] == bc::WS; --i) lv[i - 1] = base;
  // L2: reverse every run at or above each level, highest first.
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) order[i] = i;
  int high = 0, low_odd = 99;
  for (int l : lv) {
    high = std::max(high, l);
    if (l % 2 == 1) low_odd = std::min(low_odd, l);
  }
  std::vector<int> at(lv);  // levels in the current visual order
  for (int l = high; l >= low_odd && l > 0; --l) {
    for (std::size_t i = 0; i < n;) {
      if (at[i] < l) {
        ++i;
        continue;
      }
      std::size_t j = i;
      while (j < n && at[j] >= l) ++j;
      std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i), order.begin() + static_cast<std::ptrdiff_t>(j));
      std::reverse(at.begin() + static_cast<std::ptrdiff_t>(i), at.begin() + static_cast<std::ptrdiff_t>(j));
      i = j;
    }
  }
  if (levels) *levels = std::move(lv);
  return order;
}

std::vector<std::string> tokens_of(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool cur_space = false;
  int cur_dir = 0;  // 0 none yet, 1 right to left, 2 left to right or digits
  auto flush = [&] {
    if (!cur.empty()) out.push_back(std::move(cur));
    cur.clear();
    cur_dir = 0;
  };
  for (std::size_t i = 0; i < s.size();) {
    const std::size_t start = i;
    const std::uint32_t cp = next_cp(s, i);
    const bool is_space = cp == ' ' || cp == '\t';
    if (is_cjk(cp)) {
      flush();
      out.emplace_back(s.substr(start, i - start));
      continue;
    }
    const bidi_class c = cp_class(cp);
    const int d = (c == bidi_class::R || c == bidi_class::AL) ? 1
                  : (c == bidi_class::L || c == bidi_class::EN || c == bidi_class::AN) ? 2 : 0;
    if (!cur.empty() && (is_space != cur_space || (d != 0 && cur_dir != 0 && d != cur_dir))) flush();
    cur_space = is_space;
    if (d != 0) cur_dir = d;
    cur.append(s, start, i - start);
  }
  flush();
  return out;
}

// ---- paragraphs ----------------------------------------------------------------------

namespace {

struct piece {
  enum kind : std::uint8_t { word, space, tab, object, brk } k = word;
  text::shaped shaped;
  text::style style;
  std::string str;
  double w = 0.0;
  double asc = 0.0, desc = 0.0;
  double raise = 0.0;
  bidi_class cls = bidi_class::L;
  field f = field::none;
  int obj = -1;
  double obj_h = 0.0;
  int note = -1;
  span::kind brk_kind = span::line_break;
};

struct fill {
  std::size_t b = 0, e = 0, e_trim = 0;  // pieces [b, e_trim) are drawn; e is where the next line starts
  double width = 0.0;                    // from the indent, the first-line indent included
  double asc = 0.0, desc = 0.0, gap = 0.0;
  bool brk = false;
  span::kind brk_kind = span::line_break;
};

fill break_line(std::vector<piece>& pcs, std::size_t i, double width, double x0, bool skip_lead) {
  fill f;
  const std::size_t n = pcs.size();
  if (skip_lead) {
    while (i < n && pcs[i].k == piece::space) ++i;
  }
  f.b = i;
  double x = x0;
  std::size_t count = 0;
  std::size_t k = i;
  for (; k < n; ++k) {
    piece& pc = pcs[k];
    if (pc.k == piece::brk) {
      f.brk = true;
      f.brk_kind = pc.brk_kind;
      ++k;
      break;
    }
    if (pc.k == piece::tab) {
      constexpr double kTab = 36.0;
      const double next = (std::floor((x + 0.01) / kTab) + 1.0) * kTab;
      // A hanging indent's first tab goes to the indent itself.
      pc.w = (x0 < 0 && count <= 2 && x < 0) ? -x : next - x;
      if (x + pc.w > width && count > 0) break;
    } else if ((pc.k == piece::word || pc.k == piece::object) && x + pc.w > width && count > 0) {
      break;
    }
    x += pc.w;
    ++count;
    if (pc.k != piece::space && pc.k != piece::tab) {
      f.asc = std::max(f.asc, pc.asc);
      f.desc = std::max(f.desc, pc.desc);
      f.gap = std::max(f.gap, pc.shaped.line_gap);
    }
  }
  f.e = k;
  std::size_t t = f.brk ? k - 1 : k;
  while (t > f.b && pcs[t - 1].k == piece::space) {
    x -= pcs[t - 1].w;
    --t;
  }
  f.e_trim = t;
  f.width = x;
  return f;
}

double line_height(const para& p, double natural) {
  switch (p.rule) {
    case line_rule::exact: return std::max(1.0, p.line);
    case line_rule::at_least: return std::max(natural, p.line);
    case line_rule::automatic: break;
  }
  return natural * std::clamp(p.line / 240.0, 0.5, 5.0);
}

}  // namespace

double lay_paragraph(text::engine& eng, const para& p, frame& f, double y) {
  if (!f.at_top(y)) y += std::clamp(p.before, 0.0, 400.0);
  const double after = std::clamp(p.after, 0.0, 400.0);
  const segment col = f.column();
  const double colw = std::max(18.0, col.x1 - col.x0);
  const double start = std::clamp(p.ind_start, -std::max(0.0, col.x0), colw - 18.0);
  const double end = std::clamp(p.ind_end, 0.0, std::max(0.0, colw - start - 18.0));
  const double inset_l = p.rtl ? end : start, inset_r = p.rtl ? start : end;
  const double first = p.ind_first;

  // Pieces.
  std::vector<piece> pcs;
  if (!p.label.empty()) {
    piece lab;
    lab.style = p.label_style;
    lab.shaped = eng.shape(p.label, lab.style);
    lab.str = p.label;
    lab.w = lab.shaped.advance;
    lab.asc = lab.shaped.ascent;
    lab.desc = lab.shaped.descent;
    lab.cls = classify(p.label);
    pcs.push_back(std::move(lab));
    piece gap;
    gap.k = piece::tab;
    gap.style = p.label_style;
    gap.cls = bidi_class::WS;
    pcs.push_back(std::move(gap));
  }
  const double max_obj_w = std::max(18.0, colw - start - end);
  for (const span& sp : p.spans) {
    switch (sp.k) {
      case span::text:
        for (std::string& tok : tokens_of(sp.str)) {
          piece pc;
          pc.k = tok[0] == ' ' || tok[0] == '\t' ? piece::space : piece::word;
          pc.style = sp.style;
          pc.cls = classify(tok);
          if (sp.rtl && pc.cls == bidi_class::ON) pc.cls = bidi_class::R;
          pc.shaped = eng.shape(tok, sp.style);
          pc.w = pc.shaped.advance;
          pc.asc = pc.shaped.ascent + std::max(0.0, sp.raise);
          pc.desc = pc.shaped.descent + std::max(0.0, -sp.raise);
          pc.raise = sp.raise;
          pc.note = sp.note;
          pc.str = std::move(tok);
          pcs.push_back(std::move(pc));
        }
        break;
      case span::tab: {
        piece pc;
        pc.k = piece::tab;
        pc.style = sp.style;
        pc.cls = bidi_class::WS;
        pcs.push_back(std::move(pc));
        break;
      }
      case span::line_break:
      case span::column_break:
      case span::page_break: {
        piece pc;
        pc.k = piece::brk;
        pc.brk_kind = sp.k;
        pcs.push_back(std::move(pc));
        break;
      }
      case span::object: {
        if (!(sp.obj_w > 0.0) || !(sp.obj_h > 0.0)) break;
        piece pc;
        pc.k = piece::object;
        pc.obj = sp.obj;
        pc.cls = bidi_class::ON;
        const double k = std::min({1.0, max_obj_w / sp.obj_w, f.max_object_h() / sp.obj_h});
        pc.w = sp.obj_w * k;
        pc.obj_h = sp.obj_h * k;
        pc.asc = pc.obj_h;
        pc.note = sp.note;
        pcs.push_back(std::move(pc));
        break;
      }
    }
    if (sp.k == span::text && sp.f != field::none) {
      // A field: its value now, or a placeholder the painter replaces.
      piece pc;
      pc.style = sp.style;
      pc.f = sp.f;
      std::string value = f.field_text(sp.f);
      if (!value.empty()) pc.f = field::none;
      pc.str = value.empty() ? std::string("00") : value;
      pc.cls = bidi_class::EN;
      pc.shaped = eng.shape(pc.str, sp.style);
      pc.w = pc.shaped.advance;
      pc.asc = pc.shaped.ascent + std::max(0.0, sp.raise);
      pc.desc = pc.shaped.descent + std::max(0.0, -sp.raise);
      pc.raise = sp.raise;
      pcs.push_back(std::move(pc));
    }
  }

  const text::shaped empty = eng.metrics(p.mark);
  const double empty_natural = empty.ascent + empty.descent + empty.line_gap;
  double guess = line_height(p, empty_natural);
  const std::size_t n = pcs.size();
  std::size_t pos = 0;
  bool first_line = true, after_break = false;
  std::vector<int> notes, notes_guess;
  for (int guard = 0; guard < 1000000; ++guard) {
    if (!first_line) {
      if (pos >= n && !after_break) break;
      if (!after_break) {
        std::size_t k = pos;
        while (k < n && pcs[k].k == piece::space) ++k;
        if (k >= n) break;  // only spaces left after a wrap
      }
    }
    slot s;
    fill fills[3];
    int nf = 0;
    double h = guess, asc = 0.0, desc = 0.0, gap = 0.0;
    notes_guess.clear();
    for (int attempt = 0;; ++attempt) {
      s = f.fit(y, guess, inset_l, inset_r, notes_guess);
      std::size_t k = pos;
      nf = 0;
      const int count = std::clamp(s.count, 1, 3);
      for (int si = 0; si < count; ++si) {
        const double sw = std::max(1.0, s.seg[si].x1 - s.seg[si].x0);
        const bool lead = first_line && si == 0;
        fills[nf] = break_line(pcs, k, sw, lead ? first : 0.0, !lead);
        k = fills[nf].e;
        ++nf;
        if (fills[nf - 1].brk || k >= n) break;
      }
      asc = desc = gap = 0.0;
      notes.clear();
      for (int i = 0; i < nf; ++i) {
        asc = std::max(asc, fills[i].asc);
        desc = std::max(desc, fills[i].desc);
        gap = std::max(gap, fills[i].gap);
        for (std::size_t j = fills[i].b; j < fills[i].e_trim; ++j) {
          if (pcs[j].note >= 0) notes.push_back(pcs[j].note);
        }
      }
      if (asc == 0.0 && desc == 0.0) {
        asc = empty.ascent;
        desc = empty.descent;
        gap = empty.line_gap;
      }
      h = line_height(p, asc + desc + gap);
      const bool notes_ok = std::all_of(notes.begin(), notes.end(), [&](int id) {
        return std::find(notes_guess.begin(), notes_guess.end(), id) != notes_guess.end();
      });
      if (attempt >= 3 || (h <= guess + 1e-6 && notes_ok)) break;
      guess = std::max(guess, h);
      notes_guess = notes;
      y = s.y;
    }

    if (display* out = f.out()) {
      const double natural = asc + desc + gap;
      const double baseline = s.y + (h - natural) * (p.rule == line_rule::exact ? 0.5 : 0.0) + asc;
      for (int si = 0; si < nf; ++si) {
        const fill& fl = fills[si];
        const segment& sg = s.seg[si];
        const double width = std::max(1.0, sg.x1 - sg.x0);
        const double x0 = first_line && si == 0 ? first : 0.0;
        const bool last = (si == nf - 1) && (fl.brk || fl.e >= n);
        const double room = width - fl.width;
        double offset = 0.0, extra = 0.0;
        std::vector<std::size_t> idx;
        std::vector<bidi_class> cls;
        for (std::size_t j = fl.b; j < fl.e_trim; ++j) {
          if (pcs[j].k == piece::brk) continue;
          idx.push_back(j);
          cls.push_back(pcs[j].cls);
        }
        int spaces = 0;
        for (std::size_t j : idx) spaces += pcs[j].k == piece::space ? 1 : 0;
        switch (p.jc) {
          case align::start: break;
          case align::end: offset = room; break;
          case align::center: offset = room / 2.0; break;
          case align::justify:
            if (!last && room > 0 && spaces > 0) extra = room / spaces;
            break;
        }
        const double content = fl.width - x0 + extra * spaces;
        double x = p.rtl ? sg.x1 - offset - x0 - content : sg.x0 + offset + x0;
        // A left-to-right line with nothing right-to-left in it keeps its order.
        const bool mixed = p.rtl || std::any_of(cls.begin(), cls.end(), [](bidi_class c) {
                             return c == bidi_class::R || c == bidi_class::AL || c == bidi_class::AN;
                           });
        std::vector<int> levels(cls.size(), 0);
        std::vector<std::size_t> order;
        if (mixed) {
          order = visual_order(cls, p.rtl ? 1 : 0, &levels);
        } else {
          order.resize(cls.size());
          for (std::size_t o = 0; o < order.size(); ++o) order[o] = o;
        }
        for (std::size_t o : order) {
          piece& pc = pcs[idx[o]];
          // A neutral read right to left: its brackets mirror.
          if (pc.k == piece::word && levels[o] % 2 == 1 &&
              (pc.cls == bidi_class::ON || pc.cls == bidi_class::R) && classify(pc.str) == bidi_class::ON &&
              has_mirror(pc.str)) {
            pc.shaped = eng.shape(pc.str, pc.style, text::direction::rtl);
          }
          switch (pc.k) {
            case piece::word:
              out->add(display::word{pc.shaped, pc.style, x, baseline - pc.raise, pc.f, pc.w});
              break;
            case piece::space:
              if (pc.style.underline || pc.style.strike) {
                out->add(display::word{pc.shaped, pc.style, x, baseline - pc.raise, field::none, pc.w});
              }
              break;
            case piece::object:
              f.object(pc.obj, x, baseline - pc.obj_h, pc.w, pc.obj_h);
              break;
            case piece::tab:
            case piece::brk:
              break;
          }
          x += pc.w + (pc.k == piece::space ? extra : 0.0);
        }
      }
    }
    f.placed(s.y, h, notes);
    y = s.y + h;
    guess = h;
    pos = fills[nf - 1].e;
    // Where the segments of a wrapped line ran out before the text did.
    after_break = fills[nf - 1].brk;
    if (after_break && fills[nf - 1].brk_kind != span::line_break) {
      y = f.hard_break(fills[nf - 1].brk_kind, y);
    }
    first_line = false;
  }
  return y + after;
}

}  // namespace mv::codec::doclayout
