// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// DOCX (docs/plans/audio-and-documents.md §2.5): the WordprocessingML body laid
// out by codec/ itself and drawn with codec/text — readable pages, not a Word
// replica. What it draws:
//
//   paragraphs with their style chain (docDefaults, basedOn, direct), runs with
//   font, size, bold, italic, underline, strike, colour, caps, super/subscript;
//   alignment incl. justify, indents, spacing, line spacing (auto / exact /
//   at least); numbered and bulleted lists; tabs (every half inch); line and
//   page breaks, page-break-before, section breaks; inline and anchored images
//   (as inline, at their extent); tables with grid widths, spans and borders.
//
// What it does not: headers and footers, footnotes, comments, text boxes and
// shapes, columns, floating placement, right-to-left reordering, hyphenation.
// Where a page or line breaks can differ from Word's.
//
// Layout is in points and does not depend on the drawing size, so a preview
// and the full page agree on the page count. Only the page asked for is
// drawn; every page before it is laid out to know where it starts.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "codec/decode.h"
#include "codec/text.h"
#include "codec/xml.h"
#include "codec/zip.h"

namespace mv::codec {
namespace {

using xml::token;

// ---- units ---------------------------------------------------------------------

double twips(std::string_view v) noexcept {  // 1/20 pt
  if (v.empty()) return 0.0;
  char buf[32];
  const std::size_t n = std::min(v.size(), sizeof buf - 1);
  std::memcpy(buf, v.data(), n);
  buf[n] = '\0';
  return std::strtod(buf, nullptr) / 20.0;
}
long number(std::string_view v, long fallback = 0) noexcept {
  if (v.empty()) return fallback;
  char buf[32];
  const std::size_t n = std::min(v.size(), sizeof buf - 1);
  std::memcpy(buf, v.data(), n);
  buf[n] = '\0';
  char* end = nullptr;
  const long out = std::strtol(buf, &end, 10);
  return end == buf ? fallback : out;
}
// w:val on/off: absent or anything but false/0/off is on.
bool on(const xml::reader& r) noexcept {
  const auto v = r.attr("w:val");
  return !(v == "false" || v == "0" || v == "off" || v == "none");
}
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

// ---- properties ------------------------------------------------------------------

enum class valign : std::uint8_t { baseline, super, sub };

// A set of run properties, any of which may be unset; styles merge them.
struct rpr {
  std::optional<std::string> family;
  std::optional<std::string> theme;  // "minor" / "major"
  std::optional<double> size;
  std::optional<bool> bold, italic, underline, strike, caps, hidden;
  std::optional<std::uint32_t> rgb;
  std::optional<valign> va;

