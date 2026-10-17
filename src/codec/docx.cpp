// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// DOCX (docs/plans/audio-and-documents.md §2.5, issue #429): the
// WordprocessingML body laid out by codec/ itself and drawn with codec/text —
// readable pages, not a Word replica. What it draws:
//
//   paragraphs with their style chain (docDefaults, basedOn, direct), runs with
//   font, size, bold, italic, underline, strike, colour, caps, super/subscript;
//   alignment incl. justify, indents, spacing, line spacing (auto / exact /
//   at least); numbered and bulleted lists; tabs (every half inch); line,
//   column and page breaks, page-break-before; sections (next page, continuous,
//   even / odd page, next column) with their page size and margins; columns
//   (equal or explicit widths, separator line, balanced before a continuous
//   break); headers and footers (default / first / even) with PAGE and NUMPAGES
//   fields; footnotes at the foot of the page that cites them, endnotes at the
//   end; inline pictures and shapes; floating pictures and shapes placed by
//   their anchor, with text wrapped around them (square, tight and through as
//   their box, top and bottom) or not (none, behind text); text boxes (DrawingML
//   and the VML fallback) with their fill, outline and paragraphs; tables with
//   grid widths, spans and borders; right-to-left paragraphs and runs.
//
// What it does not: comments, hyphenation, grouped shapes and drawing canvases,
// charts and SmartArt (a grey box), wrap polygons (a tight wrap is its box),
// tab stops other than the default, a footnote split across pages (it moves
// whole), table cells broken across pages. Where a page or line breaks can
// differ from Word's.
//
// Layout is in points and does not depend on the drawing size, so a preview
// and the full page agree on the page count. Every page is laid out; only the
// pages asked for keep what they draw. The shared paragraph and shape code is
// codec/doclayout.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

// ---- units ---------------------------------------------------------------------

double twips(std::string_view v) noexcept {  // 1/20 pt
  if (v.empty()) return 0.0;
  char buf[32];
  const std::size_t n = std::min(v.size(), sizeof buf - 1);
  std::memcpy(buf, v.data(), n);
  buf[n] = '\0';
  const double out = std::strtod(buf, nullptr) / 20.0;
  return std::isfinite(out) ? out : 0.0;
}
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
long number(std::string_view v, long fallback = 0) noexcept {
  const long long out = big_number(v, fallback);
  return static_cast<long>(std::clamp<long long>(out, -2000000000LL, 2000000000LL));
}
double emu(std::string_view v) noexcept {  // 12700 to the point
  return static_cast<double>(big_number(v, 0)) / 12700.0;
}
// w:val on/off: absent or anything but false/0/off is on.
bool on(const xml::reader& r) noexcept {
  const auto v = r.attr("w:val");
  return !(v == "false" || v == "0" || v == "off" || v == "none");
}
bool attr_true(std::string_view v) noexcept { return v == "1" || v == "true" || v == "t" || v == "on"; }
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

// The text inside the element just started (posOffset, align); consumes it.
std::string element_text(xml::reader& r) {
  std::string out;
  const int d = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < d) break;
    if (t == token::text && out.size() < 64) out += r.text();
  }
  return out;
}

std::string_view trim(std::string_view s) noexcept {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// ---- properties ------------------------------------------------------------------

enum class valign : std::uint8_t { baseline, super, sub };

// A set of run properties, any of which may be unset; styles merge them.
struct rpr {
  std::optional<std::string> family;
  std::optional<std::string> family_cs;  // complex script (Arabic, Hebrew) runs
  std::optional<std::string> theme;      // "minor" / "major"
  std::optional<double> size, size_cs;
  std::optional<bool> bold, italic, underline, strike, caps, hidden, rtl;
  std::optional<std::uint32_t> rgb;
  std::optional<valign> va;

  void over(const rpr& o) {
    if (o.family) { family = o.family; theme.reset(); }
    if (o.theme) { theme = o.theme; family.reset(); }
    if (o.family_cs) family_cs = o.family_cs;
    if (o.size) size = o.size;
    if (o.size_cs) size_cs = o.size_cs;
    if (o.bold) bold = o.bold;
    if (o.italic) italic = o.italic;
    if (o.underline) underline = o.underline;
    if (o.strike) strike = o.strike;
    if (o.caps) caps = o.caps;
    if (o.hidden) hidden = o.hidden;
    if (o.rtl) rtl = o.rtl;
    if (o.rgb) rgb = o.rgb;
    if (o.va) va = o.va;
  }
  bool operator==(const rpr&) const = default;
};

enum class line_rule : std::uint8_t { automatic, exact, at_least };

struct ppr {
  std::optional<dl::align> jc;  // start / end: left / right in a left-to-right paragraph
  std::optional<double> before, after, line;  // line: 240ths for auto, pt otherwise
  std::optional<line_rule> rule;
  std::optional<double> ind_left, ind_right, ind_first;  // first < 0 is a hanging indent
  std::optional<bool> page_break_before, bidi;
  std::optional<long> num_id;
  std::optional<long> ilvl;

  void over(const ppr& o) {
    if (o.jc) jc = o.jc;
    if (o.before) before = o.before;
    if (o.after) after = o.after;
    if (o.line) line = o.line;
    if (o.rule) rule = o.rule;
    if (o.ind_left) ind_left = o.ind_left;
    if (o.ind_right) ind_right = o.ind_right;
    if (o.ind_first) ind_first = o.ind_first;
    if (o.page_break_before) page_break_before = o.page_break_before;
    if (o.bidi) bidi = o.bidi;
    if (o.num_id) num_id = o.num_id;
    if (o.ilvl) ilvl = o.ilvl;
  }
};

// One run-property element just started (rFonts, b, sz, …); consumes it.
rpr rpr_property(xml::reader& r) {
  rpr out;
  const auto n = r.local();
  if (n == "rFonts") {
    const auto ascii = r.attr("w:ascii").empty() ? r.attr("w:hAnsi") : r.attr("w:ascii");
    const auto theme = r.attr("w:asciiTheme").empty() ? r.attr("w:hAnsiTheme") : r.attr("w:asciiTheme");
    if (!theme.empty()) out.theme = theme.substr(0, 5) == "major" ? "major" : "minor";
    else if (!ascii.empty()) out.family = std::string(ascii);
    if (!r.attr("w:cs").empty()) out.family_cs = std::string(r.attr("w:cs"));
  } else if (n == "b") out.bold = on(r);
  else if (n == "i") out.italic = on(r);
  else if (n == "u") out.underline = on(r);
  else if (n == "strike" || n == "dstrike") out.strike = on(r);
  else if (n == "caps" || n == "smallCaps") out.caps = on(r);
  else if (n == "vanish") out.hidden = on(r);
  else if (n == "rtl") out.rtl = on(r);
  else if (n == "sz" || n == "szCs") {
    const long hp = number(r.attr("w:val"), 0);
    if (hp > 0 && hp < 3200) (n == "sz" ? out.size : out.size_cs) = static_cast<double>(hp) / 2.0;
  } else if (n == "color") {
    std::uint32_t c = 0;
    if (r.attr("w:val") == "auto") out.rgb = 0x000000;
    else if (parse_rgb(r.attr("w:val"), c)) out.rgb = c;
  } else if (n == "vertAlign") {
    const auto v = r.attr("w:val");
    out.va = v == "superscript" ? valign::super : v == "subscript" ? valign::sub : valign::baseline;
  }
  r.skip_element();
  return out;
}

// Reads the children of the rPr just started. `char_style` gets an rStyle.
rpr read_rpr(xml::reader& r, std::string* char_style = nullptr) {
  rpr out;
  const int depth = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < depth) break;
    if (t != token::start) continue;
    if (r.local() == "rStyle") {
      if (char_style) *char_style = std::string(r.attr("w:val"));
      r.skip_element();
      continue;
    }
    out.over(rpr_property(r));
  }
  return out;
}

// ---- sections ------------------------------------------------------------------

enum class sect_start : std::uint8_t { next_page, continuous, next_column, even_page, odd_page };

// One section: a run of blocks with its page, margins, columns, headers and
// footers. Header / footer slots: 0 default, 1 first, 2 even.
struct section {
  double page_w = 612.0, page_h = 792.0;  // US Letter until sectPr says
  double margin_t = 72.0, margin_b = 72.0, margin_l = 72.0, margin_r = 72.0;
  bool top_fixed = false, bottom_fixed = false;  // a negative margin: headers do not push the body
  double header_d = 36.0, footer_d = 36.0;
  int cols = 1;
  double col_space = 36.0;
  bool col_sep = false, col_equal = true;
  std::vector<std::pair<double, double>> col_w;  // width, space after
  sect_start type = sect_start::next_page;
  bool title_pg = false;
  long page_start = -1;
  std::string num_fmt = "decimal";
  std::string header_rel[3], footer_rel[3];
  int header[3] = {-1, -1, -1}, footer[3] = {-1, -1, -1};  // stories
  std::size_t first = 0, end = 0;                         // blocks [first, end)

  [[nodiscard]] double text_l() const noexcept { return margin_l; }
  [[nodiscard]] double text_r() const noexcept { return std::max(margin_l + 36.0, page_w - margin_r); }
};

constexpr int kMaxColumns = 16;

int hf_slot(std::string_view type) noexcept { return type == "first" ? 1 : type == "even" ? 2 : 0; }

// The sectPr just started; consumes it.
void read_section(xml::reader& r, section& s) {
  const int depth = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < depth) break;
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "pgSz") {
      const double w = twips(r.attr("w:w")), h = twips(r.attr("w:h"));
      if (w > 72 && h > 72 && w < 14400 && h < 14400) {
        s.page_w = w;
        s.page_h = h;
      }
    } else if (n == "pgMar") {
      const double top = twips(r.attr("w:top")), bottom = twips(r.attr("w:bottom"));
      s.top_fixed = top < 0;
      s.bottom_fixed = bottom < 0;
      s.margin_t = std::clamp(std::fabs(top), 0.0, s.page_h / 3);
      s.margin_b = std::clamp(std::fabs(bottom), 0.0, s.page_h / 3);
      s.margin_l = std::clamp(twips(r.attr("w:left")), 0.0, s.page_w / 3);
      s.margin_r = std::clamp(twips(r.attr("w:right")), 0.0, s.page_w / 3);
      if (r.has("w:header")) s.header_d = std::clamp(twips(r.attr("w:header")), 0.0, s.page_h / 3);
      if (r.has("w:footer")) s.footer_d = std::clamp(twips(r.attr("w:footer")), 0.0, s.page_h / 3);
    } else if (n == "cols") {
      s.cols = static_cast<int>(std::clamp<long>(number(r.attr("w:num"), 1), 1, kMaxColumns));
      if (r.has("w:space")) s.col_space = std::clamp(twips(r.attr("w:space")), 0.0, 720.0);
      s.col_sep = attr_true(r.attr("w:sep"));
      s.col_equal = !(r.attr("w:equalWidth") == "0" || r.attr("w:equalWidth") == "false");
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u == token::start && r.local() == "col" && s.col_w.size() < kMaxColumns) {
          s.col_w.emplace_back(std::clamp(twips(r.attr("w:w")), 0.0, 14400.0),
                               std::clamp(twips(r.attr("w:space")), 0.0, 720.0));
        }
      }
      continue;
    } else if (n == "type") {
      const auto v = r.attr("w:val");
      s.type = v == "continuous" ? sect_start::continuous : v == "nextColumn" ? sect_start::next_column
               : v == "evenPage" ? sect_start::even_page : v == "oddPage" ? sect_start::odd_page
                                                         : sect_start::next_page;
    } else if (n == "titlePg") {
      s.title_pg = on(r);
    } else if (n == "pgNumType") {
      if (r.has("w:start")) s.page_start = std::clamp<long>(number(r.attr("w:start"), 1), 0, 100000);
      if (!r.attr("w:fmt").empty()) s.num_fmt = std::string(r.attr("w:fmt"));
    } else if (n == "headerReference" || n == "footerReference") {
      (n == "headerReference" ? s.header_rel : s.footer_rel)[hf_slot(r.attr("w:type"))] = std::string(r.attr("r:id"));
    }
    if (r.depth() > depth) r.skip_element();
  }
}

struct ppr_read {
  ppr p;
  std::string style;
  bool section_break = false;
};

