// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/agent/script.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "nle/rational.h"

namespace mv::agent {
namespace {

constexpr std::size_t kMaxLines = 2000;
constexpr std::size_t kMaxText = 4000;

std::string clock_ms(std::int64_t ms) {
  if (ms < 0) ms = 0;
  const std::int64_t cs = (ms + 5) / 10;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld:%02lld.%02lld", static_cast<long long>(cs / 6000),
                static_cast<long long>((cs / 100) % 60), static_cast<long long>(cs % 100));
  return buf;
}

std::string seconds(std::int64_t ms) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", static_cast<double>(ms) / 1000.0);
  return buf;
}

std::string file_name(const std::string& path) {
  const std::size_t cut = path.find_last_of("/\\");
  return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string label(const script_line& l) { return l.name.empty() ? file_name(l.path) : l.name; }

std::string heading(const script& s) { return s.project.empty() ? std::string("Script") : s.project; }

// One line of text for a line's header: "Anna_A 0:03.42-0:07.10".
std::string where(const script_line& l) { return label(l) + "  " + clock_ms(l.from_ms) + "-" + clock_ms(l.to_ms); }

// ---- DOCX: a ZIP of three parts, stored (no compression) --------------------------------

std::uint32_t crc32(const std::string& data) {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  std::uint32_t c = 0xFFFFFFFFu;
  for (const unsigned char b : data) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFF));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  put16(out, v & 0xFFFF);
  put16(out, v >> 16);
}

std::vector<std::uint8_t> zip_stored(const std::vector<std::pair<std::string, std::string>>& files) {
  std::vector<std::uint8_t> out;
  std::vector<std::uint8_t> central;
  // 1980-01-01 00:00: a fixed time, so the same script makes the same file.
  constexpr std::uint32_t kTime = 0, kDate = (0 << 9) | (1 << 5) | 1;
  for (const auto& [name, data] : files) {
    const std::uint32_t offset = static_cast<std::uint32_t>(out.size());
    const std::uint32_t crc = crc32(data);
    const auto size = static_cast<std::uint32_t>(data.size());
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, 0x0800);  // UTF-8 names
    put16(out, 0);       // stored
    put16(out, kTime);
    put16(out, kDate);
    put32(out, crc);
    put32(out, size);
    put32(out, size);
    put16(out, static_cast<std::uint32_t>(name.size()));
    put16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), data.begin(), data.end());
    put32(central, 0x02014b50);
    put16(central, 20);
    put16(central, 20);
    put16(central, 0x0800);
    put16(central, 0);
    put16(central, kTime);
    put16(central, kDate);
    put32(central, crc);
    put32(central, size);
    put32(central, size);
    put16(central, static_cast<std::uint32_t>(name.size()));
    put16(central, 0);
    put16(central, 0);
    put16(central, 0);
    put16(central, 0);
    put32(central, 0);
    put32(central, offset);
    central.insert(central.end(), name.begin(), name.end());
  }
  const auto central_at = static_cast<std::uint32_t>(out.size());
  out.insert(out.end(), central.begin(), central.end());
  put32(out, 0x06054b50);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<std::uint32_t>(files.size()));
  put16(out, static_cast<std::uint32_t>(files.size()));
  put32(out, static_cast<std::uint32_t>(central.size()));
  put32(out, central_at);
  put16(out, 0);
  return out;
}

std::string xml_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default:
        // XML 1.0 refuses most control characters.
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t') out += c;
    }
  }
  return out;
}

std::string docx_paragraph(std::string_view text, bool bold, int size_half_points, bool grey = false) {
  std::string p = "<w:p><w:r><w:rPr>";
  if (bold) p += "<w:b/>";
  if (grey) p += "<w:color w:val=\"6E6E73\"/>";
  p += "<w:sz w:val=\"" + std::to_string(size_half_points) + "\"/></w:rPr><w:t xml:space=\"preserve\">";
  p += xml_escape(text);
  p += "</w:t></w:r></w:p>";
  return p;
}