  void over(const rpr& o) {
    if (o.family) { family = o.family; theme.reset(); }
    if (o.theme) { theme = o.theme; family.reset(); }
    if (o.size) size = o.size;
    if (o.bold) bold = o.bold;
    if (o.italic) italic = o.italic;
    if (o.underline) underline = o.underline;
    if (o.strike) strike = o.strike;
    if (o.caps) caps = o.caps;
    if (o.hidden) hidden = o.hidden;
    if (o.rgb) rgb = o.rgb;
    if (o.va) va = o.va;
  }
};

enum class align : std::uint8_t { left, center, right, justify };
enum class line_rule : std::uint8_t { automatic, exact, at_least };

struct ppr {
  std::optional<align> jc;
  std::optional<double> before, after, line;  // line: 240ths for auto, pt otherwise
  std::optional<line_rule> rule;
  std::optional<double> ind_left, ind_right, ind_first;  // first < 0 is a hanging indent
  std::optional<bool> page_break_before;
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
  } else if (n == "b") out.bold = on(r);
  else if (n == "i") out.italic = on(r);
  else if (n == "u") out.underline = on(r);
  else if (n == "strike" || n == "dstrike") out.strike = on(r);
  else if (n == "caps" || n == "smallCaps") out.caps = on(r);
  else if (n == "vanish") out.hidden = on(r);
  else if (n == "sz") {
    const long hp = number(r.attr("w:val"), 0);
    if (hp > 0 && hp < 3200) out.size = static_cast<double>(hp) / 2.0;
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

struct ppr_read {
  ppr p;
  std::string style;
  bool section_break = false;
};

ppr_read read_ppr(xml::reader& r) {
  ppr_read out;
  const int depth = r.depth();
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t == token::end && r.depth() < depth) break;
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "pStyle") out.style = std::string(r.attr("w:val"));
    else if (n == "jc") {
      const auto v = r.attr("w:val");
      out.p.jc = v == "center" ? align::center
                 : (v == "right" || v == "end") ? align::right
                 : (v == "both" || v == "distribute") ? align::justify
                                                     : align::left;
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
      out.section_break = true;
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
  for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
    if (t != token::start) continue;
    const auto n = r.local();
    if (n == "majorFont" || n == "minorFont") which = n;
    else if (n == "latin" && !which.empty() && !r.attr("typeface").empty()) {
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
    const long k = std::max<long>(1, v);
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

// ---- the document model --------------------------------------------------------

struct image_ref {
  std::shared_ptr<const raster> pixels;  // null: could not decode, drawn as a box
  double w = 0.0, h = 0.0;               // points
};

struct item {
  enum kind : std::uint8_t { text, tab, line_break, page_break, image } k = text;
  std::string str;
  rpr r;
  image_ref img;
};

struct paragraph {
  ppr p;
  rpr mark;  // the paragraph style's run props (for the list label)
  std::vector<item> items;
  std::string label;  // list number or bullet
  bool section_break = false;
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

struct document {
  double page_w = 612.0, page_h = 792.0;  // US Letter until sectPr says
  double margin_t = 72.0, margin_b = 72.0, margin_l = 72.0, margin_r = 72.0;
  std::vector<block> body;
};

// Parses document.xml into blocks.
class body_reader {
 public:
  body_reader(const zip::archive& zip, const styles& st, const numbering& nb,
              const std::unordered_map<std::string, std::string>& rels, const job_context* ctx)
      : zip_(zip), st_(st), nb_(nb), rels_(rels), ctx_(ctx) {}

  status read(std::string_view doc, document& out) {
    xml::reader r(doc);
    for (token t = r.next(); t != token::eof; t = r.next()) {
      if (t == token::error) return out.body.empty() ? status::corrupt : status::ok;
      if (t != token::start) continue;
      if (r.local() == "body") return read_blocks(r, out.body, &out);
    }
    return status::ok;
  }

 private:
  // Blocks until the element that contains them ends.
  status read_blocks(xml::reader& r, std::vector<block>& out, document* doc) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      if (ctx_ && ctx_->cancelled()) return status::cancelled;
      if (++blocks_ > kMaxBlocks) return status::ok;  // enough to show; the rest is not read
      const auto n = r.local();
      if (n == "p") {
        block b;
        b.para = std::make_unique<paragraph>(read_paragraph(r));
        out.push_back(std::move(b));
      } else if (n == "tbl") {
        block b;
        b.tbl = std::make_unique<table>(read_table(r));
        out.push_back(std::move(b));
      } else if (n == "sectPr" && doc) {
        read_section(r, *doc);
      } else if (n == "sdt" || n == "sdtContent" || n == "customXml" || n == "ins" ||
                 n == "smartTag") {
        continue;  // containers: their blocks are read as if they were here
      } else {
        r.skip_element();
      }
    }
    return status::ok;
  }

  void read_section(xml::reader& r, document& doc) {
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      if (r.local() == "pgSz") {
        const double w = twips(r.attr("w:w")), h = twips(r.attr("w:h"));
        if (w > 72 && h > 72 && w < 14400 && h < 14400) {
          doc.page_w = w;
          doc.page_h = h;
        }
      } else if (r.local() == "pgMar") {
        doc.margin_t = std::clamp(std::fabs(twips(r.attr("w:top"))), 0.0, doc.page_h / 3);
        doc.margin_b = std::clamp(std::fabs(twips(r.attr("w:bottom"))), 0.0, doc.page_h / 3);
        doc.margin_l = std::clamp(twips(r.attr("w:left")), 0.0, doc.page_w / 3);
        doc.margin_r = std::clamp(twips(r.attr("w:right")), 0.0, doc.page_w / 3);
      }
    }
  }

  paragraph read_paragraph(xml::reader& r) {
    paragraph p;
    std::string style = st_.default_para;
    ppr direct;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "pPr") {
        ppr_read pr = read_ppr(r);
        if (!pr.style.empty()) style = pr.style;
        direct = pr.p;
        p.section_break = pr.section_break;
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
    } else if (n == "hyperlink" || n == "ins" || n == "smartTag" || n == "sdt" ||
               n == "sdtContent" || n == "fldSimple" || n == "customXml" || n == "bdo" ||
               n == "dir") {
      const int depth = r.depth();
      for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
        if (t == token::end && r.depth() < depth) break;
        if (t == token::start) read_inline(r, out, inherited);
      }
    } else {
      r.skip_element();  // deletions, bookmarks, comments, proofing marks …
    }
  }

