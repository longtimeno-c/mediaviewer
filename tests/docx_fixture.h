// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Hand-built DOCX packages for the DOCX tests: a stored (uncompressed) zip of
// the parts a test names, so the same bytes are read on every platform. Deflate
// is exercised by zip_deflated() through zlib.
#pragma once

#include <zlib.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mv::test::docxfx {

struct part {
  std::string name;
  std::string data;
  bool deflate = false;
};

inline void put16(std::string& s, unsigned v) {
  s += static_cast<char>(v & 0xFF);
  s += static_cast<char>((v >> 8) & 0xFF);
}
inline void put32(std::string& s, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}

inline std::vector<std::uint8_t> zip(const std::vector<part>& parts) {
  std::string out, central;
  for (const part& p : parts) {
    std::string body = p.data;
    unsigned method = 0;
    if (p.deflate) {
      z_stream z{};
      deflateInit2(&z, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
      std::string packed(deflateBound(&z, static_cast<uLong>(p.data.size())), '\0');
      z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(p.data.data()));
      z.avail_in = static_cast<uInt>(p.data.size());
      z.next_out = reinterpret_cast<Bytef*>(packed.data());
      z.avail_out = static_cast<uInt>(packed.size());
      deflate(&z, Z_FINISH);
      packed.resize(z.total_out);
      deflateEnd(&z);
      body = packed;
      method = 8;
    }
    const auto crc = static_cast<std::uint32_t>(
        crc32(0, reinterpret_cast<const Bytef*>(p.data.data()), static_cast<uInt>(p.data.size())));
    const auto offset = static_cast<std::uint32_t>(out.size());
    put32(out, 0x04034b50);
    put16(out, 20);
    put16(out, 0);
    put16(out, method);
    put16(out, 0);
    put16(out, 0);
    put32(out, crc);
    put32(out, static_cast<std::uint32_t>(body.size()));
    put32(out, static_cast<std::uint32_t>(p.data.size()));
    put16(out, static_cast<unsigned>(p.name.size()));
    put16(out, 0);
    out += p.name;
    out += body;

    put32(central, 0x02014b50);
    put16(central, 20);
    put16(central, 20);
    put16(central, 0);
    put16(central, method);
    put16(central, 0);
    put16(central, 0);
    put32(central, crc);
    put32(central, static_cast<std::uint32_t>(body.size()));
    put32(central, static_cast<std::uint32_t>(p.data.size()));
    put16(central, static_cast<unsigned>(p.name.size()));
    put16(central, 0);
    put16(central, 0);
    put16(central, 0);
    put16(central, 0);
    put32(central, 0);
    put32(central, offset);
    central += p.name;
  }
  const auto dir_offset = static_cast<std::uint32_t>(out.size());
  out += central;
  put32(out, 0x06054b50);
  put16(out, 0);
  put16(out, 0);
  put16(out, static_cast<unsigned>(parts.size()));
  put16(out, static_cast<unsigned>(parts.size()));
  put32(out, static_cast<std::uint32_t>(central.size()));
  put32(out, dir_offset);
  put16(out, 0);
  return {out.begin(), out.end()};
}

constexpr const char* kContentTypes =
    R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
    R"(<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
    R"(<Default Extension="xml" ContentType="application/xml"/>)"
    R"(<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>)"
    R"(</Types>)";

constexpr const char* kStyles =
    R"(<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
    R"(<w:docDefaults><w:rPrDefault><w:rPr><w:sz w:val="22"/></w:rPr></w:rPrDefault>)"
    R"(<w:pPrDefault><w:pPr><w:spacing w:after="160" w:line="259" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults>)"
    R"(<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/></w:style>)"
    R"(<w:style w:type="paragraph" w:styleId="Heading1"><w:basedOn w:val="Normal"/><w:pPr><w:spacing w:before="240"/></w:pPr>)"
    R"(<w:rPr><w:b/><w:sz w:val="48"/><w:color w:val="C00000"/></w:rPr></w:style>)"
    R"(<w:style w:type="table" w:styleId="TableGrid"><w:tblPr><w:tblBorders><w:top w:val="single" w:sz="4"/></w:tblBorders></w:tblPr></w:style>)"
    R"(</w:styles>)";