// ---- PDF: Helvetica, A4, the text wrapped -------------------------------------------------

// A UTF-8 string as WinAnsi bytes (Helvetica's encoding): Latin-1 as is, a few
// typographic marks mapped, anything else '?'.
std::string win_ansi(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = c;
    std::size_t n = 1;
    if (c >= 0xF0 && i + 3 < s.size()) {
      cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
           ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
      n = 4;
    } else if (c >= 0xE0 && i + 2 < s.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
           (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
      n = 3;
    } else if (c >= 0xC0 && i + 1 < s.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
      n = 2;
    } else if (c >= 0x80) {
      cp = '?';
    }
    i += n;
    if (cp < 0x20) continue;
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
      out += static_cast<char>(static_cast<unsigned char>(cp));
    } else if (cp == 0x2018) {
      out += "\x91";  // WinAnsi's typographic marks
    } else if (cp == 0x2019) {
      out += "\x92";
    } else if (cp == 0x201C) {
      out += "\x93";
    } else if (cp == 0x201D) {
      out += "\x94";
    } else if (cp == 0x2013) {
      out += "\x96";
    } else if (cp == 0x2014) {
      out += "\x97";
    } else if (cp == 0x2026) {
      out += "\x85";
    } else {
      out += '?';
    }
  }
  return out;
}

std::string pdf_string(std::string_view bytes) {
  std::string out = "(";
  for (const char c : bytes) {
    if (c == '(' || c == ')' || c == '\\') out += '\\';
    out += c;
  }
  return out + ")";
}

// Greedy wrap at `width` characters (Helvetica is not fixed-width; ~0.5 em a
// character at these sizes keeps every line inside the margin).
std::vector<std::string> wrap(const std::string& text, std::size_t width) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string word, line;
  while (in >> word) {
    if (!line.empty() && line.size() + 1 + word.size() > width) {
      lines.push_back(line);
      line.clear();
    }
    while (word.size() > width) {
      lines.push_back(word.substr(0, width));
      word.erase(0, width);
    }
    line += (line.empty() ? "" : " ") + word;
  }
  if (!line.empty() || lines.empty()) lines.push_back(line);
  return lines;
}

}  // namespace

std::int64_t script::length_ms() const noexcept {
  std::int64_t n = 0;
  for (const script_line& l : lines) n += std::max<std::int64_t>(0, l.to_ms - l.from_ms);
  return n;
}