// `sect` takes a sectPr in the paragraph (the section it ends), if given.
ppr_read read_ppr(xml::reader& r, section* sect = nullptr) {
  ppr_read out;
  const int depth = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < depth) break;
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "pStyle") out.style = std::string(r.attr("w:val"));
    else if (n == "jc") {
      const auto v = r.attr("w:val");
      out.p.jc = v == "center" ? dl::align::center
                 : (v == "right" || v == "end") ? dl::align::end
                 : (v == "both" || v == "distribute") ? dl::align::justify
                                                     : dl::align::start;
    } else if (n == "bidi") {
      out.p.bidi = on(r);
    } else if (n == "spacing") {
      if (r.has("w:before")) out.p.before = twips(r.attr("w:before"));
      if (r.has("w:after")) out.p.after = twips(r.attr("w:after"));
      if (r.has("w:line")) {
        const auto rule = r.attr("w:lineRule");
        out.p.rule = rule == "exact" ? line_rule::exact : rule == "atLeast" ? line_rule::at_least
                                                                           : line_rule::automatic;
        out.p.line = *out.p.rule == line_rule::automatic ? static_cast<double>(number(r.attr("w:line"), 240))
                                                          : twips(r.attr("w:line"));
      }
    } else if (n == "ind") {
      const auto left = r.has("w:left") ? r.attr("w:left") : r.attr("w:start");
      const auto right = r.has("w:right") ? r.attr("w:right") : r.attr("w:end");
      if (!left.empty()) out.p.ind_left = twips(left);
      if (!right.empty()) out.p.ind_right = twips(right);
      if (r.has("w:hanging")) out.p.ind_first = -twips(r.attr("w:hanging"));
      else if (r.has("w:firstLine")) out.p.ind_first = twips(r.attr("w:firstLine"));
      else if (r.has("w:first-line")) out.p.ind_first = twips(r.attr("w:first-line"));  // Cocoa's spelling
    } else if (n == "pageBreakBefore") {
      out.p.page_break_before = on(r);
    } else if (n == "numPr") {
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u != token::start) continue;
        if (r.local() == "numId") out.p.num_id = number(r.attr("w:val"), 0);
        else if (r.local() == "ilvl") out.p.ilvl = number(r.attr("w:val"), 0);
        if (r.depth() > d) r.skip_element();
      }
      continue;
    } else if (n == "sectPr") {
      if (sect) {
        out.section_break = true;
        read_section(r, *sect);
        continue;
      }
    }
    if (r.depth() > depth) r.skip_element();
  }
  return out;
}

// ---- styles, theme, numbering ------------------------------------------------------

struct style_def {
  std::string based_on;
  ppr p;
  rpr r;
  bool table_grid = false;  // a table style with borders of its own
};

struct styles {
  rpr default_r;
  ppr default_p;
  std::string default_para;  // the style w:default="1" of type paragraph
  std::unordered_map<std::string, style_def> by_id;
  std::string minor = "Calibri", major = "Calibri Light";
  // Theme colours by scheme name (dk1, lt1, accent1 …); Office's until the
  // theme says.
  std::unordered_map<std::string, std::uint32_t> scheme = {
      {"dk1", 0x000000}, {"lt1", 0xFFFFFF}, {"dk2", 0x44546A}, {"lt2", 0xE7E6E6},
      {"accent1", 0x4472C4}, {"accent2", 0xED7D31}, {"accent3", 0xA5A5A5}, {"accent4", 0xFFC000},
      {"accent5", 0x5B9BD5}, {"accent6", 0x70AD47}, {"hlink", 0x0563C1}, {"folHlink", 0x954F72}};

  // The style's chain, root first, bounded against a cycle.
  void chain(const std::string& id, rpr& r, ppr* p) const {
    std::vector<const style_def*> defs;
    std::string at = id;
    for (int guard = 0; guard < 16 && !at.empty(); ++guard) {
      auto it = by_id.find(at);
      if (it == by_id.end()) break;
      defs.push_back(&it->second);
      at = it->second.based_on;
    }
    for (auto d = defs.rbegin(); d != defs.rend(); ++d) {
      r.over((*d)->r);
      if (p) p->over((*d)->p);
    }
  }
};

void read_styles(std::string_view doc, styles& st) {
  xml::reader r(doc);
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "rPrDefault") {
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u == token::start && r.local() == "rPr") st.default_r.over(read_rpr(r));
      }
    } else if (n == "pPrDefault") {
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u == token::start && r.local() == "pPr") st.default_p.over(read_ppr(r).p);
      }
    } else if (n == "style") {
      const std::string id(r.attr("w:styleId"));
      const std::string type(r.attr("w:type"));
      const bool is_default = r.attr("w:default") == "1" || r.attr("w:default") == "true";
      style_def def;
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u != token::start) continue;
        const auto m = r.local();
        if (m == "basedOn") def.based_on = std::string(r.attr("w:val"));
        else if (m == "rPr") { def.r.over(read_rpr(r)); continue; }
        else if (m == "pPr") { def.p.over(read_ppr(r).p); continue; }
        else if (m == "tblBorders") def.table_grid = true;
        else if (m == "tblStylePr") { r.skip_element(); continue; }  // first-row etc.: not the base
        else if (m == "tblPr") continue;  // descend: tblBorders lives inside
        if (r.depth() > d + 1) r.skip_element();
      }
      if (type == "paragraph" && is_default) st.default_para = id;
      if (!id.empty()) st.by_id.emplace(id, std::move(def));
    }
  }
}

void read_theme(std::string_view doc, styles& st) {
  xml::reader r(doc);
  std::string_view which;
  std::string colour;  // the scheme slot being read
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "dk1" || n == "lt1" || n == "dk2" || n == "lt2" || n == "hlink" || n == "folHlink" ||
        (n.size() == 7 && n.substr(0, 6) == "accent")) {
      colour = std::string(n);
    } else if (!colour.empty() && (n == "srgbClr" || n == "sysClr")) {
      std::uint32_t c = 0;
      if (parse_rgb(n == "srgbClr" ? r.attr("val") : r.attr("lastClr"), c)) st.scheme[colour] = c;
      colour.clear();
    } else if (n == "majorFont" || n == "minorFont") {
      which = n;
    } else if (n == "latin" && !which.empty() && !r.attr("typeface").empty()) {
      (which == "majorFont" ? st.major : st.minor) = std::string(r.attr("typeface"));
      which = {};
    }
  }
}

struct level {
  std::string fmt = "decimal";
  std::string text = "%1.";
  long start = 1;
  ppr indent;
};
struct numbering {
  std::unordered_map<long, std::vector<level>> abstract;  // id -> levels 0..8
  std::unordered_map<long, long> num_to_abstract;
};

void read_numbering(std::string_view doc, numbering& nb) {
  xml::reader r(doc);
  long current_abstract = -1;
  long current_num = -1;
  int current_level = -1;
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "abstractNum") {
      current_abstract = number(r.attr("w:abstractNumId"), -1);
      nb.abstract[current_abstract].resize(9);
      current_num = -1;
    } else if (n == "num") {
      current_num = number(r.attr("w:numId"), -1);
      current_abstract = -1;
    } else if (n == "abstractNumId" && current_num >= 0) {
      nb.num_to_abstract[current_num] = number(r.attr("w:val"), -1);
    } else if (n == "lvl" && current_abstract >= 0) {
      current_level = static_cast<int>(number(r.attr("w:ilvl"), -1));
      if (current_level < 0 || current_level > 8) current_level = -1;
    } else if (current_abstract >= 0 && current_level >= 0) {
      level& lv = nb.abstract[current_abstract][static_cast<std::size_t>(current_level)];
      if (n == "numFmt") lv.fmt = std::string(r.attr("w:val"));
      else if (n == "lvlText") lv.text = std::string(r.attr("w:val"));
      else if (n == "start") lv.start = number(r.attr("w:val"), 1);
      else if (n == "pPr") lv.indent.over(read_ppr(r).p);
    }
  }
}

std::string roman(long v, bool upper) {
  static constexpr std::pair<long, const char*> kTable[] = {
      {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
      {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},   {4, "iv"}, {1, "i"}};
  std::string out;
  v = std::clamp<long>(v, 1, 3999);
  for (const auto& [n, s] : kTable) {
    while (v >= n) {
      out += s;
      v -= n;
    }
  }
  if (upper) for (char& c : out) c = static_cast<char>(c - 'a' + 'A');
  return out;
}

std::string format_number(long v, const std::string& fmt) {
  if (fmt == "lowerLetter" || fmt == "upperLetter") {
    std::string out;
    const long k = std::clamp<long>(v, 1, 26 * 40);
    const char base = fmt == "lowerLetter" ? 'a' : 'A';
    out.assign(static_cast<std::size_t>((k - 1) / 26 + 1), static_cast<char>(base + (k - 1) % 26));
    return out;
  }
  if (fmt == "lowerRoman") return roman(v, false);
  if (fmt == "upperRoman") return roman(v, true);
  return std::to_string(v);
}

// Symbol-font bullets live in the private-use area; draw them as text bullets.
std::string bullet_text(const std::string& t) {
  if (t.empty()) return "\xE2\x80\xA2";  // •
  if (t == "\xEF\x82\xB7" || t == "\xEF\x82\xA7" || t == "\xEF\x81\xAC") return "\xE2\x80\xA2";
  if (t == "o") return "\xE2\x97\xA6";  // ◦
  if (t == "\xEF\x82\xA7" || t == "\xEF\x81\xAE") return "\xE2\x96\xAA";  // ▪
  const auto lead = static_cast<unsigned char>(t[0]);
  if (lead == 0xEF) return "\xE2\x80\xA2";  // any other PUA glyph
  return t;
}

// ---- colours (DrawingML) -------------------------------------------------------------

struct colour {
  bool set = false;
  std::uint32_t rgb = 0;
  double alpha = 1.0;
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

// The colour inside the element just started (solidFill, fillRef, a gradient
// stop …): srgbClr / schemeClr / prstClr / sysClr with lumMod, lumOff, shade,
// tint and alpha. Consumes the element.
colour read_colour(xml::reader& r, const styles& st) {
  colour out;
  const int depth = r.depth();
  bool in_colour = false;
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < depth) break;
    if (t != token::start) continue;
    const auto n = r.local();
    if (!out.set && (n == "srgbClr" || n == "schemeClr" || n == "prstClr" || n == "sysClr")) {
      std::uint32_t c = 0;
      if (n == "srgbClr" && parse_rgb(r.attr("val"), c)) out.set = true;
      else if (n == "sysClr" && parse_rgb(r.attr("lastClr"), c)) out.set = true;
      else if (n == "schemeClr") {
        std::string key(r.attr("val"));
        if (key == "tx1") key = "dk1";
        else if (key == "bg1") key = "lt1";
        else if (key == "tx2") key = "dk2";
        else if (key == "bg2") key = "lt2";
        else if (key == "phClr") key = "accent1";
        if (auto it = st.scheme.find(key); it != st.scheme.end()) {
          c = it->second;
          out.set = true;
        }
      } else if (n == "prstClr") {
        static constexpr std::pair<std::string_view, std::uint32_t> kPreset[] = {
            {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000}, {"green", 0x008000},
            {"blue", 0x0000FF},  {"yellow", 0xFFFF00}, {"gray", 0x808080}, {"orange", 0xFFA500}};
        for (const auto& [name, v] : kPreset) {
          if (r.attr("val") == name) {
            c = v;
            out.set = true;
          }
        }
      }
      out.rgb = c;
      in_colour = out.set;
      continue;
    }
    if (!in_colour) {
      // A gradient's first stop wins; anything else is not a colour.
      continue;
    }
    const double v = static_cast<double>(number(r.attr("val"), 100000)) / 100000.0;
    if (n == "lumMod" || n == "lumOff") {
      double h = 0, s = 0, l = 0;
      to_hsl(out.rgb, h, s, l);
      l = n == "lumMod" ? l * v : l + v;
      out.rgb = from_hsl(h, s, std::clamp(l, 0.0, 1.0));
    } else if (n == "shade" || n == "tint") {
      std::uint32_t c = 0;
      for (int k = 16; k >= 0; k -= 8) {
        const double ch = (out.rgb >> k) & 0xFF;
        const double nv = n == "shade" ? ch * v : ch + (255.0 - ch) * (1.0 - v);
        c |= static_cast<std::uint32_t>(std::lround(std::clamp(nv, 0.0, 255.0))) << k;
      }
      out.rgb = c;
    } else if (n == "alpha") {
      out.alpha = std::clamp(v, 0.0, 1.0);
    }
  }
  return out;
}

// ---- the document model --------------------------------------------------------

enum class wrap_mode : std::uint8_t { none, square, top_bottom };
enum class wrap_side : std::uint8_t { both, left, right, largest };
enum class href : std::uint8_t { page, margin, column, left_margin, right_margin };
enum class vref : std::uint8_t { page, margin, paragraph, top_margin, bottom_margin };
enum class place : std::uint8_t { offset, start, center, end };  // left/top, centre, right/bottom

struct item {
  enum kind : std::uint8_t { text, tab, line_break, column_break, page_break, object, field } k = text;
  std::string str;
  rpr r;
  int obj = -1;                  // object: an index into document::objects
  dl::field f = dl::field::none;
  int note = -1;                 // a footnote this reference places on its page
};

struct paragraph {
  ppr p;
  rpr mark;  // the paragraph style's run props (for the list label)
  std::vector<item> items;
  std::string label;  // list number or bullet
};

struct table;
struct block {
  std::unique_ptr<paragraph> para;
  std::unique_ptr<table> tbl;
};

struct cell {
  std::vector<block> blocks;
  long span = 1;
  bool merged_continue = false;
};
struct table {
  std::vector<double> grid;  // column widths, pt
  std::vector<std::vector<cell>> rows;
  bool borders = false;
  double indent = 0.0;
};

// A picture or a shape, inline in a line or floating at its anchor.
struct object_def {
  enum class kind : std::uint8_t { picture, shape, unknown, hidden } k = kind::unknown;
  std::shared_ptr<const raster> pixels;  // picture; null draws a grey box
  dl::shape_style shape;
  std::vector<block> text;               // a text box's paragraphs
  std::optional<std::uint32_t> text_rgb; // the shape style's font colour
  double w = 0.0, h = 0.0;
  double ins_l = 7.2, ins_t = 3.6, ins_r = 7.2, ins_b = 3.6;
  std::uint8_t text_anchor = 0;  // 0 top, 1 centre, 2 bottom
  bool floating = false, behind = false;
  href hrel = href::column;
  vref vrel = vref::paragraph;
  place hplace = place::offset, vplace = place::offset;
  double hoff = 0.0, voff = 0.0;
  wrap_mode wrap = wrap_mode::none;
  wrap_side side = wrap_side::both;
  double dist_t = 0.0, dist_b = 0.0, dist_l = 0.0, dist_r = 0.0;
  std::int64_t z = 0;
};

