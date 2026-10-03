// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "nle/fcpxml.h"

#include <algorithm>
#include <map>

namespace mv::nle {
namespace {

std::string escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default:
        // Control characters are not allowed in XML 1.0; a file name cannot
        // usefully carry one anyway.
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
          out += ' ';
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string ms(std::int64_t v) { return v == 0 ? std::string("0s") : std::to_string(v) + "/1000s"; }

std::string name_of(std::string_view path) {
  const std::size_t cut = path.find_last_of("/\\");
  return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

}  // namespace

std::string file_url(std::string_view path) {
  static const char* const kHex = "0123456789ABCDEF";
  std::string p(path);
  std::replace(p.begin(), p.end(), '\\', '/');
  std::string out = "file://";
  // C:/x -> file:///C:/x ; //server/share -> file://server/share
  if (p.size() >= 2 && p[1] == ':') out += '/';
  if (p.rfind("//", 0) == 0) p.erase(0, 2);
  for (std::size_t i = 0; i < p.size(); ++i) {
    const auto c = static_cast<unsigned char>(p[i]);
    const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '.' || c == '_' || c == '~' || c == '/' || (c == ':' && i == 1);
    if (keep) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

clip_range range_around(std::int64_t pts_ms, std::int64_t clip_ms, const fcpxml_options& o) {
  clip_range r;
  const std::int64_t at = std::max<std::int64_t>(0, pts_ms);
  r.start_ms = std::max<std::int64_t>(0, at - std::max<std::int64_t>(0, o.before_ms));
  std::int64_t end = at + std::max<std::int64_t>(1, o.after_ms);
  if (clip_ms > 0) {
    end = std::min(end, clip_ms);
    r.start_ms = std::min(r.start_ms, std::max<std::int64_t>(0, clip_ms - 1));
  }
  r.duration_ms = std::max<std::int64_t>(1, end - r.start_ms);
  return r;
}

std::string fcpxml(std::span<const row> rows, const fcpxml_options& o) {
  // One asset per file, however many of its moments were dragged.
  std::map<std::string, std::string> asset_of;  // path -> id
  std::string resources;
  resources += "    <format id=\"r0\" name=\"FFVideoFormatRateUndefined\"/>\n";
  int next = 1;
  for (const row& x : rows) {
    if (asset_of.count(x.path)) continue;
    const std::string id = "a" + std::to_string(next++);
    asset_of[x.path] = id;
    const bool video = x.kind == 2;
    resources += "    <asset id=\"" + id + "\" name=\"" + escape(name_of(x.path)) + "\" start=\"0s\" duration=\"" +
                 ms(video ? x.duration_ms : 0) + "\" hasVideo=\"1\"" + (video ? " hasAudio=\"1\"" : "") +
                 (video ? "" : " format=\"r0\"") + ">\n";
    resources += "      <media-rep kind=\"original-media\" src=\"" + escape(file_url(x.path)) + "\"/>\n";
    resources += "    </asset>\n";
  }
  std::string clips;
  const std::string kw = escape(o.keyword);
  for (const row& x : rows) {
    const std::string& id = asset_of[x.path];
    const std::string name = escape(name_of(x.path));
    if (x.kind == 2 && x.pts_ms >= 0) {
      const clip_range r = range_around(x.pts_ms, x.duration_ms, o);
      clips += "    <asset-clip ref=\"" + id + "\" name=\"" + name + "\" start=\"" + ms(r.start_ms) +
               "\" duration=\"" + ms(r.duration_ms) + "\">\n";
      // The match, and the clip's other matches inside the range.
      clips += "      <marker start=\"" + ms(x.pts_ms) + "\" duration=\"1/1000s\" value=\"" +
               (kw.empty() ? std::string("MediaViewer match") : kw) + "\"/>\n";
      for (const moment& m : x.moments) {
        if (m.pts_ms == x.pts_ms || m.pts_ms < r.start_ms || m.pts_ms >= r.start_ms + r.duration_ms) continue;
        clips += "      <marker start=\"" + ms(m.pts_ms) + "\" duration=\"1/1000s\" value=\"" +
                 (kw.empty() ? std::string("MediaViewer match") : kw) + "\"/>\n";
      }
      if (!kw.empty()) {
        clips += "      <keyword start=\"" + ms(r.start_ms) + "\" duration=\"" + ms(r.duration_ms) +
                 "\" value=\"" + kw + "\"/>\n";
      }
      clips += "    </asset-clip>\n";
    } else {
      const std::int64_t d = std::max<std::int64_t>(1, o.still_ms);
      clips += "    <asset-clip ref=\"" + id + "\" name=\"" + name + "\" duration=\"" + ms(d) + "\" format=\"r0\">\n";
      if (!kw.empty()) {
        clips += "      <keyword start=\"0s\" duration=\"" + ms(d) + "\" value=\"" + kw + "\"/>\n";
      }
      clips += "    </asset-clip>\n";
    }
  }
  std::string out;
  out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE fcpxml>\n";
  out += "<fcpxml version=\"" + escape(o.version) + "\">\n";
  out += "  <resources>\n" + resources + "  </resources>\n";
  out += "  <event name=\"" + escape(o.event_name) + "\">\n" + clips + "  </event>\n";
  out += "</fcpxml>\n";
  return out;
}

}  // namespace mv::nle
