// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/video_timeline.h"

#include <algorithm>
#include <utility>

namespace mv::shell {

using edit::clip::range;

void video_timeline::load(time_ns duration_ns) {
  duration_ = std::max<time_ns>(0, duration_ns);
  pieces_.clear();
  if (duration_ > 0) pieces_.push_back({0, duration_});
  undo_.clear();
  redo_.clear();
}

video_timeline::time_ns video_timeline::length() const noexcept {
  time_ns n = 0;
  for (const range& p : pieces_) n += p.out_ns - p.in_ns;
  return n;
}

video_timeline::time_ns video_timeline::piece_start(std::size_t i) const noexcept {
  time_ns t = 0;
  for (std::size_t k = 0; k < i && k < pieces_.size(); ++k) t += pieces_[k].out_ns - pieces_[k].in_ns;
  return t;
}

std::size_t video_timeline::piece_at(time_ns timeline_t) const noexcept {
  if (pieces_.empty()) return 0;
  time_ns t = 0;
  for (std::size_t i = 0; i < pieces_.size(); ++i) {
    const time_ns len = pieces_[i].out_ns - pieces_[i].in_ns;
    if (timeline_t < t + len) return i;
    t += len;
  }
  return pieces_.size() - 1;
}

video_timeline::time_ns video_timeline::to_source(time_ns timeline_t) const noexcept {
  if (pieces_.empty()) return 0;
  timeline_t = std::clamp<time_ns>(timeline_t, 0, length());
  const std::size_t i = piece_at(timeline_t);
  return pieces_[i].in_ns + (timeline_t - piece_start(i));
}

std::optional<video_timeline::time_ns> video_timeline::to_timeline(time_ns source_t) const noexcept {
  time_ns t = 0;
  for (const range& p : pieces_) {
    if (source_t >= p.in_ns && source_t < p.out_ns) return t + (source_t - p.in_ns);
    t += p.out_ns - p.in_ns;
  }
  if (!pieces_.empty() && source_t == pieces_.back().out_ns) return t;  // the very end
  return std::nullopt;
}

video_timeline::time_ns video_timeline::next_play_start(time_ns source_t, time_ns lead) const noexcept {
  for (const range& p : pieces_) {
    if (source_t < p.in_ns) return p.in_ns;                                // in a cut before this piece
    if (source_t < p.out_ns - std::max<time_ns>(0, lead)) return source_t;  // playing inside it
  }
  return -1;
}

void video_timeline::push_history() {
  undo_.push_back(pieces_);
  if (undo_.size() > kMaxHistory) undo_.erase(undo_.begin());
  redo_.clear();
}

bool video_timeline::replace(std::vector<range> next) {
  if (next.empty() || next == pieces_) return false;
  push_history();
  pieces_ = std::move(next);
  return true;
}

bool video_timeline::split(time_ns timeline_t) {
  if (pieces_.empty()) return false;
  const std::size_t i = piece_at(timeline_t);
  const time_ns at = pieces_[i].in_ns + (timeline_t - piece_start(i));
  if (at - pieces_[i].in_ns < kMinPiece || pieces_[i].out_ns - at < kMinPiece) return false;
  std::vector<range> next = pieces_;
  next.insert(next.begin() + static_cast<std::ptrdiff_t>(i) + 1, range{at, pieces_[i].out_ns});
  next[i].out_ns = at;
  return replace(std::move(next));
}

bool video_timeline::remove(std::size_t index) {
  if (index >= pieces_.size() || pieces_.size() == 1) return false;
  std::vector<range> next = pieces_;
  next.erase(next.begin() + static_cast<std::ptrdiff_t>(index));
  return replace(std::move(next));
}

bool video_timeline::set_in(time_ns timeline_t) {
  if (pieces_.empty() || timeline_t <= 0) return false;
  const std::size_t i = piece_at(timeline_t);
  const time_ns at = pieces_[i].in_ns + (timeline_t - piece_start(i));
  if (pieces_[i].out_ns - at < kMinPiece) return false;
  std::vector<range> next(pieces_.begin() + static_cast<std::ptrdiff_t>(i), pieces_.end());
  next.front().in_ns = at;
  return replace(std::move(next));
}

bool video_timeline::set_out(time_ns timeline_t) {
  if (pieces_.empty() || timeline_t >= length()) return false;
  const std::size_t i = piece_at(timeline_t);
  const time_ns at = pieces_[i].in_ns + (timeline_t - piece_start(i));
  if (at - pieces_[i].in_ns < kMinPiece) return false;
  std::vector<range> next(pieces_.begin(), pieces_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
  next.back().out_ns = at;
  return replace(std::move(next));
}

bool video_timeline::undo() {
  if (undo_.empty()) return false;
  redo_.push_back(std::move(pieces_));
  pieces_ = std::move(undo_.back());
  undo_.pop_back();
  return true;
}

bool video_timeline::redo() {
  if (redo_.empty()) return false;
  undo_.push_back(std::move(pieces_));
  pieces_ = std::move(redo_.back());
  redo_.pop_back();
  return true;
}

bool video_timeline::edited() const noexcept {
  return !(pieces_.size() == 1 && pieces_.front().in_ns == 0 && pieces_.front().out_ns == duration_);
}

edit::clip::request video_timeline::export_request(std::string source, bool exact) const {
  edit::clip::request r;
  r.kind = edit::clip::op::keep_ranges;
  r.source = std::move(source);
  r.ranges = pieces_;
  r.ranges_exact = exact;
  // The last piece to the end of the clip is "to the end", whatever rounding
  // the duration had.
  if (!r.ranges.empty() && r.ranges.back().out_ns >= duration_) r.ranges.back().out_ns = -1;
  return r;
}

}  // namespace mv::shell
