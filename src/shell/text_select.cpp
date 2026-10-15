// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/text_select.h"

#include <algorithm>
#include <utility>

namespace mv::shell {

bool text_select::same(const source& a, const source& b) noexcept {
  return a.item == b.item && a.geometry.orient == b.geometry.orient &&
         a.geometry.straighten == b.geometry.straighten && a.geometry.crop == b.geometry.crop;
}

bool text_select::show(const source& now) {
  if (same(now, current_)) return false;
  const bool had = has_selection();
  current_ = now;
  reading_ = false;
  read_ = false;
  layout_ = {};
  sel_ = {};
  pressed_ = false;
  return had;
}

bool text_select::set_words(const source& from, text_layout layout) {
  if (!same(from, current_)) return false;
  reading_ = false;
  read_ = true;
  layout_ = std::move(layout);
  sel_ = {};
  return true;
}

bool text_select::over_text(float u, float v, float aspect) const noexcept {
  return word_at(layout_, u, v, aspect) >= 0;
}

bool text_select::press(float u, float v, float aspect, int clicks) {
  pressed_ = false;
  const int hit = word_at(layout_, u, v, aspect);
  if (hit < 0) {
    sel_ = {};
    return false;
  }
  pressed_ = true;
  dragged_ = false;
  press_clicks_ = std::clamp(clicks, 1, 3);
  press_first_ = press_last_ = hit;
  if (press_clicks_ == 3) line_bounds(layout_, hit, press_first_, press_last_);
  if (press_clicks_ == 1) {
    // Nothing yet: a click that never drags clears, as in Photos.
    sel_ = {};
  } else {
    sel_ = {press_first_, press_last_};
  }
  return true;
}

void text_select::drag(float u, float v) {
  if (!pressed_) return;
  const int to = nearest_word(layout_, u, v);
  if (to < 0) return;
  dragged_ = true;
  if (press_clicks_ == 3) {
    int first = 0, last = 0;
    line_bounds(layout_, to, first, last);
    sel_ = to < press_first_ ? text_selection{press_last_, first} : text_selection{press_first_, std::max(last, press_last_)};
    return;
  }
  // From the pressed word (or the double-clicked one) to the word under the
  // pointer, the pressed one always included.
  sel_ = to < press_first_ ? text_selection{press_last_, to} : text_selection{press_first_, std::max(to, press_last_)};
}

void text_select::release() {
  if (pressed_ && press_clicks_ == 1 && !dragged_) sel_ = {};
  pressed_ = false;
}

bool text_select::select_all() {
  if (layout_.empty()) return false;
  sel_ = {0, static_cast<int>(layout_.words.size()) - 1};
  return true;
}

bool text_select::clear() {
  const bool had = has_selection();
  sel_ = {};
  pressed_ = false;
  return had;
}

void text_select::fill(text_highlight& out) const noexcept {
  // selection_rects() without its vector: this runs inside the hosts'
  // noexcept publish.
  out.count = 0;
  if (sel_.empty() || static_cast<std::size_t>(sel_.last()) >= layout_.words.size()) return;
  for (int i = sel_.first(); i <= sel_.last(); ++i) {
    const text_word& w = layout_.words[static_cast<std::size_t>(i)];
    const bool new_line = i == sel_.first() || layout_.words[static_cast<std::size_t>(i - 1)].line != w.line;
    if (new_line) {
      if (out.count == text_highlight::kMaxRects) return;
      float* r = out.rect[out.count++];
      r[0] = w.x0;
      r[1] = w.y0;
      r[2] = w.x1;
      r[3] = w.y1;
      continue;
    }
    float* r = out.rect[out.count - 1];
    r[0] = std::min(r[0], w.x0);
    r[1] = std::min(r[1], w.y0);
    r[2] = std::max(r[2], w.x1);
    r[3] = std::max(r[3], w.y1);
  }
}

}  // namespace mv::shell