  void read_run(xml::reader& r, std::vector<item>& out, const rpr& inherited) {
    rpr props = inherited;
    std::string char_style;
    const int depth = r.depth();
    auto push_text = [&](std::string s) {
      if (s.empty()) return;
      if (!out.empty() && out.back().k == item::text && out.back().r.size == props.size &&
          out.back().str.size() < 4096 && same(out.back().r, props)) {
        out.back().str += s;
        return;
      }
      item it;
      it.k = item::text;
      it.str = std::move(s);
      it.r = props;
      out.push_back(std::move(it));
    };
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "rPr") {
        rpr direct = read_rpr(r, &char_style);
        rpr eff;
        if (!char_style.empty()) st_.chain(char_style, eff, nullptr);
        eff.over(direct);
        props = inherited;
        props.over(eff);
      } else if (n == "t") {
        std::string text;
        const int d = r.depth();
        for (token u = r.next(); u != token::eof && u != token::error; u = r.next()) {
          if (u == token::end && r.depth() < d) break;
          if (u == token::text) text += r.text();
        }
        push_text(std::move(text));
      } else if (n == "tab" || n == "ptab") {
        item it;
        it.k = item::tab;
        it.r = props;
        out.push_back(std::move(it));
        r.skip_element();
      } else if (n == "br" || n == "cr") {
        item it;
        it.k = r.attr("w:type") == "page" ? item::page_break : item::line_break;
        it.r = props;
        out.push_back(std::move(it));
        r.skip_element();
      } else if (n == "noBreakHyphen") {
        push_text("-");
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
        push_text(std::move(s));
        r.skip_element();
      } else if (n == "drawing") {
        read_drawing(r, out, props);
      } else {
        r.skip_element();  // instrText, footnote references, pictures (VML) …
      }
    }
  }

  static bool same(const rpr& a, const rpr& b) {
    return a.family == b.family && a.theme == b.theme && a.size == b.size && a.bold == b.bold &&
           a.italic == b.italic && a.underline == b.underline && a.strike == b.strike &&
           a.caps == b.caps && a.hidden == b.hidden && a.rgb == b.rgb && a.va == b.va;
  }

  void read_drawing(xml::reader& r, std::vector<item>& out, const rpr& props) {
    double w = 0.0, h = 0.0;
    std::string embed;
    const int depth = r.depth();
    for (token t = r.next(); t != token::eof && t != token::error; t = r.next()) {
      if (t == token::end && r.depth() < depth) break;
      if (t != token::start) continue;
      const auto n = r.local();
      if (n == "extent" && w == 0.0) {
        w = static_cast<double>(number(r.attr("cx"), 0)) / 12700.0;  // EMU -> pt
        h = static_cast<double>(number(r.attr("cy"), 0)) / 12700.0;
      } else if (n == "blip" && embed.empty()) {
        embed = std::string(r.attr("r:embed"));
      } else if (n == "txbx" || n == "txbxContent") {
        r.skip_element();  // text boxes are not drawn
      }
    }
    if (!(w > 0.5) || !(h > 0.5) || w > 10000 || h > 10000) return;
    item it;
    it.k = item::image;
    it.r = props;
    it.img.w = w;
    it.img.h = h;
    it.img.pixels = load_image(embed);
    out.push_back(std::move(it));
  }

  std::shared_ptr<const raster> load_image(const std::string& rel) {
    if (rel.empty()) return nullptr;
    if (auto it = images_.find(rel); it != images_.end()) return it->second;
    std::shared_ptr<const raster> pixels;
    if (auto target = rels_.find(rel); target != rels_.end() && images_loaded_ < kMaxImages) {
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
    images_.emplace(rel, pixels);
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
          b.para = std::make_unique<paragraph>(read_paragraph(r));
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

  static constexpr std::size_t kMaxBlocks = 200000;
  static constexpr int kMaxImages = 256;

  const zip::archive& zip_;
  const styles& st_;
  const numbering& nb_;
  const std::unordered_map<std::string, std::string>& rels_;
  const job_context* ctx_;
  std::size_t blocks_ = 0;
  int images_loaded_ = 0;
  std::unordered_map<long, std::vector<long>> counters_;
  std::unordered_map<std::string, std::shared_ptr<const raster>> images_;
};

std::unordered_map<std::string, std::string> read_rels(std::string_view doc) {
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

// ---- layout ----------------------------------------------------------------------

struct draw_word {
  text::shaped shaped;
  text::style style;
  double x = 0.0, baseline = 0.0;
};
struct draw_rect {
  double x, y, w, h;
  std::uint32_t rgb;
};
struct draw_image {
  std::shared_ptr<const raster> pixels;
  double x, y, w, h;
};
struct page_out {
  std::vector<draw_word> words;
  std::vector<draw_rect> rects;
  std::vector<draw_image> images;
};

text::style style_of(const rpr& r, const styles& st) {
  text::style s;
  s.family = r.family ? *r.family : (r.theme && *r.theme == "major" ? st.major : st.minor);
  s.size_pt = std::clamp(r.size.value_or(11.0), 1.0, 400.0);
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

// Breaks text into words and runs of spaces; each CJK character is its own
// word so a line may break between any two of them.
std::vector<std::string> tokens_of(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  bool cur_space = false;
  auto flush = [&] {
    if (!cur.empty()) out.push_back(std::move(cur));
    cur.clear();
  };
  for (std::size_t i = 0; i < s.size();) {
    const auto b = static_cast<unsigned char>(s[i]);
    std::size_t len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
    len = std::min(len, s.size() - i);
    std::uint32_t cp = b;
    if (len == 3) {
      cp = (static_cast<std::uint32_t>(b & 0x0F) << 12) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F));
    }
    const bool is_space = b == ' ' || b == '\t';
    const bool cjk = len == 3 && ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x9FFF) ||
                                  (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                                  (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x3000 && cp <= 0x303F));
    if (cjk) {
      flush();
      out.emplace_back(s.substr(i, len));
    } else {
      if (!cur.empty() && is_space != cur_space) flush();
      cur_space = is_space;
      cur.append(s, i, len);
    }
    i += len;
  }
  flush();
  return out;
}