struct story {
  std::vector<block> blocks;
};

struct document {
  std::vector<block> body;
  std::vector<section> sections;
  std::vector<story> stories;  // headers and footers
  std::unordered_map<int, story> footnotes, endnotes;
  std::vector<int> endnote_order;
  std::vector<object_def> objects;
  bool even_odd = false;  // settings.xml: evenAndOddHeaders
};

constexpr std::size_t kMaxObjects = 4096;
constexpr std::size_t kMaxStories = 64;
constexpr std::size_t kMaxSections = 4096;
constexpr int kMaxNotes = 4096;
constexpr int kMaxNest = 3;  // text box in a cell in a text box: enough

// Requires="…" of an mc:Choice: every prefix one this reader knows.
bool choice_supported(std::string_view req) {
  while (!req.empty()) {
    const auto sp = req.find(' ');
    const std::string_view p = req.substr(0, sp);
    if (!p.empty() && p != "wps" && p != "wp14" && p != "w14" && p != "w15" && p != "w16se" && p != "a14" &&
        p != "v" && p != "o" && p != "w10" && p != "w16cid" && p != "w16" && p != "aink" && p != "am3d") {
      return false;
    }
    if (sp == std::string_view::npos) break;
    req.remove_prefix(sp + 1);
  }
  return true;
}

// The field an instruction asks for, when it is one drawn here.
dl::field field_of(std::string_view instr) {
  instr = trim(instr);
  const auto sp = instr.find_first_of(" \\");
  std::string word(instr.substr(0, sp));
  for (char& c : word) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (word == "PAGE") return dl::field::page;
  if (word == "NUMPAGES" || word == "SECTIONPAGES") return dl::field::pages;
  return dl::field::none;
}

// VML lengths: "12pt", "1.5in", "2cm", "30px", bare numbers as px.
double vml_length(std::string_view v) {
  v = trim(v);
  if (v.empty()) return 0.0;
  char buf[32];
  const std::size_t n = std::min(v.size(), sizeof buf - 1);
  std::memcpy(buf, v.data(), n);
  buf[n] = '\0';
  char* end = nullptr;
  const double x = std::strtod(buf, &end);
  if (!std::isfinite(x)) return 0.0;
  const std::string_view unit(end, static_cast<std::size_t>(buf + n - end));
  if (unit == "pt") return x;
  if (unit == "in") return x * 72.0;
  if (unit == "cm") return x * 72.0 / 2.54;
  if (unit == "mm") return x * 72.0 / 25.4;
  if (unit == "pc") return x * 12.0;
  return x * 0.75;  // px
}

bool vml_colour(std::string_view v, std::uint32_t& out) {
  v = trim(v);
  if (v.empty()) return false;
  if (v[0] == '#') {
    v.remove_prefix(1);
    const auto sp = v.find(' ');
    v = v.substr(0, sp);
    if (v.size() == 3) {
      const std::string six{v[0], v[0], v[1], v[1], v[2], v[2]};
      return parse_rgb(six, out);
    }
    return parse_rgb(v, out);
  }
  static constexpr std::pair<std::string_view, std::uint32_t> kNamed[] = {
      {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},  {"green", 0x008000}, {"blue", 0x0000FF},
      {"yellow", 0xFFFF00}, {"gray", 0x808080}, {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"navy", 0x000080}};
  for (const auto& [name, c] : kNamed) {
    if (v.substr(0, name.size()) == name) {
      out = c;
      return true;
    }
  }
  return false;
}

// Parses document.xml (and the header, footer and note parts) into blocks.
class body_reader {
 public:
  body_reader(const zip::archive& zip, const styles& st, const numbering& nb,
              const std::unordered_map<std::string, std::string>& rels, document& doc, const job_context* ctx)
      : zip_(zip), st_(st), nb_(nb), rels_(&rels), doc_rels_(rels), doc_(doc), ctx_(ctx) {}

  // docx_text (#354): pictures keep their size for the layout but are never
  // decoded; the text does not need their pixels.
  void skip_pixels() noexcept { decode_images_ = false; }

  status read(std::string_view xml_doc) {
    xml::reader r(xml_doc);
    status s = status::ok;
    for (token t = r.next(); t != token::eof; t = r.next()) {
      if (t == token::error) {
        s = doc_.body.empty() ? status::corrupt : status::ok;
        break;
      }
      if (t != token::start) continue;
      if (r.local() == "body") {
        s = read_blocks(r, doc_.body, true);
        break;
      }
    }
    // The body's own sectPr is the last section; a body without one is one
    // US Letter section.
    section last = final_.value_or(section{});
    last.first = section_start_;
    last.end = doc_.body.size();
    if (last.first < last.end || doc_.sections.empty()) doc_.sections.push_back(std::move(last));
    if (s != status::ok) return s;
    read_parts();
    return status::ok;
  }

