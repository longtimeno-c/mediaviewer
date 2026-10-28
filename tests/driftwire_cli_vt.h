// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the driftwire command line's tests share (tests/test_driftwire_cli*.cpp):
// a small terminal emulator that holds what was drawn, and a console for a
// session that a test types into from its own thread.
#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "addons/driftwire/cli/term.h"
#include "addons/driftwire/cli/text.h"

namespace mv::test {

// A screen `rows` high over a scrollback: printable UTF-8 (two cells for a
// wide character), CR, LF (scrolling at the bottom row), BS, CSI n A / n C,
// CSI J, SGR and private modes (recorded, not drawn; autowrap, ?7, obeyed),
// OSC (recorded), xterm's pending wrap at the last column. Everything the command line
// writes, and nothing it does not: an unknown sequence is put in `errors`
// (no assertion here: a session draws from its own thread).
class vt_screen {
 public:
  vt_screen(int columns, int rows) : columns_(columns), rows_(rows), screen_(static_cast<std::size_t>(rows)) {
    for (auto& r : screen_) r.assign(static_cast<std::size_t>(columns_), " ");
  }

  void write(std::string_view b) {
    writes.emplace_back(b);
    for (std::size_t i = 0; i < b.size();) {
      const char c = b[i];
      if (c == '\x1B') {
        if (i + 1 >= b.size()) {
          errors.emplace_back("a lone ESC at the end of a write");
          return;
        }
        if (b[i + 1] == ']') {  // OSC ... BEL (or ESC \)
          std::size_t j = i + 2;
          while (j < b.size() && b[j] != '\x07' && !(b[j] == '\x1B' && j + 1 < b.size() && b[j + 1] == '\\')) ++j;
          osc.emplace_back(b.substr(i + 2, j - i - 2));
          i = j < b.size() && b[j] == '\x07' ? j + 1 : j + 2;
          continue;
        }
        if (b[i + 1] != '[') {
          errors.emplace_back("ESC followed by something other than [ or ]");
          return;
        }
        std::size_t j = i + 2;
        while (j < b.size() && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';' || b[j] == '?')) ++j;
        if (j >= b.size()) {
          errors.emplace_back("a CSI cut off at the end of a write");
          return;
        }
        csi(b.substr(i + 2, j - i - 2), b[j]);
        i = j + 1;
        continue;
      }
      if (c == '\r') {
        x_ = 0;
        ++i;
        continue;
      }
      if (c == '\n') {
        line_feed();
        if (x_ >= columns_) x_ = columns_ - 1;
        ++i;
        continue;
      }
      if (c == '\b') {
        if (x_ >= columns_) x_ = columns_ - 1;
        x_ = std::max(0, x_ - 1);
        ++i;
        continue;
      }
      if (c == '\x07') {
        ++bells;
        ++i;
        continue;
      }
      std::size_t n = 1;
      const auto u = static_cast<unsigned char>(c);
      if (u >= 0xF0) {
        n = 4;
      } else if (u >= 0xE0) {
        n = 3;
      } else if (u >= 0xC0) {
        n = 2;
      }
      put(b.substr(i, n));
      i += n;
    }
  }

  // Every row ever shown, scrolled off first, trailing blanks and blank rows at the end dropped.
  [[nodiscard]] std::vector<std::string> lines() const {
    std::vector<std::string> out = scrolled_;
    for (const auto& r : screen_) out.push_back(row_text(r));
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
  }
  // The visible rows only.
  [[nodiscard]] std::vector<std::string> visible() const {
    std::vector<std::string> out;
    for (const auto& r : screen_) out.push_back(row_text(r));
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
  }
  // The visible rows as cells (a wide character's second cell is "").
  [[nodiscard]] const std::vector<std::vector<std::string>>& cells() const noexcept { return screen_; }
  [[nodiscard]] int x() const noexcept { return x_; }
  [[nodiscard]] int y() const noexcept { return y_; }
  [[nodiscard]] bool cursor_shown() const noexcept { return cursor_shown_; }
  [[nodiscard]] bool synchronized() const noexcept { return sync_; }
  [[nodiscard]] bool autowrap() const noexcept { return autowrap_; }
  [[nodiscard]] int columns() const noexcept { return columns_; }
  [[nodiscard]] int rows() const noexcept { return rows_; }

