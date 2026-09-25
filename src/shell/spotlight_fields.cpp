// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/spotlight_fields.h"

#include <algorithm>

namespace mv::shell {
namespace {

void number(std::vector<spotlight_field>& out, const char* key, double v) {
  if (v <= 0) return;
  spotlight_field f;
  f.key = key;
  f.type = spotlight_field::kind::number;
  f.number = v;
  out.push_back(std::move(f));
}

void text(std::vector<spotlight_field>& out, const char* key, const std::string& v) {
  if (v.empty()) return;
  spotlight_field f;
  f.key = key;
  f.type = spotlight_field::kind::text;
  f.text = v;
  out.push_back(std::move(f));
}

void texts(std::vector<spotlight_field>& out, const char* key, std::vector<std::string> v) {
  if (v.empty()) return;
  spotlight_field f;
  f.key = key;
  f.type = spotlight_field::kind::texts;
  f.texts = std::move(v);
  out.push_back(std::move(f));
}

const std::string* container_tag(const meta::metadata& m, const char* name) {
  for (const auto& p : m.properties) {
    if (p.space == meta::origin::container && p.group == "Container" && p.name == name) return &p.value;
  }
  return nullptr;
}

}  // namespace

std::vector<spotlight_field> spotlight_fields(const meta::metadata& m) {
  std::vector<spotlight_field> out;
  const meta::summary& s = m.s;
  number(out, "kMDItemPixelWidth", s.width);
  number(out, "kMDItemPixelHeight", s.height);
  if (!m.is_clip) return out;
  number(out, "kMDItemDurationSeconds", s.duration_seconds);
  number(out, "kMDItemTotalBitRate", static_cast<double>(s.bitrate_bps));
  number(out, "kMDItemAudioChannelCount", s.audio_channels);
  number(out, "kMDItemAudioSampleRate", s.audio_sample_rate);
  texts(out, "kMDItemCodecs", s.codecs);
  std::vector<std::string> media;
  for (const auto& st : m.streams) {
    const char* kind = st.kind == meta::stream_kind::video ? "Video"
                       : st.kind == meta::stream_kind::audio ? "Sound" : nullptr;
    if (!kind) continue;
    if (std::find(media.begin(), media.end(), kind) == media.end()) media.emplace_back(kind);
  }
  texts(out, "kMDItemMediaTypes", std::move(media));
  // Container stamps (creation_time) are UTC; date_taken_key reads a stamp
  // as UTC, which for a clip is the real instant.
  if (s.date_taken_key > 0) {
    spotlight_field f;
    f.key = "kMDItemContentCreationDate";
    f.type = spotlight_field::kind::date;
    f.unix_seconds = s.date_taken_key;
    out.push_back(std::move(f));
  }
  if (const std::string* title = container_tag(m, "title")) text(out, "kMDItemTitle", *title);
  text(out, "kMDItemAcquisitionModel", s.camera);
  return out;
}

}  // namespace mv::shell