 private:
  // Headers, footers, notes and settings, after the body has named them.
  void read_parts() {
    if (auto settings = zip_.read("word/settings.xml")) {
      xml::reader r(std::string_view(reinterpret_cast<const char*>(settings->data()), settings->size()));
      for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
        if (t == token::start && r.local() == "evenAndOddHeaders") doc_.even_odd = on(r);
      }
    }
    std::unordered_map<std::string, int> loaded;
    for (std::size_t si = 0; si < doc_.sections.size(); ++si) {
      section& s = doc_.sections[si];
      for (int k = 0; k < 3; ++k) {
        s.header[k] = story_for(s.header_rel[k], loaded);
        s.footer[k] = story_for(s.footer_rel[k], loaded);
        // A section without a reference of this kind keeps the previous one's.
        if (si > 0) {
          if (s.header_rel[k].empty()) s.header[k] = doc_.sections[si - 1].header[k];
          if (s.footer_rel[k].empty()) s.footer[k] = doc_.sections[si - 1].footer[k];
        }
      }
    }
    if (!footnote_numbers_.empty()) read_notes("word/footnotes.xml", "footnote", footnote_numbers_, doc_.footnotes);
    if (!endnote_numbers_.empty()) read_notes("word/endnotes.xml", "endnote", endnote_numbers_, doc_.endnotes);
  }

  int story_for(const std::string& rel, std::unordered_map<std::string, int>& loaded) {
    if (rel.empty()) return -1;
    auto target = doc_rels_.find(rel);
    if (target == doc_rels_.end()) return -1;
    if (auto it = loaded.find(target->second); it != loaded.end()) return it->second;
    int index = -1;
    if (doc_.stories.size() < kMaxStories) {
      if (auto bytes = zip_.read(target->second)) {
        story st;
        with_part(target->second, [&](xml::reader& r) {
          for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
            if (t == token::start && (r.local() == "hdr" || r.local() == "ftr")) {
              read_blocks(r, st.blocks, false);
              break;
            }
          }
        }, *bytes);
        doc_.stories.push_back(std::move(st));
        index = static_cast<int>(doc_.stories.size()) - 1;
      }
    }
    loaded.emplace(target->second, index);
    return index;
  }

  // Reads a part with its own relationships (pictures in a header are the
  // header's), the reader's field state set aside.
  template <class F>
  void with_part(const std::string& part, F&& body, const std::vector<std::uint8_t>& bytes) {
    std::unordered_map<std::string, std::string> rels;
    const auto slash = part.rfind('/');
    const std::string rels_name = part.substr(0, slash + 1) + "_rels/" + part.substr(slash + 1) + ".rels";
    if (auto rb = zip_.read(rels_name)) rels = read_rels(std::string_view(reinterpret_cast<const char*>(rb->data()), rb->size()));
    const auto* saved = rels_;
    auto saved_fields = std::move(fields_);
    fields_.clear();
    rels_ = &rels;
    xml::reader r(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    body(r);
    rels_ = saved;
    fields_ = std::move(saved_fields);
  }

  void read_notes(const std::string& part, std::string_view element, const std::unordered_map<int, std::string>& numbers,
                  std::unordered_map<int, story>& out) {
    auto bytes = zip_.read(part);
    if (!bytes) return;
    with_part(part, [&](xml::reader& r) {
      for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
        if (t != token::start || r.local() != element) continue;
        const int id = static_cast<int>(number(r.attr("w:id"), -1));
        auto it = numbers.find(id);
        const auto type = r.attr("w:type");
        if (it == numbers.end() || (!type.empty() && type != "normal")) {
          r.skip_element();
          continue;
        }
        note_label_ = it->second;
        story st;
        ++in_note_;
        read_blocks(r, st.blocks, false);
        --in_note_;
        out[id] = std::move(st);
      }
    }, *bytes);
    note_label_.clear();
  }

  // Blocks until the element that contains them ends. `body` marks the
  // document body, whose paragraphs end sections.
  status read_blocks(xml::reader& r, std::vector<block>& out, bool body) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      if (ctx_ && ctx_->cancelled()) return status::cancelled;
      if (++blocks_ > kMaxBlocks) return status::ok;  // enough to show; the rest is not read
      const auto n = r.local();
      if (n == "p") {
        section sect;
        bool ends_section = false;
        block b;
        b.para = std::make_unique<paragraph>(read_paragraph(r, body ? &sect : nullptr, &ends_section));
        out.push_back(std::move(b));
        if (body && ends_section && doc_.sections.size() + 1 < kMaxSections) {
          sect.first = section_start_;
          sect.end = out.size();
          doc_.sections.push_back(std::move(sect));
          section_start_ = out.size();
        }
      } else if (n == "tbl") {
        block b;
        b.tbl = std::make_unique<table>(read_table(r));
        out.push_back(std::move(b));
      } else if (n == "sectPr" && body) {
        section s;
        read_section(r, s);
        final_ = std::move(s);
      } else if (n == "sdt" || n == "sdtContent" || n == "customXml" || n == "ins" ||
                 n == "smartTag") {
        continue;  // containers: their blocks are read as if they were here
      } else if (n == "AlternateContent") {
        alternate(r, [&](xml::reader& rr) {
          if (rr.local() == "p") {
            block b;
            b.para = std::make_unique<paragraph>(read_paragraph(rr, nullptr, nullptr));
            out.push_back(std::move(b));
          } else {
            rr.skip_element();
          }
        });
      } else {
        r.skip_element();
      }
    }
    return status::ok;
  }

  // mc:AlternateContent just started: the first Choice this reader can draw,
  // else the Fallback. `child` gets each element of the branch and consumes it.
  template <class F>
  void alternate(xml::reader& r, F&& child) {
    bool taken = false;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      const bool pick = !taken && ((n == "Choice" && choice_supported(r.attr("Requires"))) || n == "Fallback");
      if (!pick) {
        r.skip_element();
        continue;
      }
      taken = true;
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u == token::start) child(r);
      }
    }
  }

  paragraph read_paragraph(xml::reader& r, section* sect, bool* ends_section) {
    paragraph p;
    std::string style = st_.default_para;
    ppr direct;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "pPr") {
        ppr_read pr = read_ppr(r, sect);
        if (!pr.style.empty()) style = pr.style;
        direct = pr.p;
        if (ends_section) *ends_section = pr.section_break;
      } else {
        read_inline(r, p.items, rpr{});
      }
    }
    // Effective: defaults, then the style chain, then the paragraph's own.
    p.p = st_.default_p;
    p.mark = st_.default_r;
    st_.chain(style, p.mark, &p.p);
    // List indents come from the numbering level unless the paragraph sets them.
    ppr numbered = p.p;
    numbered.over(direct);
    if (numbered.num_id && *numbered.num_id > 0) {
      const long ilvl = std::clamp<long>(numbered.ilvl.value_or(0), 0, 8);
      if (const level* lv = level_of(*numbered.num_id, ilvl)) {
        ppr withlist = p.p;
        withlist.over(lv->indent);
        withlist.over(direct);
        p.p = withlist;
        p.label = label_for(*numbered.num_id, ilvl, *lv);
      } else {
        p.p = numbered;
      }
    } else {
      p.p = numbered;
    }
    for (item& it : p.items) {
      rpr eff = p.mark;
      eff.over(it.r);
      it.r = eff;
    }
    return p;
  }

  const level* level_of(long num_id, long ilvl) const {
    auto a = nb_.num_to_abstract.find(num_id);
    if (a == nb_.num_to_abstract.end()) return nullptr;
    auto levels = nb_.abstract.find(a->second);
    if (levels == nb_.abstract.end() || static_cast<std::size_t>(ilvl) >= levels->second.size()) return nullptr;
    return &levels->second[static_cast<std::size_t>(ilvl)];
  }

  std::string label_for(long num_id, long ilvl, const level& lv) {
    std::vector<long>& counters = counters_[num_id];
    if (counters.size() < 9) counters.assign(9, 0);
    for (long k = 0; k < 9; ++k) {
      if (counters[static_cast<std::size_t>(k)] == 0) {
        const level* l = level_of(num_id, k);
        counters[static_cast<std::size_t>(k)] = (l ? l->start : 1) - 1;
      }
    }
    ++counters[static_cast<std::size_t>(ilvl)];
    for (long k = ilvl + 1; k < 9; ++k) counters[static_cast<std::size_t>(k)] = 0;
    if (lv.fmt == "bullet") return bullet_text(lv.text);
    if (lv.fmt == "none") return {};
    std::string out;
    for (std::size_t i = 0; i < lv.text.size(); ++i) {
      if (lv.text[i] == '%' && i + 1 < lv.text.size() && lv.text[i + 1] >= '1' && lv.text[i + 1] <= '9') {
        const long k = lv.text[i + 1] - '1';
        const level* l = level_of(num_id, k);
        long v = counters[static_cast<std::size_t>(k)];
        if (v <= 0) v = l ? l->start : 1;
        out += format_number(v, l ? l->fmt : lv.fmt);
        ++i;
      } else {
        out += lv.text[i];
      }
    }
    return out;
  }

  // Runs and what holds them (hyperlinks, fields, insertions, content controls).
  void read_inline(xml::reader& r, std::vector<item>& out, const rpr& inherited) {
    const auto n = r.local();
    if (n == "r") {
      read_run(r, out, inherited);
    } else if (n == "fldSimple") {
      const dl::field f = field_of(r.attr("w:instr"));
      if (f != dl::field::none) {
        // The run inside gives the look; its cached text is not drawn.
        rpr props = inherited;
        const int depth = r.depth();
        for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
          if (t == token::end && r.depth() < depth) break;
          if (t == token::start && r.local() == "rPr") {
            props.over(read_rpr(r));
          }
        }
        push_field(out, f, props);
        return;
      }
      descend(r, out, inherited);
    } else if (n == "hyperlink" || n == "ins" || n == "smartTag" || n == "sdt" || n == "sdtContent" ||
               n == "customXml" || n == "bdo" || n == "dir") {
      descend(r, out, inherited);
    } else if (n == "AlternateContent") {
      alternate(r, [&](xml::reader& rr) { read_inline(rr, out, inherited); });
    } else {
      r.skip_element();  // deletions, bookmarks, comments, proofing marks …
    }
  }

  void descend(xml::reader& r, std::vector<item>& out, const rpr& inherited) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t == token::start) read_inline(r, out, inherited);
    }
  }

  void push_field(std::vector<item>& out, dl::field f, const rpr& props) {
    item it;
    it.k = item::field;
    it.f = f;
    it.r = props;
    out.push_back(std::move(it));
  }

  // A field's state across the runs it spans (fldChar begin / separate / end).
  struct fld {
    rpr props;  // the begin run's: a field's result looks like it
    std::string instr;
    bool result = false;
    bool emitted = false;
    dl::field kind = dl::field::none;
  };
  // Inside a field drawn here, its cached result is not drawn.
  bool suppressing() const {
    for (const fld& f : fields_) {
      if (!f.result || f.kind != dl::field::none) return true;
    }
    return false;
  }

  void read_run(xml::reader& r, std::vector<item>& out, const rpr& inherited) {
    rpr props = inherited;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      if (r.local() == "rPr") {
        std::string char_style;
        rpr direct = read_rpr(r, &char_style);
        rpr eff;
        if (!char_style.empty()) st_.chain(char_style, eff, nullptr);
        eff.over(direct);
        props = inherited;
        props.over(eff);
      } else {
        run_child(r, out, props);
      }
    }
  }

  void push_text(std::vector<item>& out, std::string s, const rpr& props) {
    if (s.empty()) return;
    if (!out.empty() && out.back().k == item::text && out.back().note < 0 && out.back().str.size() < 4096 &&
        out.back().r == props) {
      out.back().str += s;
      return;
    }
    item it;
    it.k = item::text;
    it.str = std::move(s);
    it.r = props;
    out.push_back(std::move(it));
  }

  // One element of a run, just started; consumes it.
  void run_child(xml::reader& r, std::vector<item>& out, const rpr& props) {
    const auto n = r.local();
    if (n == "t") {
      std::string text;
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u == token::text) text += r.text();
      }
      if (!suppressing()) push_text(out, std::move(text), props);
    } else if (n == "fldChar") {
      const auto type = r.attr("w:fldCharType");
      if (type == "begin") {
        if (fields_.size() < 16) fields_.push_back(fld{props, {}, false, false, dl::field::none});
      } else if (type == "separate" && !fields_.empty()) {
        fld& f = fields_.back();
        f.result = true;
        f.kind = field_of(f.instr);
        if (f.kind != dl::field::none && !f.emitted && fields_.size() == 1) {
          push_field(out, f.kind, f.props);
          f.emitted = true;
        }
      } else if (type == "end" && !fields_.empty()) {
        fld f = std::move(fields_.back());
        fields_.pop_back();
        const dl::field kind = field_of(f.instr);
        if (kind != dl::field::none && !f.emitted && fields_.empty()) push_field(out, kind, f.props);
      }
      r.skip_element();
    } else if (n == "instrText") {
      std::string text = element_text(r);
      if (!fields_.empty() && fields_.back().instr.size() < 256) fields_.back().instr += text;
    } else if (n == "tab" || n == "ptab") {
      item it;
      it.k = item::tab;
      it.r = props;
      out.push_back(std::move(it));
      r.skip_element();
    } else if (n == "br" || n == "cr") {
      item it;
      const auto type = r.attr("w:type");
      it.k = type == "page" ? item::page_break : type == "column" ? item::column_break : item::line_break;
      it.r = props;
      out.push_back(std::move(it));
      r.skip_element();
    } else if (n == "noBreakHyphen") {
      push_text(out, "-", props);
      r.skip_element();
    } else if (n == "sym") {
      std::uint32_t cp = 0;
      const auto hex = r.attr("w:char");
      for (char c : hex) {
        cp <<= 4;
        if (c >= '0' && c <= '9') cp |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') cp |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') cp |= static_cast<std::uint32_t>(c - 'A' + 10);
      }
      if (cp >= 0xF000 && cp <= 0xF0FF) cp = 0x2022;  // Symbol/Wingdings: a bullet
      std::string s;
      if (cp >= 0x20 && cp < 0x80) s += static_cast<char>(cp);
      else if (cp > 0) s = "\xE2\x80\xA2";
      push_text(out, std::move(s), props);
      r.skip_element();
    } else if (n == "footnoteReference" || n == "endnoteReference") {
      const bool foot = n == "footnoteReference";
      const int id = static_cast<int>(number(r.attr("w:id"), -1));
      r.skip_element();
      auto& numbers = foot ? footnote_numbers_ : endnote_numbers_;
      if (in_note_ > 0 || id < 0 || numbers.size() >= static_cast<std::size_t>(kMaxNotes)) return;
      auto it = numbers.find(id);
      if (it == numbers.end()) {
        const long k = static_cast<long>(numbers.size()) + 1;
        it = numbers.emplace(id, foot ? std::to_string(k) : roman(k, false)).first;
        if (!foot) doc_.endnote_order.push_back(id);
      }
      rpr mark = props;
      if (!mark.va) mark.va = valign::super;
      item ref;
      ref.k = item::text;
      ref.str = it->second;
      ref.r = mark;
      ref.note = foot ? id : -1;
      out.push_back(std::move(ref));
    } else if (n == "footnoteRef" || n == "endnoteRef") {
      rpr mark = props;
      if (!mark.va) mark.va = valign::super;
      item ref;
      ref.k = item::text;
      ref.str = note_label_;
      ref.r = mark;
      if (!ref.str.empty()) out.push_back(std::move(ref));
      r.skip_element();
    } else if (n == "drawing") {
      read_drawing(r, out, props);
    } else if (n == "pict" || n == "object") {
      read_pict(r, out, props);
    } else if (n == "AlternateContent") {
      alternate(r, [&](xml::reader& rr) { run_child(rr, out, props); });
    } else {
      r.skip_element();  // comments, last-rendered breaks …
    }
  }

  int add_object(object_def o) {
    if (doc_.objects.size() >= kMaxObjects) return -1;
    doc_.objects.push_back(std::move(o));
    return static_cast<int>(doc_.objects.size()) - 1;
  }

  void push_object(std::vector<item>& out, object_def o, const rpr& props) {
    if (o.k == object_def::kind::hidden) return;
    if (!(o.w > 0.5) || !(o.h > 0.5) || o.w > 10000 || o.h > 10000) {
      // A line has no height; anything else that small is nothing to draw.
      if (!(o.k == object_def::kind::shape && o.shape.geom == dl::geometry::line && (o.w > 0.5 || o.h > 0.5))) return;
      o.w = std::clamp(o.w, 0.0, 10000.0);
      o.h = std::clamp(o.h, 0.0, 10000.0);
    }
    const int id = add_object(std::move(o));
    if (id < 0) return;
    item it;
    it.k = item::object;
    it.obj = id;
    it.r = props;
    out.push_back(std::move(it));
  }

  void read_drawing(xml::reader& r, std::vector<item>& out, const rpr& props) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "inline" || n == "anchor") push_object(out, read_object(r, n == "anchor"), props);
      else r.skip_element();
    }
  }

  static href h_from(std::string_view v) {
    if (v == "page") return href::page;
    if (v == "margin") return href::margin;
    if (v == "leftMargin" || v == "insideMargin") return href::left_margin;
    if (v == "rightMargin" || v == "outsideMargin") return href::right_margin;
    return href::column;  // column, character
  }
  static vref v_from(std::string_view v) {
    if (v == "page") return vref::page;
    if (v == "margin") return vref::margin;
    if (v == "topMargin" || v == "insideMargin") return vref::top_margin;
    if (v == "bottomMargin" || v == "outsideMargin") return vref::bottom_margin;
    return vref::paragraph;  // paragraph, line
  }
  static place place_from(std::string_view v) {
    if (v == "center") return place::center;
    if (v == "right" || v == "bottom" || v == "outside") return place::end;
    return place::start;
  }

  // wp:inline or wp:anchor just started; consumes it.
  object_def read_object(xml::reader& r, bool anchor) {
    object_def o;
    o.floating = anchor;
    if (anchor) {
      o.behind = attr_true(r.attr("behindDoc"));
      o.dist_t = std::clamp(emu(r.attr("distT")), 0.0, 400.0);
      o.dist_b = std::clamp(emu(r.attr("distB")), 0.0, 400.0);
      o.dist_l = std::clamp(emu(r.attr("distL")), 0.0, 400.0);
      o.dist_r = std::clamp(emu(r.attr("distR")), 0.0, 400.0);
      o.z = big_number(r.attr("relativeHeight"), 0);
    }
    std::string embed;
    bool sized = false;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "positionH" || n == "positionV") {
        const bool horizontal = n == "positionH";
        if (horizontal) o.hrel = h_from(r.attr("relativeFrom"));
        else o.vrel = v_from(r.attr("relativeFrom"));
        const int d = r.depth();
        for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
          if (u == token::end && r.depth() < d) break;
          if (u != token::start) continue;
          if (r.local() == "posOffset") {
            const double v = std::clamp(emu(element_text(r)), -10000.0, 10000.0);
            (horizontal ? o.hoff : o.voff) = v;
            (horizontal ? o.hplace : o.vplace) = place::offset;
          } else if (r.local() == "align") {
            (horizontal ? o.hplace : o.vplace) = place_from(trim(element_text(r)));
          } else {
            r.skip_element();
          }
        }
      } else if (n == "extent" && !sized) {
        o.w = emu(r.attr("cx"));
        o.h = emu(r.attr("cy"));
        sized = true;
      } else if (n == "wrapNone") {
        o.wrap = wrap_mode::none;
      } else if (n == "wrapSquare" || n == "wrapTight" || n == "wrapThrough") {
        o.wrap = wrap_mode::square;
        const auto side = r.attr("wrapText");
        o.side = side == "left" ? wrap_side::left : side == "right" ? wrap_side::right
                 : side == "largest" ? wrap_side::largest : wrap_side::both;
        r.skip_element();  // a tight wrap's polygon: its box is what is used
      } else if (n == "wrapTopAndBottom") {
        o.wrap = wrap_mode::top_bottom;
      } else if (n == "blip" && embed.empty()) {
        embed = std::string(r.attr("r:embed"));
        o.k = object_def::kind::picture;
      } else if (n == "wsp") {
        read_shape(r, o);
      } else if (n == "wgp" || n == "wpc") {
        o.k = object_def::kind::hidden;  // groups and canvases are not drawn
        r.skip_element();
      } else if (n == "spPr" || n == "docPr" || n == "effectExtent" || n == "simplePos" ||
                 n == "cNvGraphicFramePr" || n == "nvPicPr") {
        r.skip_element();
      }
      // graphic, graphicData, pic, blipFill: descend
    }
    if (o.k == object_def::kind::picture) o.pixels = load_image(embed);
    return o;
  }

  // wps:wsp just started: geometry, fill, outline, text box; consumes it.
  void read_shape(xml::reader& r, object_def& o) {
    o.k = object_def::kind::shape;
    bool have_fill = false, have_line = false;
    colour style_fill, style_line;
    long fill_idx = 0, line_idx = 0;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "spPr") {
        read_sppr(r, o.shape, have_fill, have_line);
      } else if (n == "fillRef" || n == "lnRef" || n == "fontRef") {
        const long idx = number(r.attr("idx"), 0);
        const std::string which(n);
        const colour c = read_colour(r, st_);
        if (which == "fillRef") {
          fill_idx = idx;
          style_fill = c;
        } else if (which == "lnRef") {
          line_idx = idx;
          style_line = c;
        } else if (c.set) {
          o.text_rgb = c.rgb;
        }
      } else if (n == "txbxContent") {
        if (nest_ < kMaxNest) {
          ++nest_;
          read_blocks(r, o.text, false);
          --nest_;
        } else {
          r.skip_element();
        }
      } else if (n == "bodyPr") {
        if (r.has("lIns")) o.ins_l = std::clamp(emu(r.attr("lIns")), 0.0, 200.0);
        if (r.has("tIns")) o.ins_t = std::clamp(emu(r.attr("tIns")), 0.0, 200.0);
        if (r.has("rIns")) o.ins_r = std::clamp(emu(r.attr("rIns")), 0.0, 200.0);
        if (r.has("bIns")) o.ins_b = std::clamp(emu(r.attr("bIns")), 0.0, 200.0);
        const auto a = r.attr("anchor");
        o.text_anchor = static_cast<std::uint8_t>(a == "ctr" ? 1 : a == "b" ? 2 : 0);
        r.skip_element();
      } else if (n == "cNvPr" || n == "cNvSpPr" || n == "linkedTxbx" || n == "extLst") {
        r.skip_element();
      }
      // txbx, style: descend
    }
    if (!have_fill && fill_idx > 0 && style_fill.set) {
      o.shape.filled = true;
      o.shape.fill = style_fill.rgb;
      o.shape.fill_alpha = style_fill.alpha;
    }
    if (!have_line && line_idx > 0 && style_line.set) {
      o.shape.stroked = true;
      o.shape.stroke = style_line.rgb;
      o.shape.stroke_w = line_idx == 1 ? 0.5 : line_idx == 2 ? 1.0 : 1.5;
    }
  }

  void read_sppr(xml::reader& r, dl::shape_style& s, bool& have_fill, bool& have_line) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "xfrm") {
        s.flip_h = attr_true(r.attr("flipH"));
        s.flip_v = attr_true(r.attr("flipV"));
        r.skip_element();
      } else if (n == "prstGeom") {
        const auto p = r.attr("prst");
        s.geom = p == "roundRect" ? dl::geometry::round_rect
                 : (p == "ellipse" || p == "flowChartConnector") ? dl::geometry::ellipse
                 : (p == "line" || p.find("Connector") != std::string_view::npos) ? dl::geometry::line
                                                                                 : dl::geometry::rect;
        const int d = r.depth();
        for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
          if (u == token::end && r.depth() < d) break;
          if (u == token::start && r.local() == "gd" && r.attr("name") == "adj") {
            const auto f = r.attr("fmla");  // "val 16667"
            if (f.substr(0, 4) == "val ") s.radius = std::clamp(number(f.substr(4), 16667) / 100000.0, 0.0, 0.5);
          }
        }
      } else if (n == "solidFill" || n == "gradFill") {
        const colour c = read_colour(r, st_);
        have_fill = true;
        s.filled = c.set;
        s.fill = c.rgb;
        s.fill_alpha = c.alpha;
      } else if (n == "noFill") {
        have_fill = true;
        s.filled = false;
        r.skip_element();
      } else if (n == "ln") {
        if (r.has("w")) s.stroke_w = std::clamp(emu(r.attr("w")), 0.25, 72.0);
        const int d = r.depth();
        for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
          if (u == token::end && r.depth() < d) break;
          if (u != token::start) continue;
          if (r.local() == "solidFill" || r.local() == "gradFill") {
            const colour c = read_colour(r, st_);
            have_line = true;
            s.stroked = c.set;
            s.stroke = c.rgb;
          } else if (r.local() == "noFill") {
            have_line = true;
            s.stroked = false;
            r.skip_element();
          } else {
            r.skip_element();
          }
        }
      } else {
        r.skip_element();
      }
    }
  }

  // VML (w:pict, w:object, and mc:Fallback): the first rect, roundrect, oval
  // or shape, with its style box, fill, stroke, text box and picture.
  void read_pict(xml::reader& r, std::vector<item>& out, const rpr& props) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "rect" || n == "roundrect" || n == "oval" || n == "shape") {
        push_object(out, read_vml_shape(r), props);
      } else {
        r.skip_element();  // groups, lines, shape types, OLE data
      }
    }
  }

  object_def read_vml_shape(xml::reader& r) {
    object_def o;
    const auto n = r.local();
    o.k = object_def::kind::shape;
    o.shape.geom = n == "oval" ? dl::geometry::ellipse : n == "roundrect" ? dl::geometry::round_rect : dl::geometry::rect;
    if (n == "roundrect" && !r.attr("arcsize").empty()) {
      const double v = vml_length(r.attr("arcsize"));  // "10%" reads as a bare number
      const auto a = r.attr("arcsize");
      s_radius(o, a.back() == 'f' ? v / 0.75 / 65536.0 : v / 0.75 / 100.0);
    }
    o.shape.filled = !(r.attr("filled") == "f" || r.attr("filled") == "false");
    o.shape.fill = 0xFFFFFF;
    vml_colour(r.attr("fillcolor"), o.shape.fill);
    o.shape.stroked = !(r.attr("stroked") == "f" || r.attr("stroked") == "false");
    vml_colour(r.attr("strokecolor"), o.shape.stroke);
    if (r.has("strokeweight")) o.shape.stroke_w = std::clamp(vml_length(r.attr("strokeweight")), 0.25, 72.0);
    bool absolute = false;
    std::string_view style = r.attr("style");
    std::string hrel = "text", vrel = "text";
    while (!style.empty()) {
      const auto semi = style.find(';');
      const std::string_view kv = style.substr(0, semi);
      style = semi == std::string_view::npos ? std::string_view{} : style.substr(semi + 1);
      const auto colon = kv.find(':');
      if (colon == std::string_view::npos) continue;
      const std::string_view key = trim(kv.substr(0, colon)), val = trim(kv.substr(colon + 1));
      if (key == "width") o.w = vml_length(val);
      else if (key == "height") o.h = vml_length(val);
      else if (key == "position") absolute = val == "absolute";
      else if (key == "margin-left" || key == "left") o.hoff = std::clamp(vml_length(val), -10000.0, 10000.0);
      else if (key == "margin-top" || key == "top") o.voff = std::clamp(vml_length(val), -10000.0, 10000.0);
      else if (key == "mso-position-horizontal") o.hplace = val == "absolute" ? place::offset : place_from(val);
      else if (key == "mso-position-vertical") o.vplace = val == "absolute" ? place::offset : place_from(val);
      else if (key == "mso-position-horizontal-relative") hrel = std::string(val);
      else if (key == "mso-position-vertical-relative") vrel = std::string(val);
      else if (key == "z-index") o.z = big_number(val, 0);
      else if (key == "visibility" && val == "hidden") o.k = object_def::kind::hidden;
    }
    o.floating = absolute;
    o.behind = absolute && o.z < 0;
    o.hrel = hrel == "page" ? href::page : hrel == "margin" ? href::margin
             : hrel == "left-margin-area" ? href::left_margin : hrel == "right-margin-area" ? href::right_margin
                                                               : href::column;
    o.vrel = vrel == "page" ? vref::page : vrel == "margin" ? vref::margin
             : vrel == "top-margin-area" ? vref::top_margin : vrel == "bottom-margin-area" ? vref::bottom_margin
                                                             : vref::paragraph;
    if (o.hplace != place::offset) o.hoff = 0.0;
    if (o.vplace != place::offset) o.voff = 0.0;
    std::string image;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto m = r.local();
      if (m == "txbxContent") {
        if (nest_ < kMaxNest) {
          ++nest_;
          read_blocks(r, o.text, false);
          --nest_;
        } else {
          r.skip_element();
        }
      } else if (m == "imagedata" && image.empty()) {
        image = std::string(r.attr("r:id").empty() ? r.attr("o:relid") : r.attr("r:id"));
      } else if (m == "wrap") {
        const auto type = r.attr("type");
        o.wrap = (type == "square" || type == "tight" || type == "through") ? wrap_mode::square
                 : type == "topAndBottom" ? wrap_mode::top_bottom : wrap_mode::none;
        const auto side = r.attr("side");
        o.side = side == "left" ? wrap_side::left : side == "right" ? wrap_side::right
                 : side == "largest" ? wrap_side::largest : wrap_side::both;
      } else if (m == "textbox") {
        const auto inset = r.attr("inset");  // "l,t,r,b"
        if (!inset.empty()) {
          double v[4] = {o.ins_l, o.ins_t, o.ins_r, o.ins_b};
          std::string_view rest = inset;
          for (int k = 0; k < 4 && !rest.empty(); ++k) {
            const auto comma = rest.find(',');
            const auto part = trim(rest.substr(0, comma));
            if (!part.empty()) v[k] = std::clamp(vml_length(part), 0.0, 200.0);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
          }
          o.ins_l = v[0];
          o.ins_t = v[1];
          o.ins_r = v[2];
          o.ins_b = v[3];
        }
      }
    }
    if (!image.empty() && o.text.empty()) {
      o.k = object_def::kind::picture;
      o.pixels = load_image(image);
    }
    return o;
  }

  static void s_radius(object_def& o, double fraction) {
    if (std::isfinite(fraction)) o.shape.radius = std::clamp(fraction, 0.0, 0.5);
  }

  std::shared_ptr<const raster> load_image(const std::string& rel) {
    if (rel.empty()) return nullptr;
    auto target = rels_->find(rel);
    if (target == rels_->end()) return nullptr;
    if (auto it = images_.find(target->second); it != images_.end()) return it->second;
    std::shared_ptr<const raster> pixels;
    if (!decode_images_) return pixels;
    if (images_loaded_ < kMaxImages) {
      ++images_loaded_;
      if (auto bytes = zip_.read(target->second, std::size_t{48} << 20)) {
        // Never a document inside a document: only the still formats.
        const auto family = probe(bytes.value());
        if (family != format_family::pdf && family != format_family::docx && family != format_family::unknown) {
          if (auto decoded = decode(bytes.value(), ctx_, 1)) {
            pixels = std::make_shared<const raster>(std::move(decoded).value());
          }
        }
      }
    }
    images_.emplace(target->second, pixels);
    return pixels;
  }

  table read_table(xml::reader& r) {
    table tb;
    std::string style;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "tblPr") {
        const int d = r.depth();
        for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
          if (u == token::end && r.depth() < d) break;
          if (u != token::start) continue;
          const auto m = r.local();
          if (m == "tblStyle") style = std::string(r.attr("w:val"));
          else if (m == "tblInd") tb.indent = twips(r.attr("w:w"));
          else if (m == "top" || m == "bottom" || m == "left" || m == "right" || m == "insideH" ||
                   m == "insideV" || m == "start" || m == "end") {
            const auto v = r.attr("w:val");
            if (!v.empty() && v != "nil" && v != "none") tb.borders = true;
          }
          if (m != "tblBorders" && r.depth() > d + 1) r.skip_element();
        }
      } else if (n == "tblGrid") {
        const int d = r.depth();
        for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
          if (u == token::end && r.depth() < d) break;
          if (u == token::start && r.local() == "gridCol") {
            tb.grid.push_back(std::max(1.0, twips(r.attr("w:w"))));
            r.skip_element();
          }
        }
      } else if (n == "tr") {
        tb.rows.emplace_back();
        read_row(r, tb.rows.back());
      } else {
        r.skip_element();
      }
    }
    if (auto it = st_.by_id.find(style); it != st_.by_id.end()) {
      if (it->second.table_grid) tb.borders = true;
    }
    for (char& c : style) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (style.find("grid") != std::string::npos) tb.borders = true;
    return tb;
  }

  void read_row(xml::reader& r, std::vector<cell>& row) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      if (r.local() != "tc") {
        if (r.local() != "sdt" && r.local() != "sdtContent") r.skip_element();
        continue;
      }
      cell c;
      const int d = r.depth();
      for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
        if (u == token::end && r.depth() < d) break;
        if (u != token::start) continue;
        const auto m = r.local();
        if (m == "tcPr") {
          const int e = r.depth();
          for (token v = r.next(); v != token::eof && v != token::error; v = r.next()) {
            if (v == token::end && r.depth() < e) break;
            if (v != token::start) continue;
            if (r.local() == "gridSpan") c.span = std::clamp<long>(number(r.attr("w:val"), 1), 1, 63);
            else if (r.local() == "vMerge") c.merged_continue = r.attr("w:val") != "restart";
            r.skip_element();
          }
        } else if (m == "p") {
          block b;
          b.para = std::make_unique<paragraph>(read_paragraph(r, nullptr, nullptr));
          c.blocks.push_back(std::move(b));
        } else if (m == "tbl") {
          // A table in a cell: its cells' paragraphs, in order.
          table inner = read_table(r);
          for (auto& inner_row : inner.rows) {
            for (auto& inner_cell : inner_row) {
              for (auto& b : inner_cell.blocks) c.blocks.push_back(std::move(b));
            }
          }
        } else {
          r.skip_element();
        }
      }
      row.push_back(std::move(c));
    }
  }

 public:
  static std::unordered_map<std::string, std::string> read_rels(std::string_view doc) {
    std::unordered_map<std::string, std::string> out;
    xml::reader r(doc);
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t != token::start || r.local() != "Relationship") continue;
      if (r.attr("TargetMode") == "External") continue;
      std::string target(r.attr("Target"));
      if (target.empty()) continue;
      // Relative to word/; an absolute part name starts with '/'.
      if (target[0] == '/') target.erase(0, 1);
      else target = "word/" + target;
      // Resolve "../" once, for parts kept beside word/.
      for (std::size_t p; (p = target.find("/../")) != std::string::npos;) {
        const std::size_t prev = target.rfind('/', p - 1);
        target.erase(prev == std::string::npos ? 0 : prev + 1, p + 4 - (prev == std::string::npos ? 0 : prev + 1));
      }
      out.emplace(std::string(r.attr("Id")), std::move(target));
    }
    return out;
  }

 private:
  static constexpr std::size_t kMaxBlocks = 200000;
  static constexpr int kMaxImages = 256;

  const zip::archive& zip_;
  const styles& st_;
  const numbering& nb_;
  const std::unordered_map<std::string, std::string>* rels_;
  const std::unordered_map<std::string, std::string>& doc_rels_;
  document& doc_;
  const job_context* ctx_;
  std::size_t blocks_ = 0;
  std::size_t section_start_ = 0;
  std::optional<section> final_;
  int images_loaded_ = 0;
  bool decode_images_ = true;
  int nest_ = 0;
  int in_note_ = 0;
  std::string note_label_;
  std::vector<fld> fields_;
  std::unordered_map<int, std::string> footnote_numbers_, endnote_numbers_;
  std::unordered_map<long, std::vector<long>> counters_;
  std::unordered_map<std::string, std::shared_ptr<const raster>> images_;  // by part name
};

