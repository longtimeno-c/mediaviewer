// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire command line's terminal layer and its look (docs/design/28
// "From a terminal"), with no engine: the key decoder, the line editor, the
// paths a person types or pastes, Tab completion and the slash palette, the
// cards read back as text from FTXUI's cells at 80 and 40 columns, and the
// live region drawn into a small terminal emulator.
#include "catch_compat.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#endif

#include "addons/driftwire/cli/complete.h"
#include "addons/driftwire/cli/inline_view.h"
#include "addons/driftwire/cli/keys.h"
#include "addons/driftwire/cli/line_editor.h"
#include "addons/driftwire/cli/paths_arg.h"
#include "addons/driftwire/cli/render.h"
#include "addons/driftwire/cli/term.h"
#include "addons/driftwire/cli/text.h"
#include "addons/driftwire/cli/theme.h"
#include "addons/driftwire/wire.h"
#include "import_fixture.h"

namespace cli = mv::driftwire::cli;
namespace fs = std::filesystem;
using cli::key;
using mv::test::scratch_dir;

namespace {

// ---- keys -------------------------------------------------------------------------------------

// Everything `bytes` decodes to, the held-back rest flushed as if the wait ran out.
std::vector<cli::key_event> decode(std::string_view bytes) {
  cli::key_decoder d;
  std::vector<cli::key_event> out;
  d.feed(bytes, out);
  if (d.pending_ms() >= 0) d.flush(out);
  return out;
}

cli::key_event one(std::string_view bytes) {
  const auto keys = decode(bytes);
  REQUIRE(keys.size() == 1);
  return keys[0];
}

cli::key_event ctrl(char c) {
  cli::key_event e;
  e.k = key::ctrl;
  e.letter = c;
  return e;
}

cli::key_event text_key(std::string_view s, bool alt = false) {
  cli::key_event e;
  e.k = key::text;
  e.text = std::string(s);
  e.alt = alt;
  return e;
}

cli::key_event plain(key k) {
  cli::key_event e;
  e.k = k;
  return e;
}

// ---- the look -------------------------------------------------------------------------------------

std::vector<std::string> rows_of(const ftxui::Element& e, int width) { return cli::rasterize(e, width).plain; }

std::string joined(const std::vector<std::string>& rows) {
  std::string s;
  for (const auto& r : rows) s += r + "\n";
  return s;
}

// The rows as text, shown whole when they differ (the golden is the look).
void check_rows(const std::vector<std::string>& got, const std::vector<std::string>& want) {
  INFO("got:\n" << joined(got) << "want:\n" << joined(want));
  CHECK(got == want);
}

bool any_row_has(const std::vector<std::string>& rows, std::string_view s) {
  return std::any_of(rows.begin(), rows.end(), [&](const std::string& r) { return r.find(s) != std::string::npos; });
}

int widest(const std::vector<std::string>& rows) {
  int w = 0;
  for (const auto& r : rows) w = std::max(w, cli::display_width(r));
  return w;
}

// Each theme sets FTXUI's colour depth as it is made (theme.h), so a test
// makes the one it renders with.
cli::theme colour_theme() { return cli::make_theme(cli::color_depth::truecolor, true); }

// SGR foreground for one of the palette's colours in truecolor.
std::string fg(cli::rgb c) {
  return "38;2;" + std::to_string(c.r) + ";" + std::to_string(c.g) + ";" + std::to_string(c.b);
}

cli::progress_model moving(double fraction, std::uint32_t count = 3) {
  cli::progress_model m;
  m.link.here = "ThinkPad";
  m.link.there = "Ann's MacBook";
  m.total = 4'800'000'000ULL;
  m.bytes = static_cast<std::uint64_t>(fraction * 4.8e9);
  m.count = count;
  m.files = static_cast<std::uint32_t>(fraction * count);
  m.rate = fraction < 1 ? 14'000'000 : 0;
  m.left_s = fraction < 1 ? (4.8e9 - static_cast<double>(m.bytes)) / 14e6 : -1;
  return m;
}

cli::code_card_model code_card_model() {
  cli::code_card_model m;
  m.peer = "Ann's MacBook";
  m.code = "342 917";
  return m;
}

cli::offer_model offer_model() {
  cli::offer_model m;
  m.peer = "Ann's MacBook";
  m.count = 24;
  m.bytes = 412'000'000;
  m.names = {"IMG_4471.HEIC", "IMG_4472.HEIC", "IMG_4473.MOV", "IMG_4474.HEIC"};
  m.save_to = "~/Downloads";
  return m;
}

cli::code_entry_model wrong_code_model() {
  cli::code_entry_model m;
  m.peer = "Ann's MacBook";
  m.typed = "4829";
  m.note = "That is not the code on Ann's MacBook's screen. 2 tries left.";
  m.wrong = true;
  return m;
}

cli::nearby_model nearby_two() {
  cli::nearby_model m;
  m.peers = {{"Ann's MacBook", "Wi-Fi"}, {"Living-room PC", "Wi-Fi \xC2\xB7 Bluetooth"}};
  m.selected = 0;
  return m;
}

// ---- a terminal, as far as inline_view uses one ---------------------------------------------

// A screen `rows` high over a scrollback: printable UTF-8 (two cells for a
// wide character), CR, LF (scrolling at the bottom row), CSI n A / n C, CSI J,
// SGR and private modes (recorded, not drawn), xterm's pending wrap at the
// last column. Everything inline_view writes, and nothing it does not: an
// unknown sequence fails the test.
class fake_terminal final : public cli::output {
 public:
  fake_terminal(int columns, int rows) : columns_(columns), rows_(rows), screen_(static_cast<std::size_t>(rows)) {
    for (auto& r : screen_) r.assign(static_cast<std::size_t>(columns_), " ");
  }