class layout {
 public:
  layout(text::engine& eng, const styles& st, const document& doc, std::uint32_t want)
      : eng_(eng), st_(st), doc_(doc), want_(want) {
    top_ = doc.margin_t;
    bottom_ = doc.page_h - doc.margin_b;
    y_ = top_;
  }

  status run(const job_context* ctx) {
    for (std::size_t i = 0; i < doc_.body.size(); ++i) {
      if (ctx && (i & 63) == 0 && ctx->cancelled()) return status::cancelled;
      const block& b = doc_.body[i];
      if (b.para) {
        place_paragraph(*b.para, doc_.margin_l, doc_.page_w - doc_.margin_r, true);
        if (b.para->section_break) new_page();
      } else if (b.tbl) {
        place_table(*b.tbl);
      }
      if (ctx && ctx->cancelled()) return status::cancelled;
    }
    return status::ok;
  }

  [[nodiscard]] std::uint32_t pages() const noexcept { return page_ + 1; }
  page_out& out() noexcept { return out_; }

 private:
  struct piece {
    enum kind : std::uint8_t { word, space, tab, image } k = word;
    text::shaped shaped;
    text::style style;
    double w = 0.0;
    double asc = 0.0, desc = 0.0;
    image_ref img;
    double raise = 0.0;  // super/subscript shift, pt (up positive)
  };
  struct line {
    std::vector<piece> pieces;
    double width = 0.0;
    double asc = 0.0, desc = 0.0, gap = 0.0;
    bool ends_paragraph = false;
    bool page_break_after = false;
  };