// ---- layout ----------------------------------------------------------------------

text::style style_of(const rpr& r, const styles& st, bool complex = false) {
  text::style s;
  if (complex && r.family_cs) s.family = *r.family_cs;
  else s.family = r.family ? *r.family : (r.theme && *r.theme == "major" ? st.major : st.minor);
  const double size = complex && r.size_cs ? *r.size_cs : r.size.value_or(11.0);
  s.size_pt = std::clamp(size, 1.0, 400.0);
  s.bold = r.bold.value_or(false);
  s.italic = r.italic.value_or(false);
  s.underline = r.underline.value_or(false);
  s.strike = r.strike.value_or(false);
  s.rgb = r.rgb.value_or(0x000000);
  if (r.va && *r.va != valign::baseline) s.size_pt *= 0.65;
  return s;
}

std::string upper_ascii(std::string s) {
  for (char& c : s) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return s;
}

// What a page needs drawn after the body is laid out: its section, whether it
// starts that section, its number, and its size.
struct page_info {
  std::uint32_t sec = 0;
  bool first = false;
  long number = 1;
  std::string label;
};

class layout;

// A fixed box inside a page: a cell, a header, a note, a text box.
class story_frame final : public dl::box_frame {
 public:
  story_frame(layout& owner, double x0, double x1, dl::display* out) : box_frame(x0, x1, out), owner_(owner) {}
  void object(int id, double x, double y, double w, double h) override;
  std::string field_text(dl::field f) override;