  std::vector<std::string> writes;
  std::vector<int> ups;  // every CSI n A, in order
  std::vector<std::string> osc;
  std::vector<std::string> errors;
  int bells = 0;
  // The columns a character takes on this terminal: FTXUI's (display_width)
  // unless set, as for a terminal whose width table is newer than FTXUI's.
  std::function<int(std::string_view)> measure;

 private:
  static std::string row_text(const std::vector<std::string>& r) {
    std::string s;
    for (const auto& cell : r) s += cell;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
  }

  void line_feed() {
    if (y_ + 1 < rows_) {
      ++y_;
      return;
    }
    scrolled_.push_back(row_text(screen_.front()));
    screen_.erase(screen_.begin());
    screen_.emplace_back(static_cast<std::size_t>(columns_), " ");
  }

  void put(std::string_view glyph) {
    const int w = std::max(1, measure ? measure(glyph) : mv::driftwire::cli::display_width(glyph));
    if (x_ + w > columns_) {  // the pending wrap, or a wide character at the edge
      if (autowrap_) {
        x_ = 0;
        line_feed();
      } else {
        x_ = std::max(0, columns_ - w);  // no wrap: the last cells are written over
      }
    }
    auto& row = screen_[static_cast<std::size_t>(y_)];
    row[static_cast<std::size_t>(x_)] = std::string(glyph);
    if (w == 2) row[static_cast<std::size_t>(x_) + 1] = "";
    x_ += w;  // == columns_: the pending wrap
  }

  void csi(std::string_view params, char f) {
    const bool priv = !params.empty() && params[0] == '?';
    const int n = params.empty() || priv ? 1 : std::max(1, std::atoi(std::string(params).c_str()));
    switch (f) {
      case 'A':
        ups.push_back(n);
        y_ = std::max(0, y_ - n);
        if (x_ >= columns_) x_ = columns_ - 1;
        break;
      case 'C': x_ = std::min(columns_ - 1, x_ + n); break;
      case 'J': {
        if (!(params.empty() || params == "0")) errors.emplace_back("CSI J other than to the end");
        auto& row = screen_[static_cast<std::size_t>(y_)];
        for (int i = std::min(x_, columns_); i < columns_; ++i) row[static_cast<std::size_t>(i)] = " ";
        for (int r = y_ + 1; r < rows_; ++r) {
          screen_[static_cast<std::size_t>(r)].assign(static_cast<std::size_t>(columns_), " ");
        }
        break;
      }
      case 'm': break;
      case 'h':
      case 'l':
        if (!priv) {
          errors.emplace_back("a non-private mode");
          break;
        }
        if (params == "?25") cursor_shown_ = f == 'h';
        if (params == "?2026") sync_ = f == 'h';
        if (params == "?7") autowrap_ = f == 'h';
        if (params == "?1000") mouse_ = f == 'h';
        if (params == "?1006") sgr_mouse_ = f == 'h';
        break;
      case 'n':
        // The cursor position report, as a terminal answers it, into the input.
        if (params != "6") {
          errors.emplace_back("a device status report other than 6");
          break;
        }
        ++cursor_asks;
        if (reply) reply("\x1B[" + std::to_string(y_ + 1) + ";" + std::to_string(std::min(x_, columns_ - 1) + 1) + "R");
        break;
      default: errors.emplace_back("a sequence the command line does not write: CSI " + std::string(params) + f);
    }
  }

  int columns_, rows_;
  std::vector<std::vector<std::string>> screen_;
  std::vector<std::string> scrolled_;
  int x_ = 0, y_ = 0;
  bool cursor_shown_ = true;
  bool sync_ = false;
  bool autowrap_ = true;
  bool mouse_ = false, sgr_mouse_ = false;

 public:
  // Mouse reporting on, in SGR's form (CSI ?1000h and ?1006h).
  [[nodiscard]] bool mouse() const noexcept { return mouse_ && sgr_mouse_; }
  int cursor_asks = 0;  // CSI 6n seen
  // Where a terminal's answers go (the input); none: it does not answer.
  std::function<void(std::string)> reply;
};

// A terminal for a session running on its own thread: the test types into
// it (keys as the terminal would send them), reads what is on its screen,
// and can close it (SIGTERM / the console closing) or end its input.
class fake_console final : public mv::driftwire::cli::terminal {
 public:
  fake_console(int columns, int rows) : vt_(columns, rows) {
    // Called from write(), under m_: the answer is the next read.
    vt_.reply = [this](std::string s) { input_.push_back(std::move(s)); };
  }

