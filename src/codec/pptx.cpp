// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PPTX (issue #429, D5 as amended 2026-10-15): a slide a page, drawn by our own
// layout over the DOCX path (codec/zip, codec/xml, codec/doclayout, HarfBuzz +
// FreeType through codec/text). Readable slides, not a PowerPoint replica.
//
// What is drawn: the slide size (presentation.xml), the background (solid, the
// first gradient stop, a picture), the master's and layout's own shapes under
// the slide's, placeholders inheriting position, insets and text styles from
// layout and master (txStyles, lstStyle levels), shapes (rect, roundRect,
// ellipse, lines and connectors; any other geometry as its box) with fills,
// outlines and the p:style references, pictures, groups (child offsets and
// scale), tables (grid, spans, cell fills, borders) and text: runs (font,
// size, bold, italic, underline, strike, colour, super/subscript), alignment,
// indents, line and paragraph spacing, bullets and automatic numbers, line
// breaks, tabs, right-to-left paragraphs, the slide number field, top / centre /
// bottom anchoring and normAutofit's font scale. Theme colours go through the
// master's colour map with lumMod / lumOff / shade / tint / alpha.
//
// Not drawn: rotation (a shape is drawn at its unrotated box), custom and most
// preset geometries (their box), effects (shadow, glow, 3-D), gradients beyond
// their first stop, charts, SmartArt and media (a grey box), speaker notes,
// comments, animations, picture crops.
//
// Every slide is parsed only when it is drawn: page 250's thumbnail reads
// presentation.xml, slide 250, its layout and its master, not the 249 before it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codec/decode.h"
#include "codec/doclayout.h"
#include "codec/text.h"
#include "codec/xml.h"
#include "codec/zip.h"

namespace mv::codec {
namespace {

namespace dl = doclayout;
using xml::token;

constexpr std::size_t kMaxSlides = 10000;
constexpr std::size_t kMaxShapes = 20000;    // per part, groups included
constexpr int kMaxGroupDepth = 16;
constexpr int kMaxImages = 256;             // decoded per call
constexpr std::size_t kMaxImageBytes = std::size_t{48} << 20;
constexpr double kEmuPerPt = 12700.0;

// ---- units and small readers ----------------------------------------------------

long long big_number(std::string_view v, long long fallback = 0) noexcept {
  if (v.empty()) return fallback;
  char buf[32];
  const std::size_t n = std::min(v.size(), sizeof buf - 1);
  std::memcpy(buf, v.data(), n);
  buf[n] = '\0';
  char* end = nullptr;
  const long long out = std::strtoll(buf, &end, 10);
  return end == buf ? fallback : out;
}
double emu_pt(std::string_view v, double fallback = 0.0) noexcept {
  if (v.empty()) return fallback;
  const double out = static_cast<double>(big_number(v, 0)) / kEmuPerPt;
  return std::isfinite(out) ? std::clamp(out, -100000.0, 100000.0) : fallback;
}
bool attr_true(std::string_view v) noexcept { return v == "1" || v == "true"; }
bool parse_rgb(std::string_view hex, std::uint32_t& out) noexcept {
  if (hex.size() != 6) return false;
  std::uint32_t v = 0;
  for (char c : hex) {
    v <<= 4;
    if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
    else return false;
  }
  out = v;
  return true;
}
std::string_view view(const std::vector<std::uint8_t>& b) {
  return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
}
// Runs `f` on every start element below the one just read; consumes it. `f`
// may consume a child itself (it then returns true) or leave it to be walked.
template <class F>
void each_child(xml::reader& r, F&& f) {
  const int depth = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < depth) return;
    if (t == token::start && r.depth() == depth + 1) {
      if (!f(r.local())) r.skip_element();
    }
  }
}

// "ppt/slides/slide1.xml" + "../slideLayouts/slideLayout2.xml".
std::string resolve(std::string_view base_part, std::string_view target) {
  if (!target.empty() && target.front() == '/') return std::string(target.substr(1));
  std::string dir(base_part.substr(0, base_part.rfind('/') + 1));
  std::vector<std::string> parts;
  std::string cur;
  const std::string whole = dir + std::string(target);
  for (char c : whole) {
    if (c == '/') {
      if (cur == "..") {
        if (!parts.empty()) parts.pop_back();
      } else if (!cur.empty() && cur != ".") {
        parts.push_back(cur);
      }
      cur.clear();
    } else {
      cur += c;
    }
  }
  std::string out;
  for (const auto& p : parts) out += p + "/";
  return out + cur;
}
std::string rels_of(std::string_view part) {
  const auto slash = part.rfind('/');
  return std::string(part.substr(0, slash + 1)) + "_rels/" + std::string(part.substr(slash + 1)) + ".rels";
}

struct relationship {
  std::string target;  // resolved part name
  std::string type;    // the type's last path segment ("slideLayout", "image")
};
using relationships = std::unordered_map<std::string, relationship>;

relationships read_rels(const zip::archive& zip, std::string_view part) {
  relationships out;
  auto bytes = zip.read(rels_of(part), std::size_t{4} << 20);
  if (!bytes) return out;
  xml::reader r(view(*bytes));
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t != token::start || r.local() != "Relationship") continue;
    if (r.attr("TargetMode") == "External") continue;
    const std::string_view type = r.attr("Type");
    out[std::string(r.attr("Id"))] = {resolve(part, r.attr("Target")),
                                      std::string(type.substr(type.rfind('/') + 1))};
  }
  return out;
}

// ---- colours ---------------------------------------------------------------------

struct colour {
  bool set = false;
  std::uint32_t rgb = 0;
  double alpha = 1.0;
};

struct theme {
  std::unordered_map<std::string, std::uint32_t> scheme;
  std::string major = "Calibri Light", minor = "Calibri";
};

// The master's colour map (bg1 -> lt1 ...) over the theme.
struct palette {
  const theme* th = nullptr;
  std::unordered_map<std::string, std::string> map;
  [[nodiscard]] bool lookup(std::string key, std::uint32_t& out) const {
    if (auto m = map.find(key); m != map.end()) key = m->second;
    else if (key == "tx1") key = "dk1";
    else if (key == "bg1") key = "lt1";
    else if (key == "tx2") key = "dk2";
    else if (key == "bg2") key = "lt2";
    if (!th) return false;
    if (auto it = th->scheme.find(key); it != th->scheme.end()) {
      out = it->second;
      return true;
    }
    return false;
  }
};

void to_hsl(std::uint32_t rgb, double& h, double& s, double& l) {
  const double r = ((rgb >> 16) & 0xFF) / 255.0, g = ((rgb >> 8) & 0xFF) / 255.0, b = (rgb & 0xFF) / 255.0;
  const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  l = (mx + mn) / 2.0;
  h = s = 0.0;
  if (mx - mn < 1e-9) return;
  const double d = mx - mn;
  s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
  if (mx == r) h = (g - b) / d + (g < b ? 6.0 : 0.0);
  else if (mx == g) h = (b - r) / d + 2.0;
  else h = (r - g) / d + 4.0;
  h /= 6.0;
}
std::uint32_t from_hsl(double h, double s, double l) {
  const auto hue = [](double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
  };
  double r = l, g = l, b = l;
  if (s > 1e-9) {
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const double p = 2 * l - q;
    r = hue(p, q, h + 1.0 / 3);
    g = hue(p, q, h);
    b = hue(p, q, h - 1.0 / 3);
  }
  const auto c = [](double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); };
  return (c(r) << 16) | (c(g) << 8) | c(b);
}