 private:
  layout& owner_;
};

class layout final : public dl::frame {
 public:
  layout(text::engine& eng, const styles& st, const document& doc, std::vector<std::uint32_t> want)
      : eng_(eng), st_(st), doc_(doc), want_(std::move(want)) {
    std::sort(want_.begin(), want_.end());
    want_.erase(std::unique(want_.begin(), want_.end()), want_.end());
  }

  status run(const job_context* ctx) {
    sec_ = 0;
    geo_ = &doc_.sections[0];
    set_columns();
    c_ = cursor{};
    begin_page(true);
    for (std::size_t si = 0; si < doc_.sections.size(); ++si) {
      if (si > 0) start_section(static_cast<std::uint32_t>(si));
      const section& s = doc_.sections[si];
      const bool balance = cols_.size() > 1 && si + 1 < doc_.sections.size() &&
                           doc_.sections[si + 1].type == sect_start::continuous && same_size(s, doc_.sections[si + 1]);
      const status st = balance ? lay_balanced(s, ctx) : lay_range(s.first, s.end, ctx);
      if (st != status::ok) return st;
    }
    if (!doc_.endnote_order.empty()) {
      // Endnotes close the document, under a short rule.
      const dl::slot s = fit(c_.y + 6.0, 12.0, 0.0, 0.0, {});
      if (dl::display* d = out()) {
        const dl::segment col = column();
        d->add(dl::display::rect{col.x0, s.y + 6.0, std::min(144.0, col.x1 - col.x0), 0.5, 0x000000});
      }
      placed(s.y, 12.0, {});
      c_.y = s.y + 12.0;
      for (int id : doc_.endnote_order) {
        if (ctx && ctx->cancelled()) return status::cancelled;
        if (auto it = doc_.endnotes.find(id); it != doc_.endnotes.end()) {
          c_.y = lay_blocks(it->second.blocks, *this, c_.y, true);
        }
      }
    }
    // Notes still waiting for room get pages of their own.
    for (int guard = 0; guard < 64 && !c_.carry.empty(); ++guard) new_page(false);
    finish_page();
    pages_ = c_.page + 1;
    // Headers and footers last: NUMPAGES is known now.
    pages_label_ = std::to_string(pages_);
    for (auto& [page, info] : infos_) {
      if (page >= pages_) continue;
      draw_furniture(page, info);
    }
    return status::ok;
  }

  [[nodiscard]] std::uint32_t pages() const noexcept { return pages_; }
  // docx_text (#354): the page each body paragraph's first line lands on (a
  // table cell's paragraph: its row's), recorded as the body places them.
  void record_pages(std::unordered_map<const paragraph*, std::uint32_t>* to) noexcept { pages_of_ = to; }
  const page_info* info(std::uint32_t page) const {
    auto it = infos_.find(page);
    return it == infos_.end() ? nullptr : &it->second;
  }
  dl::display* page_display(std::uint32_t page) {
    auto it = displays_.find(page);
    return it == displays_.end() ? nullptr : &it->second;
  }
  const section& page_section(std::uint32_t page) const {
    const page_info* pi = info(page);
    return doc_.sections[pi ? pi->sec : 0];
  }
  const std::string& pages_label() const noexcept { return pages_label_; }

  // ---- the body as a frame ----

  dl::slot fit(double y, double h, double inset_l, double inset_r, std::span<const int> notes) override {
    for (int guard = 0; guard < 512; ++guard) {
      const dl::segment col = cols_[static_cast<std::size_t>(c_.col)];
      dl::slot s;
      s.y = y;
      s.seg[0] = {col.x0 + inset_l, std::max(col.x0 + inset_l + 1.0, col.x1 - inset_r)};
      if (overflow_) return s;  // a balancing trial that already failed
      double extra = 0.0;
      for (int id : notes) {
        if (!has_note(id)) extra += note_height(id);
      }
      if (extra > 0.0 && c_.notes.empty()) extra += kNoteSep;
      const double bottom = std::min(c_.bottom - note_area() - extra, limit_);
      const bool moved = y > c_.region_top + 0.5;
      if (y + h > bottom + 0.01 && (c_.col_content || moved)) {
        next_column();
        if (overflow_) return s;
        y = c_.y;
        continue;
      }
      if (limit_ < kNoLimit && y + h > limit_ + 0.01) overflow_ = true;
      if (c_.floats.empty()) return s;
      // Floating objects: a band they block, or the spans beside them.
      std::vector<dl::segment> free{s.seg[0]};
      double below = 1e9;
      bool blocked = false;
      for (const excl& e : c_.floats) {
        if (e.y0 >= y + h || e.y1 <= y) continue;
        below = std::min(below, e.y1);
        if (e.wrap == wrap_mode::top_bottom) {
          blocked = true;
          continue;
        }
        double a = e.x0, b = e.x1;
        wrap_side side = e.side;
        if (side == wrap_side::largest) {
          side = (e.x0 - s.seg[0].x0) >= (s.seg[0].x1 - e.x1) ? wrap_side::left : wrap_side::right;
        }
        if (side == wrap_side::left) b = 1e9;
        else if (side == wrap_side::right) a = -1e9;
        subtract(free, a, b);
      }
      if (!blocked) {
        int k = 0;
        for (const dl::segment& sg : free) {
          if (sg.x1 - sg.x0 >= kMinSegment && k < 3) s.seg[k++] = sg;
        }
        if (k > 0) {
          s.count = k;
          return s;
        }
      }
      y = std::max(below, y + 1.0);  // under the lowest thing in the way, and on
    }
    dl::slot s;
    s.y = y;
    const dl::segment col = cols_[static_cast<std::size_t>(c_.col)];
    s.seg[0] = {col.x0 + inset_l, std::max(col.x0 + inset_l + 1.0, col.x1 - inset_r)};
    return s;
  }

  void placed(double y, double h, std::span<const int> notes) override {
    if (pages_of_) {
      for (const paragraph* p : pending_pages_) (*pages_of_)[p] = c_.page;
      pending_pages_.clear();
    }
    c_.col_content = c_.page_content = true;
    c_.region_low = std::max(c_.region_low, y + h);
    for (int id : notes) {
      if (has_note(id)) continue;
      const double nh = note_height(id);
      const double area = note_area() + nh + (c_.notes.empty() ? kNoteSep : 0.0);
      const double cap = (c_.bottom - c_.top) * kNoteShare;
      if (c_.carry.empty() && c_.bottom - area >= c_.region_low - 0.01 && area <= cap) {
        c_.notes.push_back(id);
        c_.notes_h += nh;
      } else {
        c_.carry.push_back(id);  // the next page's foot
      }
    }
  }

  double hard_break(dl::span::kind k, double y) override {
    if (k == dl::span::page_break) new_page(false);
    else if (k == dl::span::column_break) next_column();
    return overflow_ ? y : c_.y;
  }

  dl::display* out() override { return drawing(c_.page) ? &displays_[c_.page] : nullptr; }

  void object(int id, double x, double y, double w, double h) override {
    if (dl::display* d = out()) draw_object(id, x, y, w, h, *d);
  }

  [[nodiscard]] bool at_top(double) const override { return !c_.col_content; }
  std::string field_text(dl::field f) override { return field_value(f); }
  dl::segment column() const override { return cols_[static_cast<std::size_t>(c_.col)]; }
  [[nodiscard]] double max_object_h() const override { return std::max(36.0, (c_.bottom - c_.top) * 0.95); }

  // ---- shared with story frames ----

  std::string field_value(dl::field f) const {
    if (f == dl::field::page) return page_label_;
    if (f == dl::field::pages) return pages_known_ ? pages_label_ : std::string{};
    return {};
  }

  // An object drawn at its box: a picture, or a shape and the text in it.
  void draw_object(int id, double x, double y, double w, double h, dl::display& d) {
    if (id < 0 || static_cast<std::size_t>(id) >= doc_.objects.size() || draw_depth_ > kMaxNest) return;
    const object_def& o = doc_.objects[static_cast<std::size_t>(id)];
    switch (o.k) {
      case object_def::kind::picture:
      case object_def::kind::unknown:
        d.add(dl::display::image{o.pixels, x, y, w, h});
        return;
      case object_def::kind::hidden:
        return;
      case object_def::kind::shape:
        break;
    }
    d.add(dl::display::shape{o.shape, x, y, w, h});
    if (o.text.empty()) return;
    ++draw_depth_;
    const auto* saved_rgb = text_rgb_;
    std::uint32_t rgb = o.text_rgb.value_or(0);
    text_rgb_ = o.text_rgb ? &rgb : saved_rgb;
    const double x0 = x + o.ins_l, x1 = std::max(x0 + 1.0, x + w - o.ins_r);
    double ty = y + o.ins_t;
    if (o.text_anchor != 0) {
      story_frame measure(*this, x0, x1, nullptr);
      const double th = lay_blocks(o.text, measure, 0.0, false);
      const double room = h - o.ins_t - o.ins_b - th;
      ty += o.text_anchor == 1 ? room / 2.0 : room;
    }
    story_frame box(*this, x0, x1, &d);
    lay_blocks(o.text, box, ty, false);
    text_rgb_ = saved_rgb;
    --draw_depth_;
  }

  // Blocks into a frame; `flow` is the body (page breaks, floats that wrap).
  double lay_blocks(const std::vector<block>& blocks, dl::frame& f, double y, bool flow) {
    for (const block& b : blocks) y = lay_block(b, f, y, flow);
    return y;
  }

 private:
  static constexpr double kNoLimit = 1e9;
  static constexpr double kNoteSep = 12.0;     // the rule above the notes and its gap
  static constexpr double kNoteShare = 0.6;    // notes take at most this much of a page
  static constexpr double kMinSegment = 24.0;  // a narrower gap beside a float is left empty
  static constexpr std::size_t kMaxFloats = 256;

  struct excl {
    double x0, y0, x1, y1;
    wrap_mode wrap;
    wrap_side side;
  };
  // Where the body is; copied whole to try a column height and go back.
  struct cursor {
    std::uint32_t page = 0;
    int col = 0;
    double y = 0.0, region_top = 0.0, region_low = 0.0;
    double top = 72.0, bottom = 720.0;
    bool page_content = false, col_content = false;
    std::vector<excl> floats;
    std::vector<int> notes, carry;
    double notes_h = 0.0;
    long number = 1;
    std::uint32_t page_sec = 0;
  };

  static bool same_size(const section& a, const section& b) {
    return std::fabs(a.page_w - b.page_w) < 1.0 && std::fabs(a.page_h - b.page_h) < 1.0;
  }

  static void subtract(std::vector<dl::segment>& segs, double a, double b) {
    std::vector<dl::segment> out;
    for (const dl::segment& s : segs) {
      if (b <= s.x0 || a >= s.x1) {
        out.push_back(s);
        continue;
      }
      if (a > s.x0) out.push_back({s.x0, a});
      if (b < s.x1) out.push_back({b, s.x1});
    }
    segs.swap(out);
  }

  bool drawing(std::uint32_t page) const {
    return !measuring_ && std::binary_search(want_.begin(), want_.end(), page);
  }