constexpr const char* kNumbering =
    R"(<w:numbering xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">)"
    R"(<w:abstractNum w:abstractNumId="0"><w:lvl w:ilvl="0"><w:start w:val="1"/><w:numFmt w:val="decimal"/><w:lvlText w:val="%1."/>)"
    R"(<w:pPr><w:ind w:left="720" w:hanging="360"/></w:pPr></w:lvl></w:abstractNum>)"
    R"(<w:abstractNum w:abstractNumId="1"><w:lvl w:ilvl="0"><w:numFmt w:val="bullet"/><w:lvlText w:val=""/></w:lvl></w:abstractNum>)"
    R"(<w:num w:numId="1"><w:abstractNumId w:val="0"/></w:num><w:num w:numId="2"><w:abstractNumId w:val="1"/></w:num>)"
    R"(</w:numbering>)";

// A document.xml around `body` (the w:body's children, sectPr included or not).
inline std::string document(const std::string& body) {
  return R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)"
         R"(<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" )"
         R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">)"
         "<w:body>" + body + "</w:body></w:document>";
}

inline std::string para(const std::string& text, const std::string& ppr = {}, const std::string& rpr = {}) {
  return "<w:p>" + (ppr.empty() ? std::string{} : "<w:pPr>" + ppr + "</w:pPr>") + "<w:r>" +
         (rpr.empty() ? std::string{} : "<w:rPr>" + rpr + "</w:rPr>") + "<w:t xml:space=\"preserve\">" +
         text + "</w:t></w:r></w:p>";
}

// word/_rels/*.rels: {id, target} pairs (the type is not read).
inline std::string rels(const std::vector<std::pair<std::string, std::string>>& targets) {
  std::string out = R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
  for (const auto& [id, target] : targets) {
    out += R"(<Relationship Id=")" + id +
           R"(" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/x" Target=")" + target + R"("/>)";
  }
  return out + "</Relationships>";
}

constexpr const char* kNamespaces =
    R"(xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" )"
    R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" )"
    R"(xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" )"
    R"(xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" )"
    R"(xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture" )"
    R"(xmlns:wps="http://schemas.microsoft.com/office/word/2010/wordprocessingShape" )"
    R"(xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006" )"
    R"(xmlns:v="urn:schemas-microsoft-com:vml" xmlns:w10="urn:schemas-microsoft-com:office:word")";

// A header (`w:hdr`) or footer (`w:ftr`) part around paragraphs.
inline std::string story(const std::string& root, const std::string& body) {
  return std::string(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:)") + root + " " + kNamespaces +
         ">" + body + "</w:" + root + ">";
}

// word/footnotes.xml with Word's two separator notes and `notes` ({id, paragraphs}).
inline std::string notes(const std::string& root, const std::vector<std::pair<int, std::string>>& list) {
  const std::string item = root.substr(0, root.size() - 1);  // footnotes -> footnote
  std::string out = std::string(R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><w:)") + root + " " +
                    kNamespaces + ">";
  out += "<w:" + item + R"( w:type="separator" w:id="-1"><w:p><w:r><w:separator/></w:r></w:p></w:)" + item + ">";
  out += "<w:" + item + R"( w:type="continuationSeparator" w:id="0"><w:p><w:r><w:continuationSeparator/></w:r></w:p></w:)" + item + ">";
  for (const auto& [id, body] : list) {
    out += "<w:" + item + " w:id=\"" + std::to_string(id) + "\">" + body + "</w:" + item + ">";
  }
  return out + "</w:" + root + ">";
}

// A note's paragraph: its number, then `text` in `rgb`.
inline std::string note_para(const std::string& text, const std::string& rgb = "000000") {
  return R"(<w:p><w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteRef/></w:r><w:r><w:rPr><w:color w:val=")" +
         rgb + R"("/></w:rPr><w:t xml:space="preserve"> )" + text + "</w:t></w:r></w:p>";
}

// A paragraph citing footnote `id` after `text`.
inline std::string para_with_note(const std::string& text, int id) {
  return R"(<w:p><w:r><w:t xml:space="preserve">)" + text +
         R"(</w:t></w:r><w:r><w:rPr><w:vertAlign w:val="superscript"/></w:rPr><w:footnoteReference w:id=")" +
         std::to_string(id) + R"("/></w:r></w:p>)";
}