// The colour inside the element just started (solidFill, a gradient, a
// style reference, buClr …). A scheme colour stays a name until resolved, so a
// layout's "accent1" follows the slide's master. Consumes the element.
struct colour_ref {
  bool set = false;
  std::uint32_t rgb = 0;
  std::string scheme;  // a scheme name, resolved against the palette
  struct mod {
    std::string op;
    double v;
  };
  std::vector<mod> mods;
  [[nodiscard]] colour resolve(const palette& pal, const colour* ph_clr = nullptr) const {
    colour out;
    if (!set) return out;
    out.rgb = rgb;
    out.set = true;
    if (!scheme.empty()) {
      if (scheme == "phClr" && ph_clr && ph_clr->set) out.rgb = ph_clr->rgb;
      else if (!pal.lookup(scheme, out.rgb)) out.rgb = 0x000000;
    }
    for (const mod& m : mods) {
      if (m.op == "lumMod" || m.op == "lumOff") {
        double h = 0, s = 0, l = 0;
        to_hsl(out.rgb, h, s, l);
        l = m.op == "lumMod" ? l * m.v : l + m.v;
        out.rgb = from_hsl(h, s, std::clamp(l, 0.0, 1.0));
      } else if (m.op == "shade" || m.op == "tint") {
        std::uint32_t c = 0;
        for (int k = 16; k >= 0; k -= 8) {
          const double ch = (out.rgb >> k) & 0xFF;
          const double nv = m.op == "shade" ? ch * m.v : ch + (255.0 - ch) * (1.0 - m.v);
          c |= static_cast<std::uint32_t>(std::lround(std::clamp(nv, 0.0, 255.0))) << k;
        }
        out.rgb = c;
      } else if (m.op == "alpha") {
        out.alpha = std::clamp(m.v, 0.0, 1.0);
      }
    }
    return out;
  }
};

colour_ref read_colour(xml::reader& r) {
  colour_ref out;
  bool in_colour = false, done = false;
  const int depth = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < depth) break;
    if (t == token::end && in_colour && r.depth() <= depth) {
      in_colour = false;
      done = true;
    }
    if (t != token::start) continue;
    const auto n = r.local();
    if (!out.set && !done && (n == "srgbClr" || n == "schemeClr" || n == "prstClr" || n == "sysClr" ||
                              n == "scrgbClr")) {
      if (n == "srgbClr") out.set = parse_rgb(r.attr("val"), out.rgb);
      else if (n == "sysClr") out.set = parse_rgb(r.attr("lastClr"), out.rgb);
      else if (n == "schemeClr") {
        out.scheme = std::string(r.attr("val"));
        out.set = !out.scheme.empty();
      } else if (n == "prstClr") {
        static constexpr std::pair<std::string_view, std::uint32_t> kPreset[] = {
            {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},  {"green", 0x008000},
            {"blue", 0x0000FF},  {"yellow", 0xFFFF00}, {"gray", 0x808080}, {"orange", 0xFFA500}};
        for (const auto& [name, v] : kPreset) {
          if (r.attr("val") == name) {
            out.rgb = v;
            out.set = true;
          }
        }
      }
      in_colour = out.set;
      continue;
    }
    if (!in_colour) continue;  // a gradient's first stop wins
    if (n == "lumMod" || n == "lumOff" || n == "shade" || n == "tint" || n == "alpha") {
      const double v = static_cast<double>(big_number(r.attr("val"), 100000)) / 100000.0;
      if (std::isfinite(v) && out.mods.size() < 8) out.mods.push_back({std::string(n), v});
    }
  }
  return out;
}

// ---- text properties --------------------------------------------------------------

struct run_props {
  std::optional<double> size;  // points
  std::optional<bool> bold, italic, underline, strike;
  std::optional<colour_ref> fill;
  std::optional<std::string> font;
  std::optional<double> baseline;  // fraction of the size, up positive

  void over(const run_props& o) {
    if (o.size) size = o.size;
    if (o.bold) bold = o.bold;
    if (o.italic) italic = o.italic;
    if (o.underline) underline = o.underline;
    if (o.strike) strike = o.strike;
    if (o.fill) fill = o.fill;
    if (o.font) font = o.font;
    if (o.baseline) baseline = o.baseline;
  }
};

enum class bullet_kind : std::uint8_t { none, character, number };

struct para_props {
  std::optional<dl::align> align;
  std::optional<bool> rtl;
  std::optional<double> mar_l, indent;  // points
  std::optional<double> line_pct, line_pts;
  std::optional<double> before_pts, before_pct, after_pts, after_pct;
  std::optional<bullet_kind> bullet;
  std::optional<std::string> bu_char, bu_scheme, bu_font;
  std::optional<long long> bu_start;
  std::optional<colour_ref> bu_clr;
  std::optional<double> bu_size_pct;
  run_props def;

  void over(const para_props& o) {
    if (o.align) align = o.align;
    if (o.rtl) rtl = o.rtl;
    if (o.mar_l) mar_l = o.mar_l;
    if (o.indent) indent = o.indent;
    if (o.line_pct) { line_pct = o.line_pct; line_pts.reset(); }
    if (o.line_pts) { line_pts = o.line_pts; line_pct.reset(); }
    if (o.before_pts) { before_pts = o.before_pts; before_pct.reset(); }
    if (o.before_pct) { before_pct = o.before_pct; before_pts.reset(); }
    if (o.after_pts) { after_pts = o.after_pts; after_pct.reset(); }
    if (o.after_pct) { after_pct = o.after_pct; after_pts.reset(); }
    if (o.bullet) bullet = o.bullet;
    if (o.bu_char) bu_char = o.bu_char;
    if (o.bu_scheme) bu_scheme = o.bu_scheme;
    if (o.bu_font) bu_font = o.bu_font;
    if (o.bu_start) bu_start = o.bu_start;
    if (o.bu_clr) bu_clr = o.bu_clr;
    if (o.bu_size_pct) bu_size_pct = o.bu_size_pct;
    def.over(o.def);
  }
};

// Nine list levels (lvl1pPr .. lvl9pPr); `set` says which were present.
struct level_styles {
  para_props lvl[9];
  bool set[9] = {};
  void over(const level_styles& o) {
    for (int i = 0; i < 9; ++i) {
      if (o.set[i]) {
        lvl[i].over(o.lvl[i]);
        set[i] = true;
      }
    }
  }
};

// Attributes of a:rPr / a:defRPr / a:endParaRPr; consumes the element.
run_props read_run_props(xml::reader& r) {
  run_props p;
  if (r.has("sz")) p.size = std::clamp(static_cast<double>(big_number(r.attr("sz"), 1800)) / 100.0, 1.0, 4000.0);
  if (r.has("b")) p.bold = attr_true(r.attr("b"));
  if (r.has("i")) p.italic = attr_true(r.attr("i"));
  if (r.has("u")) p.underline = r.attr("u") != "none";
  if (r.has("strike")) p.strike = r.attr("strike") != "noStrike";
  if (r.has("baseline")) p.baseline = static_cast<double>(big_number(r.attr("baseline"), 0)) / 100000.0;
  each_child(r, [&](std::string_view n) {
    if (n == "solidFill") {
      p.fill = read_colour(r);
      return true;
    }
    if (n == "noFill") p.fill = colour_ref{};
    if (n == "latin" && !r.attr("typeface").empty()) p.font = std::string(r.attr("typeface"));
    return false;
  });
  return p;
}

void read_spacing(xml::reader& r, std::optional<double>& pts, std::optional<double>& pct) {
  each_child(r, [&](std::string_view n) {
    if (n == "spcPts") {
      pts = std::clamp(static_cast<double>(big_number(r.attr("val"), 0)) / 100.0, 0.0, 1584.0);
      pct.reset();
    } else if (n == "spcPct") {
      pct = std::clamp(static_cast<double>(big_number(r.attr("val"), 100000)) / 100000.0, 0.0, 132.0);
      pts.reset();
    }
    return false;
  });
}