  void write(std::string_view b) override {
    writes.emplace_back(b);
    for (std::size_t i = 0; i < b.size();) {
      const char c = b[i];
      if (c == '\x1B') {
        REQUIRE(i + 1 < b.size());
        REQUIRE(b[i + 1] == '[');
        std::size_t j = i + 2;
        while (j < b.size() && ((b[j] >= '0' && b[j] <= '9') || b[j] == ';' || b[j] == '?')) ++j;
        REQUIRE(j < b.size());
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
  [[nodiscard]] int x() const noexcept { return x_; }
  [[nodiscard]] int y() const noexcept { return y_; }
  [[nodiscard]] bool cursor_shown() const noexcept { return cursor_shown_; }
  [[nodiscard]] bool synchronized() const noexcept { return sync_; }

  std::vector<std::string> writes;
  std::vector<int> ups;  // every CSI n A, in order

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
    const int w = std::max(1, cli::display_width(glyph));
    if (x_ + w > columns_) {  // the pending wrap, or a wide character at the edge
      x_ = 0;
      line_feed();
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
        REQUIRE((params.empty() || params == "0"));
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
        REQUIRE(priv);
        if (params == "?25") cursor_shown_ = f == 'h';
        if (params == "?2026") sync_ = f == 'h';
        break;
      default: FAIL("a sequence inline_view does not write: CSI " << params << f);
    }
  }

  int columns_, rows_;
  std::vector<std::vector<std::string>> screen_;
  std::vector<std::string> scrolled_;
  int x_ = 0, y_ = 0;
  bool cursor_shown_ = true;
  bool sync_ = false;
};

ftxui::Element rows_doc(const std::vector<std::string>& rows) {
  ftxui::Elements e;
  for (const auto& r : rows) e.push_back(ftxui::text(r));
  return ftxui::vbox(std::move(e));
}

}  // namespace

// ==== keys ===========================================================================================

TEST_CASE("driftwire cli keys: text, UTF-8 cut across reads, a stray byte", "[driftwire][cli]") {
  CHECK(decode("a") == std::vector{text_key("a")});
  CHECK(decode("\xC3\xA9") == std::vector{text_key("\xC3\xA9")});  // é

  cli::key_decoder d;
  std::vector<cli::key_event> out;
  d.feed("\xF0\x9F", out);  // the first half of 🌸
  CHECK(out.empty());
  CHECK(d.pending_ms() == cli::key_decoder::kCutWaitMs);
  d.feed("\x8C", out);
  CHECK(out.empty());
  d.feed("\xB8z", out);
  CHECK(out == std::vector{text_key("\xF0\x9F\x8C\xB8"), text_key("z")});
  CHECK(d.pending_ms() == -1);

  // A byte that cannot start a character is U+FFFD, and what follows is read.
  CHECK(decode("\xFF" "b") == std::vector{text_key("\xEF\xBF\xBD"), text_key("b")});
  // Half a character the wait gave up on is dropped.
  CHECK(decode("\xE6\x9D").empty());
}

TEST_CASE("driftwire cli keys: the control keys", "[driftwire][cli]") {
  CHECK(one("\r") == plain(key::enter));
  CHECK(one("\n") == plain(key::enter));
  CHECK(one("\t") == plain(key::tab));
  CHECK(one("\x7F") == plain(key::backspace));
  CHECK(one("\x08") == plain(key::backspace));
  for (const char c : std::string_view("acdeklnpuwz")) {
    CAPTURE(c);
    CHECK(one(std::string(1, static_cast<char>(c - 'a' + 1))) == ctrl(c));
  }
  CHECK(one(std::string(1, '\0')) == plain(key::unknown));  // Ctrl+Space
}

TEST_CASE("driftwire cli keys: arrows, Home/End, Delete, Page, Shift+Tab in every form", "[driftwire][cli]") {
  struct row {
    std::string_view bytes;
    key k;
  };
  const row rows[] = {
      {"\x1B[A", key::up},        {"\x1B[B", key::down},      {"\x1B[C", key::right},   {"\x1B[D", key::left},
      {"\x1BOA", key::up},        {"\x1BOB", key::down},      {"\x1BOC", key::right},   {"\x1BOD", key::left},
      {"\x1B[H", key::home},      {"\x1B[F", key::end},       {"\x1BOH", key::home},    {"\x1BOF", key::end},
      {"\x1B[1~", key::home},     {"\x1B[7~", key::home},     {"\x1B[4~", key::end},    {"\x1B[8~", key::end},
      {"\x1B[3~", key::del},      {"\x1B[2~", key::insert},   {"\x1B[5~", key::page_up}, {"\x1B[6~", key::page_down},
      {"\x1B[Z", key::backtab},   {"\x1BOM", key::enter},
  };
  for (const auto& r : rows) {
    CAPTURE(r.bytes.substr(1));
    CHECK(one(r.bytes) == plain(r.k));
  }
}

TEST_CASE("driftwire cli keys: Alt, Ctrl and Shift on the arrows; Esc as Alt", "[driftwire][cli]") {
  cli::key_event alt_left = plain(key::left);
  alt_left.alt = true;
  CHECK(one("\x1B[1;3D") == alt_left);     // xterm
  CHECK(one("\x1B\x1B[D") == alt_left);    // ESC + the sequence (rxvt, some macOS setups)
  CHECK(one("\x1B[1;9D") == alt_left);     // Meta
  cli::key_event ctrl_right = plain(key::right);
  ctrl_right.ctrl = true;
  CHECK(one("\x1B[1;5C") == ctrl_right);
  cli::key_event shift_up = plain(key::up);
  shift_up.shift = true;
  CHECK(one("\x1B[1;2A") == shift_up);
  cli::key_event ctrl_del = plain(key::del);
  ctrl_del.ctrl = true;
  CHECK(one("\x1B[3;5~") == ctrl_del);
  // macOS Terminal's Option+Left / Right are ESC b / ESC f.
  CHECK(one("\x1B" "b") == text_key("b", true));
  CHECK(one("\x1B" "f") == text_key("f", true));
  cli::key_event alt_bs = plain(key::backspace);
  alt_bs.alt = true;
  CHECK(one("\x1B\x7F") == alt_bs);
}

TEST_CASE("driftwire cli keys: a lone Esc waits 30 ms and is Esc; a cut sequence waits for the rest",
          "[driftwire][cli]") {
  cli::key_decoder d;
  std::vector<cli::key_event> out;
  d.feed("\x1B", out);
  CHECK(out.empty());
  CHECK(d.pending_ms() == cli::key_decoder::kEscWaitMs);
  CHECK(cli::key_decoder::kEscWaitMs == 30);
  d.flush(out);
  CHECK(out == std::vector{plain(key::esc)});
  CHECK(d.pending_ms() == -1);

  // Esc twice in one read: two Esc keys (the double Esc that cancels).
  out.clear();
  d.feed("\x1B\x1B", out);
  CHECK(out.empty());
  CHECK(d.pending_ms() == cli::key_decoder::kEscWaitMs);
  d.flush(out);
  CHECK(out == std::vector{plain(key::esc), plain(key::esc)});

  // A sequence cut by the read is one key when the rest comes.
  out.clear();
  d.feed("\x1B", out);
  d.feed("[", out);
  CHECK(out.empty());
  CHECK(d.pending_ms() == cli::key_decoder::kCutWaitMs);
  d.feed("1;3", out);
  CHECK(out.empty());
  d.feed("C", out);
  cli::key_event alt_right = plain(key::right);
  alt_right.alt = true;
  CHECK(out == std::vector{alt_right});

  // Esc, then a key a moment later: Esc and the key.
  out.clear();
  d.feed("\x1B", out);
  d.flush(out);
  d.feed("q", out);
  CHECK(out == std::vector{plain(key::esc), text_key("q")});
}

TEST_CASE("driftwire cli keys: bracketed paste is one event, cut anywhere", "[driftwire][cli]") {
  cli::key_event paste;
  paste.k = key::paste;
  paste.text = "/home/ann/a.jpg\n/home/ann/b c.jpg\r\n\x1B[Anot a key";
  CHECK(decode("\x1B[200~/home/ann/a.jpg\n/home/ann/b c.jpg\r\n\x1B[Anot a key\x1B[201~") == std::vector{paste});

  // The markers and the text split over reads, a key typed after.
  cli::key_decoder d;
  std::vector<cli::key_event> out;
  for (const std::string_view part : {"\x1B[2", "00~ab", "c\x1B[20", "1", "~x"}) d.feed(part, out);
  paste.text = "abc";
  CHECK(out == std::vector{paste, text_key("x")});

  // A paste whose end never comes is delivered when the wait runs out.
  out.clear();
  d.feed("\x1B[200~half", out);
  CHECK(out.empty());
  CHECK(d.pending_ms() == cli::key_decoder::kPasteWaitMs);
  d.flush(out);
  paste.text = "half";
  CHECK(out == std::vector{paste});
}

TEST_CASE("driftwire cli keys: a terminal without bracketed paste: lines in one read are a paste", "[driftwire][cli]") {
  cli::key_event paste;
  paste.k = key::paste;
  paste.text = "/home/ann/a.jpg\n/home/ann/b.jpg";
  CHECK(decode("/home/ann/a.jpg\n/home/ann/b.jpg") == std::vector{paste});
  // Typed fast and Enter: the keys and Enter, not a paste.
  CHECK(decode("ab\r") == std::vector{text_key("a"), text_key("b"), plain(key::enter)});
  CHECK(decode("\r") == std::vector{plain(key::enter)});
  // A character the read cut waits for the rest.
  cli::key_decoder d;
  std::vector<cli::key_event> out;
  d.feed("x\ny\xC3", out);
  paste.text = "x\ny";
  CHECK(out == std::vector{paste});
  d.feed("\xA9", out);
  CHECK(out == std::vector{paste, text_key("\xC3\xA9")});
}

TEST_CASE("driftwire cli keys: reports, mouse and function keys are not keys", "[driftwire][cli]") {
  CHECK(decode("\x1B[15~q") == std::vector{plain(key::unknown), text_key("q")});        // F5
  CHECK(decode("\x1B[?1;2cq") == std::vector{plain(key::unknown), text_key("q")});      // a DA report
  CHECK(decode("\x1B[<0;10;5Mq") == std::vector{plain(key::unknown), text_key("q")});   // a mouse click
  CHECK(decode("\x1BOPq") == std::vector{plain(key::unknown), text_key("q")});          // F1
}

// ==== the line editor =================================================================================

TEST_CASE("driftwire cli line editor: edits move by glyph, measure by column", "[driftwire][cli]") {
  cli::line_editor ed;
  ed.insert("caf");
  ed.insert("e\xCC\x81");  // e + a combining acute: one glyph
  CHECK(ed.text() == "cafe\xCC\x81");
  CHECK(ed.width() == 4);
  ed.left();
  CHECK(ed.cursor() == 3);  // before the e, not between it and its accent
  ed.right();
  CHECK(ed.cursor() == ed.text().size());
  ed.backspace();
  CHECK(ed.text() == "caf");

  ed.set("\xE6\x9D\x8E\xE6\x98\x8E");  // 李明: two wide characters
  CHECK(ed.width() == 4);
  CHECK(ed.cursor_column() == 4);
  ed.left();
  CHECK(ed.cursor_column() == 2);
  ed.del();
  CHECK(ed.text() == "\xE6\x9D\x8E");
  ed.home();
  ed.insert("x");
  CHECK(ed.text() == "x\xE6\x9D\x8E");
  ed.end();
  CHECK(ed.cursor_column() == 3);

  // Control characters typed are dropped.
  ed.clear();
  ed.insert("a\x01" "b\x7F" "c");
  CHECK(ed.text() == "abc");
}

TEST_CASE("driftwire cli line editor: words, kills, the keys", "[driftwire][cli]") {
  cli::line_editor ed;
  ed.set("send /home/ann/My Photos");
  REQUIRE(ed.handle(plain(key::home)));
  CHECK(ed.cursor() == 0);
  cli::key_event alt_right = plain(key::right);
  alt_right.alt = true;
  ed.handle(alt_right);
  CHECK(ed.cursor() == 4);  // after "send"
  ed.handle(alt_right);
  CHECK(ed.cursor() == 10);  // after "home"
  cli::key_event ctrl_left = plain(key::left);
  ctrl_left.ctrl = true;
  ed.handle(ctrl_left);
  CHECK(ed.cursor() == 6);  // the start of "home"
  ed.handle(text_key("b", true));  // Alt+B
  CHECK(ed.cursor() == 0);
  ed.handle(text_key("f", true));  // Alt+F
  CHECK(ed.cursor() == 4);
  ed.handle(ctrl('e'));
  CHECK(ed.cursor() == ed.text().size());

  ed.handle(ctrl('w'));  // back to a space, as bash
  CHECK(ed.text() == "send /home/ann/My ");
  ed.handle(ctrl('w'));
  CHECK(ed.text() == "send ");
  ed.set("send /home/ann/My");
  cli::key_event alt_bs = plain(key::backspace);
  alt_bs.alt = true;
  ed.handle(alt_bs);  // a word of letters only
  CHECK(ed.text() == "send /home/ann/");
  ed.handle(ctrl('a'));
  ed.handle(text_key("d", true));  // Alt+D
  CHECK(ed.text() == " /home/ann/");
  ed.handle(plain(key::right));
  ed.handle(ctrl('k'));
  CHECK(ed.text() == " ");
  ed.set("abc def");
  ed.left();
  ed.left();
  ed.handle(ctrl('u'));
  CHECK(ed.text() == "ef");
  CHECK(ed.cursor() == 0);
  ed.handle(ctrl('d'));  // Delete on text
  CHECK(ed.text() == "f");

  // What is the caller's: Enter, Esc, Tab, the arrows up and down, Ctrl+C,
  // and Ctrl+D on an empty line (quit).
  for (const key k : {key::enter, key::esc, key::tab, key::up, key::down}) CHECK_FALSE(ed.handle(plain(k)));
  CHECK_FALSE(ed.handle(ctrl('c')));
  ed.clear();
  CHECK_FALSE(ed.handle(ctrl('d')));
}

TEST_CASE("driftwire cli line editor: history keeps the draft", "[driftwire][cli]") {
  cli::line_editor ed;
  ed.remember("/code");
  ed.remember("/devices");
  ed.remember("/devices");  // a repeat once
  ed.remember("");
  CHECK(ed.history() == std::vector<std::string>{"/code", "/devices"});
  ed.set("/he");
  CHECK(ed.handle(ctrl('p')));
  CHECK(ed.text() == "/devices");
  ed.handle(ctrl('p'));
  CHECK(ed.text() == "/code");
  CHECK_FALSE(ed.history_older());
  CHECK(ed.text() == "/code");
  ed.handle(ctrl('n'));
  CHECK(ed.text() == "/devices");
  ed.handle(ctrl('n'));
  CHECK(ed.text() == "/he");
  CHECK_FALSE(ed.history_newer());

  for (std::size_t i = 0; i < cli::line_editor::kHistory + 5; ++i) ed.remember("line " + std::to_string(i));
  CHECK(ed.history().size() == cli::line_editor::kHistory);
  CHECK(ed.history().back() == "line " + std::to_string(cli::line_editor::kHistory + 4));
}

TEST_CASE("driftwire cli line editor: the code field takes six digits only", "[driftwire][cli]") {
  cli::line_editor ed;
  ed.set_filter([](char32_t c) { return c >= U'0' && c <= U'9'; });
  ed.set_max_glyphs(6);
  ed.insert("12a3");
  ed.paste("4-5 6\n789");
  CHECK(ed.text() == "123456");
}

TEST_CASE("driftwire cli line editor: a paste never submits; a pasted list keeps each path whole",
          "[driftwire][cli]") {
  cli::line_editor ed;
  ed.paste("one\ntwo\r\nthree\tfour\r");
  CHECK(ed.text() == "one two three four ");

  // The prompt: each line of a pasted list that needs it is quoted.
  ed.clear();
  ed.set_paste_quote('\'');
  ed.paste("/home/ann/My File.jpg\n/home/ann/b.jpg\n/home/ann/Ann's.jpg\n'/home/ann/quoted one.jpg'\n");
  CHECK(ed.text() == "'/home/ann/My File.jpg' /home/ann/b.jpg '/home/ann/Ann'\\''s.jpg' '/home/ann/quoted one.jpg' ");
  // One line (what a terminal pastes for files dropped on it) is left as it is.
  ed.clear();
  ed.paste("/home/ann/My\\ File.jpg /home/ann/b.jpg");
  CHECK(ed.text() == "/home/ann/My\\ File.jpg /home/ann/b.jpg");
  // Windows: double quotes; a backslash and an apostrophe are letters.
  ed.clear();
  ed.set_paste_quote('"');
  ed.paste("C:\\Users\\ann\\My File.jpg\r\nC:\\Users\\ann\\Ann's.jpg\r\n");
  CHECK(ed.text() == "\"C:\\Users\\ann\\My File.jpg\" C:\\Users\\ann\\Ann's.jpg ");

  // And a pasted list is the paths it names.
  scratch_dir s("driftwire_cli_paste");
  mv::test::write_text(s / "My File.jpg", "x");
  mv::test::write_text(s / "Ann's.jpg", "x");
  cli::path_options o = cli::default_path_options();
  o.cwd = mv::test::utf8(s.root());
  ed.clear();
  ed.set_paste_quote(o.windows ? '"' : '\'');
  ed.paste("My File.jpg\nAnn's.jpg\n");
  const cli::parsed_paths r = cli::parse_paths(ed.text(), o);
  CHECK(r.missing.empty());
  CHECK(r.paths == std::vector<std::string>{mv::test::utf8(s / "My File.jpg"), mv::test::utf8(s / "Ann's.jpg")});
}

TEST_CASE("driftwire cli line editor: a narrow field scrolls to keep the cursor in sight", "[driftwire][cli]") {
  cli::line_editor ed;
  ed.set("0123456789abcdefghij");  // 20 columns, the cursor at the end
  auto w = ed.view(10);
  CHECK(w.cut_left);
  CHECK_FALSE(w.cut_right);
  CHECK(w.text == "bcdefghij");  // the last column is the cursor's
  CHECK(w.cursor_col == 9);
  ed.home();
  w = ed.view(10);
  CHECK_FALSE(w.cut_left);
  CHECK(w.cut_right);
  CHECK(w.text == "012345678");  // the last column for the ellipsis
  CHECK(w.cursor_col == 0);
  // Moving right inside the view does not scroll it.
  for (int i = 0; i < 5; ++i) ed.right();
  w = ed.view(10);
  CHECK(w.text == "012345678");
  CHECK(w.cursor_col == 5);
  // Short enough: all of it.
  ed.set("abc");
  w = ed.view(10);
  CHECK(w.text == "abc");
  CHECK(w.cursor_col == 3);
  CHECK_FALSE(w.cut_left);
  CHECK_FALSE(w.cut_right);
  // Wide characters are never cut in half.
  ed.set("\xE6\x9D\x8E\xE6\x98\x8E\xE7\x9A\x84\xE7\x94\xB5\xE8\x84\x91\xE6\x9D\x8E\xE6\x98\x8E");  // 7 wide
  w = ed.view(9);
  CHECK(cli::display_width(w.text) <= 8);
  CHECK(cli::display_width(w.text) % 2 == 0);
  CHECK(w.cursor_col <= 8);
}

// ==== paths ============================================================================================

TEST_CASE("driftwire cli paths: words split the shell's way", "[driftwire][cli]") {
  const auto texts = [](const std::vector<cli::word>& ws) {
    std::vector<std::string> out;
    for (const auto& w : ws) out.push_back(w.text);
    return out;
  };
  // POSIX rules (pure: the same on every OS).
  auto ws = cli::split_words(R"(a 'b c' "d \" e" f\ g  'it'\''s')", false);
  CHECK(texts(ws) == std::vector<std::string>{"a", "b c", "d \" e", "f g", "it's"});
  ws = cli::split_words(R"(*.jpg '*.png' \*.gif ~ ~/x ~ann)", false);
  REQUIRE(ws.size() == 6);
  CHECK(ws[0].glob == std::vector<bool>{true, false, false, false, false});
  CHECK(ws[1].glob == std::vector<bool>(5, false));
  CHECK(ws[2].glob == std::vector<bool>(5, false));
  CHECK(ws[3].tilde);
  CHECK(ws[4].tilde);
  CHECK_FALSE(ws[5].tilde);  // ann's home is not looked up
  ws = cli::split_words("'open quote", false);
  REQUIRE(ws.size() == 1);
  CHECK(ws[0].open_quote == '\'');
  CHECK(ws[0].text == "open quote");
  // Windows rules: a backslash separates, an apostrophe is a letter, "..." quotes.
  ws = cli::split_words(R"("C:\Users\ann\My Photos\a.jpg" b.jpg Ann's.jpg)", true);
  CHECK(texts(ws) == std::vector<std::string>{R"(C:\Users\ann\My Photos\a.jpg)", "b.jpg", "Ann's.jpg"});
}

TEST_CASE("driftwire cli paths: file:// URIs are percent-decoded", "[driftwire][cli]") {
  CHECK(cli::file_uri_path("file:///home/ann/My%20Photos/a%2Bb.jpg", false) == "/home/ann/My Photos/a+b.jpg");
  CHECK(cli::file_uri_path("FILE://localhost/tmp/x", false) == "/tmp/x");
  CHECK(cli::file_uri_path("file://elsewhere/tmp/x", false).empty());  // another machine's
  CHECK(cli::file_uri_path("file:///C:/Users/ann/a%20b.jpg", true) == "C:/Users/ann/a b.jpg");
  CHECK(cli::file_uri_path("file://server/share/a.jpg", true) == "\\\\server/share/a.jpg");
  CHECK(cli::file_uri_path("http://example.com/a", false).empty());
}

TEST_CASE("driftwire cli paths: globs match the shell's way", "[driftwire][cli]") {
  const auto live = [](std::string_view p) {
    std::vector<bool> v;
    for (const char c : p) v.push_back(c == '*' || c == '?' || c == '[');
    return v;
  };
  CHECK(cli::glob_match("IMG_1.jpg", "*.jpg", live("*.jpg")));
  CHECK_FALSE(cli::glob_match("IMG_1.png", "*.jpg", live("*.jpg")));
  CHECK(cli::glob_match("IMG_7.HEIC", "IMG_?.HEIC", live("IMG_?.HEIC")));
  CHECK_FALSE(cli::glob_match("IMG_17.HEIC", "IMG_?.HEIC", live("IMG_?.HEIC")));
  CHECK(cli::glob_match("b-roll.png", "[ab]*.png", live("[ab]*.png")));
  CHECK_FALSE(cli::glob_match("c.png", "[ab]*.png", live("[ab]*.png")));
  CHECK(cli::glob_match("m.png", "[a-z].png", live("[a-z].png")));
  CHECK(cli::glob_match("c.png", "[!ab].png", live("[!ab].png")));
  CHECK_FALSE(cli::glob_match("a.png", "[!ab].png", live("[!ab].png")));
  CHECK(cli::glob_match("\xE6\x9D\x8E.jpg", "?.jpg", live("?.jpg")));  // one character, not one byte
  // A quoted star is a star.
  CHECK(cli::glob_match("*.jpg", "*.jpg", std::vector<bool>(5, false)));
  CHECK_FALSE(cli::glob_match("a.jpg", "*.jpg", std::vector<bool>(5, false)));
}

TEST_CASE("driftwire cli paths: a typed line is the absolute paths it names", "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_paths");
  for (const char* f : {"a.jpg", "b.jpg", "My File.jpg", ".hidden.jpg", "x[1].jpg", "notes.txt", "sub/c.jpg"}) {
    mv::test::write_text(s / f, "x");
  }
  cli::path_options o = cli::default_path_options();
  o.cwd = mv::test::utf8(s.root());
  o.home = mv::test::utf8(s.root());
  const auto at = [&](const char* rel) { return mv::test::utf8(fs::path(s / rel).lexically_normal()); };
  const auto paths = [&](std::string_view line) { return cli::parse_paths(line, o).paths; };

  CHECK(paths("a.jpg") == std::vector{at("a.jpg")});
  CHECK(paths("./sub/../a.jpg") == std::vector{at("a.jpg")});
  CHECK(paths("'My File.jpg' \"b.jpg\"") == std::vector{at("My File.jpg"), at("b.jpg")});
  // Repeats once, in the order typed.
  CHECK(paths("b.jpg a.jpg ./b.jpg b.jpg") == std::vector{at("b.jpg"), at("a.jpg")});
  // Globs: sorted, never a hidden name unless the dot is typed, through folders.
  CHECK(paths("*.jpg") == std::vector{at("My File.jpg"), at("a.jpg"), at("b.jpg"), at("x[1].jpg")});
  CHECK(paths(".*.jpg") == std::vector{at(".hidden.jpg")});
  CHECK(paths("*/c.jpg") == std::vector{at("sub/c.jpg")});
  // "[1]" matches no x1.jpg, so the name is taken as it is.
  CHECK(paths("x[1].jpg") == std::vector{at("x[1].jpg")});
  // The home folder.
  CHECK(paths("~/a.jpg") == std::vector{at("a.jpg")});
  // A folder is a path like any other (the session decides what it means).
  CHECK(paths("sub") == std::vector{at("sub")});
  // A file:// URI, as a file manager drags one.
  std::string uri = "file://";
  if (o.windows) uri += "/";
  for (const char8_t c : fs::path(s / "My File.jpg").generic_u8string()) {
    if (c == u8' ') {
      uri += "%20";
    } else {
      uri += static_cast<char>(c);
    }
  }
  CHECK(paths(uri) == std::vector{at("My File.jpg")});
  // Missing paths are named as typed; the rest still come back.
  const cli::parsed_paths r = cli::parse_paths("a.jpg nope.jpg ~/gone.jpg *.png", o);
  CHECK(r.paths == std::vector{at("a.jpg")});
  CHECK(r.missing == std::vector<std::string>{"nope.jpg", "~/gone.jpg", "*.png"});
  // One unquoted path with a space, pasted from somewhere that does not quote.
  CHECK(paths("My File.jpg") == std::vector{at("My File.jpg")});
  CHECK(paths("  My File.jpg  ") == std::vector{at("My File.jpg")});
  // An unclosed quote is closed at the end, and said.
  const cli::parsed_paths q = cli::parse_paths("'My File.jpg", o);
  CHECK(q.unterminated);
  CHECK(q.paths == std::vector{at("My File.jpg")});
}

#if !defined(_WIN32)
TEST_CASE("driftwire cli paths: a backslash escapes on POSIX", "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_escape");
  mv::test::write_text(s / "My File.jpg", "x");
  mv::test::write_text(s / "star*.jpg", "x");
  mv::test::write_text(s / "starry.jpg", "x");
  cli::path_options o = cli::default_path_options();
  o.cwd = mv::test::utf8(s.root());
  CHECK(cli::parse_paths("My\\ File.jpg", o).paths == std::vector{mv::test::utf8(s / "My File.jpg")});
  CHECK(cli::parse_paths("star\\*.jpg", o).paths == std::vector{mv::test::utf8(s / "star*.jpg")});
  CHECK(cli::parse_paths("star*.jpg", o).paths ==
        std::vector{mv::test::utf8(s / "star*.jpg"), mv::test::utf8(s / "starry.jpg")});
}
#endif

// ==== completion and the palette ===========================================================================

TEST_CASE("driftwire cli palette: commands filter by what is typed after the slash", "[driftwire][cli]") {
  const auto names = [](const std::vector<std::size_t>& idx) {
    std::vector<std::string> out;
    for (const std::size_t i : idx) out.emplace_back(cli::commands()[i].name);
    return out;
  };
  CHECK(cli::filter_commands("/").size() == cli::commands().size());
  CHECK(names(cli::filter_commands("/co")) == std::vector<std::string>{"/code", "/to-code"});
  CHECK(names(cli::filter_commands("/CO")) == std::vector<std::string>{"/code", "/to-code"});
  CHECK(names(cli::filter_commands("/ex")) == std::vector<std::string>{"/quit"});  // its alias /exit
  CHECK(names(cli::filter_commands("/mine 2 on")) == std::vector<std::string>{"/mine"});
  CHECK(cli::filter_commands("/zzz").empty());
  // Prefix matches first, then the ones that contain it.
  const auto re = names(cli::filter_commands("/re"));
  REQUIRE(re.size() >= 2);
  CHECK(re[0] == "/relay");
  CHECK(re[1] == "/relay-server");
  CHECK(std::find(re.begin(), re.end(), "/unfinished") == re.end());
  CHECK(std::find(re.begin(), re.end(), "/forget") == re.end());
  CHECK(cli::find_command("/exit") == cli::find_command("/quit"));
  CHECK(cli::find_command("/quit") >= 0);
  CHECK(cli::find_command("/nope") == -1);
  // Every command has a line to say what it does.
  for (const auto& c : cli::commands()) {
    CAPTURE(c.name);
    CHECK(c.name.size() > 1);
    CHECK(c.name[0] == '/');
    CHECK_FALSE(c.description.empty());
  }
}

TEST_CASE("driftwire cli completion: Tab completes paths and keeps the quoting", "[driftwire][cli]") {
  scratch_dir s("driftwire_cli_complete");
  for (const char* f : {"photo-one.jpg", "photo-two.jpg", "My File.jpg", ".secret", "Pictures/in.jpg"}) {
    mv::test::write_text(s / f, "x");
  }
  cli::path_options o = cli::default_path_options();
  o.cwd = mv::test::utf8(s.root());
  o.home = mv::test::utf8(s.root());
  const std::string sep(1, o.windows ? '\\' : '/');
  const auto complete = [&](std::string_view line, bool folders = false) {
    return cli::complete_path(line, line.size(), o, folders);
  };

  auto c = complete("pho");  // two matches: as far as they agree, and the list
  CHECK(c.changed);
  CHECK(c.line == "photo-");
  CHECK(c.cursor == 6);
  CHECK(c.choices == std::vector<std::string>{"photo-one.jpg", "photo-two.jpg"});
  c = complete("photo-");
  CHECK_FALSE(c.changed);
  CHECK(c.choices.size() == 2);
  c = complete("Pic");  // a folder: its separator, no space
  CHECK(c.line == "Pictures" + sep);
  c = complete("Pictures" + sep);
  CHECK(c.line == "Pictures" + sep + "in.jpg ");
  c = complete("My");  // a file: the word closed
  CHECK(c.line == (o.windows ? "\"My File.jpg\" " : "My\\ File.jpg "));
  c = complete("'My");
  CHECK(c.line == "'My File.jpg' ");
  c = complete(".s");  // hidden names when the dot is typed
  CHECK(c.line == ".secret ");
  c = complete("s");
  CHECK_FALSE(c.changed);
  c = complete("", true);  // folders only
  CHECK(c.line == "Pictures" + sep);
  c = complete("~");
  CHECK(c.line == "~" + sep);
  c = complete("~" + sep + "Pic");
  CHECK(c.line == "~" + sep + "Pictures" + sep);
  c = complete("*.jpg");  // a glob is Enter's to expand
  CHECK_FALSE(c.changed);
  // In the middle of a line: the word before the cursor, the rest kept.
  const std::string line = "a.jpg pho rest";
  c = cli::complete_path(line, 9, o, false);
  CHECK(c.line == "a.jpg photo- rest");
  CHECK(c.cursor == 12);
}

// ==== text by display width ===================================================================================

TEST_CASE("driftwire cli text: names are cut by columns, never inside a character", "[driftwire][cli]") {
  const std::string name = "\xE6\x9D\x8E\xE6\x98\x8E\xE7\x9A\x84\xE7\x94\xB5\xE8\x84\x91 \xF0\x9F\x8C\xB8 laptop";
  CHECK(cli::display_width(name) == 10 + 1 + 2 + 7);
  const std::string cut = cli::fit(name, 9, "\xE2\x80\xA6");
  CHECK(cut == "\xE6\x9D\x8E\xE6\x98\x8E\xE7\x9A\x84\xE7\x94\xB5\xE2\x80\xA6");  // 李明的电…
  CHECK(cli::display_width(cut) == 9);
  CHECK(cli::fit(name, 13, "\xE2\x80\xA6") == "\xE6\x9D\x8E\xE6\x98\x8E\xE7\x9A\x84\xE7\x94\xB5\xE8\x84\x91\xE2\x80\xA6");
  CHECK(cli::fit("e\xCC\x81" "e\xCC\x81" "e\xCC\x81", 2, "\xE2\x80\xA6") == "e\xCC\x81\xE2\x80\xA6");  // é…
  CHECK(cli::fit("short", 10, "\xE2\x80\xA6") == "short");
  CHECK(cli::fit("Ann's MacBook", 7, "...") == "Ann'...");
  CHECK(cli::fit("Ann's MacBook", 7, "\xE2\x80\xA6") == "Ann's\xE2\x80\xA6");  // no space before the ellipsis
  CHECK(cli::fit("abc", 0, "...").empty());
  CHECK(cli::wrap("the quick brown fox", 9) == std::vector<std::string>{"the quick", "brown fox"});
  CHECK(cli::wrap("abcdefghij", 4) == std::vector<std::string>{"abcd", "efgh", "ij"});
  CHECK(cli::wrap("a\n\nb", 9) == std::vector<std::string>{"a", "", "b"});
  CHECK(cli::wrap("", 9) == std::vector<std::string>{""});
}

TEST_CASE("driftwire cli words: sizes, speeds and times as the window says them", "[driftwire][cli]") {
  CHECK(mv::driftwire::rate_text(14'000'000) == mv::driftwire::size_text(14'000'000) + "/s");
  CHECK(mv::driftwire::rate_text(14'000'000) == "14 MB/s");
  CHECK(mv::driftwire::rate_text(-5) == mv::driftwire::size_text(0) + "/s");
  CHECK(cli::elapsed_text(0.2) == "under a second");
  CHECK(cli::elapsed_text(31) == "31 s");
  CHECK(cli::elapsed_text(92) == "1 min 32 s");
  CHECK(cli::elapsed_text(120) == "2 min");
  CHECK(cli::elapsed_text(7500) == "2 h 5 min");
  CHECK(cli::wait_text(12.7) == "12s");
  CHECK(cli::wait_text(65) == "1m 05s");
  CHECK(cli::wait_text(3725) == "1h 02m");
  CHECK(cli::home_short("/home/ann/Downloads", "/home/ann") == "~/Downloads");
  CHECK(cli::home_short("/home/ann", "/home/ann/") == "~");
  CHECK(cli::home_short("/home/anna/x", "/home/ann") == "/home/anna/x");
  CHECK(cli::home_short("/srv/x", "") == "/srv/x");
  const cli::theme th = colour_theme();
  const std::vector<std::string> names{"IMG_4471.HEIC", "IMG_4472.HEIC", "IMG_4473.MOV"};
  CHECK(cli::names_text(names, 24, 80, th) == "IMG_4471.HEIC, IMG_4472.HEIC, IMG_4473.MOV and 21 more");
  CHECK(cli::names_text(names, 24, 40, th) == "IMG_4471.HEIC, IMG_4472.HEIC and 22 more");
  CHECK(cli::names_text(names, 3, 80, th) == "IMG_4471.HEIC, IMG_4472.HEIC, IMG_4473.MOV");
  CHECK(cli::names_text({}, 1, 80, th) == "1 file");
  CHECK(cli::display_width(cli::names_text(names, 24, 20, th)) <= 20);
}

// ==== the cards, read back as text ===========================================================================

TEST_CASE("driftwire cli look: the code to type, at 80 and 40 columns", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  check_rows(rows_of(cli::code_card(th, code_card_model(), 80), 80),
             {
                 "╭─ Ann's MacBook wants to send you files ──────────────────────────────────────╮",
                 "│                                                                              │",
                 "│      ╶──╮ ╷  ╷ ╭──╮    ╭──╮  ╶╮  ╶──╮                                        │",
                 "│       ──┤ ╰──┤ ╭──╯    ╰──┤   │     │                                        │",
                 "│      ╶──╯    ╵ ╰──╴    ╶──╯  ╶┴╴    ╵                                        │",
                 "│                                                                              │",
                 "│   Type this code on Ann's MacBook.                            esc to decline │",
                 "╰──────────────────────────────────────────────────────────────────────────────╯",
             });
  check_rows(rows_of(cli::code_card(th, code_card_model(), 40), 40),
             {
                 "╭──────────────────────────────────────╮",
                 "│ Ann's MacBook wants to send you      │",
                 "│ files                                │",
                 "│                                      │",
                 "│     ╶──╮ ╷  ╷ ╭──╮    ╭──╮  ╶╮  ╶──╮ │",
                 "│      ──┤ ╰──┤ ╭──╯    ╰──┤   │     │ │",
                 "│     ╶──╯    ╵ ╰──╴    ╶──╯  ╶┴╴    ╵ │",
                 "│                                      │",
                 "│   Type this code on Ann's MacBook.   │",
                 "│                       esc to decline │",
                 "╰──────────────────────────────────────╯",
             });
  // Below 40 columns: plain bold digits.
  const auto narrow = rows_of(cli::code_card(th, code_card_model(), 39), 39);
  CHECK(any_row_has(narrow, "342 917"));
  CHECK(widest(narrow) == 39);
  // Every digit of the font, three rows each.
  const auto digits = rows_of(cli::big_digits(th, "0123456789", 80), 80);
  check_rows(digits, {
                         "╭──╮  ╶╮  ╭──╮ ╶──╮ ╷  ╷ ╭──╴ ╭──╴ ╶──╮ ╭──╮ ╭──╮",
                         "│  │   │  ╭──╯  ──┤ ╰──┤ ╰──╮ ├──╮    │ ├──┤ ╰──┤",
                         "╰──╯  ╶┴╴ ╰──╴ ╶──╯    ╵ ╶──╯ ╰──╯    ╵ ╰──╯ ╶──╯",
                     });
  // A remembered device has no code; an internet one shows the check to compare, not to type.
  cli::code_card_model known = code_card_model();
  known.known = true;
  const auto k = rows_of(cli::code_card(th, known, 80), 80);
  CHECK(k[0].find("Ann's MacBook (remembered) wants to send you files") != std::string::npos);
  CHECK_FALSE(any_row_has(k, "342"));
  cli::code_card_model far = code_card_model();
  far.internet = true;
  const auto f = rows_of(cli::code_card(th, far, 80), 80);
  CHECK(any_row_has(f, "Make sure Ann's MacBook shows 342 917"));
  CHECK_FALSE(any_row_has(f, "Type this code"));
}

TEST_CASE("driftwire cli look: the offer, at 80 and 40 columns", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  check_rows(rows_of(cli::offer_card(th, offer_model(), 80), 80),
             {
                 "╭─ Ann's MacBook wants to send you 24 files · 412 MB ──────────────────────────╮",
                 "│   IMG_4471.HEIC, IMG_4472.HEIC, IMG_4473.MOV, IMG_4474.HEIC and 20 more      │",
                 "│                                                                              │",
                 "│ ❯ 1. Accept · save to ~/Downloads                                            │",
                 "│   2. Accept and save somewhere else…                                         │",
                 "│   3. Decline                                                                 │",
                 "╰──────────────────────────────────────────────────────────────────────────────╯",
             });
  check_rows(rows_of(cli::offer_card(th, offer_model(), 40), 40),
             {
                 "╭──────────────────────────────────────╮",
                 "│ Ann's MacBook wants to send you 24   │",
                 "│ files · 412 MB                       │",
                 "│   IMG_4471.HEIC and 23 more          │",
                 "│                                      │",
                 "│ ❯ 1. Accept · save to ~/Downloads    │",
                 "│   2. Accept and save somewhere else… │",
                 "│   3. Decline                         │",
                 "╰──────────────────────────────────────╯",
             });
  // A folder, from a remembered device, the pointer on the second choice.
  cli::offer_model m = offer_model();
  m.folder = "Trip";
  m.known = true;
  m.selected = 1;
  const auto r = rows_of(cli::offer_card(th, m, 80), 80);
  CHECK(r[0].find("Ann's MacBook (remembered) wants to send you 24 files") != std::string::npos);
  CHECK(r[1].find("the folder \"Trip\"") != std::string::npos);
  CHECK(any_row_has(r, "│ ❯ 2. Accept and save somewhere else…"));
  CHECK(any_row_has(r, "│   1. Accept"));
  // Its hint line drops whole parts when narrow.
  const cli::hint_model h{cli::offer_hint(th), ""};
  check_rows(rows_of(cli::hints(th, h, 80), 80), {"  ↑↓ or 1–3 · enter to choose · y accept · n or esc decline"});
  check_rows(rows_of(cli::hints(th, h, 40), 40), {"  ↑↓ or 1–3 · n or esc decline"});
}

TEST_CASE("driftwire cli look: typing the code, with a wrong-code note", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  check_rows(rows_of(cli::code_entry_card(th, wrong_code_model(), 80), 80),
             {
                 "╭─ Type the code on Ann's MacBook's screen ────────────────────────────────────╮",
                 "│                                                                              │",
                 "│      ┌───┐ ┌───┐ ┌───┐   ┌───┐ ┌───┐ ┌───┐                                   │",
                 "│      │ 4 │ │ 8 │ │ 2 │   │ 9 │ │ ▏ │ │   │                                   │",
                 "│      └───┘ └───┘ └───┘   └───┘ └───┘ └───┘                                   │",
                 "│                                                                              │",
                 "│   That is not the code on Ann's MacBook's screen. 2 tries left.              │",
                 "╰──────────────────────────────────────────────────────────────────────────────╯",
             });
  check_rows(rows_of(cli::code_entry_card(th, wrong_code_model(), 40), 40),
             {
                 "╭──────────────────────────────────────╮",
                 "│ Type the code on Ann's MacBook's     │",
                 "│ screen                               │",
                 "│                                      │",
                 "│   [4][8][2] [9][▏][ ]                │",
                 "│                                      │",
                 "│   That is not the code on Ann's      │",
                 "│   MacBook's screen. 2 tries left.    │",
                 "╰──────────────────────────────────────╯",
             });
  // The note is the warning colour; with no note the card says nothing has been sent.
  const auto styled = cli::rasterize(cli::code_entry_card(th, wrong_code_model(), 80), 80).rows;
  CHECK(any_row_has(styled, fg(cli::palette::kYellow)));
  cli::code_entry_model empty;
  empty.peer = "Ann's MacBook";
  CHECK(any_row_has(rows_of(cli::code_entry_card(th, empty, 80), 80), "Nothing has been sent yet."));
  check_rows(rows_of(cli::hints(th, {cli::code_entry_hint(), ""}, 80), 80),
             {"  digits only · it goes by itself at the sixth · esc to cancel"});
  check_rows(rows_of(cli::hints(th, {cli::code_entry_hint(), ""}, 40), 40), {"  digits only · esc to cancel"});
}

TEST_CASE("driftwire cli look: progress at 0, 42 and 100 per cent", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  check_rows(rows_of(cli::progress(th, moving(0), 80, 0), 80),
             {
                 "  ThinkPad ●━━━━━━━━━━━━━━━━━━━━━━  Wi-Fi  ━━━━━━━━━━━━━━━━━━━━━━━ Ann's MacBook",
                 "  ─────────────────────────────────────────────────────────────────────    0 %",
                 "  0 of 3 files · 0 bytes of 4.8 GB · 14 MB/s · about 6 min left · esc to cancel",
             });
  check_rows(rows_of(cli::progress(th, moving(0.42), 80, 0), 80),
             {
                 "  ThinkPad ●━━━━━━━━━━━━━━━━━━━━━━  Wi-Fi  ━━━━━━━━━━━━━━━━━━━━━━━ Ann's MacBook",
                 "  ████████████████████████████▉────────────────────────────────────────   42 %",
                 "  1 of 3 files · 2.0 GB of 4.8 GB · 14 MB/s · about 3 min left · esc to cancel",
             });
  check_rows(rows_of(cli::progress(th, moving(1.0), 80, 0), 80),
             {
                 "  ThinkPad ●━━━━━━━━━━━━━━━━━━━━━━  Wi-Fi  ━━━━━━━━━━━━━━━━━━━━━━━ Ann's MacBook",
                 "  █████████████████████████████████████████████████████████████████████  100 %",
                 "  3 of 3 files · 4.8 GB of 4.8 GB · esc to cancel",
             });
  // 40 columns: the file count, the hint and the speed give way; the bytes and the time left stay.
  check_rows(rows_of(cli::progress(th, moving(0.42), 40, 0), 40),
             {
                 "  ThinkPad ●━━  Wi-Fi  ━━━ Ann's MacBook",
                 "  ████████████▏────────────────   42 %",
                 "  2.0 GB of 4.8 GB · about 3 min left",
             });
  // An eighth block for part of a cell: 4.5 of 8 cells is four full and a half.
  check_rows(rows_of(cli::bar(th, 4.5 / 8, 19), 19), {"  ████▌───   56 %"});
  check_rows(rows_of(cli::bar(th, 1.0 / 64, 19), 19), {"  ▏───────    1 %"});
  // The bar's colour runs from the accent to the indigo.
  const auto styled = cli::rasterize(cli::bar(th, 1.0, 40), 40).rows;
  CHECK(any_row_has(styled, fg(cli::palette::kTeal)));
  CHECK(any_row_has(styled, fg(cli::palette::kIndigo)));
  // Nothing about the time left until the engine has it.
  cli::progress_model early = moving(0.01);
  early.rate = 0;
  early.left_s = -1;
  const auto e = rows_of(cli::stats(th, early, 80), 80);
  CHECK_FALSE(any_row_has(e, "left"));
  CHECK_FALSE(any_row_has(e, "/s"));
}

TEST_CASE("driftwire cli look: the path in each bearer's tint, and reconnecting", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  struct row {
    cli::bearer via;
    std::string badge;
    cli::rgb tint;
    std::string_view label;
  };
  const row rows[] = {
      {cli::bearer::wifi, "", cli::palette::kTeal, "Wi-Fi"},
      {cli::bearer::bluetooth, "", cli::palette::kBlue, "Bluetooth"},
      {cli::bearer::internet, "", cli::palette::kIndigo, "internet"},
      {cli::bearer::dht, "", cli::palette::kOrange, "DHT"},
      {cli::bearer::relay, "Studio Mac", cli::palette::kPurple, "Studio Mac"},
  };
  for (const auto& r : rows) {
    CAPTURE(r.label);
    cli::path_model p = moving(0.42).link;
    p.via = r.via;
    p.badge = r.badge;
    const cli::raster ra = cli::rasterize(cli::path(th, p, 80, 0), 80);
    REQUIRE(ra.plain.size() == 1);
    CHECK(ra.plain[0].find("  " + std::string(r.label) + "  ") != std::string::npos);
    CHECK(ra.plain[0].rfind("  ThinkPad ", 0) == 0);
    CHECK(ra.plain[0].size() - ra.plain[0].rfind("Ann's MacBook") == std::string_view("Ann's MacBook").size());
    CHECK(cli::display_width(ra.plain[0]) == 80);
    CHECK(ra.rows[0].find(fg(r.tint)) != std::string::npos);
  }

  // The dots travel toward the receiver, a cell a frame at 12 frames a second.
  cli::path_model p = moving(0.42).link;
  // The train's head is its last dot in the direction it goes.
  const auto head_at = [&](std::int64_t ms) {
    const std::string row = rows_of(cli::path(th, p, 80, ms), 80)[0];
    const std::size_t at = p.sending ? row.rfind("\xE2\x97\x8F") : row.find("\xE2\x97\x8F");  // ●
    return cli::display_width(row.substr(0, at));
  };
  CHECK(head_at(1000) - head_at(0) == 12);
  CHECK(head_at(500) - head_at(0) == 6);
  p.sending = false;  // receiving: from the other end, toward this computer
  CHECK(head_at(0) - head_at(1000) == 12);

  // Reconnecting: frozen, dimmed, and saying so in the warning colour; no speed.
  cli::progress_model m = moving(0.42);
  m.link.reconnecting = true;
  const cli::raster ra = cli::rasterize(cli::progress(th, m, 80, 700), 80);
  CHECK(ra.plain[0].find("reconnecting\xE2\x80\xA6") != std::string::npos);
  CHECK(ra.rows[0].find(fg(cli::palette::kYellow)) != std::string::npos);
  CHECK(ra.rows[0].find(fg(cli::palette::kGrey)) != std::string::npos);
  CHECK(ra.rows[0].find(fg(cli::palette::kTeal)) == std::string::npos);
  CHECK(ra.plain[2].find("MB/s") == std::string::npos);
  CHECK(ra.plain[2].find("left") == std::string::npos);

  // Narrow: the line shrinks first, then the names are cut.
  const auto narrow = rows_of(cli::path(th, moving(0.42).link, 30, 0), 30)[0];
  CHECK(cli::display_width(narrow) <= 30);
  CHECK(narrow.find("\xE2\x80\xA6") != std::string::npos);
}

TEST_CASE("driftwire cli look: the waiting line spins and shimmers only in truecolor", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  const cli::wait_model w{"Waiting for Ann's MacBook to accept\xE2\x80\xA6", 12};
  const auto at = [&](std::int64_t ms) { return rows_of(cli::waiting(th, w, 80, ms), 80)[0]; };
  CHECK(at(0) == "  \xC2\xB7 Waiting for Ann's MacBook to accept\xE2\x80\xA6  (12s \xC2\xB7 esc to cancel)");
  // · ✢ ✳ ✶ ✻ ✽ and back, 120 ms a step.
  const char* frames[] = {"\xC2\xB7",     "\xE2\x9C\xA2", "\xE2\x9C\xB3", "\xE2\x9C\xB6", "\xE2\x9C\xBB",
                          "\xE2\x9C\xBD", "\xE2\x9C\xBB", "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
                          "\xC2\xB7"};
  for (int i = 0; i < 11; ++i) {
    CAPTURE(i);
    CHECK(at(i * 120).rfind("  " + std::string(frames[i]) + " ", 0) == 0);
  }
  // A band of the lighter accent moves across the text.
  const auto styled = [&](const cli::theme& t, std::int64_t ms) {
    return cli::rasterize(cli::waiting(t, w, 80, ms), 80).rows[0];
  };
  CHECK(styled(th, 300) != styled(th, 900));
  const auto colours = [](const std::string& row) {
    std::vector<std::string> seen;
    for (std::size_t pos = row.find("38;2;"); pos != std::string::npos; pos = row.find("38;2;", pos + 1)) {
      std::size_t end = pos + 5;  // past "38;2;": r ; g ; b
      for (int k = 0; k < 3 && end < row.size(); ++k) {
        end = std::min(row.size(), row.find_first_not_of("0123456789", end));
        if (k < 2) ++end;
      }
      const std::string c = row.substr(pos, end - pos);
      if (std::find(seen.begin(), seen.end(), c) == seen.end()) seen.push_back(c);
    }
    return seen.size();
  };
  CHECK(colours(styled(th, 300)) >= 4);  // the accent and the steps of the band
  // 256 colours: no sweep (the same colours whatever the time, but the glyph).
  const cli::theme t256 = cli::make_theme(cli::color_depth::c256, true);
  CHECK(styled(t256, 240) == styled(t256, 1440));  // the same spinner frame, 1.2 s apart
  // Too narrow for the suffix: the text alone, cut.
  CHECK(cli::display_width(rows_of(cli::waiting(cli::make_theme(cli::color_depth::truecolor, true), w, 30, 0), 30)[0]) <= 30);
}

TEST_CASE("driftwire cli look: the transcript", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  const auto e = [&](cli::tone t, std::string text, std::vector<std::string> details, int width = 80) {
    return cli::rasterize(cli::entry(th, {t, std::move(text), std::move(details)}, width), width);
  };
  check_rows(e(cli::tone::success, "Received 24 files from Ann's MacBook",
               {"412 MB in 31 s \xC2\xB7 Wi-Fi \xC2\xB7 every file checked \xC2\xB7 ~/Downloads"})
                 .plain,
             {"⏺ Received 24 files from Ann's MacBook", "  ⎿ 412 MB in 31 s · Wi-Fi · every file checked · ~/Downloads"});
  check_rows(e(cli::tone::warning, "Ann's MacBook declined.", {}).plain, {"⏺ Ann's MacBook declined."});
  // The bullet's colour says how it went.
  CHECK(e(cli::tone::success, "x", {}).rows[0].find(fg(cli::palette::kGreen)) != std::string::npos);
  CHECK(e(cli::tone::error, "x", {}).rows[0].find(fg(cli::palette::kRed)) != std::string::npos);
  CHECK(e(cli::tone::warning, "x", {}).rows[0].find(fg(cli::palette::kYellow)) != std::string::npos);
  CHECK(e(cli::tone::info, "x", {}).rows[0].find(fg(cli::palette::kTeal)) != std::string::npos);
  // Long lines wrap under their own text.
  check_rows(e(cli::tone::error, "The code was wrong 3 times, so nothing was sent.", {"Try again."}, 30).plain,
             {"⏺ The code was wrong 3 times,", "  so nothing was sent.", "  ⎿ Try again."});
}

TEST_CASE("driftwire cli look: the start banner, the prompt and the slash palette", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  const cli::banner_model b{"1.0.0", "Tristan's ThinkPad", "visible on Wi-Fi", "internet off", "~/Downloads"};
  check_rows(rows_of(cli::banner(th, b, 80), 80),
             {
                 "╭──────────────────────────────────────────────────────────────────────────────╮",
                 "│ ≋ Driftwire  1.0.0                                                           │",
                 "│                                                                              │",
                 "│   Tristan's ThinkPad  ·  visible on Wi-Fi  ·  internet off                   │",
                 "│   Saves to ~/Downloads  ·  /help for commands                                │",
                 "╰──────────────────────────────────────────────────────────────────────────────╯",
             });
  // The wordmark runs teal to indigo.
  const auto styled = cli::rasterize(cli::banner(th, b, 80), 80).rows;
  CHECK(styled[1].find(fg(cli::palette::kTeal)) != std::string::npos);
  CHECK(styled[1].find(fg(cli::palette::kIndigo)) != std::string::npos);

  // The prompt: the placeholder, then typed text with the cursor where the editor has it.
  cli::prompt_model p;
  p.placeholder = "Type or drop files to send";
  check_rows(rows_of(cli::prompt(th, p, 80), 80),
             {
                 "╭──────────────────────────────────────────────────────────────────────────────╮",
                 "│ > Type or drop files to send                                                 │",
                 "╰──────────────────────────────────────────────────────────────────────────────╯",
             });
  cli::line_editor ed;
  ed.set("~/Pictures/Trip 2026");
  ed.left();
  const auto v = ed.view(cli::prompt_columns(80));
  p.text = v.text;
  p.cursor_col = v.cursor_col;
  cli::raster r = cli::rasterize(cli::prompt(th, p, 80), 80);
  CHECK(r.plain[1] == "│ > ~/Pictures/Trip 2026" + std::string(55, ' ') + "│");
  CHECK(r.cursor_row == 1);
  CHECK(r.cursor_col == 4 + v.cursor_col);
  // Too long for the box: the ends that go on are marked.
  ed.set(std::string(100, 'x'));
  ed.home();
  const auto long_view = ed.view(cli::prompt_columns(40));
  p.text = long_view.text;
  p.cursor_col = long_view.cursor_col;
  p.cut_right = long_view.cut_right;
  r = cli::rasterize(cli::prompt(th, p, 40), 40);
  CHECK(r.plain[1] == "│ > " + std::string(33, 'x') + "… │");
  CHECK(r.cursor_col == 4);

  // The palette, filtered by "/co".
  cli::palette_model pal;
  for (const std::size_t i : cli::filter_commands("/co")) {
    const auto& c = cli::commands()[i];
    pal.rows.push_back({std::string(c.name), std::string(c.args), std::string(c.description)});
  }
  check_rows(rows_of(cli::command_palette(th, pal, 80), 80),
             {
                 "  ❯ /code          Receive from anywhere: make a code to give someone",
                 "    /to-code       Send these files to a code someone gave you",
             });
  pal.selected = 1;
  const auto sel = rows_of(cli::command_palette(th, pal, 40), 40);
  check_rows(sel, {"    /code          Receive from anywher…", "  ❯ /to-code       Send these files to…"});
  check_rows(rows_of(cli::command_palette(th, cli::palette_model{}, 80), 80), {"    No command matches"});
  // Under the height cap the palette keeps the selection in sight.
  cli::palette_model all;
  for (const auto& c : cli::commands()) all.rows.push_back({std::string(c.name), "", std::string(c.description)});
  all.max_rows = 4;
  all.selected = 10;
  const auto capped = rows_of(cli::command_palette(th, all, 80), 80);
  CHECK(capped.size() == 4);
  CHECK(capped.back().find("\xE2\x9D\xAF") != std::string::npos);  // ❯ on the last row shown
}

TEST_CASE("driftwire cli look: Nearby, empty, full and narrow, with a CJK and emoji name", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  check_rows(rows_of(cli::nearby(th, nearby_two(), 80), 80),
             {
                 "  Nearby",
                 "  ❯ ◉ Ann's MacBook                                  Wi-Fi",
                 "    ◉ Living-room PC                                 Wi-Fi · Bluetooth",
             });
  check_rows(rows_of(cli::nearby(th, cli::nearby_model{}, 80), 80),
             {
                 "  Nearby",
                 "  ○ No one nearby yet",
                 "    Open Driftwire on the other computer, on the same Wi-Fi network. It shows up",
                 "    here.",
             });
  check_rows(rows_of(cli::nearby(th, nearby_two(), 40), 40),
             {
                 "  Nearby",
                 "  ❯ ◉ Ann's MacBook   Wi-Fi",
                 "    ◉ Living-room PC  Wi-Fi · Bluetooth",
             });
  // A name in any script is cut by its columns, with an ellipsis.
  cli::nearby_model m = nearby_two();
  m.peers.push_back(
      {"\xE6\x9D\x8E\xE6\x98\x8E\xE7\x9A\x84\xE7\x94\xB5\xE8\x84\x91 \xF0\x9F\x8C\xB8 (a very long name indeed)",
       "Bluetooth", cli::bearer::bluetooth});
  m.known = {{"Studio Mac", "over the internet", cli::bearer::internet}};
  m.selected = 3;
  const auto r = rows_of(cli::nearby(th, m, 40), 40);
  check_rows(r, {
                    "  Nearby",
                    "    ◉ Ann's MacBook   Wi-Fi",
                    "    ◉ Living-room PC  Wi-Fi · Bluetooth",
                    "    ◉ 李明的电脑 🌸…  Bluetooth",
                    "  Your devices · over the internet",
                    "  ❯ ◉ Studio Mac      over the internet",
                });
  CHECK(widest(r) <= 40);
  // Under the height cap: a window around the selection, and what is hidden counted.
  cli::nearby_model many;
  for (int i = 0; i < 9; ++i) many.peers.push_back({"Computer " + std::to_string(i), "Wi-Fi"});
  many.selected = 7;
  many.max_rows = 3;
  const auto capped = rows_of(cli::nearby(th, many, 80), 80);
  REQUIRE(capped.size() == 5);
  CHECK(capped[1].find("Computer 6") != std::string::npos);
  CHECK(capped[2].find("\xE2\x9D\xAF \xE2\x97\x89 Computer 7") != std::string::npos);
  CHECK(capped[4].find("and 6 more") != std::string::npos);
  // The dot is the bearer's tint.
  const auto styled = cli::rasterize(cli::nearby(th, m, 80), 80).rows;
  CHECK(styled[3].find(fg(cli::palette::kBlue)) != std::string::npos);
  CHECK(styled[5].find(fg(cli::palette::kIndigo)) != std::string::npos);
}

TEST_CASE("driftwire cli look: the hint line keeps whole parts and the count on the right", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  const cli::hint_model h{{"/ for commands", "\xE2\x86\x91\xE2\x86\x93 choose a computer", "enter to send"},
                          "3 files \xC2\xB7 1.2 GB"};
  check_rows(rows_of(cli::hints(th, h, 80), 80),
             {"  / for commands · ↑↓ choose a computer · enter to send       3 files · 1.2 GB"});
  check_rows(rows_of(cli::hints(th, h, 52), 52), {"  / for commands · enter to send  3 files · 1.2 GB"});
  check_rows(rows_of(cli::hints(th, h, 51), 51), {"  / for commands                 3 files · 1.2 GB"});
  check_rows(rows_of(cli::hints(th, h, 40), 40), {"  / for commands      3 files · 1.2 GB"});
  check_rows(rows_of(cli::hints(th, h, 20), 20), {"  / for commands"});
  const cli::hint_model ask{{"Press Ctrl+C again to quit"}, "", cli::tone::warning};
  CHECK(cli::rasterize(cli::hints(th, ask, 80), 80).rows[0].find(fg(cli::palette::kYellow)) != std::string::npos);
}

TEST_CASE("driftwire cli look: the internet code, the check to compare, remembering", "[driftwire][cli]") {
  const cli::theme th = colour_theme();
  cli::internet_code_model c;
  c.code = "amber river quiet falcon";
  c.link = "driftwire://c/amber-river-quiet-falcon";
  c.left_s = 540;
  c.how = "router mapping, IPv6";
  const auto r = rows_of(cli::internet_code_card(th, c, 80), 80);
  CHECK(r[0].find("Receive from anywhere") != std::string::npos);
  CHECK(any_row_has(r, "│      amber river quiet falcon"));
  CHECK(any_row_has(r, "It works once, 9 min left."));
  CHECK(any_row_has(r, "c copy code · l copy link · esc stop │"));
  // At 40 columns the keys go under, as many whole ones as fit.
  const auto n = rows_of(cli::internet_code_card(th, c, 40), 40);
  CHECK(any_row_has(n, "c copy code · esc stop │"));
  CHECK(widest(n) == 40);
  c.copied = "Code copied";
  CHECK(any_row_has(rows_of(cli::internet_code_card(th, c, 80), 80), "│   Code copied"));

  const auto naming = rows_of(cli::naming_card(th, {"Ann's MacBook", "482 917", ""}, 80), 80);
  CHECK(naming[0].find("Connected to Ann's MacBook (new device)") != std::string::npos);
  CHECK(any_row_has(naming, "Check that Ann's MacBook's screen shows"));
  CHECK(any_row_has(naming, "╭──╮ ╭──╮"));  // big digits
  CHECK(any_row_has(naming, "│ ❯ 1. It matches · send"));
  CHECK(any_row_has(naming, "│   2. Cancel"));

  cli::remember_model rm;
  rm.peer = "Ann's MacBook";
  CHECK(rows_of(cli::remember_card(th, rm, 80), 80)[0].find("Remember Ann's MacBook?") != std::string::npos);
  rm.at = cli::remember_model::step::type_check;
  rm.typed = "61";
  CHECK(any_row_has(rows_of(cli::remember_card(th, rm, 80), 80), "│ 6 │ │ 1 │ │ ▏ │"));
}

TEST_CASE("driftwire cli look: ASCII glyphs where the terminal is not UTF-8", "[driftwire][cli]") {
  const cli::theme th = cli::make_theme(cli::color_depth::truecolor, false);
  const auto r = rows_of(cli::code_card(th, code_card_model(), 80), 80);
  check_rows(r, {
                    "+- Ann's MacBook wants to send you files --------------------------------------+",
                    "|                                                                              |",
                    "|      342 917                                                                 |",
                    "|                                                                              |",
                    "|   Type this code on Ann's MacBook.                            esc to decline |",
                    "+------------------------------------------------------------------------------+",
                });
  check_rows(rows_of(cli::code_boxes(th, "48", 80), 80), {"[4][8][_] [ ][ ][ ]"});
  check_rows(rows_of(cli::bar(th, 0.5, 19), 19), {"  ####----   50 %"});
  check_rows(rows_of(cli::entry(th, {cli::tone::success, "Sent.", {"1 MB in 1 s"}}, 80), 80), {"* Sent.", "  `- 1 MB in 1 s"});
  const auto n = rows_of(cli::nearby(th, nearby_two(), 80), 80);
  CHECK(n[1].rfind("  > * Ann's MacBook", 0) == 0);
  // Nothing outside ASCII, but what the data itself holds.
  for (const auto& row : rows_of(cli::progress(th, moving(0.42), 80, 0), 80)) {
    CAPTURE(row);
    CHECK(std::all_of(row.begin(), row.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
  }
}

TEST_CASE("driftwire cli look: NO_COLOR has no colour, only weight", "[driftwire][cli]") {
  const cli::theme th = cli::make_theme(cli::color_depth::none, true);
  CHECK_FALSE(th.colour());
  const ftxui::Element everything = cli::region({
      cli::banner(th, {"1.0.0", "Tristan's ThinkPad", "visible on Wi-Fi", "internet off", "~/Downloads"}, 80),
      cli::nearby(th, nearby_two(), 80),
      cli::code_card(th, code_card_model(), 80),
      cli::offer_card(th, offer_model(), 80),
      cli::progress(th, moving(0.42), 80, 300),
      cli::waiting(th, {"Waiting\xE2\x80\xA6", 3}, 80, 300),
      cli::entry(th, {cli::tone::error, "Failed.", {}}, 80),
  });
  const cli::raster r = cli::rasterize(everything, 80);
  bool bold = false, dim = false;
  for (const auto& row : r.rows) {
    CAPTURE(row);
    CHECK(row.find("38;") == std::string::npos);
    CHECK(row.find("48;") == std::string::npos);
    CHECK(row.find("\x1B[0;9") == std::string::npos);  // 90-97: a bright colour
    CHECK(row.find(";3") == std::string::npos);          // 30-37 or italic
    bold = bold || row.find("\x1B[0;1") != std::string::npos;
    dim = dim || row.find("\x1B[0;2") != std::string::npos;
  }
  CHECK(bold);
  CHECK(dim);
  // The flag, and the environment variable (no-color.org).
  CHECK(cli::detect_color_depth(true) == cli::color_depth::none);
#if defined(_WIN32)
  _putenv_s("NO_COLOR", "1");
  CHECK(cli::detect_color_depth(false) == cli::color_depth::none);
  _putenv_s("NO_COLOR", "");
#else
  const char* before = std::getenv("NO_COLOR");
  const std::string kept = before ? before : "";
  ::setenv("NO_COLOR", "1", 1);
  CHECK(cli::detect_color_depth(false) == cli::color_depth::none);
  if (before) {
    ::setenv("NO_COLOR", kept.c_str(), 1);
  } else {
    ::unsetenv("NO_COLOR");
  }
#endif
}

// ==== the live region =========================================================================================

TEST_CASE("driftwire cli inline view: the region is redrawn in place, only when it changed", "[driftwire][cli]") {
  fake_terminal t(40, 10);
  cli::inline_view v(t, 40, 10);
  REQUIRE(v.present(rows_doc({"one", "two", "three"})));
  CHECK(t.visible() == std::vector<std::string>{"one", "two", "three"});
  CHECK(t.y() == 2);
  CHECK(t.ups.empty());  // the first frame starts where the cursor is

  // The same again: nothing written.
  const std::size_t writes = t.writes.size();
  CHECK_FALSE(v.present(rows_doc({"one", "two", "three"})));
  CHECK(t.writes.size() == writes);

  // A shorter one: up two rows, cleared to the end, nothing left behind.
  REQUIRE(v.present(rows_doc({"four"})));
  CHECK(t.ups == std::vector<int>{2});
  CHECK(t.writes.back().find("\r\x1B[J") != std::string::npos);
  CHECK(t.visible() == std::vector<std::string>{"four"});
  // A taller one.
  REQUIRE(v.present(rows_doc({"a", "b", "c", "d"})));
  CHECK(t.ups == std::vector<int>{2});  // from row 0 of the region: no way up
  CHECK(t.visible() == std::vector<std::string>{"a", "b", "c", "d"});
  REQUIRE(v.present(rows_doc({"e"})));
  CHECK(t.ups.back() == 3);
  CHECK(t.visible() == std::vector<std::string>{"e"});

  // Every write is one synchronized frame, the cursor hidden while it draws.
  for (const auto& w : t.writes) {
    CHECK(w.rfind("\x1B[?2026h\x1B[?25l", 0) == 0);
    CHECK(w.size() >= 8);
    CHECK(w.substr(w.size() - 8) == "\x1B[?2026l");
  }
  CHECK_FALSE(t.synchronized());
  CHECK_FALSE(t.cursor_shown());  // no prompt in these: the cursor stays hidden

  // Ctrl+L, a resize: drawn again though unchanged.
  v.invalidate();
  CHECK(v.present(rows_doc({"e"})));
  v.resize(30, 10);
  CHECK(v.present(rows_doc({"e"})));
  CHECK(t.visible() == std::vector<std::string>{"e"});
}

TEST_CASE("driftwire cli inline view: the transcript goes above the region and stays", "[driftwire][cli]") {
  fake_terminal t(60, 8);
  cli::inline_view v(t, 60, 8);
  const cli::theme th = colour_theme();
  v.present(rows_doc({"live 1", "live 2"}));
  v.print(cli::entry(th, {cli::tone::success, "Sent 3 files to Ann's MacBook.", {"1.2 GB in 1 min 32 s"}}, 60));
  CHECK(t.lines() ==
        std::vector<std::string>{"⏺ Sent 3 files to Ann's MacBook.", "  ⎿ 1.2 GB in 1 min 32 s", "live 1", "live 2"});
  v.present(rows_doc({"live 3"}));
  CHECK(t.lines() == std::vector<std::string>{"⏺ Sent 3 files to Ann's MacBook.", "  ⎿ 1.2 GB in 1 min 32 s", "live 3"});
  // Enough transcript to scroll the screen: it goes into the scrollback in order.
  for (int i = 0; i < 10; ++i) v.print(rows_doc({"entry " + std::to_string(i)}));
  std::vector<std::string> want{"⏺ Sent 3 files to Ann's MacBook.", "  ⎿ 1.2 GB in 1 min 32 s"};
  for (int i = 0; i < 10; ++i) want.push_back("entry " + std::to_string(i));
  want.emplace_back("live 3");
  CHECK(t.lines() == want);

  // The end: the region goes, a last dim line, the cursor shown; the transcript stays.
  v.close("Driftwire closed \xC2\xB7 this computer is no longer visible");
  want.back() = "Driftwire closed · this computer is no longer visible";
  CHECK(t.lines() == want);
  CHECK(t.cursor_shown());
  CHECK_FALSE(v.present(rows_doc({"after"})));
  CHECK(t.lines() == want);
}

TEST_CASE("driftwire cli inline view: the cursor goes into the prompt", "[driftwire][cli]") {
  fake_terminal t(40, 10);
  cli::inline_view v(t, 40, 10);
  const cli::theme th = colour_theme();
  cli::line_editor ed;
  ed.set("hello");
  ed.left();
  const auto view = ed.view(cli::prompt_columns(40));
  cli::prompt_model p;
  p.text = view.text;
  p.cursor_col = view.cursor_col;
  const auto doc = [&] {
    return ftxui::vbox({rows_doc({"Nearby"}), cli::prompt(th, p, 40), cli::hints(th, {{"/ for commands"}, ""}, 40)});
  };
  REQUIRE(v.present(doc()));
  CHECK(t.cursor_shown());
  CHECK(t.y() == 2);              // the prompt's row
  CHECK(t.x() == 4 + 4);          // "│ > " and four letters
  // The next frame goes up from there, not from the bottom.
  p.text = "help";
  p.cursor_col = 4;
  REQUIRE(v.present(doc()));
  CHECK(t.ups.back() == 2);
  CHECK(t.visible()[2] == "│ > help                               │");
  CHECK(t.x() == 8);
  // Erased for a moment (Ctrl+Z): the screen is clear from the region down, the cursor shown.
  v.erase();
  CHECK(t.visible().empty());
  CHECK(t.cursor_shown());
  REQUIRE(v.present(doc()));
  CHECK(t.visible().size() == 5);
}

TEST_CASE("driftwire cli inline view: never taller than the terminal less one row", "[driftwire][cli]") {
  fake_terminal t(30, 6);
  cli::inline_view v(t, 30, 6);
  CHECK(v.max_rows() == 5);
  std::vector<std::string> tall;
  for (int i = 0; i < 9; ++i) tall.push_back("row " + std::to_string(i));
  REQUIRE(v.present(rows_doc(tall)));
  // The top rows go: the region never scrolls its first row out of reach.
  CHECK(t.lines() == std::vector<std::string>{"row 4", "row 5", "row 6", "row 7", "row 8"});
  REQUIRE(v.present(rows_doc({"short"})));
  CHECK(t.lines() == std::vector<std::string>{"short"});
  // A smaller terminal: the cap follows.
  v.resize(30, 3);
  CHECK(v.max_rows() == 2);
  REQUIRE(v.present(rows_doc(tall)));
  CHECK(t.visible() == std::vector<std::string>{"row 7", "row 8"});
}

TEST_CASE("driftwire cli inline view: a narrower terminal counts the way up at its new width", "[driftwire][cli]") {
  fake_terminal t(40, 12);
  cli::inline_view v(t, 40, 12);
  const cli::theme th = colour_theme();
  cli::prompt_model p;
  p.text = "abc";
  p.cursor_col = 3;
  const auto doc = [&] {
    return ftxui::vbox({rows_doc({std::string(40, 'x'), std::string(25, 'y')}), cli::prompt(th, p, v.width())});
  };
  REQUIRE(v.present(doc()));
  CHECK(t.y() == 3);  // the prompt's line, under two rows and the box's top
  // Every row at 40 columns takes two at 20 (the 25 too): 2 + 2 + 2 rows up,
  // the cursor's own row not rewrapped past its column.
  // (The frame's first move up is the way to its top; a second one puts the
  // cursor back into the prompt.)
  v.resize(20, 12);
  std::size_t from = t.ups.size();
  REQUIRE(v.present(doc()));
  REQUIRE(t.ups.size() > from);
  CHECK(t.ups[from] == 6);
  // Wider again: no rewrap to count, the region's own rows.
  v.resize(40, 12);
  from = t.ups.size();
  REQUIRE(v.present(doc()));
  REQUIRE(t.ups.size() > from);
  CHECK(t.ups[from] == 3);
}

#if !defined(_WIN32)
namespace {

// A pseudo-terminal: the test holds the master, the console the other side.
struct pty_pair {
  int master = -1, slave = -1;
  pty_pair() {
    master = ::posix_openpt(O_RDWR | O_NOCTTY);
    REQUIRE(master >= 0);
    REQUIRE(::grantpt(master) == 0);
    REQUIRE(::unlockpt(master) == 0);
    slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
    REQUIRE(slave >= 0);
    winsize ws{};
    ws.ws_col = 97;
    ws.ws_row = 31;
    ::ioctl(master, TIOCSWINSZ, &ws);
  }
  ~pty_pair() {
    if (slave >= 0) ::close(slave);
    if (master >= 0) ::close(master);
  }
  pty_pair(const pty_pair&) = delete;
  pty_pair& operator=(const pty_pair&) = delete;

  void type(std::string_view s) const { REQUIRE(::write(master, s.data(), s.size()) == static_cast<ssize_t>(s.size())); }
  // What the console wrote, waiting up to `ms` for the first of it.
  [[nodiscard]] std::string drain(int ms = 500) const {
    std::string out;
    while (true) {
      fd_set r;
      FD_ZERO(&r);
      FD_SET(master, &r);
      timeval tv{ms / 1000, (ms % 1000) * 1000};
      if (::select(master + 1, &r, nullptr, nullptr, &tv) <= 0) break;
      char buf[256];
      const ssize_t n = ::read(master, buf, sizeof buf);
      if (n <= 0) break;
      out.append(buf, static_cast<std::size_t>(n));
      ms = 20;  // the rest follows at once
    }
    return out;
  }
};

}  // namespace

TEST_CASE("driftwire cli terminal: raw mode, reads, the wake, the size, restored after", "[driftwire][cli]") {
  pty_pair pty;
  termios before{};
  REQUIRE(::tcgetattr(pty.slave, &before) == 0);
  {
    cli::console con(pty.slave, pty.slave, cli::console::options{false});
    CHECK_FALSE(con.raw());
    REQUIRE(con.enter_raw());
    CHECK(con.raw());
    CHECK(pty.drain().find("\x1B[?2004h") != std::string::npos);  // bracketed paste on
    termios raw{};
    REQUIRE(::tcgetattr(pty.slave, &raw) == 0);
    CHECK((raw.c_lflag & ICANON) == 0);
    CHECK((raw.c_lflag & ECHO) == 0);
    CHECK((raw.c_lflag & ISIG) == 0);  // Ctrl+C is a key
    CHECK((raw.c_iflag & ICRNL) == 0);

    const cli::term_size size = con.size();
    CHECK(size.columns == 97);
    CHECK(size.rows == 31);

    // Bytes as typed: Ctrl+C and Enter among them, untranslated.
    pty.type("a\x03\r");
    cli::term_input in;
    con.wait(in, 2000);
    CHECK(in.bytes == "a\x03\r");
    CHECK_FALSE(in.timed_out);

    // Nothing comes: the wait ends at its time.
    const auto t0 = std::chrono::steady_clock::now();
    con.wait(in, 60);
    CHECK(in.timed_out);
    CHECK(in.bytes.empty());
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(50));

    // Another thread wakes it, with no time limit set.
    std::thread waker([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      con.wake();
    });
    con.wait(in, -1);
    waker.join();
    CHECK(in.woken);
    CHECK_FALSE(in.timed_out);
    // Many wakes before one wait: one wake-up, then the wait blocks again.
    for (int i = 0; i < 1000; ++i) con.wake();
    con.wait(in, 0);
    CHECK(in.woken);
    con.wait(in, 20);
    CHECK(in.timed_out);

    con.write("drawn");
    CHECK(pty.drain() == "drawn");

    con.leave_raw();
    CHECK_FALSE(con.raw());
    const std::string restore = pty.drain();
    CHECK(restore.find("\x1B[?25h") != std::string::npos);    // the cursor shown
    CHECK(restore.find("\x1B[?2004l") != std::string::npos);  // bracketed paste off
    CHECK(restore.find("\x1B[0m") != std::string::npos);
    termios after{};
    REQUIRE(::tcgetattr(pty.slave, &after) == 0);
    CHECK(after.c_lflag == before.c_lflag);
    CHECK(after.c_iflag == before.c_iflag);

    // Raw again, then the console goes: restored by the destructor too.
    REQUIRE(con.enter_raw());
    (void)pty.drain();
  }
  termios after{};
  REQUIRE(::tcgetattr(pty.slave, &after) == 0);
  CHECK(after.c_lflag == before.c_lflag);
  CHECK(pty.drain().find("\x1B[?2004l") != std::string::npos);
}

TEST_CASE("driftwire cli terminal: signals arrive as input; the other end closing is the end", "[driftwire][cli]") {
  pty_pair pty;
  {
    cli::console con(pty.slave, pty.slave, cli::console::options{true});
    cli::term_input in;
    ::raise(SIGWINCH);
    con.wait(in, 1000);
    CHECK(in.resized);
    ::raise(SIGTERM);  // ours while the console lives: an orderly quit, not the end of the tests
    con.wait(in, 1000);
    CHECK(in.quit);
    ::raise(SIGHUP);
    con.wait(in, 1000);
    CHECK(in.quit);
  }
  // The handlers are the ones from before: SIGWINCH's default is to ignore it.
  ::raise(SIGWINCH);

  pty_pair gone;
  cli::console con(gone.slave, gone.slave, cli::console::options{false});
  ::close(gone.master);
  gone.master = -1;
  cli::term_input in;
  con.wait(in, 1000);
  CHECK(in.eof);
}
#endif
