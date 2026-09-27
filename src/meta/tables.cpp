// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "meta/tables.h"

namespace mv::meta {
namespace {

// Tabs and newlines are the table's separators, so they cannot appear in values.
std::string flat(std::string s) {
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

const char* origin_name(origin o) noexcept {
  switch (o) {
    case origin::exif: return "exif";
    case origin::iptc: return "iptc";
    case origin::xmp: return "xmp";
    case origin::container: return "container";
    case origin::computed: return "computed";
  }
  return "exif";
}

const char* kind_name(stream_kind k) noexcept {
  switch (k) {
    case stream_kind::video: return "video";
    case stream_kind::audio: return "audio";
    case stream_kind::subtitle: return "subtitle";
    case stream_kind::attachment: return "attachment";
    case stream_kind::data: return "data";
  }
  return "data";
}

}  // namespace

std::string summary_table(const metadata& m) {
  std::string out;
  for (const auto& row : summary_rows(m)) out += flat(row.label) + "\t" + flat(row.value) + "\n";
  return out;
}

std::string properties_table(const metadata& m) {
  std::string out;
  for (const auto& p : m.properties) {
    out += std::string(origin_name(p.space)) + "\t" + flat(p.group) + "\t" + flat(p.label) + "\t" +
           flat(p.value) + "\t" + flat(p.raw_tag) + "\n";
  }
  return out;
}

std::string streams_table(const metadata& m) {
  std::string out;
  for (const auto& st : m.streams) {
    out += "S\t" + std::to_string(st.index) + "\t" + kind_name(st.kind) + "\t" + flat(st.codec) + "\n";
    for (const auto& f : st.fields) out += "F\t" + flat(f.label) + "\t" + flat(f.value) + "\n";
  }
  for (const auto& c : m.chapters) {
    out += "C\t" + std::to_string(c.start_ms) + "\t" + flat(c.title) + "\n";
  }
  return out;
}

}  // namespace mv::meta