int script::find(std::string_view id) const noexcept {
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

std::string script_path(const std::string& bridge_dir, const std::string& timeline_file) {
  if (!timeline_file.empty()) return timeline_file + ".script.json";
  if (bridge_dir.empty()) return {};
  return (std::filesystem::path(bridge_dir) / "script.json").string();
}

std::string to_json(const script& s) {
  json::writer w;
  w.begin_object();
  w.key("format").string("mediaviewer.script.1");
  w.key("project").string(s.project);
  w.key("next_id").integer(static_cast<std::int64_t>(s.next_id));
  w.key("lines").begin_array();
  for (const script_line& l : s.lines) {
    w.begin_object().key("id").string(l.id).key("path").string(l.path).key("name").string(l.name);
    w.key("from_ms").integer(l.from_ms).key("to_ms").integer(l.to_ms).key("text").string(l.text).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

bool from_json(std::string_view text, script& out, std::string* why) {
  const auto v = json::parse(text, 8);
  const std::string* format = v ? v->str("format") : nullptr;
  if (!v || !format || *format != "mediaviewer.script.1") {
    if (why) *why = "not a MediaViewer script";
    return false;
  }
  script s;
  if (const std::string* p = v->str("project")) s.project = *p;
  const json::value* lines = v->find("lines");
  if (!lines || lines->k != json::kind::array || lines->a.size() > kMaxLines) {
    if (why) *why = "the script's lines are missing or too many";
    return false;
  }
  std::uint64_t top = 0;
  for (const json::value& l : lines->a) {
    const std::string* id = l.str("id");
    const std::string* path = l.str("path");
    const auto from = l.integer("from_ms");
    const auto to = l.integer("to_ms");
    if (!id || id->empty() || !path || path->empty() || !from || !to || *from < 0 || *to <= *from ||
        s.find(*id) >= 0) {
      if (why) *why = "a line of the script is malformed";
      return false;
    }
    script_line x;
    x.id = *id;
    x.path = *path;
    if (const std::string* n = l.str("name")) x.name = *n;
    if (const std::string* t = l.str("text")) x.text = t->substr(0, kMaxText);
    x.from_ms = *from;
    x.to_ms = *to;
    if (x.id.size() > 1 && x.id[0] == 'l') top = std::max<std::uint64_t>(top, std::strtoull(x.id.c_str() + 1, nullptr, 10));
    s.lines.push_back(std::move(x));
  }
  const auto next = v->integer("next_id");
  s.next_id = std::max<std::uint64_t>(top + 1, next && *next > 0 ? static_cast<std::uint64_t>(*next) : 1);
  out = std::move(s);
  return true;
}

script load_script(const std::string& path) {
  script s;
  if (path.empty()) return s;
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) return s;
  std::stringstream ss;
  ss << f.rdbuf();
  script read;
  if (from_json(ss.str(), read, nullptr)) s = std::move(read);
  return s;
}

bool save_script(const std::string& path, const script& s) {
  if (path.empty()) return false;
  const std::string tmp = path + ".mvtmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    const std::string text = to_json(s);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
  if (ec) std::filesystem::remove(std::filesystem::path(tmp), ec);
  return !ec;
}

bool apply(script& s, const std::vector<script_op>& ops, std::string* why, std::vector<std::string>* added) {
  script next = s;
  std::vector<std::string> ids;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const script_op& op = ops[i];
    const std::string at = "op " + std::to_string(i) + ": ";
    auto fail = [&](const std::string& m) {
      if (why) *why = at + m + " (nothing was changed)";
      return false;
    };
    switch (op.k) {
      case script_op::kind::add: {
        if (op.path.empty() || op.from_ms < 0 || op.to_ms <= op.from_ms) return fail("add needs a file and a range");
        if (next.lines.size() >= kMaxLines) return fail("the script is full");
        script_line l;
        l.id = "l" + std::to_string(next.next_id++);
        l.path = op.path;
        l.name = op.name;
        l.from_ms = op.from_ms;
        l.to_ms = op.to_ms;
        l.text = op.text.substr(0, kMaxText);
        const int n = static_cast<int>(next.lines.size());
        const int where = op.index < 0 || op.index > n ? n : op.index;
        next.lines.insert(next.lines.begin() + where, l);
        ids.push_back(l.id);
        break;
      }
      case script_op::kind::remove: {
        const int k = next.find(op.id);
        if (k < 0) return fail("no line " + op.id);
        next.lines.erase(next.lines.begin() + k);
        break;
      }
      case script_op::kind::move: {
        const int k = next.find(op.id);
        if (k < 0) return fail("no line " + op.id);
        script_line l = next.lines[static_cast<std::size_t>(k)];
        next.lines.erase(next.lines.begin() + k);
        const int n = static_cast<int>(next.lines.size());
        const int where = op.index < 0 || op.index > n ? n : op.index;
        next.lines.insert(next.lines.begin() + where, std::move(l));
        break;
      }
      case script_op::kind::text: {
        const int k = next.find(op.id);
        if (k < 0) return fail("no line " + op.id);
        next.lines[static_cast<std::size_t>(k)].text = op.text.substr(0, kMaxText);
        break;
      }
      case script_op::kind::clear:
        next.lines.clear();
        break;
    }
  }
  s = std::move(next);
  if (added) *added = std::move(ids);
  return true;
}

bool parse_script_ops(const json::value& ops, std::vector<script_op>& out, std::string* why) {
  out.clear();
  if (ops.k != json::kind::array || ops.a.empty() || ops.a.size() > 500) {
    if (why) *why = "\"ops\" is an array of 1 to 500 operations";
    return false;
  }
  auto ms = [](const json::value& v, const char* key, std::int64_t& to) {
    const json::value* x = v.find(key);
    if (!x || x->k != json::kind::number) return false;
    to = static_cast<std::int64_t>((x->is_integer ? static_cast<double>(x->i) : x->d) * 1000.0 + 0.5);
    return true;
  };
  for (std::size_t i = 0; i < ops.a.size(); ++i) {
    const json::value& v = ops.a[i];
    const std::string* name = v.str("op");
    const std::string at = "op " + std::to_string(i) + ": ";
    script_op op;
    if (!name) {
      if (why) *why = at + "needs \"op\"";
      return false;
    }
    if (const auto idx = v.integer("at")) op.index = static_cast<int>(*idx);
    if (const std::string* id = v.str("id")) op.id = *id;
    if (const std::string* t = v.str("text")) op.text = *t;
    if (*name == "add") {
      op.k = script_op::kind::add;
      const std::string* path = v.str("path");
      if (!path || !ms(v, "from", op.from_ms) || !ms(v, "to", op.to_ms)) {
        if (why) *why = at + "add needs \"path\", \"from\" and \"to\" (seconds, file time)";
        return false;
      }
      op.path = *path;
      if (const std::string* n = v.str("name")) op.name = *n;
    } else if (*name == "remove" || *name == "move" || *name == "text") {
      op.k = *name == "remove" ? script_op::kind::remove : *name == "move" ? script_op::kind::move : script_op::kind::text;
      if (op.id.empty()) {
        if (why) *why = at + *name + " needs \"id\"";
        return false;
      }
      if (op.k == script_op::kind::text && !v.str("text")) {
        if (why) *why = at + "text needs \"text\"";
        return false;
      }
    } else if (*name == "clear") {
      op.k = script_op::kind::clear;
    } else {
      if (why) *why = at + "unknown op \"" + *name + "\" (add, remove, move, text, clear)";
      return false;
    }
    out.push_back(std::move(op));
  }
  return true;
}

std::string export_txt(const script& s) {
  std::string out = heading(s) + "\n";
  out += "Script, " + std::to_string(s.lines.size()) + " lines, " + clock_ms(s.length_ms()) + "\n\n";
  for (const script_line& l : s.lines) {
    out += "[" + where(l) + "]\n" + l.text + "\n\n";
  }
  return out;
}

std::string export_csv(const script& s) {
  auto cell = [](const std::string& v) {
    // RFC 4180, and a leading = + - @ is quoted text, never a spreadsheet formula.
    std::string q = v;
    if (!q.empty() && std::strchr("=+-@", q[0])) q = "'" + q;
    std::string out = "\"";
    for (const char c : q) {
      if (c == '"') out += '"';
      if (c != '\r') out += c;
    }
    return out + "\"";
  };
  std::string out = "line,clip,file,in_s,out_s,duration_s,text\r\n";
  int n = 1;
  for (const script_line& l : s.lines) {
    out += std::to_string(n++) + "," + cell(label(l)) + "," + cell(l.path) + "," + seconds(l.from_ms) + "," +
           seconds(l.to_ms) + "," + seconds(l.to_ms - l.from_ms) + "," + cell(l.text) + "\r\n";
  }
  return out;
}

std::vector<std::uint8_t> export_docx(const script& s) {
  std::string body;
  body += docx_paragraph(heading(s), true, 36);
  body += docx_paragraph("Script, " + std::to_string(s.lines.size()) + " lines, " + clock_ms(s.length_ms()), false,
                         20, true);
  for (const script_line& l : s.lines) {
    body += docx_paragraph(where(l), true, 18, true);
    body += docx_paragraph(l.text, false, 24);
  }
  const std::string document =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>" +
      body + "</w:body></w:document>";
  const std::string types =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
      "<Override PartName=\"/word/document.xml\" "
      "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
      "</Types>";
  const std::string rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Id=\"rId1\" "
      "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" "
      "Target=\"word/document.xml\"/></Relationships>";
  return zip_stored({{"[Content_Types].xml", types}, {"_rels/.rels", rels}, {"word/document.xml", document}});
}

std::vector<std::uint8_t> export_pdf(const script& s) {
  // Lines of (font, size, grey, text), laid on A4 pages top to bottom.
  struct run {
    bool bold;
    int size;
    bool grey;
    std::string text;
    int space_before;
  };
  std::vector<run> runs;
  runs.push_back({true, 18, false, win_ansi(heading(s)), 0});
  runs.push_back({false, 10, true,
                  win_ansi("Script, " + std::to_string(s.lines.size()) + " lines, " + clock_ms(s.length_ms())), 4});
  for (const script_line& l : s.lines) {
    runs.push_back({true, 9, true, win_ansi(where(l)), 14});
    bool first = true;
    for (const std::string& w : wrap(win_ansi(l.text), 88)) {
      runs.push_back({false, 12, false, w, first ? 3 : 0});
      first = false;
    }
  }
  constexpr int kWidth = 595, kHeight = 842, kMargin = 56;
  std::vector<std::string> pages;
  std::string page;
  int y = kHeight - kMargin;
  for (const run& r : runs) {
    const int step = r.space_before + r.size + r.size / 3;
    if (y - step < kMargin && !page.empty()) {
      pages.push_back(page);
      page.clear();
      y = kHeight - kMargin;
    }
    y -= step;
    page += r.grey ? "0.43 0.43 0.45 rg\n" : "0 0 0 rg\n";
    page += "BT /" + std::string(r.bold ? "F2" : "F1") + " " + std::to_string(r.size) + " Tf " +
            std::to_string(kMargin) + " " + std::to_string(y) + " Td " + pdf_string(r.text) + " Tj ET\n";
  }
  if (!page.empty() || pages.empty()) pages.push_back(page);

  // Objects: 1 catalog, 2 pages, 3 F1, 4 F2, then a page and its content each.
  std::vector<std::string> objects;
  std::string kids;
  for (std::size_t i = 0; i < pages.size(); ++i) kids += std::to_string(5 + 2 * i) + " 0 R ";
  objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objects.push_back("<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(pages.size()) + " >>");
  objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
  objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
  for (std::size_t i = 0; i < pages.size(); ++i) {
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + std::to_string(kWidth) + " " +
                      std::to_string(kHeight) + "] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " +
                      std::to_string(6 + 2 * i) + " 0 R >>");
    objects.push_back("<< /Length " + std::to_string(pages[i].size()) + " >>\nstream\n" + pages[i] + "endstream");
  }
  std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
  std::vector<std::size_t> offsets;
  for (std::size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.size());
    out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const std::size_t xref = out.size();
  out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
  for (const std::size_t o : offsets) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
    out += buf;
  }
  out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xref) + "\n%%EOF\n";
  return std::vector<std::uint8_t>(out.begin(), out.end());
}

std::vector<nle::edit_op> to_edit_ops(const script& s, const nle::timeline& t) {
  std::vector<nle::edit_op> ops;
  const auto& items = t.items();
  for (std::size_t i = 0; i < items.size(); ++i) {
    const nle::item& it = items[i];
    if (it.parent != -1 || !it.in_storyline || it.storyline != -1) continue;
    nle::edit_op op;
    op.k = nle::edit_op::kind::remove;
    op.item = static_cast<int>(i);
    op.ripple = true;
    ops.push_back(op);
  }
  for (const script_line& l : s.lines) {
    nle::edit_op op;
    op.k = nle::edit_op::kind::insert;
    op.path = l.path;
    op.source_in = nle::from_ms(l.from_ms);
    op.duration = nle::from_ms(l.to_ms - l.from_ms);
    op.before = -1;
    ops.push_back(op);
  }
  return ops;
}

}  // namespace mv::agent