// a:pPr or a lvlNpPr; consumes the element.
para_props read_para_props(xml::reader& r, int* lvl = nullptr) {
  para_props p;
  if (r.has("algn")) {
    const auto a = r.attr("algn");
    p.align = a == "ctr" ? dl::align::center
              : a == "r" ? dl::align::end
              : (a == "just" || a == "dist" || a == "justLow") ? dl::align::justify
                                                              : dl::align::start;
  }
  if (r.has("rtl")) p.rtl = attr_true(r.attr("rtl"));
  if (r.has("marL")) p.mar_l = emu_pt(r.attr("marL"));
  if (r.has("indent")) p.indent = emu_pt(r.attr("indent"));
  if (lvl && r.has("lvl")) *lvl = static_cast<int>(std::clamp<long long>(big_number(r.attr("lvl"), 0), 0, 8));
  each_child(r, [&](std::string_view n) {
    if (n == "lnSpc") {
      read_spacing(r, p.line_pts, p.line_pct);
      return true;
    }
    if (n == "spcBef") {
      read_spacing(r, p.before_pts, p.before_pct);
      return true;
    }
    if (n == "spcAft") {
      read_spacing(r, p.after_pts, p.after_pct);
      return true;
    }
    if (n == "buNone") p.bullet = bullet_kind::none;
    if (n == "buChar") {
      p.bullet = bullet_kind::character;
      p.bu_char = std::string(r.attr("char"));
    }
    if (n == "buAutoNum") {
      p.bullet = bullet_kind::number;
      p.bu_scheme = std::string(r.attr("type"));
      if (r.has("startAt")) p.bu_start = std::clamp<long long>(big_number(r.attr("startAt"), 1), 1, 32767);
    }
    if (n == "buFont" && !r.attr("typeface").empty()) p.bu_font = std::string(r.attr("typeface"));
    if (n == "buSzPct") p.bu_size_pct = std::clamp(static_cast<double>(big_number(r.attr("val"), 100000)) / 100000.0, 0.25, 4.0);
    if (n == "buClr") {
      p.bu_clr = read_colour(r);
      return true;
    }
    if (n == "defRPr") {
      p.def = read_run_props(r);
      return true;
    }
    return false;
  });
  return p;
}

// a:lstStyle, p:titleStyle, p:bodyStyle, p:otherStyle, p:defaultTextStyle.
level_styles read_levels(xml::reader& r) {
  level_styles out;
  each_child(r, [&](std::string_view n) {
    if (n.size() == 7 && n.substr(0, 3) == "lvl" && n.substr(4) == "pPr" && n[3] >= '1' && n[3] <= '9') {
      const int i = n[3] - '1';
      out.lvl[i] = read_para_props(r);
      out.set[i] = true;
      return true;
    }
    return false;
  });
  return out;
}

// ---- text bodies --------------------------------------------------------------------

struct text_run {
  enum kind : std::uint8_t { text, line_break, slide_number } k = text;
  std::string str;
  run_props props;
};

struct text_para {
  int lvl = 0;
  para_props props;
  std::vector<text_run> runs;
  run_props end;
};

struct body_props {
  std::optional<double> l, t, r, b;
  std::optional<int> anchor;  // 0 top, 1 centre, 2 bottom
  std::optional<double> font_scale, line_reduction;
  void over(const body_props& o) {
    if (o.l) l = o.l;
    if (o.t) t = o.t;
    if (o.r) r = o.r;
    if (o.b) b = o.b;
    if (o.anchor) anchor = o.anchor;
    if (o.font_scale) font_scale = o.font_scale;
    if (o.line_reduction) line_reduction = o.line_reduction;
  }
};

struct text_body {
  bool present = false;
  body_props body;
  level_styles levels;
  std::vector<text_para> paras;
};

void read_body_props(xml::reader& r, body_props& b) {
  if (r.has("lIns")) b.l = emu_pt(r.attr("lIns"));
  if (r.has("tIns")) b.t = emu_pt(r.attr("tIns"));
  if (r.has("rIns")) b.r = emu_pt(r.attr("rIns"));
  if (r.has("bIns")) b.b = emu_pt(r.attr("bIns"));
  if (r.has("anchor")) {
    const auto a = r.attr("anchor");
    b.anchor = a == "ctr" ? 1 : a == "b" ? 2 : 0;
  }
  each_child(r, [&](std::string_view n) {
    if (n == "normAutofit") {
      if (r.has("fontScale")) b.font_scale = std::clamp(static_cast<double>(big_number(r.attr("fontScale"), 100000)) / 100000.0, 0.1, 1.0);
      if (r.has("lnSpcReduction")) b.line_reduction = std::clamp(static_cast<double>(big_number(r.attr("lnSpcReduction"), 0)) / 100000.0, 0.0, 0.9);
    }
    return false;
  });
}

text_para read_paragraph(xml::reader& r) {
  text_para p;
  each_child(r, [&](std::string_view n) {
    if (n == "pPr") {
      p.props = read_para_props(r, &p.lvl);
      return true;
    }
    if (n == "r" || n == "fld") {
      text_run run;
      if (n == "fld" && r.attr("type") == "slidenum") run.k = text_run::slide_number;
      each_child(r, [&](std::string_view m) {
        if (m == "rPr") {
          run.props = read_run_props(r);
          return true;
        }
        if (m == "t") {
          const int d = r.depth();
          for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
            if (t == token::end && r.depth() < d) break;
            if (t == token::text && run.str.size() < 65536) run.str += r.text();
          }
          return true;
        }
        return false;
      });
      if (p.runs.size() < 4096) p.runs.push_back(std::move(run));
      return true;
    }
    if (n == "br") {
      text_run run;
      run.k = text_run::line_break;
      each_child(r, [&](std::string_view m) {
        if (m == "rPr") {
          run.props = read_run_props(r);
          return true;
        }
        return false;
      });
      if (p.runs.size() < 4096) p.runs.push_back(std::move(run));
      return true;
    }
    if (n == "endParaRPr") {
      p.end = read_run_props(r);
      return true;
    }
    return false;
  });
  return p;
}

// p:txBody or a:txBody; consumes it.
text_body read_text_body(xml::reader& r) {
  text_body tb;
  tb.present = true;
  each_child(r, [&](std::string_view n) {
    if (n == "bodyPr") {
      read_body_props(r, tb.body);
      return true;
    }
    if (n == "lstStyle") {
      tb.levels = read_levels(r);
      return true;
    }
    if (n == "p") {
      if (tb.paras.size() < 4096) tb.paras.push_back(read_paragraph(r));
      else r.skip_element();
      return true;
    }
    return false;
  });
  return tb;
}

// ---- shapes -----------------------------------------------------------------------

struct box {
  bool set = false;
  double x = 0, y = 0, w = 0, h = 0;
  bool flip_h = false, flip_v = false;
};

enum class fill_kind : std::uint8_t { unset, none, solid, picture };

struct fill {
  fill_kind k = fill_kind::unset;
  colour_ref c;
  std::string rel;  // a picture fill's image
};

struct outline {
  fill_kind k = fill_kind::unset;
  colour_ref c;
  std::optional<double> w;  // points
};

struct table_cell {
  text_body text;
  fill bg;
  outline l, r, t, b;
  int span = 1, row_span = 1;
  bool merged = false;  // covered by a neighbour's span
  double mar_l = 7.2, mar_r = 7.2, mar_t = 3.6, mar_b = 3.6;
};
struct table_row {
  double h = 0;
  std::vector<table_cell> cells;
};
struct table_def {
  std::vector<double> cols;
  std::vector<table_row> rows;
};