  status lay_range(std::size_t first, std::size_t end, const job_context* ctx) {
    end = std::min(end, doc_.body.size());
    for (std::size_t i = first; i < end; ++i) {
      if (ctx && (i & 63) == 0 && ctx->cancelled()) return status::cancelled;
      c_.y = lay_block(doc_.body[i], *this, c_.y, true);
    }
    return status::ok;
  }

  // A multi-column section before a continuous break: columns of even height.
  // The column height is found by laying the section out with a limit and
  // halving; only a section that fits on its page is balanced.
  status lay_balanced(const section& s, const job_context* ctx) {
    const cursor saved = c_;
    measuring_ = true;
    status st = lay_range(s.first, s.end, ctx);
    const bool one_page = c_.page == saved.page;
    const double natural = c_.region_low - saved.region_top;
    c_ = saved;
    if (st == status::ok && one_page && natural > 1.0) {
      double lo = natural / static_cast<double>(cols_.size()) - 1.0, hi = natural + 1.0;
      for (int iter = 0; iter < 12 && hi - lo > 1.0 && st == status::ok; ++iter) {
        const double mid = (lo + hi) / 2.0;
        limit_ = saved.region_top + mid;
        overflow_ = false;
        st = lay_range(s.first, s.end, ctx);
        const bool fits = !overflow_ && c_.page == saved.page;
        c_ = saved;
        (fits ? hi : lo) = mid;
      }
      limit_ = saved.region_top + hi;
    }
    overflow_ = false;
    measuring_ = false;
    if (st != status::ok) {
      limit_ = kNoLimit;
      return st;
    }
    st = lay_range(s.first, s.end, ctx);
    limit_ = kNoLimit;
    overflow_ = false;
    return st;
  }

  double lay_block(const block& b, dl::frame& f, double y, bool flow) {
    if (b.para) {
      const paragraph& p = *b.para;
      if (flow && p.p.page_break_before.value_or(false) && c_.page_content) {
        new_page(false);
        y = c_.y;
      }
      for (const item& it : p.items) {
        if (it.k == item::object && it.obj >= 0 && doc_.objects[static_cast<std::size_t>(it.obj)].floating) {
          place_anchor(it.obj, f, y, flow);
        }
      }
      if (pages_of_ && flow) pending_pages_.assign(1, &p);
      return dl::lay_paragraph(eng_, to_para(p), f, y);
    }
    if (b.tbl) return lay_table(*b.tbl, f, y);
    return y;
  }

  dl::para to_para(const paragraph& p) const {
    dl::para out;
    const ppr& pp = p.p;
    out.rtl = pp.bidi.value_or(false);
    out.jc = pp.jc.value_or(dl::align::start);
    out.before = pp.before.value_or(0.0);
    out.after = pp.after.value_or(0.0);
    out.line = pp.line.value_or(240.0);
    const line_rule rule = pp.rule.value_or(line_rule::automatic);
    out.rule = rule == line_rule::exact ? dl::line_rule::exact
               : rule == line_rule::at_least ? dl::line_rule::at_least : dl::line_rule::automatic;
    out.ind_start = pp.ind_left.value_or(0.0);
    out.ind_end = pp.ind_right.value_or(0.0);
    out.ind_first = pp.ind_first.value_or(0.0);
    out.label = p.label;
    out.label_style = style_of(p.mark, st_);
    if (text_rgb_ && !p.mark.rgb) out.label_style.rgb = *text_rgb_;
    out.mark = out.label_style;
    out.spans.reserve(p.items.size());
    for (const item& it : p.items) {
      if (it.r.hidden.value_or(false)) continue;
      dl::span sp;
      const bool rtl = it.r.rtl.value_or(false);
      sp.rtl = rtl;
      sp.style = style_of(it.r, st_, rtl);
      if (text_rgb_ && !it.r.rgb) sp.style.rgb = *text_rgb_;
      sp.raise = it.r.va == valign::super ? sp.style.size_pt * 0.55 : it.r.va == valign::sub ? -sp.style.size_pt * 0.2 : 0.0;
      switch (it.k) {
        case item::text:
          sp.str = it.r.caps.value_or(false) ? upper_ascii(it.str) : it.str;
          sp.note = it.note;
          break;
        case item::field:
          sp.f = it.f;
          break;
        case item::tab: sp.k = dl::span::tab; break;
        case item::line_break: sp.k = dl::span::line_break; break;
        case item::column_break: sp.k = dl::span::column_break; break;
        case item::page_break: sp.k = dl::span::page_break; break;
        case item::object: {
          if (it.obj < 0) continue;
          const object_def& o = doc_.objects[static_cast<std::size_t>(it.obj)];
          if (o.floating) continue;  // placed at its anchor
          sp.k = dl::span::object;
          sp.obj = it.obj;
          sp.obj_w = std::max(o.w, 1.0);
          sp.obj_h = std::max(o.h, 1.0);
          break;
        }
      }
      out.spans.push_back(std::move(sp));
    }
    return out;
  }

  // A floating object, positioned from its anchor paragraph at `para_y`.
  void place_anchor(int id, dl::frame& f, double para_y, bool flow) {
    const object_def& o = doc_.objects[static_cast<std::size_t>(id)];
    const section& s = *geo_;
    const dl::segment col = f.column();
    double rx0 = col.x0, rx1 = col.x1;
    switch (o.hrel) {
      case href::page: rx0 = 0.0; rx1 = s.page_w; break;
      case href::margin: rx0 = s.text_l(); rx1 = s.text_r(); break;
      case href::left_margin: rx0 = 0.0; rx1 = s.margin_l; break;
      case href::right_margin: rx0 = s.page_w - s.margin_r; rx1 = s.page_w; break;
      case href::column: break;
    }
    double ry0 = para_y, ry1 = para_y;
    switch (o.vrel) {
      case vref::page: ry0 = 0.0; ry1 = s.page_h; break;
      case vref::margin: ry0 = s.margin_t; ry1 = s.page_h - s.margin_b; break;
      case vref::top_margin: ry0 = 0.0; ry1 = s.margin_t; break;
      case vref::bottom_margin: ry0 = s.page_h - s.margin_b; ry1 = s.page_h; break;
      case vref::paragraph: break;
    }
    const double w = o.w, h = o.h;
    double x = rx0 + o.hoff;
    if (o.hplace == place::start) x = rx0;
    else if (o.hplace == place::center) x = (rx0 + rx1 - w) / 2.0;
    else if (o.hplace == place::end) x = rx1 - w;
    double y = ry0 + o.voff;
    if (o.vplace == place::start) y = ry0;
    else if (o.vplace == place::center) y = (ry0 + ry1 - h) / 2.0;
    else if (o.vplace == place::end) y = ry1 - h;
    if (flow && o.wrap != wrap_mode::none && c_.floats.size() < kMaxFloats) {
      c_.floats.push_back({x - o.dist_l, y - o.dist_t, x + w + o.dist_r, y + h + o.dist_b, o.wrap, o.side});
    }
    if (dl::display* d = f.out()) {
      const dl::layer saved_layer = d->current;
      const std::int64_t saved_z = d->z;
      d->current = o.behind ? dl::layer::behind : dl::layer::front;
      d->z = o.z;
      draw_object(id, x, y, w, h, *d);
      d->current = saved_layer;
      d->z = saved_z;
    }
  }

  double lay_table(const table& tb, dl::frame& f, double y) {
    if (tb.rows.empty()) return y;
    std::size_t columns = tb.grid.size();
    for (const auto& row : tb.rows) {
      std::size_t n = 0;
      for (const cell& c : row) n += static_cast<std::size_t>(c.span);
      columns = std::max(columns, n);
    }
    if (columns == 0) return y;
    constexpr double kPad = 5.4;  // Word's default cell margins
    constexpr double kRule = 0.5;
    for (const auto& row : tb.rows) {
      // The grid fitted to the column the row lands in.
      auto grid_for = [&](const dl::segment& col, double& left) {
        left = col.x0 + std::clamp(tb.indent, -std::max(0.0, col.x0), 200.0);
        const double avail = std::max(18.0, col.x1 - left);
        std::vector<double> grid = tb.grid;
        if (grid.size() < columns) grid.resize(columns, avail / static_cast<double>(columns));
        double total = 0.0;
        for (double w : grid) total += w;
        if (total > avail && total > 0) {
          for (double& w : grid) w *= avail / total;
        }
        return grid;
      };
      double left = 0.0;
      std::vector<double> grid = grid_for(f.column(), left);
      auto cells_at = [&](double l, const std::vector<double>& g, std::vector<double>& xs, std::vector<double>& ws) {
        xs.clear();
        ws.clear();
        std::size_t col = 0;
        double x = l;
        for (const cell& c : row) {
          double w = 0.0;
          for (long s = 0; s < c.span && col < g.size(); ++s) w += g[col++];
          xs.push_back(x);
          ws.push_back(w);
          x += w;
        }
      };
      std::vector<double> xs, ws;
      cells_at(left, grid, xs, ws);
      // Measure each cell, then place the row.
      double height = 0.0;
      std::vector<int> notes;
      for (std::size_t i = 0; i < row.size(); ++i) {
        story_frame measure(*this, xs[i] + kPad, std::max(xs[i] + kPad + 1.0, xs[i] + ws[i] - kPad), nullptr);
        double cy = lay_blocks(row[i].blocks, measure, 0.0, false);
        height = std::max(height, cy);
        notes.insert(notes.end(), measure.notes.begin(), measure.notes.end());
      }
      height += 2 * kRule + 2.0;
      if (pages_of_ && &f == this) {
        pending_pages_.clear();
        for (const cell& c : row) {
          for (const block& b : c.blocks) {
            if (b.para) pending_pages_.push_back(b.para.get());
          }
        }
      }
      const dl::slot s = f.fit(y, height, 0.0, 0.0, notes);
      y = s.y;
      grid = grid_for(f.column(), left);
      cells_at(left, grid, xs, ws);
      if (dl::display* d = f.out()) {
        for (std::size_t i = 0; i < row.size(); ++i) {
          if (row[i].merged_continue) continue;
          story_frame box(*this, xs[i] + kPad, std::max(xs[i] + kPad + 1.0, xs[i] + ws[i] - kPad), d);
          lay_blocks(row[i].blocks, box, y + kRule + 1.0, false);
        }
        if (tb.borders) {
          const double right = xs.empty() ? left : xs.back() + ws.back();
          d->add(dl::display::rect{left, y, right - left, kRule, 0x000000});
          d->add(dl::display::rect{left, y + height - kRule, right - left, kRule, 0x000000});
          for (std::size_t i = 0; i < xs.size(); ++i) d->add(dl::display::rect{xs[i], y, kRule, height, 0x000000});
          d->add(dl::display::rect{right - kRule, y, kRule, height, 0x000000});
        }
      }
      f.placed(y, height, notes);
      y += height;
    }
    return y + 6.0;
  }

  // ---- pages, columns, sections ----

  void set_columns() {
    const section& s = *geo_;
    cols_.clear();
    const double left = s.text_l(), right = s.text_r(), width = right - left;
    const int n = std::clamp(s.cols, 1, kMaxColumns);
    if (n > 1 && !s.col_equal && s.col_w.size() >= static_cast<std::size_t>(n)) {
      double total = 0.0;
      for (int i = 0; i < n; ++i) total += s.col_w[static_cast<std::size_t>(i)].first + (i + 1 < n ? s.col_w[static_cast<std::size_t>(i)].second : 0.0);
      const double k = total > width && total > 0 ? width / total : 1.0;
      double x = left;
      for (int i = 0; i < n; ++i) {
        const double w = std::max(18.0, s.col_w[static_cast<std::size_t>(i)].first * k);
        cols_.push_back({x, std::min(right, x + w)});
        x += w + s.col_w[static_cast<std::size_t>(i)].second * k;
      }
    } else if (n > 1) {
      const double space = std::min(s.col_space, width / n);
      const double w = (width - space * (n - 1)) / n;
      if (w >= 18.0) {
        for (int i = 0; i < n; ++i) cols_.push_back({left + i * (w + space), left + i * (w + space) + w});
      }
    }
    if (cols_.empty()) cols_.push_back({left, right});
    sep_ = s.col_sep && cols_.size() > 1;
  }

  // The header or footer story a page uses.
  int furniture(const section& s, bool first, long number, bool header) const {
    const int* slots = header ? s.header : s.footer;
    if (s.title_pg && first) return slots[1];
    if (doc_.even_odd && number % 2 == 0) return slots[2];
    return slots[0];
  }

  double story_height(int story, const section& s) {
    if (story < 0 || static_cast<std::size_t>(story) >= doc_.stories.size()) return 0.0;
    const long long key = static_cast<long long>(story) * 100000 + static_cast<long long>(std::lround(s.text_r() - s.text_l()));
    if (auto it = story_h_.find(key); it != story_h_.end()) return it->second;
    story_frame measure(*this, s.text_l(), s.text_r(), nullptr);
    const double h = lay_blocks(doc_.stories[static_cast<std::size_t>(story)].blocks, measure, 0.0, false);
    story_h_.emplace(key, h);
    return h;
  }