// sectPr: Letter, 1" margins, header/footer at 0.5", then `inner`.
inline std::string sect(const std::string& inner = {}) {
  return R"(<w:sectPr>)" + inner +
         R"(<w:pgSz w:w="12240" w:h="15840"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" w:gutter="0"/></w:sectPr>)";
}

inline std::string page_break() { return R"(<w:p><w:r><w:br w:type="page"/></w:r></w:p>)"; }

// A floating object: `graphic` (a:graphic's child XML) at (x, y) EMU from
// `hrel` / `vrel`, `w` x `h` EMU, wrapped by `wrap` (wrapSquare, wrapNone …).
inline std::string anchor(const std::string& graphic, long long x, long long y, long long w, long long h,
                          const std::string& wrap = R"(<wp:wrapSquare wrapText="bothSides"/>)",
                          const std::string& hrel = "column", const std::string& vrel = "paragraph",
                          bool behind = false) {
  return R"(<w:r><w:drawing><wp:anchor distT="0" distB="0" distL="0" distR="0" simplePos="0" relativeHeight="2" behindDoc=")" +
         std::string(behind ? "1" : "0") + R"(" locked="0" layoutInCell="1" allowOverlap="1"><wp:simplePos x="0" y="0"/>)"
         R"(<wp:positionH relativeFrom=")" + hrel + R"("><wp:posOffset>)" + std::to_string(x) + R"(</wp:posOffset></wp:positionH>)"
         R"(<wp:positionV relativeFrom=")" + vrel + R"("><wp:posOffset>)" + std::to_string(y) + R"(</wp:posOffset></wp:positionV>)"
         R"(<wp:extent cx=")" + std::to_string(w) + R"(" cy=")" + std::to_string(h) + R"("/><wp:effectExtent l="0" t="0" r="0" b="0"/>)" +
         wrap + R"(<wp:docPr id="1" name="Object"/><a:graphic><a:graphicData>)" + graphic +
         R"(</a:graphicData></a:graphic></wp:anchor></w:drawing></w:r>)";
}

// A picture (pic:pic) for anchor() or an inline drawing.
inline std::string picture(const std::string& rel) {
  return R"(<pic:pic><pic:nvPicPr><pic:cNvPr id="1" name="p"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=")" + rel +
         R"("/></pic:blipFill><pic:spPr><a:prstGeom prst="rect"/></pic:spPr></pic:pic>)";
}

// A shape (wps:wsp): `prst` geometry filled `fill`, outlined `line` (empty:
// none), with `paragraphs` in its text box.
inline std::string shape(const std::string& prst, const std::string& fill, const std::string& line,
                         const std::string& paragraphs = {}) {
  std::string out = R"(<wps:wsp><wps:cNvSpPr txBox="1"/><wps:spPr><a:prstGeom prst=")" + prst + R"("><a:avLst/></a:prstGeom>)";
  out += fill.empty() ? "<a:noFill/>" : R"(<a:solidFill><a:srgbClr val=")" + fill + R"("/></a:solidFill>)";
  out += line.empty() ? R"(<a:ln><a:noFill/></a:ln>)"
                      : R"(<a:ln w="12700"><a:solidFill><a:srgbClr val=")" + line + R"("/></a:solidFill></a:ln>)";
  out += "</wps:spPr>";
  if (!paragraphs.empty()) out += "<wps:txbx><w:txbxContent>" + paragraphs + "</w:txbxContent></wps:txbx>";
  out += R"(<wps:bodyPr lIns="91440" tIns="45720" rIns="91440" bIns="45720" anchor="t"/></wps:wsp>)";
  return out;
}

inline std::vector<std::uint8_t> docx(const std::string& body, std::vector<part> extra = {}) {
  std::vector<part> parts = {
      {"[Content_Types].xml", kContentTypes},
      {"word/document.xml", document(body), true},
      {"word/styles.xml", kStyles},
      {"word/numbering.xml", kNumbering},
  };
  for (part& p : extra) parts.push_back(std::move(p));
  return zip(parts);
}