struct shape {
  enum kind : std::uint8_t { sp, pic, cxn, table, other, group } k = sp;
  box at;
  std::string geom = "rect";
  double adj = 0.16667;  // roundRect's corner over the shorter side
  fill bg;
  outline line;
  // p:style references: idx 0 is "none".
  long long fill_ref = 0, line_ref = 0;
  colour_ref fill_ref_clr, line_ref_clr, font_ref_clr;
  bool ph = false;
  std::string ph_type;  // normalised: title, body, dt, ftr, sldNum, pic …
  long long ph_idx = -1;
  text_body text;
  std::string pic_rel;
  std::shared_ptr<table_def> tbl;
  std::vector<shape> children;
  // A group's child coordinate space.
  box ch;
};

std::string normalise_ph(std::string_view t) {
  if (t.empty() || t == "obj") return "body";
  if (t == "ctrTitle") return "title";
  if (t == "subTitle") return "body";
  return std::string(t);
}

void read_xfrm(xml::reader& r, box& b, box* ch = nullptr) {
  b.flip_h = attr_true(r.attr("flipH"));
  b.flip_v = attr_true(r.attr("flipV"));
  each_child(r, [&](std::string_view n) {
    if (n == "off") {
      b.x = emu_pt(r.attr("x"));
      b.y = emu_pt(r.attr("y"));
      b.set = true;
    } else if (n == "ext") {
      b.w = std::max(0.0, emu_pt(r.attr("cx")));
      b.h = std::max(0.0, emu_pt(r.attr("cy")));
      b.set = true;
    } else if (ch && n == "chOff") {
      ch->x = emu_pt(r.attr("x"));
      ch->y = emu_pt(r.attr("y"));
      ch->set = true;
    } else if (ch && n == "chExt") {
      ch->w = emu_pt(r.attr("cx"));
      ch->h = emu_pt(r.attr("cy"));
    }
    return false;
  });
}

outline read_outline(xml::reader& r) {
  outline o;
  if (r.has("w")) o.w = std::clamp(emu_pt(r.attr("w")), 0.0, 200.0);
  each_child(r, [&](std::string_view n) {
    if (n == "noFill") o.k = fill_kind::none;
    if (n == "solidFill" || n == "gradFill") {
      o.k = fill_kind::solid;
      o.c = read_colour(r);
      return true;
    }
    return false;
  });
  return o;
}

// spPr / grpSpPr / tcPr fills; the caller hands over each fill element.
bool read_fill(xml::reader& r, std::string_view n, fill& f) {
  if (n == "noFill") {
    f.k = fill_kind::none;
    r.skip_element();
    return true;
  }
  if (n == "solidFill" || n == "gradFill") {
    f.k = fill_kind::solid;
    f.c = read_colour(r);
    return true;
  }
  if (n == "blipFill") {
    f.k = fill_kind::picture;
    const int d = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < d) break;
      if (t == token::start && r.local() == "blip") f.rel = std::string(r.attr("r:embed"));
    }
    return true;
  }
  return false;
}

void read_sp_pr(xml::reader& r, shape& s) {
  each_child(r, [&](std::string_view n) {
    if (n == "xfrm") {
      read_xfrm(r, s.at, s.k == shape::group ? &s.ch : nullptr);
      return true;
    }
    if (n == "prstGeom") {
      s.geom = std::string(r.attr("prst"));
      each_child(r, [&](std::string_view m) {
        if (m == "gd" && r.attr("name") == "adj") {
          const auto f = r.attr("fmla");
          if (f.size() > 4 && f.substr(0, 4) == "val ") {
            s.adj = std::clamp(static_cast<double>(big_number(f.substr(4), 16667)) / 100000.0, 0.0, 0.5);
          }
        }
        return false;
      });
      return true;
    }
    if (n == "custGeom") {
      s.geom = "custom";
      return false;
    }
    if (n == "ln") {
      s.line = read_outline(r);
      return true;
    }
    return read_fill(r, n, s.bg);
  });
}

void read_style_refs(xml::reader& r, shape& s) {
  each_child(r, [&](std::string_view n) {
    if (n == "lnRef") {
      s.line_ref = big_number(r.attr("idx"), 0);
      s.line_ref_clr = read_colour(r);
      return true;
    }
    if (n == "fillRef") {
      s.fill_ref = big_number(r.attr("idx"), 0);
      s.fill_ref_clr = read_colour(r);
      return true;
    }
    if (n == "fontRef") {
      s.font_ref_clr = read_colour(r);
      return true;
    }
    return false;
  });
}

void read_nv(xml::reader& r, shape& s) {
  const int d = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < d) break;
    if (t == token::start && r.local() == "ph") {
      s.ph = true;
      s.ph_type = normalise_ph(r.attr("type"));
      s.ph_idx = r.has("idx") ? big_number(r.attr("idx"), -1) : -1;
    }
  }
}

std::shared_ptr<table_def> read_table(xml::reader& r) {
  auto tb = std::make_shared<table_def>();
  each_child(r, [&](std::string_view n) {
    if (n == "tblGrid") {
      each_child(r, [&](std::string_view m) {
        if (m == "gridCol" && tb->cols.size() < 256) tb->cols.push_back(std::max(0.0, emu_pt(r.attr("w"))));
        return false;
      });
      return true;
    }
    if (n == "tr") {
      table_row row;
      row.h = std::max(0.0, emu_pt(r.attr("h")));
      each_child(r, [&](std::string_view m) {
        if (m != "tc") return false;
        table_cell c;
        c.span = static_cast<int>(std::clamp<long long>(big_number(r.attr("gridSpan"), 1), 1, 256));
        c.row_span = static_cast<int>(std::clamp<long long>(big_number(r.attr("rowSpan"), 1), 1, 4096));
        c.merged = attr_true(r.attr("hMerge")) || attr_true(r.attr("vMerge"));
        each_child(r, [&](std::string_view k) {
          if (k == "txBody") {
            c.text = read_text_body(r);
            return true;
          }
          if (k == "tcPr") {
            if (r.has("marL")) c.mar_l = emu_pt(r.attr("marL"));
            if (r.has("marR")) c.mar_r = emu_pt(r.attr("marR"));
            if (r.has("marT")) c.mar_t = emu_pt(r.attr("marT"));
            if (r.has("marB")) c.mar_b = emu_pt(r.attr("marB"));
            each_child(r, [&](std::string_view e) {
              if (e == "lnL") c.l = read_outline(r);
              else if (e == "lnR") c.r = read_outline(r);
              else if (e == "lnT") c.t = read_outline(r);
              else if (e == "lnB") c.b = read_outline(r);
              else return read_fill(r, e, c.bg);
              return true;
            });
            return true;
          }
          return false;
        });
        if (row.cells.size() < 256) row.cells.push_back(std::move(c));
        return true;
      });
      if (tb->rows.size() < 4096) tb->rows.push_back(std::move(row));
      return true;
    }
    return false;
  });
  return tb;
}

void read_tree(xml::reader& r, std::vector<shape>& out, std::size_t& budget, int depth);