  [[nodiscard]] mv::driftwire::cli::term_size size() const override { return {vt_.columns(), vt_.rows()}; }

  void wait(mv::driftwire::cli::term_input& in, int timeout_ms) override {
    in = {};
    std::unique_lock lock(m_);
    const auto ready = [&] { return !input_.empty() || woken_ || quit_ || eof_; };
    if (timeout_ms < 0) {
      cv_.wait(lock, ready);
    } else if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
      in.timed_out = true;
      return;
    }
    if (!input_.empty()) {
      in.bytes = std::move(input_.front());
      input_.pop_front();
    }
    in.woken = woken_;
    woken_ = false;
    in.quit = quit_;
    in.eof = eof_ && input_.empty() && in.bytes.empty();
  }

  void wake() noexcept override {
    {
      std::lock_guard lock(m_);
      woken_ = true;
    }
    cv_.notify_all();
  }

  void write(std::string_view bytes) override {
    {
      std::lock_guard lock(m_);
      vt_.write(bytes);
    }
    cv_.notify_all();
  }

  // ---- the test's side
  // One read's worth of bytes (a paste, a key or a burst of typing).
  void type(std::string_view bytes) {
    {
      std::lock_guard lock(m_);
      input_.emplace_back(bytes);
    }
    cv_.notify_all();
  }
  // Each character as its own read, as a person types.
  void type_keys(std::string_view text) {
    for (const char c : text) type(std::string_view(&c, 1));
  }
  void quit() {
    {
      std::lock_guard lock(m_);
      quit_ = true;
    }
    cv_.notify_all();
  }
  void end_input() {
    {
      std::lock_guard lock(m_);
      eof_ = true;
    }
    cv_.notify_all();
  }
  [[nodiscard]] std::vector<std::string> lines() const {
    std::lock_guard lock(m_);
    return vt_.lines();
  }
  [[nodiscard]] std::vector<std::string> visible() const {
    std::lock_guard lock(m_);
    return vt_.visible();
  }
  [[nodiscard]] std::vector<std::vector<std::string>> cells() const {
    std::lock_guard lock(m_);
    return vt_.cells();
  }
  [[nodiscard]] std::vector<std::string> errors() const {
    std::lock_guard lock(m_);
    return vt_.errors;
  }
  // ---- the mouse
  [[nodiscard]] bool mouse_on() const {
    std::lock_guard lock(m_);
    return vt_.mouse();
  }
  [[nodiscard]] int cursor_asks() const {
    std::lock_guard lock(m_);
    return vt_.cursor_asks;
  }
  // Waits until the session has asked where the cursor is (CSI 6n) at least
  // once, up to `seconds`.
  bool wait_cursor_ask(int seconds = 5) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock lock(m_);
    while (vt_.cursor_asks == 0) {
      if (cv_.wait_until(lock, until) == std::cv_status::timeout) return vt_.cursor_asks > 0;
    }
    return true;
  }
  // Waits until mouse reporting is `on` (or off), up to `seconds`.
  bool wait_mouse(bool on, int seconds = 5) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock lock(m_);
    while (vt_.mouse() != on) {
      if (cv_.wait_until(lock, until) == std::cv_status::timeout) return vt_.mouse() == on;
    }
    return true;
  }
  // The first visible cell where `text` starts: (column, row), 1-based as a
  // terminal reports them.
  [[nodiscard]] std::optional<std::pair<int, int>> find(std::string_view text) const {
    std::lock_guard lock(m_);
    const auto& cells = vt_.cells();
    for (std::size_t r = 0; r < cells.size(); ++r) {
      std::string row;
      std::vector<int> col_of;  // the byte offset of each cell's glyph
      for (const auto& c : cells[r]) {
        col_of.push_back(static_cast<int>(row.size()));
        row += c;
      }
      const auto at = row.find(text);
      if (at == std::string::npos) continue;
      for (std::size_t x = 0; x < col_of.size(); ++x) {
        if (col_of[x] >= static_cast<int>(at)) return std::pair<int, int>{static_cast<int>(x) + 1, static_cast<int>(r) + 1};
      }
    }
    return std::nullopt;
  }
  // A left click (press and release) at a cell, 1-based, as SGR reports it.
  void click(int x, int y) {
    type("\x1B[<0;" + std::to_string(x) + ";" + std::to_string(y) + "M");
    type("\x1B[<0;" + std::to_string(x) + ";" + std::to_string(y) + "m");
  }
  // A click on the text, where the screen shows it; false when it does not.
  bool click_on(std::string_view text) {
    const auto at = find(text);
    if (!at) return false;
    click(at->first, at->second);
    return true;
  }
  void wheel(bool up, int x = 1, int y = 1) {
    type("\x1B[<" + std::to_string(up ? 64 : 65) + ";" + std::to_string(x) + ";" + std::to_string(y) + "M");
  }

  // Everything written, as written, in one string.
  [[nodiscard]] std::string written() const {
    std::lock_guard lock(m_);
    std::string all;
    for (const auto& w : vt_.writes) all += w;
    return all;
  }
  [[nodiscard]] std::vector<std::string> osc() const {
    std::lock_guard lock(m_);
    return vt_.osc;
  }
  // Everything shown so far as one string, a row a line.
  [[nodiscard]] std::string text() const {
    std::string s;
    for (const auto& l : lines()) s += l + "\n";
    return s;
  }
  [[nodiscard]] std::string screen() const {
    std::string s;
    for (const auto& l : visible()) s += l + "\n";
    return s;
  }
  // Waits until `pred(everything shown)` holds, up to `seconds`.
  bool wait_for(const std::function<bool(const std::string&)>& pred, int seconds = 20) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock lock(m_);
    while (true) {
      std::string all;
      for (const auto& l : vt_.lines()) all += l + "\n";
      if (pred(all)) return true;
      if (cv_.wait_until(lock, until) == std::cv_status::timeout) {
        all.clear();
        for (const auto& l : vt_.lines()) all += l + "\n";
        return pred(all);
      }
    }
  }
  // The same over the visible screen's cells.
  bool wait_cells(const std::function<bool(const std::vector<std::vector<std::string>>&)>& pred,
                  int seconds = 20) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock lock(m_);
    while (!pred(vt_.cells())) {
      if (cv_.wait_until(lock, until) == std::cv_status::timeout) return pred(vt_.cells());
    }
    return true;
  }
  // The same over the visible screen only.
  bool wait_screen(const std::function<bool(const std::string&)>& pred, int seconds = 20) const {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    std::unique_lock lock(m_);
    while (true) {
      std::string all;
      for (const auto& l : vt_.visible()) all += l + "\n";
      if (pred(all)) return true;
      if (cv_.wait_until(lock, until) == std::cv_status::timeout) {
        all.clear();
        for (const auto& l : vt_.visible()) all += l + "\n";
        return pred(all);
      }
    }
  }

 private:
  mutable std::mutex m_;
  mutable std::condition_variable cv_;
  vt_screen vt_;
  std::deque<std::string> input_;
  bool woken_ = false, quit_ = false, eof_ = false;
};

