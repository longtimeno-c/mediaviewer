// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit/clip_wire.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "core/json.h"

namespace mv::edit::clip::wire {

bool runs_in_helper(const request& req) noexcept {
  switch (req.kind) {
    case op::trim_reencode:
    case op::frame:
    case op::animation: return true;
    case op::audio: return req.audio != audio_format::copy;
    case op::keep_ranges: return req.ranges_exact;
    default: return false;
  }
}

std::string encode_request(const request& r) {
  json::writer w;
  w.begin_object();
  w.key("op").integer(static_cast<int>(r.kind));
  w.key("source").string(r.source);
  w.key("out_dir").string(r.out_dir);
  w.key("in").integer(r.in_ns);
  w.key("out").integer(r.out_ns);
  w.key("rotate").integer(r.rotate_degrees);
  w.key("remux").integer(static_cast<int>(r.remux));
  w.key("frame").integer(static_cast<int>(r.frame));
  w.key("audio").integer(static_cast<int>(r.audio));
  w.key("animation").integer(static_cast<int>(r.animation));
  w.key("width").integer(r.animation_width);
  w.key("fps").integer(r.animation_fps);
  w.key("quality").integer(r.jpeg_quality);
  // PR 30: the Video Editor's pieces, [in, out] pairs.
  w.key("exact").integer(r.ranges_exact ? 1 : 0);
  w.key("ranges").begin_array();
  for (const range& g : r.ranges) {
    w.begin_array();
    w.integer(g.in_ns);
    w.integer(g.out_ns);
    w.end_array();
  }
  w.end_array();
  // The Video Editor's look, only when it is not the identity. Whole numbers:
  // colour in thousandths, volume and speed in thousandths.
  if (!r.look.identity()) {
    const look& l = r.look;
    w.key("look").begin_object();
    w.key("turns").integer(l.turns);
    w.key("flip_h").integer(l.flip_h ? 1 : 0);
    w.key("flip_v").integer(l.flip_v ? 1 : 0);
    w.key("crop").integer(static_cast<int>(l.crop));
    w.key("light").begin_array();
    for (const float v : l.light.v) w.integer(std::lround(v * 1000.0f));
    w.end_array();
    w.key("mute").integer(l.mute ? 1 : 0);
    w.key("volume").integer(std::lround(l.volume * 1000.0f));
    w.key("fade_in").integer(l.fade_in_ms);
    w.key("fade_out").integer(l.fade_out_ms);
    w.key("speed").integer(std::lround(l.speed * 1000.0f));
    w.end_object();
  }
  w.end_object();
  return w.take();
}

bool decode_request(std::string_view line, request& out) {
  const auto v = json::parse(line);
  if (!v) return false;
  const auto op_n = v->integer("op");
  const std::string* source = v->str("source");
  const std::string* out_dir = v->str("out_dir");
  if (!op_n || *op_n < static_cast<int>(op::trim_keyframe) || *op_n > static_cast<int>(op::keep_ranges)) return false;
  if (!source || source->empty() || !out_dir) return false;
  request r;
  r.kind = static_cast<op>(*op_n);
  r.source = *source;
  r.out_dir = *out_dir;
  const auto get = [&](const char* key, std::int64_t& into) {
    const auto n = v->integer(key);
    if (!n) return false;
    into = *n;
    return true;
  };
  std::int64_t in = 0, outp = 0, rotate = 0, remux = 0, frame = 0, audio = 0, anim = 0, width = 0, fps = 0, q = 0;
  if (!get("in", in) || !get("out", outp) || !get("rotate", rotate) || !get("remux", remux) ||
      !get("frame", frame) || !get("audio", audio) || !get("animation", anim) || !get("width", width) ||
      !get("fps", fps) || !get("quality", q)) {
    return false;
  }
  if (remux < 1 || remux > 2 || frame < 1 || frame > 2 || audio < 1 || audio > 3 || anim < 1 || anim > 2) return false;
  if (width < 0 || width > 100000 || fps < 0 || fps > 1000 || q < 0 || q > 100) return false;
  r.in_ns = in;
  r.out_ns = outp;
  r.rotate_degrees = static_cast<int>(rotate);
  r.remux = static_cast<remux_target>(remux);
  r.frame = static_cast<frame_format>(frame);
  r.audio = static_cast<audio_format>(audio);
  r.animation = static_cast<anim_format>(anim);
  r.animation_width = static_cast<std::uint32_t>(width);
  r.animation_fps = static_cast<std::uint32_t>(fps);
  r.jpeg_quality = static_cast<int>(q);
  // PR 30. Optional, so a line from an older queue still decodes.
  if (const auto exact = v->integer("exact")) r.ranges_exact = *exact != 0;
  if (const json::value* rs = v->find("ranges")) {
    if (rs->k != json::kind::array || rs->a.size() > kMaxRanges) return false;
    for (const json::value& g : rs->a) {
      if (g.k != json::kind::array || g.a.size() != 2) return false;
      for (const json::value& t : g.a) {
        if (t.k != json::kind::number || !t.is_integer) return false;
      }
      r.ranges.push_back({g.a[0].i, g.a[1].i});
    }
  }
  if (const json::value* lv = v->find("look")) {
    if (lv->k != json::kind::object) return false;
    look l;
    const auto num = [&](const char* key, std::int64_t lo, std::int64_t hi, std::int64_t& into) {
      const auto n = lv->integer(key);
      if (!n || *n < lo || *n > hi) return false;
      into = *n;
      return true;
    };
    std::int64_t turns = 0, fh = 0, fv = 0, crop = 0, mute = 0, vol = 0, fin = 0, fout = 0, speed = 0;
    if (!num("turns", 0, 3, turns) || !num("flip_h", 0, 1, fh) || !num("flip_v", 0, 1, fv) ||
        !num("crop", 0, static_cast<int>(crop_ratio::count) - 1, crop) || !num("mute", 0, 1, mute) ||
        !num("volume", 0, 2000, vol) || !num("fade_in", 0, kMaxFadeMs, fin) || !num("fade_out", 0, kMaxFadeMs, fout) ||
        !num("speed", 500, 2000, speed)) {
      return false;
    }
    const json::value* light = lv->find("light");
    if (!light || light->k != json::kind::array || light->a.size() != l.light.v.size()) return false;
    for (std::size_t i = 0; i < light->a.size(); ++i) {
      const json::value& t = light->a[i];
      if (t.k != json::kind::number || !t.is_integer || t.i < -100000 || t.i > 100000) return false;
      l.light.v[i] = static_cast<float>(t.i) / 1000.0f;
    }
    l.turns = static_cast<std::uint8_t>(turns);
    l.flip_h = fh != 0;
    l.flip_v = fv != 0;
    l.crop = static_cast<crop_ratio>(crop);
    l.mute = mute != 0;
    l.volume = static_cast<float>(vol) / 1000.0f;
    l.fade_in_ms = static_cast<std::int32_t>(fin);
    l.fade_out_ms = static_cast<std::int32_t>(fout);
    l.speed = static_cast<float>(speed) / 1000.0f;
    r.look = sanitised(l);
  }
  out = std::move(r);
  return true;
}

std::string progress_line(double fraction) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "progress %d", static_cast<int>(fraction * 10000.0 + 0.5));
  return buf;  // parts per ten thousand: locale-free, no float parsing
}