// One child of a shape tree (or a group); false when `n` is not a shape.
bool read_shape(xml::reader& r, std::string_view n, std::vector<shape>& out, std::size_t& budget, int depth) {
  if (n == "AlternateContent") {
    // The Fallback is what an older reader draws; take it when present.
    bool taken = false;
    each_child(r, [&](std::string_view m) {
      if ((m == "Fallback" || m == "Choice") && !taken) {
        std::vector<shape> got;
        read_tree(r, got, budget, depth);
        if (!got.empty() || m == "Fallback") {
          taken = true;
          for (auto& s : got) out.push_back(std::move(s));
        }
        return true;
      }
      return false;
    });
    return true;
  }
  if (n != "sp" && n != "pic" && n != "cxnSp" && n != "graphicFrame" && n != "grpSp") return false;
  if (budget == 0) {
    r.skip_element();
    return true;
  }
  --budget;
  shape s;
  s.k = n == "pic" ? shape::pic : n == "cxnSp" ? shape::cxn : n == "graphicFrame" ? shape::other
        : n == "grpSp" ? shape::group : shape::sp;
  if (s.k == shape::cxn) s.geom = "line";
  each_child(r, [&](std::string_view m) {
    if (m == "nvSpPr" || m == "nvPicPr" || m == "nvCxnSpPr" || m == "nvGraphicFramePr" || m == "nvGrpSpPr") {
      read_nv(r, s);
      return true;
    }
    if (m == "spPr" || m == "grpSpPr") {
      read_sp_pr(r, s);
      return true;
    }
    if (m == "xfrm" && s.k == shape::other) {  // p:xfrm on a graphic frame
      read_xfrm(r, s.at);
      return true;
    }
    if (m == "style") {
      read_style_refs(r, s);
      return true;
    }
    if (m == "txBody") {
      s.text = read_text_body(r);
      return true;
    }
    if (m == "blipFill") {
      fill f;
      (void)read_fill(r, m, f);
      s.pic_rel = f.rel;
      return true;
    }
    if (m == "graphic") {
      const int d = r.depth();
      for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
        if (t == token::end && r.depth() < d) break;
        if (t == token::start && r.local() == "tbl") {
          s.tbl = read_table(r);
          s.k = shape::table;
        }
      }
      return true;
    }
    if (s.k == shape::group && depth < kMaxGroupDepth) {
      return read_shape(r, m, s.children, budget, depth + 1);
    }
    return false;
  });
  out.push_back(std::move(s));
  return true;
}

void read_tree(xml::reader& r, std::vector<shape>& out, std::size_t& budget, int depth) {
  each_child(r, [&](std::string_view n) { return read_shape(r, n, out, budget, depth); });
}

// A group's children into the slide's coordinates.
void place_group(shape& g) {
  if (!g.at.set) return;
  const double sx = g.ch.w > 0 ? g.at.w / g.ch.w : 1.0;
  const double sy = g.ch.h > 0 ? g.at.h / g.ch.h : 1.0;
  for (shape& c : g.children) {
    if (c.at.set) {
      c.at.x = g.at.x + (c.at.x - g.ch.x) * sx;
      c.at.y = g.at.y + (c.at.y - g.ch.y) * sy;
      c.at.w *= sx;
      c.at.h *= sy;
    }
    if (c.k == shape::group) {
      // The child's own child space is unchanged; its box is now in slide space.
      place_group(c);
    }
  }
}

// ---- parts ----------------------------------------------------------------------

struct part {
  std::string name;
  relationships rels;
  std::vector<shape> shapes;
  fill bg;
  bool show_master_shapes = true;
  // Masters only.
  level_styles title, body, other;
  std::unordered_map<std::string, std::string> clr_map;
  std::string layout, master, theme_part;  // related parts
};

status read_part(const zip::archive& zip, const std::string& name, part& out) {
  auto bytes = zip.read(name, std::size_t{16} << 20);
  if (!bytes) return bytes.error() == status::out_of_memory ? status::out_of_memory : status::corrupt;
  out.name = name;
  out.rels = read_rels(zip, name);
  for (const auto& [id, rel] : out.rels) {
    if (rel.type == "slideLayout") out.layout = rel.target;
    else if (rel.type == "slideMaster") out.master = rel.target;
    else if (rel.type == "theme") out.theme_part = rel.target;
  }
  xml::reader r(view(*bytes));
  std::size_t budget = kMaxShapes;
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t != token::start) continue;
    const auto n = r.local();
    if (r.depth() == 1) {
      if (r.has("showMasterSp")) out.show_master_shapes = r.attr("showMasterSp") != "0" && r.attr("showMasterSp") != "false";
      continue;
    }
    if (n == "bg") {
      each_child(r, [&](std::string_view m) {
        if (m == "bgPr") {
          each_child(r, [&](std::string_view k) { return read_fill(r, k, out.bg); });
          return true;
        }
        if (m == "bgRef") {
          out.bg.k = fill_kind::solid;
          out.bg.c = read_colour(r);
          return true;
        }
        return false;
      });
    } else if (n == "spTree") {
      read_tree(r, out.shapes, budget, 0);
    } else if (n == "clrMap") {
      for (std::size_t i = 0; i < r.attribute_count(); ++i) {
        out.clr_map[std::string(r.attribute_name(i))] = std::string(r.attribute_value(i));
      }
    } else if (n == "titleStyle") {
      out.title = read_levels(r);
    } else if (n == "bodyStyle") {
      out.body = read_levels(r);
    } else if (n == "otherStyle") {
      out.other = read_levels(r);
    }
  }
  for (shape& s : out.shapes) {
    if (s.k == shape::group) place_group(s);
  }
  return status::ok;
}

void read_theme(std::string_view doc, theme& th) {
  xml::reader r(doc);
  bool in_major = false, in_minor = false;
  int scheme_depth = 0;
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end) {
      if (r.local() == "majorFont") in_major = false;
      if (r.local() == "minorFont") in_minor = false;
      if (r.local() == "clrScheme") scheme_depth = 0;
      continue;
    }
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "clrScheme") {
      scheme_depth = r.depth();
    } else if (scheme_depth && r.depth() == scheme_depth + 1) {
      const std::string key(n);
      const colour_ref c = read_colour(r);
      if (c.set && c.scheme.empty()) th.scheme[key] = c.rgb;
    } else if (n == "majorFont") {
      in_major = true;
    } else if (n == "minorFont") {
      in_minor = true;
    } else if (n == "latin" && (in_major || in_minor) && !r.attr("typeface").empty()) {
      (in_major ? th.major : th.minor) = std::string(r.attr("typeface"));
    }
  }
}

// ---- the presentation -----------------------------------------------------------------

struct presentation {
  double w = 720.0, h = 540.0;  // 10 x 7.5 in
  std::vector<std::string> slides;
  level_styles defaults;  // defaultTextStyle
};

status read_presentation(const zip::archive& zip, presentation& out) {
  auto bytes = zip.read("ppt/presentation.xml", std::size_t{16} << 20);
  if (!bytes) return bytes.error() == status::out_of_memory ? status::out_of_memory : status::corrupt;
  const relationships rels = read_rels(zip, "ppt/presentation.xml");
  xml::reader r(view(*bytes));
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "sldSz") {
      const double w = emu_pt(r.attr("cx")), h = emu_pt(r.attr("cy"));
      if (w >= 36.0 && h >= 36.0) {
        out.w = std::min(w, 4000.0);
        out.h = std::min(h, 4000.0);
      }
    } else if (n == "sldId" && out.slides.size() < kMaxSlides) {
      if (auto it = rels.find(std::string(r.attr("r:id"))); it != rels.end() && zip.find(it->second.target)) {
        out.slides.push_back(it->second.target);
      }
    } else if (n == "defaultTextStyle") {
      out.defaults = read_levels(r);
    }
  }
  return out.slides.empty() ? status::corrupt : status::ok;
}

// ---- drawing ------------------------------------------------------------------------