  // The page at c_.page starts: its number, body area, notes carried over.
  void begin_page(bool first_of_section) {
    const section& s = *geo_;
    c_.page_sec = sec_;
    if (first_of_section && s.page_start >= 0) c_.number = s.page_start;
    page_label_ = format_number(c_.number, s.num_fmt);
    double top = s.margin_t, bottom = s.page_h - s.margin_b;
    if (!s.top_fixed) top = std::max(top, s.header_d + story_height(furniture(s, first_of_section, c_.number, true), s));
    if (!s.bottom_fixed) bottom = std::min(bottom, s.page_h - s.footer_d - story_height(furniture(s, first_of_section, c_.number, false), s));
    top = std::min(top, s.page_h * 0.6);
    if (bottom < top + 72.0) bottom = std::min(s.page_h, top + 72.0);
    c_.top = top;
    c_.bottom = bottom;
    c_.y = c_.region_top = c_.region_low = top;
    c_.col = 0;
    c_.page_content = c_.col_content = false;
    c_.floats.clear();
    c_.notes.clear();
    c_.notes_h = 0.0;
    std::vector<int> carry;
    carry.swap(c_.carry);
    const double cap = (bottom - top) * kNoteShare;
    for (int id : carry) {
      const double nh = note_height(id);
      if (c_.carry.empty() && (c_.notes.empty() || kNoteSep + c_.notes_h + nh <= cap)) {
        c_.notes.push_back(id);
        c_.notes_h += nh;
      } else {
        c_.carry.push_back(id);
      }
    }
    if (drawing(c_.page)) {
      infos_[c_.page] = page_info{sec_, first_of_section, c_.number, page_label_};
      displays_[c_.page];
    }
  }

  // The page ends: its notes at its foot, and column rules.
  void finish_page() {
    if (!drawing(c_.page)) return;
    dl::display& d = displays_[c_.page];
    const section& s = doc_.sections[c_.page_sec];
    if (!c_.notes.empty()) {
      double y = std::max(c_.top, c_.bottom - note_area());
      d.add(dl::display::rect{s.text_l(), y + kNoteSep / 2.0, std::min(144.0, s.text_r() - s.text_l()), 0.5, 0x000000});
      y += kNoteSep;
      for (int id : c_.notes) {
        auto it = doc_.footnotes.find(id);
        if (it == doc_.footnotes.end()) continue;
        story_frame box(*this, s.text_l(), s.text_r(), &d);
        y = lay_blocks(it->second.blocks, box, y, false);
      }
    }
    rule_columns();
  }

  // The separator lines of the column region that ends here.
  void rule_columns() {
    if (!sep_ || !drawing(c_.page) || c_.region_low <= c_.region_top + 1.0) return;
    dl::display& d = displays_[c_.page];
    for (std::size_t i = 1; i < cols_.size(); ++i) {
      const double x = (cols_[i - 1].x1 + cols_[i].x0) / 2.0;
      d.add(dl::display::rect{x - 0.25, c_.region_top, 0.5, c_.region_low - c_.region_top, 0x000000});
    }
  }

  void new_page(bool first_of_section) {
    if (limit_ < kNoLimit) {  // a balancing trial: past its page is a miss
      overflow_ = true;
      return;
    }
    finish_page();
    ++c_.page;
    ++c_.number;
    begin_page(first_of_section);
  }

  void next_column() {
    if (static_cast<std::size_t>(c_.col) + 1 < cols_.size()) {
      ++c_.col;
      c_.y = c_.region_top;
      c_.col_content = false;
      return;
    }
    new_page(false);
  }

  void start_section(std::uint32_t si) {
    const section& prev = doc_.sections[si - 1];
    const section& s = doc_.sections[si];
    sect_start type = s.type;
    if (type == sect_start::continuous && !same_size(prev, s)) type = sect_start::next_page;
    if (type == sect_start::continuous || type == sect_start::next_column) {
      rule_columns();
      sec_ = si;
      geo_ = &s;
      const std::size_t before = cols_.size();
      set_columns();
      if (type == sect_start::next_column && cols_.size() == before && cols_.size() > 1 && c_.col_content) {
        c_.col = std::min<int>(c_.col, static_cast<int>(cols_.size()) - 1);
        next_column();
        return;
      }
      // A new region under what is on the page.
      c_.col = 0;
      c_.region_top = c_.y = c_.region_low;
      c_.col_content = false;
      return;
    }
    sec_ = si;
    geo_ = &s;
    finish_page();
    set_columns();
    ++c_.page;
    ++c_.number;
    begin_page(true);
    const bool want_even = type == sect_start::even_page, want_odd = type == sect_start::odd_page;
    if ((want_even && c_.number % 2 != 0) || (want_odd && c_.number % 2 == 0)) {
      finish_page();
      ++c_.page;
      ++c_.number;
      begin_page(false);
    }
  }

  // ---- notes ----

  bool has_note(int id) const {
    return std::find(c_.notes.begin(), c_.notes.end(), id) != c_.notes.end() ||
           std::find(c_.carry.begin(), c_.carry.end(), id) != c_.carry.end();
  }
  double note_area() const { return c_.notes.empty() ? 0.0 : kNoteSep + c_.notes_h; }
  double note_height(int id) {
    if (auto it = note_h_.find(id); it != note_h_.end()) return it->second;
    double h = 0.0;
    if (auto it = doc_.footnotes.find(id); it != doc_.footnotes.end()) {
      story_frame measure(*this, geo_->text_l(), geo_->text_r(), nullptr);
      h = lay_blocks(it->second.blocks, measure, 0.0, false);
    }
    note_h_.emplace(id, h);
    return h;
  }

  // Header and footer of a page drawn, with its number and the page count.
  void draw_furniture(std::uint32_t page, const page_info& info) {
    dl::display& d = displays_[page];
    const section& s = doc_.sections[info.sec];
    geo_ = &s;
    page_label_ = info.label;
    pages_known_ = true;
    const int header = furniture(s, info.first, info.number, true);
    const int footer = furniture(s, info.first, info.number, false);
    if (header >= 0) {
      story_frame box(*this, s.text_l(), s.text_r(), &d);
      lay_blocks(doc_.stories[static_cast<std::size_t>(header)].blocks, box, s.header_d, false);
    }
    if (footer >= 0) {
      const double h = story_height(footer, s);
      story_frame box(*this, s.text_l(), s.text_r(), &d);
      lay_blocks(doc_.stories[static_cast<std::size_t>(footer)].blocks, box, s.page_h - s.footer_d - h, false);
    }
  }

  text::engine& eng_;
  const styles& st_;
  const document& doc_;
  std::vector<std::uint32_t> want_;
  cursor c_;
  std::uint32_t sec_ = 0;
  const section* geo_ = nullptr;  // the section whose page objects are placed on
  std::vector<dl::segment> cols_;
  bool sep_ = false;
  double limit_ = kNoLimit;
  bool measuring_ = false, overflow_ = false;
  std::uint32_t pages_ = 1;
  std::string page_label_ = "1", pages_label_;
  bool pages_known_ = false;
  const std::uint32_t* text_rgb_ = nullptr;
  int draw_depth_ = 0;
  std::map<std::uint32_t, dl::display> displays_;
  std::map<std::uint32_t, page_info> infos_;
  std::unordered_map<int, double> note_h_;
  std::unordered_map<long long, double> story_h_;
  std::unordered_map<const paragraph*, std::uint32_t>* pages_of_ = nullptr;
  std::vector<const paragraph*> pending_pages_;
};

void story_frame::object(int id, double x, double y, double w, double h) {
  if (out_) owner_.draw_object(id, x, y, w, h, *out_);
}
std::string story_frame::field_text(dl::field f) { return owner_.field_value(f); }

// The parts read once for every page drawn.
struct parsed {
  styles st;
  numbering nb;
  document doc;
};

status parse(std::span<const std::uint8_t> bytes, const job_context* ctx, parsed& out, bool pixels = true) {
  auto opened = zip::archive::open(bytes);
  if (!opened) return opened.error();
  const zip::archive& zip = opened.value();
  auto main = zip.read("word/document.xml");
  if (!main) return main.error() == status::out_of_memory ? status::out_of_memory : status::corrupt;
  if (ctx && ctx->cancelled()) return status::cancelled;

  const auto view = [](const std::vector<std::uint8_t>& b) {
    return std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
  };
  if (auto theme = zip.read("word/theme/theme1.xml")) read_theme(view(*theme), out.st);
  if (auto s = zip.read("word/styles.xml")) read_styles(view(*s), out.st);
  if (!out.st.default_r.size) out.st.default_r.size = 11.0;  // Word's own when styles say nothing
  if (auto n = zip.read("word/numbering.xml")) read_numbering(view(*n), out.nb);
  std::unordered_map<std::string, std::string> rels;
  if (auto r = zip.read("word/_rels/document.xml.rels")) rels = body_reader::read_rels(view(*r));

  body_reader reader(zip, out.st, out.nb, rels, out.doc, ctx);
  if (!pixels) reader.skip_pixels();
  return reader.read(view(*main));
}

// A paragraph's words as one line: tabs and breaks are spaces, objects and
// hidden runs drop out, a list keeps its number or bullet.
std::string paragraph_text(const paragraph& p) {
  std::string out = p.label;
  if (!out.empty()) out.push_back(' ');
  for (const item& it : p.items) {
    if (it.r.hidden.value_or(false)) continue;
    if (it.k == item::text) {
      out += it.str;
    } else if (it.k != item::object && it.k != item::field && !out.empty() && out.back() != ' ') {
      out.push_back(' ');
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
  std::size_t lead = 0;
  while (lead < out.size() && (out[lead] == ' ' || out[lead] == '\t')) ++lead;
  return out.substr(lead);
}

}  // namespace

result<std::vector<docx_paragraph>> docx_text(std::span<const std::uint8_t> bytes, const job_context* ctx) {
  if (probe(bytes) != format_family::docx) return err(status::unsupported_format);
  auto doc = std::make_unique<parsed>();
  const status read = parse(bytes, ctx, *doc, false);
  if (read != status::ok) return err(read);
  // Laid out once, drawing nothing, to learn each paragraph's page. A layout
  // that fails (cancelled aside) still gives the words, all on page one.
  std::unordered_map<const paragraph*, std::uint32_t> pages;
  std::uint32_t page_count = 1;
  {
    text::engine engine;
    layout lay(engine, doc->st, doc->doc, {});
    lay.record_pages(&pages);
    const status laid = lay.run(ctx);
    if (laid == status::cancelled) return err(status::cancelled);
    if (laid == status::ok) page_count = std::max<std::uint32_t>(1, lay.pages());
  }
  std::vector<docx_paragraph> out;
  std::uint32_t last = 0;
  const auto add = [&](const paragraph& p) {
    std::string t = paragraph_text(p);
    if (t.empty()) return;
    const auto it = pages.find(&p);
    // A paragraph the layout never placed (an empty one) reads as the page before it.
    const std::uint32_t at = it == pages.end() ? last : std::min(it->second, page_count - 1);
    last = std::max(last, at);
    out.push_back(docx_paragraph{last, std::move(t)});
  };
  for (const block& b : doc->doc.body) {
    if (b.para) {
      add(*b.para);
    } else if (b.tbl) {
      for (const auto& row : b.tbl->rows) {
        for (const cell& c : row) {
          for (const block& inner : c.blocks) {
            if (inner.para) add(*inner.para);
          }
        }
      }
    }
  }
  return out;
}

result<std::uint32_t> decode_docx_pages(std::span<const std::uint8_t> bytes, std::span<const std::uint32_t> pages,
                                        std::uint32_t long_edge, const job_context* ctx, void* user,
                                        page_sink sink) {
  if (probe(bytes) != format_family::docx) return err(status::unsupported_format);
  long_edge = std::clamp<std::uint32_t>(long_edge, 16, kDocxLongEdge);
  auto doc = std::make_unique<parsed>();
  const status read = parse(bytes, ctx, *doc);
  if (read != status::ok) return err(read);

  text::engine engine;
  layout lay(engine, doc->st, doc->doc, std::vector<std::uint32_t>(pages.begin(), pages.end()));
  const status laid = lay.run(ctx);
  if (laid != status::ok) return err(laid);
  const std::uint32_t count = lay.pages();

  for (std::uint32_t page : pages) {
    if (page >= count) continue;
    if (ctx && ctx->cancelled()) return err(status::cancelled);
    const section& s = lay.page_section(page);
    const double scale = static_cast<double>(long_edge) / std::max(s.page_w, s.page_h);
    raster out;
    out.format = format_family::docx;
    out.width = static_cast<std::uint32_t>(std::max(1.0, std::round(s.page_w * scale)));
    out.height = static_cast<std::uint32_t>(std::max(1.0, std::round(s.page_h * scale)));
    out.tagged_srgb = true;
    out.page = page;
    out.page_count = count;
    try {
      out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4u, 255);
    } catch (const std::bad_alloc&) {
      return err(status::out_of_memory);
    }
    if (const dl::display* d = lay.page_display(page)) {
      const page_info* pi = lay.info(page);
      const status painted = dl::paint(*d, engine, out, scale, pi ? pi->label : std::to_string(page + 1),
                                       lay.pages_label(), ctx);
      if (painted != status::ok) return err(painted);
    }
    if (sink && !sink(user, std::move(out))) break;
  }
  return count;
}

result<raster> decode_docx(std::span<const std::uint8_t> bytes, std::uint32_t page, const job_context* ctx,
                           std::uint32_t long_edge) {
  std::optional<raster> got;
  const std::uint32_t pages[] = {page};
  auto count = decode_docx_pages(bytes, pages, long_edge, ctx, &got, [](void* user, raster&& r) {
    static_cast<std::optional<raster>*>(user)->emplace(std::move(r));
    return false;
  });
  if (!count) return err(count.error());
  if (!got) return err(status::invalid_arg);
  return std::move(*got);
}

}  // namespace mv::codec
