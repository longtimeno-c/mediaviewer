// SPDX-License-Identifier: GPL-2.0-or-later
#include "edit/clip_wire.h"

#include <algorithm>
#include <charconv>
#include <cstdio>

#include "core/json.h"

namespace mv::edit::clip::wire {

bool runs_in_helper(const request& req) noexcept {
  switch (req.kind) {
    case op::trim_reencode:
    case op::frame:
    case op::animation: return true;
    case op::audio: return req.audio != audio_format::copy;
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
  w.end_object();
  return w.take();
}

bool decode_request(std::string_view line, request& out) {
  const auto v = json::parse(line);
  if (!v) return false;
  const auto op_n = v->integer("op");
  const std::string* source = v->str("source");
  const std::string* out_dir = v->str("out_dir");
  if (!op_n || *op_n < static_cast<int>(op::trim_keyframe) || *op_n > static_cast<int>(op::animation)) return false;
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
