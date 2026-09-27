// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/trim_state.h"

#include <algorithm>
#include <cstdio>


namespace mv::shell {
namespace clip = edit::clip;

namespace {
constexpr std::int64_t kWalkSlack = 1'000'000;  // 1 ms
}  // namespace

void trim_state::arm(std::string path, std::int64_t duration_ns) {
  if (path != path_) {
    forget();
    path_ = std::move(path);
  }
  if (duration_ns > 0) duration_ns_ = duration_ns;
  armed_ = true;
}

void trim_state::disarm() noexcept {
  armed_ = false;
  preview_ = false;
}

void trim_state::forget() noexcept {
  disarm();
  path_.clear();
  keyframes_.clear();
  index_ready_ = false;
  duration_ns_ = 0;
  in_ = out_ = -1;
}

void trim_state::set_index(const std::string& path, std::vector<std::int64_t> keyframes_ns,
                           std::int64_t duration_ns) {
  if (path != path_) return;
  keyframes_ = std::move(keyframes_ns);
  if (duration_ns > 0) duration_ns_ = duration_ns;
  index_ready_ = true;
}

void trim_state::mark_in(std::int64_t position_ns) noexcept {
  in_ = std::clamp<std::int64_t>(position_ns, 0, std::max<std::int64_t>(0, duration_ns_));
  if (out_ >= 0 && out_ <= in_) out_ = -1;
}

void trim_state::mark_out(std::int64_t position_ns) noexcept {
  out_ = std::clamp<std::int64_t>(position_ns, 0, std::max<std::int64_t>(0, duration_ns_));
  if (in_ >= 0 && in_ >= out_) in_ = -1;
}

void trim_state::clear() noexcept {
  in_ = out_ = -1;
  preview_ = false;
}

clip::range trim_state::requested() const noexcept {
  return {in_ >= 0 ? in_ : 0, out_ >= 0 ? out_ : duration_ns_};
}

clip::range trim_state::keyframe_range() const noexcept {
  const clip::range r = requested();
  if (!index_ready_) return r;
  clip::clip_info info;
  info.duration_ns = duration_ns_;
  info.keyframes_ns = keyframes_;
  return clip::keyframe_range(info, r.in_ns, r.out_ns);
}

bool trim_state::toggle_preview() noexcept {
  preview_ = !preview_ && armed_;
  return preview_;
}

std::int64_t trim_state::prev_keyframe(std::int64_t position_ns) const noexcept {
  auto it = std::lower_bound(keyframes_.begin(), keyframes_.end(), position_ns - kWalkSlack);
  return it == keyframes_.begin() ? position_ns : *(it - 1);
}

std::int64_t trim_state::next_keyframe(std::int64_t position_ns) const noexcept {
  auto it = std::upper_bound(keyframes_.begin(), keyframes_.end(), position_ns + kWalkSlack);
  return it == keyframes_.end() ? position_ns : *it;
}

clip::request trim_state::request(clip::op kind) const {
  clip::request r;
  r.kind = kind;
  r.source = path_;
  r.in_ns = in_ >= 0 ? in_ : 0;
  r.out_ns = out_;  // -1: the end
  return r;
}

std::string trim_state::label() const {
  std::string s;
  s += in_ >= 0 ? "In " + format_clip_time(in_) : std::string("In —");
  s += " · ";
  s += out_ >= 0 ? "Out " + format_clip_time(out_) : std::string("Out —");
  if (has_marker()) {
    const clip::range k = keyframe_range();
    s += index_ready_ ? " · keyframe cut " + format_clip_time(k.in_ns) + "–" + format_clip_time(k.out_ns)
                      : std::string(" · reading keyframes…");
  }
  return s;
}

std::string format_clip_time(std::int64_t ns) {
  const std::int64_t ms = std::max<std::int64_t>(0, ns) / 1'000'000;
  const long long h = ms / 3'600'000, m = (ms / 60'000) % 60, sec = (ms / 1000) % 60, f = ms % 1000;
  char buf[40];
  if (h > 0) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%03lld", h, m, sec, f);
  } else {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld.%03lld", m, sec, f);
  }
  return buf;
}

bool clip_tool_request(std::int32_t packed, const std::string& path, std::int64_t playhead_ns,
                       const trim_state* markers, clip::request& out) {
  const int kind = packed & 0xFF;
  const int option = (packed >> 8) & 0xFF;
  if (kind < static_cast<int>(clip::op::trim_keyframe) || kind > static_cast<int>(clip::op::animation)) return false;
  const bool ranged = markers != nullptr && markers->path() == path && markers->has_marker();
  clip::request r;
  r.kind = static_cast<clip::op>(kind);
  r.source = path;
  if (ranged) {
    r.in_ns = markers->in_ns() >= 0 ? markers->in_ns() : 0;
    r.out_ns = markers->out_ns();
  }
  switch (r.kind) {
    case clip::op::trim_keyframe:
    case clip::op::trim_reencode:
    case clip::op::remove_middle:
      if (!ranged) return false;
      break;
    case clip::op::rotate:
      r.rotate_degrees = option == 2 ? 270 : option == 3 ? 180 : 90;
      break;
    case clip::op::split:
      r.in_ns = playhead_ns;
      r.out_ns = -1;
      break;
    case clip::op::remux:
      r.remux = option == 2 ? clip::remux_target::mkv : clip::remux_target::mp4;
      r.in_ns = 0;
      r.out_ns = -1;
      break;
    case clip::op::frame:
      r.frame = option == 2 ? clip::frame_format::jpeg : clip::frame_format::png;
      r.in_ns = playhead_ns;
      break;
    case clip::op::audio:
      r.audio = option == 2 ? clip::audio_format::wav : option == 3 ? clip::audio_format::flac
                                                                    : clip::audio_format::copy;
      break;
    case clip::op::animation:
      r.animation = option == 2 ? clip::anim_format::webp : clip::anim_format::gif;
      if (!ranged) {
        // No markers: the next few seconds from the playhead, not a whole clip.
        r.in_ns = playhead_ns;
        r.out_ns = playhead_ns + 5'000'000'000;
      }
      break;
  }
  out = std::move(r);
  return true;
}

}  // namespace mv::shell