// The issue #429 verify file: a report with a header, a "Page X of Y"
// footer, footnotes, a two-column section between continuous breaks, a
// floating picture with square wrap, a text box and a right-to-left
// paragraph, over three pages. `jpeg` is a picture for rId20.
inline std::vector<std::uint8_t> report(const std::string& jpeg) {
  const std::string lorem =
      "Readable pages are the point: the viewer draws what the document says, in the order it says it, "
      "with the page furniture where Word puts it. ";
  std::string body = para("Quarterly field report", R"(<w:pStyle w:val="Heading1"/>)");
  body += para_with_note(lorem + "The survey covered three valleys", 1);
  body += "<w:p>" + anchor(picture("rId20"), 0, 0, 1371600, 1371600) + R"(<w:r><w:t xml:space="preserve">)" + lorem + lorem +
          lorem + "</w:t></w:r></w:p>";
  body += para_with_note(lorem + "Counts were taken twice", 2);
  // The intro ends a one-column section; the two columns start continuous.
  const std::string refs = R"(<w:headerReference w:type="default" r:id="rId10"/><w:footerReference w:type="default" r:id="rId11"/>)";
  body += R"(<w:p><w:pPr>)" + sect(refs) + R"(</w:pPr><w:r><w:t>Findings by site follow in two columns.</w:t></w:r></w:p>)";
  for (int i = 1; i <= 8; ++i) {
    body += para("Site " + std::to_string(i) + ": " + lorem.substr(0, 90 + static_cast<std::size_t>(i) * 7));
  }
  body += R"(<w:p><w:pPr>)" +
          sect(R"(<w:type w:val="continuous"/><w:cols w:num="2" w:space="720" w:sep="1"/>)") +
          R"(</w:pPr><w:r><w:t>End of the site list.</w:t></w:r></w:p>)";
  body += "<w:p>" +
          anchor(shape("roundRect", "DCE6F2", "1F4E79", para("Text box: what to read first.")), 3657600, 0, 2286000, 685800,
                 "<wp:wrapSquare wrapText=\"bothSides\"/>") +
          R"(<w:r><w:t xml:space="preserve">)" + lorem + lorem + "</w:t></w:r></w:p>";
  body += para("\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xD7\xA2\xD7\x95\xD7\x9C\xD7\x9D 2026 and more", R"(<w:bidi/>)");
  for (int i = 0; i < 30; ++i) body += para("Appendix line " + std::to_string(i + 1) + ". " + lorem);
  body += sect(R"(<w:type w:val="continuous"/>)");
  const std::string header = story("hdr", para("Field report \xE2\x80\x94 confidential", R"(<w:jc w:val="right"/>)", R"(<w:color w:val="C00000"/>)"));
  const std::string footer = story(
      "ftr", R"(<w:p><w:pPr><w:jc w:val="center"/></w:pPr><w:r><w:t xml:space="preserve">Page </w:t></w:r>)"
             R"(<w:r><w:fldChar w:fldCharType="begin"/></w:r><w:r><w:instrText xml:space="preserve"> PAGE </w:instrText></w:r>)"
             R"(<w:r><w:fldChar w:fldCharType="separate"/></w:r><w:r><w:t>1</w:t></w:r><w:r><w:fldChar w:fldCharType="end"/></w:r>)"
             R"(<w:r><w:t xml:space="preserve"> of </w:t></w:r><w:fldSimple w:instr=" NUMPAGES "><w:r><w:t>1</w:t></w:r></w:fldSimple></w:p>)");
  const std::string footnotes = notes("footnotes", {{1, note_para("Valleys north of the river only.", "0000FF")},
                                                   {2, note_para("A second pass a week later.", "0000FF")}});
  return docx(body, {{"word/_rels/document.xml.rels",
                      rels({{"rId10", "header1.xml"}, {"rId11", "footer1.xml"}, {"rId20", "media/image1.jpeg"}})},
                     {"word/header1.xml", header},
                     {"word/footer1.xml", footer},
                     {"word/footnotes.xml", footnotes},
                     {"word/media/image1.jpeg", jpeg}});
}

}  // namespace mv::test::docxfx