std::string output_line(std::string_view utf8_path) {
  json::writer w;
  w.string(utf8_path);
  return "output " + w.take();
}

std::string written_line(const range& r) {
  return "written " + std::to_string(r.in_ns) + " " + std::to_string(r.out_ns);
}

std::string error_line(status s) { return "error " + std::to_string(static_cast<int>(s)); }

namespace {

bool starts(std::string_view line, std::string_view word, std::string_view& rest) {
  if (line.substr(0, word.size()) != word) return false;
  rest = line.substr(word.size());
  return true;
}

template <class T>
bool number(std::string_view s, T& out) {
  const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

}  // namespace

void apply_line(std::string_view line, reply& into, const control& ctl) {
  std::string_view rest;
  if (starts(line, "progress ", rest)) {
    int parts = 0;
    if (number(rest, parts) && ctl.progress != nullptr) {
      ctl.progress(ctl.user, std::clamp(parts / 10000.0, 0.0, 1.0));
    }
  } else if (starts(line, "encoder ", rest)) {
    into.result.encoder = std::string(rest);
  } else if (starts(line, "output ", rest)) {
    if (const auto v = json::parse(rest); v && v->k == json::kind::string) into.result.outputs.push_back(v->s);
  } else if (starts(line, "written ", rest)) {
    const std::size_t sp = rest.find(' ');
    if (sp != std::string_view::npos) {
      (void)number(rest.substr(0, sp), into.result.written.in_ns);
      (void)number(rest.substr(sp + 1), into.result.written.out_ns);
    }
  } else if (line == "done") {
    into.done = true;
  } else if (starts(line, "error ", rest)) {
    int code = 0;
    into.failed = true;
    into.error = number(rest, code) && code > 0 && code <= static_cast<int>(status::internal)
                     ? static_cast<status>(code)
                     : status::internal;
  }
  // Anything else is ignored: a newer helper may say more.
}

}  // namespace mv::edit::clip::wire