  bool drawing() const noexcept { return page_ == want_; }

  void new_page() {
    ++page_;
    y_ = top_;
    page_has_content_ = false;
  }

  // Lays a paragraph out between `left` and `right`; `flow` is false inside a
  // table cell, where the caller handles pages.
  double place_paragraph(const paragraph& p, double left, double right, bool flow,
                         double y_start = 0.0) {
    if (flow && p.p.page_break_before.value_or(false) && page_has_content_) new_page();
    double y = flow ? y_ : y_start;
    const double before = std::clamp(p.p.before.value_or(0.0), 0.0, 400.0);
    const double after = std::clamp(p.p.after.value_or(0.0), 0.0, 400.0);
    if (!(flow && y == top_)) y += before;  // Word drops space before at the top of a page

    const double ind_l = left + std::clamp(p.p.ind_left.value_or(0.0), -left, right - left - 18.0);
    const double ind_r = right - std::clamp(p.p.ind_right.value_or(0.0), 0.0, right - ind_l - 18.0);
    const double first = p.p.ind_first.value_or(0.0);

    // Pieces.
    std::vector<piece> pieces;
    if (!p.label.empty()) {
      piece lab;
      lab.k = piece::word;
      lab.style = style_of(p.mark, st_);
      lab.shaped = eng_.shape(p.label, lab.style);
      lab.w = lab.shaped.advance;
      lab.asc = lab.shaped.ascent;
      lab.desc = lab.shaped.descent;
      pieces.push_back(std::move(lab));
      piece gap;
      gap.k = piece::tab;
      gap.style = pieces.back().style;
      pieces.push_back(std::move(gap));
    }
    std::vector<std::size_t> forced;  // indices in `pieces` where a break follows
    std::vector<bool> forced_page;
    for (const item& it : p.items) {
      if (it.r.hidden.value_or(false)) continue;
      const text::style s = style_of(it.r, st_);
      const double raise = it.r.va == valign::super ? s.size_pt * 0.55 : it.r.va == valign::sub ? -s.size_pt * 0.2 : 0.0;
      switch (it.k) {
        case item::text:
          for (std::string& tok : tokens_of(it.r.caps.value_or(false) ? upper_ascii(it.str) : it.str)) {
            piece pc;
            pc.k = tok[0] == ' ' || tok[0] == '\t' ? piece::space : piece::word;
            pc.style = s;
            pc.shaped = eng_.shape(tok, s);
            pc.w = pc.shaped.advance;
            pc.asc = pc.shaped.ascent + std::max(0.0, raise);
            pc.desc = pc.shaped.descent + std::max(0.0, -raise);
            pc.raise = raise;
            pieces.push_back(std::move(pc));
          }
          break;
        case item::tab: {
          piece pc;
          pc.k = piece::tab;
          pc.style = s;
          pieces.push_back(std::move(pc));
          break;
        }
        case item::line_break:
        case item::page_break:
          forced.push_back(pieces.size());
          forced_page.push_back(it.k == item::page_break && flow);
          break;
        case item::image: {
          piece pc;
          pc.k = piece::image;
          pc.img = it.img;
          const double max_w = ind_r - ind_l;
          const double max_h = (bottom_ - top_) * 0.95;
          const double k = std::min({1.0, max_w / pc.img.w, max_h / pc.img.h});
          pc.img.w *= k;
          pc.img.h *= k;
          pc.w = pc.img.w;
          pc.asc = pc.img.h;
          pieces.push_back(std::move(pc));
          break;
        }
      }
    }

    // Lines.
    const text::shaped empty = eng_.metrics(style_of(p.mark, st_));
    std::vector<line> lines;
    line cur;
    double x = first;  // relative to ind_l
    std::size_t next_forced = 0;
    auto finish = [&](bool page_after) {
      while (!cur.pieces.empty() && cur.pieces.back().k == piece::space) {
        cur.width -= cur.pieces.back().w;
        cur.pieces.pop_back();
      }
      if (cur.asc == 0.0 && cur.desc == 0.0) {
        cur.asc = empty.ascent;
        cur.desc = empty.descent;
        cur.gap = empty.line_gap;
      }
      cur.page_break_after = page_after;
      lines.push_back(std::move(cur));
      cur = line{};
      x = 0.0;
    };
    const double width = ind_r - ind_l;
    for (std::size_t i = 0; i <= pieces.size(); ++i) {
      while (next_forced < forced.size() && forced[next_forced] == i) {
        finish(forced_page[next_forced]);
        ++next_forced;
      }
      if (i == pieces.size()) break;
      piece pc = std::move(pieces[i]);
      if (pc.k == piece::tab) {
        constexpr double kTab = 36.0;
        const double next = (std::floor((x + 0.01) / kTab) + 1.0) * kTab;
        // A hanging indent's first tab goes to the indent itself.
        pc.w = (first < 0 && cur.pieces.size() <= 2 && x < 0) ? -x : next - x;
        if (x + pc.w > width && !cur.pieces.empty()) finish(false);
      } else if (pc.k == piece::space && cur.pieces.empty() && !lines.empty()) {
        continue;  // no space at the start of a wrapped line
      } else if ((pc.k == piece::word || pc.k == piece::image) && x + pc.w > width && !cur.pieces.empty()) {
        finish(false);
      }
      x += pc.w;
      cur.width = x;
      if (pc.k != piece::space && pc.k != piece::tab) {
        cur.asc = std::max(cur.asc, pc.asc);
        cur.desc = std::max(cur.desc, pc.desc);
        cur.gap = std::max(cur.gap, pc.shaped.line_gap);
      }
      cur.pieces.push_back(std::move(pc));
    }
    finish(false);
    lines.back().ends_paragraph = true;

    // Place them.
    const line_rule rule = p.p.rule.value_or(line_rule::automatic);
    const double line_v = p.p.line.value_or(240.0);
    for (std::size_t li = 0; li < lines.size(); ++li) {
      line& ln = lines[li];
      const double natural = ln.asc + ln.desc + ln.gap;
      double h = natural;
      if (rule == line_rule::automatic) h = natural * std::clamp(line_v / 240.0, 0.5, 5.0);
      else if (rule == line_rule::exact) h = std::max(1.0, line_v);
      else h = std::max(natural, line_v);
      if (flow && y + h > bottom_ && page_has_content_) {
        new_page();
        y = y_;
      }
      const double baseline = y + (h - natural) * (rule == line_rule::exact ? 0.5 : 0.0) + ln.asc;
      double offset = 0.0, extra_per_space = 0.0;
      const double room = width - ln.width;
      const align a = p.p.jc.value_or(align::left);
      if (a == align::center) offset = room / 2.0;
      else if (a == align::right) offset = room;
      else if (a == align::justify && !ln.ends_paragraph && !ln.page_break_after && room > 0) {
        int spaces = 0;
        for (const piece& pc : ln.pieces) spaces += pc.k == piece::space ? 1 : 0;
        if (spaces > 0) extra_per_space = room / spaces;
      }
      if (drawing() || !flow) {
        emit_line(ln, ind_l + offset + (li == 0 ? first : 0.0), baseline, extra_per_space, flow);
      }
      y += h;
      page_has_content_ = true;
      if (ln.page_break_after) {
        new_page();
        y = y_;
      }
    }
    y += after;
    if (flow) y_ = y;
    return y;
  }

