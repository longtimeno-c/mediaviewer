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

}  // namespace mv::test::docxfx
