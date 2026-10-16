// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Hand-built PPTX packages for the PPTX tests (issue #429): a theme, one
// master, one layout and the slides given, zipped with docx_fixture.h's
// writer. No generator, no corpus.
#pragma once

#include <string>
#include <vector>

#include "docx_fixture.h"

namespace mv::test::pptxfx {

using docxfx::part;

inline const char* kNs =
    R"(xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" )"
    R"(xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" )"
    R"(xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main")";

// Each relationship's type follows from its target, as PowerPoint names them.
inline std::string rels(const std::vector<std::pair<std::string, std::string>>& targets) {
  std::string out = R"(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">)";
  for (const auto& [id, target] : targets) {
    const std::string t = target.find("slideLayout") != std::string::npos   ? "slideLayout"
                          : target.find("slideMaster") != std::string::npos ? "slideMaster"
                          : target.find("theme") != std::string::npos       ? "theme"
                          : target.find("slides/") != std::string::npos     ? "slide"
                          : target.find("presentation") != std::string::npos ? "officeDocument"
                                                                             : "image";
    out += R"(<Relationship Id=")" + id + R"(" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/)" + t +
           R"(" Target=")" + target + R"("/>)";
  }
  return out + "</Relationships>";
}

// A rectangle (or `prst`) at points (x, y, w, h), filled `rgb` ("" for none),
// with optional text paragraphs (a:p elements).
inline std::string sp(double x, double y, double w, double h, const std::string& rgb,
                      const std::string& paras = {}, const std::string& prst = "rect",
                      const std::string& ph = {}) {
  const auto e = [](double pt) { return std::to_string(static_cast<long long>(pt * 12700)); };
  std::string out = R"(<p:sp><p:nvSpPr><p:cNvPr id="2" name="s"/><p:cNvSpPr/><p:nvPr>)" + ph + R"(</p:nvPr></p:nvSpPr><p:spPr>)";
  if (w > 0) {
    out += R"(<a:xfrm><a:off x=")" + e(x) + R"(" y=")" + e(y) + R"("/><a:ext cx=")" + e(w) + R"(" cy=")" + e(h) + R"("/></a:xfrm>)";
  }
  out += R"(<a:prstGeom prst=")" + prst + R"("><a:avLst/></a:prstGeom>)";
  if (!rgb.empty()) out += R"(<a:solidFill><a:srgbClr val=")" + rgb + R"("/></a:solidFill>)";
  out += "</p:spPr>";
  if (!paras.empty()) out += R"(<p:txBody><a:bodyPr/><a:lstStyle/>)" + paras + "</p:txBody>";
  return out + "</p:sp>";
}

inline std::string para(const std::string& text, const std::string& rpr = R"(sz="4000")",
                        const std::string& ppr = {}) {
  return "<a:p>" + (ppr.empty() ? std::string() : "<a:pPr " + ppr + "/>") + "<a:r><a:rPr " + rpr +
         R"(/><a:t>)" + text + "</a:t></a:r></a:p>";
}

inline std::string slide(const std::string& shapes, const std::string& bg = {}) {
  return std::string(R"(<?xml version="1.0"?><p:sld )") + kNs + "><p:cSld>" + bg +
         "<p:spTree><p:nvGrpSpPr/><p:grpSpPr/>" + shapes + "</p:spTree></p:cSld></p:sld>";
}

inline std::string solid_bg(const std::string& colour_xml) {
  return "<p:bg><p:bgPr><a:solidFill>" + colour_xml + "</a:solidFill><a:effectLst/></p:bgPr></p:bg>";
}