  void emit_line(const line& ln, double x0, double baseline, double extra_per_space, bool flow) {
    if (flow && !drawing()) return;
    if (!flow && !cell_drawing_) return;
    double x = x0;
    for (const piece& pc : ln.pieces) {
      // Pieces carry their own width; a line's first piece may start at a
      // first-line indent, already folded into the tab / x of the layout.
      switch (pc.k) {
        case piece::word:
          out_.words.push_back({pc.shaped, pc.style, x, baseline - pc.raise});
          break;
        case piece::space:
          if (pc.style.underline || pc.style.strike) {
            out_.words.push_back({pc.shaped, pc.style, x, baseline - pc.raise});
          }
          break;
        case piece::tab:
          break;
        case piece::image:
          out_.images.push_back({pc.img.pixels, x, baseline - pc.img.h, pc.img.w, pc.img.h});
          break;
      }
      x += pc.w + (pc.k == piece::space ? extra_per_space : 0.0);
    }
  }

  void place_table(const table& tb) {
    if (tb.rows.empty()) return;
    const double left = doc_.margin_l + std::clamp(tb.indent, -doc_.margin_l, 200.0);
    const double avail = doc_.page_w - doc_.margin_r - left;
    std::vector<double> grid = tb.grid;
    std::size_t columns = grid.size();
    for (const auto& row : tb.rows) {
      std::size_t n = 0;
      for (const cell& c : row) n += static_cast<std::size_t>(c.span);
      columns = std::max(columns, n);
    }
    if (columns == 0) return;
    if (grid.size() < columns) grid.resize(columns, avail / static_cast<double>(columns));
    double total = 0.0;
    for (double w : grid) total += w;
    if (total > avail && total > 0) {
      for (double& w : grid) w *= avail / total;
    }
    constexpr double kPad = 5.4;  // Word's default cell margins
    constexpr double kRule = 0.5;
    for (const auto& row : tb.rows) {
      // Measure each cell (laid out without drawing), then place the row.
      std::vector<double> xs, ws;
      std::size_t col = 0;
      double x = left;
      for (const cell& c : row) {
        double w = 0.0;
        for (long s = 0; s < c.span && col < grid.size(); ++s) w += grid[col++];
        xs.push_back(x);
        ws.push_back(w);
        x += w;
      }
      double height = 0.0;
      cell_drawing_ = false;
      for (std::size_t i = 0; i < row.size(); ++i) {
        double cy = 0.0;
        for (const block& b : row[i].blocks) {
          if (b.para) cy = place_paragraph(*b.para, xs[i] + kPad, xs[i] + ws[i] - kPad, false, cy);
        }
        height = std::max(height, cy);
      }
      height += 2 * kRule + 2.0;
      if (y_ + height > bottom_ && page_has_content_) new_page();
      if (drawing()) {
        cell_drawing_ = true;
        for (std::size_t i = 0; i < row.size(); ++i) {
          if (row[i].merged_continue) continue;
          double cy = y_ + kRule + 1.0;
          for (const block& b : row[i].blocks) {
            if (b.para) cy = place_paragraph(*b.para, xs[i] + kPad, xs[i] + ws[i] - kPad, false, cy);
          }
        }
        cell_drawing_ = false;
        if (tb.borders) {
          const double right = xs.empty() ? left : xs.back() + ws.back();
          out_.rects.push_back({left, y_, right - left, kRule, 0x000000});
          out_.rects.push_back({left, y_ + height - kRule, right - left, kRule, 0x000000});
          for (std::size_t i = 0; i < xs.size(); ++i) {
            out_.rects.push_back({xs[i], y_, kRule, height, 0x000000});
          }
          out_.rects.push_back({right - kRule, y_, kRule, height, 0x000000});
        }
      }
      y_ += height;
      page_has_content_ = true;
    }
    y_ += 6.0;
  }

