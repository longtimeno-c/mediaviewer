// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The terminal's bytes as keys, for the driftwire command line. Pure: the
// bytes come in as the terminal sends them (any split across reads), keys
// come out; the caller owns the clock.
//
// What it understands: UTF-8 text (a character cut across two reads waits
// for the rest); C0 controls as Ctrl+letter (Ctrl+H and DEL are Backspace,
// Ctrl+I Tab, Ctrl+J / Ctrl+M Enter); CSI and SS3 sequences for the arrows,
// Home/End, Insert/Delete, Page Up/Down and Shift+Tab, with xterm's modifier
// parameter (1;3D is Alt+Left, 1;5D Ctrl+Left); Esc as a prefix for Alt
// (macOS Terminal's Option+Left is ESC b); bracketed paste (CSI 200~ ...
// CSI 201~) as one paste event, whatever it holds.
//
// A lone Esc and the start of a sequence begin with the same byte. After a
// bare ESC at the end of what has arrived, pending_ms() says to wait 30 ms
// for more; if nothing comes, flush() makes it the Esc key. Other cut-off
// input (half a sequence, half a character, a paste without its end) waits
// longer and is then dropped (or, for a paste, delivered as it stands).
//
// A terminal without bracketed paste sends pasted text as typed keys, so a
// pasted newline would be Enter. One read cannot hold keys typed by a person
// fast enough to include text, a newline and more text, so a read like that
// is taken as a paste (`paste` event) too: a newline in a paste is never Enter.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mv::driftwire::cli {

enum class key : std::uint8_t {
  none,
  text,       // `text`: one character (UTF-8)
  paste,      // `text`: everything pasted, newlines included
  enter,
  tab,
  backtab,    // Shift+Tab
  backspace,
  del,
  insert,
  left,
  right,
  up,
  down,
  home,
  end,
  page_up,
  page_down,
  esc,
  ctrl,       // `letter`: 'a'..'z' (Ctrl+A, Ctrl+C, ...)
  unknown,    // a sequence we do not use (a function key, a report)
};

struct key_event {
  key k = key::none;
  std::string text;  // key::text, key::paste
  char letter = 0;   // key::ctrl
  bool alt = false;  // Alt / Option / Meta held
  bool ctrl = false; // Ctrl held on a key that has its own code (Ctrl+Left)
  bool shift = false;

  [[nodiscard]] bool is(key k2) const noexcept { return k == k2; }
  [[nodiscard]] bool is_ctrl(char c) const noexcept { return k == key::ctrl && letter == c; }
  friend bool operator==(const key_event&, const key_event&) = default;
};

class key_decoder {
 public:
  static constexpr int kEscWaitMs = 30;    // after a bare ESC
  static constexpr int kCutWaitMs = 150;   // half a sequence or a character
  static constexpr int kPasteWaitMs = 1000;  // a paste whose end has not come

  // Decodes what it can of `bytes` (after anything held back) into `out`.
  void feed(std::string_view bytes, std::vector<key_event>& out);
  // How long to wait for more before flush(); -1 when nothing is held back.
  [[nodiscard]] int pending_ms() const noexcept;
  // The wait ran out: a held ESC is the Esc key, a paste is delivered as it
  // stands, half a sequence or a character is dropped.
  void flush(std::vector<key_event>& out);

 private:
  // Decodes one key at buf_[pos]; false when the bytes there are cut off.
  bool decode_one(std::size_t& pos, std::vector<key_event>& out);
  bool decode_escape(std::size_t& pos, std::vector<key_event>& out);
  void decode_csi(std::string_view params, char final, std::vector<key_event>& out, bool alt);

  std::string buf_;
  bool in_paste_ = false;
  std::string paste_;
};

}  // namespace mv::driftwire::cli