// The six digits a screen shows in the command line's big digits (the font
// of docs/design/28 "From a terminal"), read back from its cells; "" when
// no code is on it.
inline std::string read_big_code(const std::vector<std::vector<std::string>>& cells) {
  // clang-format off
  static const char* const kFont[10][3] = {
      {"╭──╮", "│  │", "╰──╯"}, {" ╶╮ ", "  │ ", " ╶┴╴"}, {"╭──╮", "╭──╯", "╰──╴"}, {"╶──╮", " ──┤", "╶──╯"},
      {"╷  ╷", "╰──┤", "   ╵"}, {"╭──╴", "╰──╮", "╶──╯"}, {"╭──╴", "├──╮", "╰──╯"}, {"╶──╮", "   │", "   ╵"},
      {"╭──╮", "├──┤", "╰──╯"}, {"╭──╮", "╰──┤", "╶──╯"},
  };
  // clang-format on
  const auto at = [&](std::size_t r, std::size_t x) {
    std::string s;
    for (std::size_t i = 0; i < 4 && x + i < cells[r].size(); ++i) s += cells[r][x + i];
    return s;
  };
  for (std::size_t r = 0; r + 2 < cells.size(); ++r) {
    std::string code;
    for (std::size_t x = 0; x + 4 <= cells[r].size();) {
      int hit = -1;
      for (int d = 0; d < 10 && hit < 0; ++d) {
        if (at(r, x) == kFont[d][0] && at(r + 1, x) == kFont[d][1] && at(r + 2, x) == kFont[d][2]) hit = d;
      }
      if (hit >= 0) {
        code += static_cast<char>('0' + hit);
        x += 4;
      } else {
        ++x;
      }
    }
    if (code.size() == 6) return code;
  }
  return {};
}

}  // namespace mv::test