std::string roman(long long v, bool upper) {
  static constexpr std::pair<int, const char*> k[] = {{1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"},
                                                      {100, "c"},  {90, "xc"},  {50, "l"},  {40, "xl"},
                                                      {10, "x"},   {9, "ix"},   {5, "v"},   {4, "iv"},
                                                      {1, "i"}};
  std::string out;
  v = std::clamp<long long>(v, 1, 3999);
  for (const auto& [n, s] : k) {
    while (v >= n) {
      out += s;
      v -= n;
    }
  }
  if (upper) {
    for (char& c : out) c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}
std::string auto_number(const std::string& scheme, long long n) {
  std::string body;
  const bool alpha = scheme.rfind("alpha", 0) == 0, rom = scheme.rfind("roman", 0) == 0;
  if (alpha) {
    const bool upper = scheme.rfind("alphaUc", 0) == 0;
    body = std::string(1, static_cast<char>((upper ? 'A' : 'a') + (n - 1) % 26));
  } else if (rom) {
    body = roman(n, scheme.rfind("romanUc", 0) == 0);
  } else {
    body = std::to_string(n);
  }
  if (scheme.find("ParenBoth") != std::string::npos) return "(" + body + ")";
  if (scheme.find("ParenR") != std::string::npos) return body + ")";
  if (scheme.find("Plain") != std::string::npos) return body;
  return body + ".";
}

class renderer {
 public:
  renderer(const zip::archive& zip, const presentation& pres, text::engine& eng, const job_context* ctx)
      : zip_(zip), pres_(pres), eng_(eng), ctx_(ctx) {}

  // Lays slide `index` out into `out`.
  status build(std::uint32_t index, dl::display& out) {
    part slide;
    if (const status read = read_part(zip_, pres_.slides[index], slide); read != status::ok) return read;
    const part* layout = cached(slide.layout);
    const part* master = layout ? cached(layout->master) : nullptr;
    const theme* th = master ? cached_theme(master->theme_part) : nullptr;
    pal_.th = th ? th : &fallback_theme_;
    pal_.map = master ? master->clr_map : std::unordered_map<std::string, std::string>{};
    major_ = pal_.th->major;
    minor_ = pal_.th->minor;
    layout_ = layout;
    master_ = master;
    number_ = index + 1;

    // The background: the slide's, else the layout's, else the master's.
    const part* bg_from = slide.bg.k != fill_kind::unset ? &slide
                          : layout && layout->bg.k != fill_kind::unset ? layout
                          : master;
    if (bg_from && bg_from->bg.k == fill_kind::solid) {
      const colour c = bg_from->bg.c.resolve(pal_);
      if (c.set) out.add(dl::display::rect{0, 0, pres_.w, pres_.h, c.rgb});
    } else if (bg_from && bg_from->bg.k == fill_kind::picture) {
      out.add(dl::display::image{image(*bg_from, bg_from->bg.rel), 0, 0, pres_.w, pres_.h});
    }
    // The master's and the layout's own art, under the slide's.
    if (master && slide.show_master_shapes && (!layout || layout->show_master_shapes)) {
      draw_list(*master, master->shapes, out, false);
    }
    if (layout && slide.show_master_shapes) draw_list(*layout, layout->shapes, out, false);
    draw_list(slide, slide.shapes, out, true);
    return status::ok;
  }

 private:
  const part* cached(const std::string& name) {
    if (name.empty()) return nullptr;
    if (auto it = parts_.find(name); it != parts_.end()) return it->second.get();
    auto p = std::make_unique<part>();
    if (read_part(zip_, name, *p) != status::ok) p.reset();
    const part* raw = p.get();
    parts_.emplace(name, std::move(p));
    return raw;
  }
  const theme* cached_theme(const std::string& name) {
    if (name.empty()) return nullptr;
    if (auto it = themes_.find(name); it != themes_.end()) return &it->second;
    theme th;
    if (auto bytes = zip_.read(name, std::size_t{8} << 20)) read_theme(view(*bytes), th);
    return &themes_.emplace(name, std::move(th)).first->second;
  }

  std::shared_ptr<const raster> image(const part& owner, const std::string& rel) {
    auto it = owner.rels.find(rel);
    if (it == owner.rels.end()) return nullptr;
    const std::string& name = it->second.target;
    if (auto hit = images_.find(name); hit != images_.end()) return hit->second;
    std::shared_ptr<const raster> pixels;
    if (images_loaded_ < kMaxImages) {
      ++images_loaded_;
      if (auto bytes = zip_.read(name, kMaxImageBytes)) {
        // Never a document inside a document: only the still formats.
        const auto family = probe(bytes.value());
        if (family != format_family::pdf && family != format_family::docx && family != format_family::pptx &&
            family != format_family::unknown) {
          if (auto decoded = decode(bytes.value(), ctx_, 1)) {
            pixels = std::make_shared<const raster>(std::move(decoded).value());
          }
        }
      }
    }
    images_.emplace(name, pixels);
    return pixels;
  }

  // The layout's and master's placeholder a slide's placeholder inherits from.
  static const shape* match(const part* p, const shape& s, bool by_idx) {
    if (!p) return nullptr;
    if (by_idx && s.ph_idx >= 0) {
      for (const shape& c : p->shapes) {
        if (c.ph && c.ph_idx == s.ph_idx) return &c;
      }
    }
    for (const shape& c : p->shapes) {
      if (c.ph && c.ph_type == s.ph_type) return &c;
    }
    return nullptr;
  }

  void draw_list(const part& owner, const std::vector<shape>& list, dl::display& out, bool is_slide) {
    for (const shape& s : list) {
      if (ctx_ && ctx_->cancelled()) return;
      // A layout's or master's placeholder is a prompt the slide fills, not art.
      if (s.ph && !is_slide) continue;
      if (s.k == shape::group) {
        draw_list(owner, s.children, out, is_slide);
        continue;
      }
      const shape* lay = nullptr;
      const shape* mas = nullptr;
      if (s.ph && is_slide) {
        lay = match(layout_, s, true);
        mas = match(master_, lay ? *lay : s, false);
      }
      draw_shape(owner, s, lay, mas, out);
    }
  }

  void draw_shape(const part& owner, const shape& s, const shape* lay, const shape* mas, dl::display& out) {
    box at = s.at;
    if (!at.set && lay) at = lay->at;
    if (!at.set && mas) at = mas->at;
    if (!at.set || !(at.w >= 0.0) || !(at.h >= 0.0)) return;
    ++out.z;

    if (s.k == shape::table && s.tbl) {
      draw_table(*s.tbl, at, out);
      return;
    }
    if (s.k == shape::other) {
      // A chart, SmartArt or media: its box.
      dl::shape_style st;
      st.filled = true;
      st.fill = 0xF0F0F0;
      st.stroked = true;
      st.stroke = 0xBFBFBF;
      out.add(dl::display::shape{st, at.x, at.y, at.w, at.h});
      return;
    }

    // Geometry, fill and outline: the slide's own, else the inherited, else the style's.
    const shape* geo_from = &s;
    const fill* f = &s.bg;
    if (f->k == fill_kind::unset && lay) f = &lay->bg;
    if (f->k == fill_kind::unset && mas) f = &mas->bg;
    const outline* ln = &s.line;
    if (ln->k == fill_kind::unset && !ln->w && lay) ln = &lay->line;
    if (ln->k == fill_kind::unset && !ln->w && mas) ln = &mas->line;

    dl::shape_style st;
    const std::string& g = geo_from->geom;
    st.geom = g == "roundRect" ? dl::geometry::round_rect
              : (g == "ellipse" || g == "flowChartConnector") ? dl::geometry::ellipse
              : (g == "line" || g.rfind("straightConnector", 0) == 0 || g.rfind("bentConnector", 0) == 0 ||
                 g.rfind("curvedConnector", 0) == 0 || s.k == shape::cxn)
                  ? dl::geometry::line
                  : dl::geometry::rect;
    st.radius = geo_from->adj;
    st.flip_h = at.flip_h;
    st.flip_v = at.flip_v;
    if (f->k == fill_kind::solid) {
      const colour c = f->c.resolve(pal_);
      if (c.set) {
        st.filled = true;
        st.fill = c.rgb;
        st.fill_alpha = c.alpha;
      }
    } else if (f->k == fill_kind::unset && s.fill_ref > 0 && s.k == shape::sp) {
      const colour c = s.fill_ref_clr.resolve(pal_);
      if (c.set) {
        st.filled = true;
        st.fill = c.rgb;
        st.fill_alpha = c.alpha;
      }
    }
    if (ln->k == fill_kind::solid) {
      const colour c = ln->c.resolve(pal_);
      if (c.set) {
        st.stroked = true;
        st.stroke = c.rgb;
      }
    } else if (ln->k == fill_kind::unset && s.line_ref > 0) {
      const colour c = s.line_ref_clr.resolve(pal_);
      if (c.set) {
        st.stroked = true;
        st.stroke = c.rgb;
      }
    }
    st.stroke_w = ln->w ? std::max(0.25, *ln->w) : 0.75;

    if (s.k == shape::pic || f->k == fill_kind::picture) {
      const std::string& rel = s.k == shape::pic ? s.pic_rel : f->rel;
      out.add(dl::display::image{image(owner, rel), at.x, at.y, at.w, at.h});
      if (st.stroked) {
        st.filled = false;
        out.add(dl::display::shape{st, at.x, at.y, at.w, at.h});
      }
    } else if (st.filled || st.stroked) {
      out.add(dl::display::shape{st, at.x, at.y, at.w, at.h});
    }

    if (s.text.present && !s.text.paras.empty()) {
      // The text styles this shape inherits, outermost first.
      level_styles levels;
      if (s.ph) {
        if (master_) {
          levels = s.ph_type == "title" ? master_->title
                   : s.ph_type == "body" ? master_->body
                                         : master_->other;
        }
      } else {
        levels = pres_.defaults;
      }
      body_props body;
      body.l = 7.2;
      body.r = 7.2;
      body.t = 3.6;
      body.b = 3.6;
      for (const shape* from : {mas, lay}) {
        if (!from) continue;
        levels.over(from->text.levels);
        body.over(from->text.body);
      }
      levels.over(s.text.levels);
      body.over(s.text.body);
      const colour font_clr = s.font_ref_clr.resolve(pal_);
      draw_text(s.text, levels, body, s.ph && s.ph_type == "title", font_clr, at.x, at.y, at.w, at.h, out);
    }
  }

  // A text body's paragraphs in the layout's model.
  std::vector<dl::para> paragraphs(const text_body& tb, const level_styles& levels, const body_props& body,
                                   bool title, const colour& font_clr) {
    const double scale = body.font_scale.value_or(1.0);
    const double reduce = body.line_reduction.value_or(0.0);

    std::vector<dl::para> paras;
    paras.reserve(tb.paras.size());
    long long counters[9] = {};
    std::string schemes[9];
    for (const text_para& tp : tb.paras) {
      const int lvl = std::clamp(tp.lvl, 0, 8);
      para_props pp = levels.lvl[lvl];
      pp.over(tp.props);
      run_props base = pp.def;
      const auto style_of = [&](const run_props& rp) {
        run_props m = base;
        m.over(rp);
        text::style st;
        std::string font = m.font.value_or(title ? "+mj-lt" : "+mn-lt");
        if (font.rfind("+mj", 0) == 0) font = major_;
        else if (font.rfind("+mn", 0) == 0) font = minor_;
        st.family = font;
        st.size_pt = std::clamp(m.size.value_or(18.0) * scale, 1.0, 4000.0);
        st.bold = m.bold.value_or(false);
        st.italic = m.italic.value_or(false);
        st.underline = m.underline.value_or(false);
        st.strike = m.strike.value_or(false);
        colour c;
        if (m.fill) c = m.fill->resolve(pal_);
        else if (font_clr.set) c = font_clr;
        else {
          std::uint32_t tx = 0;
          if (pal_.lookup("tx1", tx)) c = {true, tx, 1.0};
        }
        st.rgb = c.set ? c.rgb : 0x000000;
        return std::pair<text::style, double>{st, m.baseline.value_or(0.0)};
      };

      dl::para p;
      p.jc = pp.align.value_or(dl::align::start);
      p.rtl = pp.rtl.value_or(false);
      p.ind_start = pp.mar_l.value_or(0.0);
      p.ind_first = pp.indent.value_or(0.0);
      p.mark = style_of(tp.end).first;
      const double size = p.mark.size_pt;
      if (pp.line_pts) {
        p.rule = dl::line_rule::exact;
        p.line = *pp.line_pts;
      } else {
        p.rule = dl::line_rule::automatic;
        p.line = 240.0 * std::max(0.1, pp.line_pct.value_or(1.0) - reduce);
      }
      p.before = pp.before_pts ? *pp.before_pts : pp.before_pct.value_or(0.0) * size * 1.2;
      p.after = pp.after_pts ? *pp.after_pts : pp.after_pct.value_or(0.0) * size * 1.2;

      bool has_text = false;
      for (const text_run& run : tp.runs) {
        auto [st, raise] = style_of(run.props);
        if (run.k == text_run::line_break) {
          dl::span sp;
          sp.k = dl::span::line_break;
          sp.style = st;
          p.spans.push_back(std::move(sp));
          continue;
        }
        std::string str = run.k == text_run::slide_number ? std::to_string(number_) : run.str;
        if (!str.empty()) has_text = true;
        if (p.spans.empty() && !tp.runs.empty()) p.mark = st;
        // Tabs are their own spans.
        std::size_t from = 0;
        for (std::size_t i = 0; i <= str.size(); ++i) {
          if (i < str.size() && str[i] != '\t') continue;
          if (i > from) {
            dl::span sp;
            sp.str = str.substr(from, i - from);
            sp.style = st;
            sp.raise = raise * st.size_pt;
            if (raise != 0.0) sp.style.size_pt *= 0.7;
            sp.rtl = p.rtl;
            p.spans.push_back(std::move(sp));
          }
          if (i < str.size()) {
            dl::span tab;
            tab.k = dl::span::tab;
            tab.style = st;
            p.spans.push_back(std::move(tab));
          }
          from = i + 1;
        }
      }

      // Bullets and numbers: only on a paragraph with text.
      const bullet_kind bk = pp.bullet.value_or(bullet_kind::none);
      if (bk == bullet_kind::number) {
        const std::string scheme = pp.bu_scheme.value_or("arabicPeriod");
        if (schemes[lvl] != scheme) {
          schemes[lvl] = scheme;
          counters[lvl] = 0;
        }
        if (counters[lvl] == 0) counters[lvl] = pp.bu_start.value_or(1) - 1;
      }
      for (int deeper = lvl + 1; deeper < 9; ++deeper) counters[deeper] = 0;
      if (has_text && bk != bullet_kind::none) {
        text::style ls = style_of(tp.runs.empty() ? tp.end : tp.runs.front().props).first;
        ls.underline = ls.strike = false;
        if (pp.bu_size_pct) ls.size_pt *= *pp.bu_size_pct;
        if (pp.bu_clr) {
          const colour c = pp.bu_clr->resolve(pal_);
          if (c.set) ls.rgb = c.rgb;
        }
        if (bk == bullet_kind::character) {
          std::string ch = pp.bu_char.value_or("\xE2\x80\xA2");
          // Symbol fonts' private-use bullets (Wingdings "§", Symbol "·") as a plain bullet.
          const std::string bf = pp.bu_font.value_or("");
          if (bf == "Wingdings" || bf == "Symbol" || bf.rfind("Wingdings", 0) == 0) ch = "\xE2\x80\xA2";
          else if (!bf.empty() && bf[0] != '+') ls.family = bf;
          p.label = ch.empty() ? std::string("\xE2\x80\xA2") : ch;
        } else {
          p.label = auto_number(schemes[lvl], ++counters[lvl]);
        }
        p.label_style = ls;
      }
      paras.push_back(std::move(p));
    }
    return paras;
  }

  // How tall `paras` are between x0 and x1.
  double height(const std::vector<dl::para>& paras, double x0, double x1) {
    dl::box_frame measure(x0, x1, nullptr);
    double end = 0.0;
    for (const dl::para& p : paras) end = dl::lay_paragraph(eng_, p, measure, end);
    return end;
  }

  // A text body in the box (x, y, w, h), measured and then anchored.
  void draw_text(const text_body& tb, const level_styles& levels, const body_props& body, bool title,
                 const colour& font_clr, double x, double y, double w, double h, dl::display& out) {
    const double l = body.l.value_or(7.2), r = body.r.value_or(7.2);
    const double t = body.t.value_or(3.6), b = body.b.value_or(3.6);
    const double x0 = x + l, x1 = std::max(x0 + 4.0, x + w - r);
    const std::vector<dl::para> paras = paragraphs(tb, levels, body, title, font_clr);
    const double end = height(paras, x0, x1);
    const double area = std::max(0.0, h - t - b);
    const int anchor = body.anchor.value_or(0);
    double top = y + t;
    if (anchor == 1) top += (area - end) / 2.0;
    else if (anchor == 2) top += area - end;
    dl::box_frame frame(x0, x1, &out);
    double at = top;
    for (const dl::para& p : paras) {
      if (ctx_ && ctx_->cancelled()) return;
      at = dl::lay_paragraph(eng_, p, frame, at);
    }
  }

  void draw_table(const table_def& tb, const box& at, dl::display& out) {
    if (tb.cols.empty()) return;
    std::vector<double> xs{at.x};
    for (double c : tb.cols) xs.push_back(xs.back() + c);
    level_styles none = pres_.defaults;
    std::uint32_t tx = 0x000000;
    (void)pal_.lookup("tx1", tx);
    const colour font_clr{true, tx, 1.0};
    double y = at.y;
    for (const table_row& row : tb.rows) {
      if (ctx_ && ctx_->cancelled()) return;
      // The row is as tall as its tallest cell's text.
      double rh = row.h;
      std::size_t col = 0;
      for (const table_cell& c : row.cells) {
        const std::size_t span = static_cast<std::size_t>(c.span);
        if (col >= tb.cols.size()) break;
        const double x0 = xs[col] + c.mar_l, x1 = std::max(x0 + 4.0, xs[std::min(col + span, tb.cols.size())] - c.mar_r);
        if (!c.merged && c.row_span == 1 && c.text.present) {
          level_styles lv = none;
          lv.over(c.text.levels);
          const double end = height(paragraphs(c.text, lv, c.text.body, false, font_clr), x0, x1);
          rh = std::max(rh, end + c.mar_t + c.mar_b);
        }
        col += span;
      }
      col = 0;
      for (const table_cell& c : row.cells) {
        const std::size_t span = static_cast<std::size_t>(c.span);
        if (col >= tb.cols.size()) break;
        const double cx = xs[col], cw = xs[std::min(col + span, tb.cols.size())] - cx;
        if (!c.merged) {
          if (c.bg.k == fill_kind::solid) {
            const colour bg = c.bg.c.resolve(pal_);
            if (bg.set) out.add(dl::display::rect{cx, y, cw, rh, bg.rgb});
          }
          if (c.text.present) {
            body_props bp;
            bp.l = c.mar_l;
            bp.r = c.mar_r;
            bp.t = c.mar_t;
            bp.b = c.mar_b;
            bp.over(c.text.body);
            level_styles lv = none;
            lv.over(c.text.levels);
            draw_text(c.text, lv, bp, false, font_clr, cx, y, cw, rh, out);
          }
        }
        // Borders: the cell's own, else a light rule so the grid reads.
        const auto edge = [&](const outline& o, double x, double yy, double w, double h) {
          std::uint32_t rgb = 0xBFBFBF;
          double width = 0.75;
          if (o.k == fill_kind::none) return;
          if (o.k == fill_kind::solid) {
            const colour c2 = o.c.resolve(pal_);
            if (c2.set) rgb = c2.rgb;
          }
          if (o.w) width = std::max(0.25, *o.w);
          out.add(dl::display::rect{x - (w == 0 ? width / 2 : 0), yy - (h == 0 ? width / 2 : 0),
                                    w == 0 ? width : w, h == 0 ? width : h, rgb});
        };
        edge(c.t, cx, y, cw, 0);
        edge(c.b, cx, y + rh, cw, 0);
        edge(c.l, cx, y, 0, rh);
        edge(c.r, cx + cw, y, 0, rh);
        col += span;
      }
      y += rh;
    }
  }

  const zip::archive& zip_;
  const presentation& pres_;
  text::engine& eng_;
  const job_context* ctx_;
  palette pal_;
  theme fallback_theme_;
  std::string major_, minor_;
  const part* layout_ = nullptr;
  const part* master_ = nullptr;
  std::uint32_t number_ = 1;
  std::map<std::string, std::unique_ptr<part>> parts_;
  std::map<std::string, theme> themes_;
  std::unordered_map<std::string, std::shared_ptr<const raster>> images_;
  int images_loaded_ = 0;
};

}  // namespace

result<std::uint32_t> decode_pptx_pages(std::span<const std::uint8_t> bytes,
                                        std::span<const std::uint32_t> pages, std::uint32_t long_edge,
                                        const job_context* ctx, void* user, page_sink sink) {
  if (probe(bytes) != format_family::pptx) return err(status::unsupported_format);
  long_edge = std::clamp<std::uint32_t>(long_edge, 16, kPptxLongEdge);
  MV_TRY(zip::archive zip, zip::archive::open(bytes));
  presentation pres;
  if (const status s = read_presentation(zip, pres); s != status::ok) return err(s);
  if (ctx && ctx->cancelled()) return err(status::cancelled);
  const auto count = static_cast<std::uint32_t>(pres.slides.size());

  text::engine engine;
  renderer render(zip, pres, engine, ctx);
  const double scale = static_cast<double>(long_edge) / std::max(pres.w, pres.h);
  for (const std::uint32_t page : pages) {
    if (page >= count) continue;
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    dl::display d;
    const status built = render.build(page, d);
    if (built == status::cancelled || built == status::out_of_memory) return err(built);
    raster out;
    out.format = format_family::pptx;
    out.width = static_cast<std::uint32_t>(std::max(1.0, std::round(pres.w * scale)));
    out.height = static_cast<std::uint32_t>(std::max(1.0, std::round(pres.h * scale)));
    out.tagged_srgb = true;
    out.page = page;
    out.page_count = count;
    try {
      out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4u, 255);
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
    // A slide that would not read is drawn white, so the others still come.
    if (built == status::ok) {
      const status painted = dl::paint(d, engine, out, scale, std::to_string(page + 1), std::to_string(count), ctx);
      if (painted != status::ok) return err(painted);
    }
    if (sink && !sink(user, std::move(out))) break;
  }
  return count;
}

result<raster> decode_pptx(std::span<const std::uint8_t> bytes, std::uint32_t page, const job_context* ctx,
                           std::uint32_t long_edge) {
  std::optional<raster> got;
  const std::uint32_t pages[] = {page};
  auto count = decode_pptx_pages(bytes, pages, long_edge, ctx, &got, [](void* user, raster&& r) {
    static_cast<std::optional<raster>*>(user)->emplace(std::move(r));
    return false;
  });
  if (!count) return err(count.error());
  if (!got) return err(status::invalid_arg);
  return std::move(*got);
}

}  // namespace mv::codec