  text::engine& eng_;
  const styles& st_;
  const document& doc_;
  std::uint32_t want_;
  std::uint32_t page_ = 0;
  double top_ = 0.0, bottom_ = 0.0, y_ = 0.0;
  bool page_has_content_ = false;
  bool cell_drawing_ = false;
  page_out out_;
};

// Bilinear, straight into the page; a missing picture is a grey box.
void blit(raster& page, const draw_image& im, double scale) {
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

}  // namespace

result<raster> decode_docx(std::span<const std::uint8_t> bytes, std::uint32_t page,
                           const job_context* ctx, std::uint32_t long_edge) {
  if (probe(bytes) != format_family::docx) return err(status::unsupported_format);
  long_edge = std::clamp<std::uint32_t>(long_edge, 16, kDocxLongEdge);
  MV_TRY(zip::archive zip, zip::archive::open(bytes));
  auto main = zip.read("word/document.xml");
  if (!main) return err(main.error() == status::out_of_memory ? status::out_of_memory : status::corrupt);
  if (ctx && ctx->cancelled()) return err(status::cancelled);

  styles st;
  if (auto theme = zip.read("word/theme/theme1.xml")) {
    read_theme(std::string_view(reinterpret_cast<const char*>(theme->data()), theme->size()), st);
  }
  if (auto s = zip.read("word/styles.xml")) {
    read_styles(std::string_view(reinterpret_cast<const char*>(s->data()), s->size()), st);
  }
  if (!st.default_r.size) st.default_r.size = 11.0;  // Word's own when styles say nothing
  numbering nb;
  if (auto n = zip.read("word/numbering.xml")) {
    read_numbering(std::string_view(reinterpret_cast<const char*>(n->data()), n->size()), nb);
  }
  std::unordered_map<std::string, std::string> rels;
  if (auto r = zip.read("word/_rels/document.xml.rels")) {
    rels = read_rels(std::string_view(reinterpret_cast<const char*>(r->data()), r->size()));
  }

  document doc;
  body_reader reader(zip, st, nb, rels, ctx);
  const status read = reader.read(
      std::string_view(reinterpret_cast<const char*>(main->data()), main->size()), doc);
  if (read != status::ok) return err(read);

  text::engine engine;
  layout lay(engine, st, doc, page);
  const status laid = lay.run(ctx);
  if (laid != status::ok) return err(laid);
  const std::uint32_t pages = lay.pages();
  if (page >= pages) return err(status::invalid_arg);

  const double scale = static_cast<double>(long_edge) / std::max(doc.page_w, doc.page_h);
  raster out;
  out.format = format_family::docx;
  out.width = static_cast<std::uint32_t>(std::max(1.0, std::round(doc.page_w * scale)));
  out.height = static_cast<std::uint32_t>(std::max(1.0, std::round(doc.page_h * scale)));
  out.tagged_srgb = true;
  out.page = page;
  out.page_count = pages;
  try {
    out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4u, 255);
  } catch (const std::bad_alloc&) {
    return err(status::out_of_memory);
  }
  page_out& draw = lay.out();
  for (const draw_image& im : draw.images) blit(out, im, scale);
  for (const draw_rect& rc : draw.rects) text::fill_rect(out, rc.x, rc.y, rc.w, rc.h, scale, rc.rgb);
  for (std::size_t i = 0; i < draw.words.size(); ++i) {
    if (ctx && (i & 255) == 0 && ctx->cancelled()) return err(status::cancelled);
    const draw_word& w = draw.words[i];
    engine.draw(w.shaped, w.style, out, w.x, w.baseline, scale);
  }
  return out;
}

}  // namespace mv::codec