// `slides` are p:sld documents; `size` the slide size in points; `master_bg`
// the master's background colour element; `layout_shapes` the layout's tree.
inline std::vector<std::uint8_t> pptx(const std::vector<std::string>& slides, double w = 960, double h = 540,
                                      const std::string& master_bg = R"(<a:srgbClr val="FFFFFF"/>)",
                                      const std::string& layout_shapes = {}, std::vector<part> extra = {},
                                      const std::string& content_types_padding = {}) {
  const auto e = [](double pt) { return std::to_string(static_cast<long long>(pt * 12700)); };
  std::vector<part> parts;
  parts.push_back({"[Content_Types].xml",
                   R"(<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">)"
                   R"(<Default Extension="xml" ContentType="application/xml"/>)" +
                       content_types_padding + "</Types>"});
  parts.push_back({"_rels/.rels", rels({{"rId1", "ppt/presentation.xml"}})});
  std::string ids;
  std::vector<std::pair<std::string, std::string>> pres_rels{{"rIdM", "slideMasters/slideMaster1.xml"}};
  for (std::size_t i = 0; i < slides.size(); ++i) {
    ids += R"(<p:sldId id=")" + std::to_string(256 + i) + R"(" r:id="rIdS)" + std::to_string(i + 1) + R"("/>)";
    pres_rels.push_back({"rIdS" + std::to_string(i + 1), "slides/slide" + std::to_string(i + 1) + ".xml"});
  }
  parts.push_back({"ppt/presentation.xml",
                   std::string(R"(<?xml version="1.0"?><p:presentation )") + kNs +
                       R"(><p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rIdM"/></p:sldMasterIdLst><p:sldIdLst>)" +
                       ids + R"(</p:sldIdLst><p:sldSz cx=")" + e(w) + R"(" cy=")" + e(h) +
                       R"("/><p:defaultTextStyle><a:lvl1pPr><a:defRPr sz="1800"/></a:lvl1pPr></p:defaultTextStyle></p:presentation>)"});
  parts.push_back({"ppt/_rels/presentation.xml.rels", rels(pres_rels)});
  parts.push_back({"ppt/theme/theme1.xml",
                   R"(<?xml version="1.0"?><a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="T"><a:themeElements>)"
                   R"(<a:clrScheme name="T"><a:dk1><a:srgbClr val="000000"/></a:dk1><a:lt1><a:srgbClr val="FFFFFF"/></a:lt1>)"
                   R"(<a:dk2><a:srgbClr val="44546A"/></a:dk2><a:lt2><a:srgbClr val="E7E6E6"/></a:lt2>)"
                   R"(<a:accent1><a:srgbClr val="00B050"/></a:accent1><a:accent2><a:srgbClr val="ED7D31"/></a:accent2></a:clrScheme>)"
                   R"(<a:fontScheme name="T"><a:majorFont><a:latin typeface="Calibri Light"/></a:majorFont><a:minorFont><a:latin typeface="Calibri"/></a:minorFont></a:fontScheme>)"
                   R"(</a:themeElements></a:theme>)"});
  parts.push_back({"ppt/slideMasters/slideMaster1.xml",
                   std::string(R"(<?xml version="1.0"?><p:sldMaster )") + kNs + "><p:cSld>" +
                       "<p:bg><p:bgPr><a:solidFill>" + master_bg + "</a:solidFill></p:bgPr></p:bg>" +
                       "<p:spTree><p:nvGrpSpPr/><p:grpSpPr/>" +
                       sp(36, 20, 888, 80, "", {}, "rect", R"(<p:ph type="title"/>)") +
                       sp(36, 120, 888, 380, "", {}, "rect", R"(<p:ph type="body" idx="1"/>)") +
                       R"(</p:spTree></p:cSld><p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2"/>)"
                       R"(<p:txStyles><p:titleStyle><a:lvl1pPr><a:defRPr sz="4400"/></a:lvl1pPr></p:titleStyle>)"
                       R"(<p:bodyStyle><a:lvl1pPr marL="228600" indent="-228600"><a:buChar char="&#8226;"/><a:defRPr sz="2800"/></a:lvl1pPr></p:bodyStyle>)"
                       R"(<p:otherStyle/></p:txStyles></p:sldMaster>)"});
  parts.push_back({"ppt/slideMasters/_rels/slideMaster1.xml.rels",
                   rels({{"rId1", "../slideLayouts/slideLayout1.xml"}, {"rId2", "../theme/theme1.xml"}})});
  parts.push_back({"ppt/slideLayouts/slideLayout1.xml",
                   std::string(R"(<?xml version="1.0"?><p:sldLayout )") + kNs +
                       "><p:cSld><p:spTree><p:nvGrpSpPr/><p:grpSpPr/>" + layout_shapes +
                       "</p:spTree></p:cSld></p:sldLayout>"});
  parts.push_back({"ppt/slideLayouts/_rels/slideLayout1.xml.rels",
                   rels({{"rId1", "../slideMasters/slideMaster1.xml"}})});
  for (std::size_t i = 0; i < slides.size(); ++i) {
    const std::string n = std::to_string(i + 1);
    parts.push_back({"ppt/slides/slide" + n + ".xml", slides[i], true});
    parts.push_back({"ppt/slides/_rels/slide" + n + ".xml.rels",
                     rels({{"rId1", "../slideLayouts/slideLayout1.xml"}})});
  }
  for (auto& p : extra) parts.push_back(std::move(p));
  return docxfx::zip(parts);
}

}  // namespace mv::test::pptxfx
