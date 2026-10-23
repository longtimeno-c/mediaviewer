// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the driftwire command line draws (docs/design/28 "From a terminal"),
// as FTXUI elements. Pure: each function takes a plain struct (a view model
// the session fills from the engine's view), the theme, the columns it has
// and, for what moves, the time in milliseconds; nothing here reads a clock,
// the engine or the terminal.
//
// The vocabulary is a chat-style terminal UI's: rounded cards with the title
// in the border, a prompt box with a hint line under it, a palette of slash
// commands, a shimmering line while something waits, choice lists with a
// pointer, and a transcript of what happened (bullets and an elbow) that stays
// in the scrollback. Names can be any Unicode: everything is measured and cut
// by display width (FTXUI's), never by bytes.
//
// Sizes, speeds and times left are the window's own words (wire.h size_text
// and rate_text, eta.h time_left_text), so a person sees the same numbers in
// both.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "addons/driftwire/cli/text.h"
#include "addons/driftwire/cli/theme.h"

namespace mv::driftwire::cli {

// ---- text by display width (text.h) ---------------------------------------------------

// `utf8` cut to `width` columns, ending in the theme's ellipsis when cut. Never
// splits a character or a character from its combining marks.
[[nodiscard]] std::string fit(std::string_view utf8, int width, const theme& th);

// ---- words --------------------------------------------------------------------------

// How long something took, for the transcript: "under a second", "31 s",
// "1 min 32 s", "2 h 5 min".
[[nodiscard]] std::string elapsed_text(double seconds);
// How long a wait has run, for the waiting line: "12s", "1m 05s", "1h 02m".
[[nodiscard]] std::string wait_text(double seconds);
// `path` with the home folder as "~" ("~/Downloads"); unchanged when `home`
// is empty or not a prefix.
[[nodiscard]] std::string home_short(std::string_view path, std::string_view home);
// "IMG_1.HEIC, IMG_2.HEIC and 21 more" in at most `width` columns: as many
// names as fit, the rest counted (the window's names_line, by width).
[[nodiscard]] std::string names_text(const std::vector<std::string>& names, std::size_t count, int width,
                                     const theme& th);

// ---- primitives ----------------------------------------------------------------------

// Colour and weight for a tone (dim is grey, or the dim attribute with no colour).
[[nodiscard]] ftxui::Element paint(const theme& th, ftxui::Element e, tone t);

// A rounded frame `width` columns wide with `title` in its top border. Body
// rows get width - 4 columns (one space each side inside the bars).
[[nodiscard]] ftxui::Element card(const theme& th, int width, std::string_view title, ftxui::Elements body,
                                  tone border = tone::dim);
// The code a person types on the other computer, three rows of rounded
// digits ("123 456" as two groups), bold, in the accent. Plain bold digits in
// fewer than 40 columns or with ASCII glyphs.
[[nodiscard]] ftxui::Element big_digits(const theme& th, std::string_view digits, int width);
// Six boxes for typing a code; `typed` holds the digits so far, the next box
// shows the caret. One row ("[4][8][2] [9][_][ ]") where the boxes do not fit.
[[nodiscard]] ftxui::Element code_boxes(const theme& th, std::string_view typed, int width);
// A numbered choice list: "❯ 1. Accept", "  2. Decline".
[[nodiscard]] ftxui::Element choices(const theme& th, int width, const std::vector<std::string>& items,
                                     int selected);
// A line of text in `width` columns: wrapped, each row indented by `indent`.
[[nodiscard]] ftxui::Element lines(const theme& th, std::string_view text, int width, tone t = tone::normal,
                                   int indent = 0);
// `left` and `right` on one row, `right` flush right; `left` is cut first.
[[nodiscard]] ftxui::Element spread(const theme& th, std::string_view left, tone left_tone, std::string_view right,
                                    tone right_tone, int width);

// ---- the start banner (transcript, once) ------------------------------------------------

struct banner_model {
  std::string version;   // "1.0.0"
  std::string name;      // this computer as others see it
  std::string presence;  // "visible on Wi-Fi", "not on a network"
  std::string internet;  // "internet off", "internet on"
  std::string saves_to;  // "~/Downloads"
  std::string help = "/help for commands";
};
[[nodiscard]] ftxui::Element banner(const theme& th, const banner_model& m, int width);

// ---- Nearby ----------------------------------------------------------------------------------

struct nearby_row {
  std::string name;
  std::string detail;  // "Wi-Fi", "Wi-Fi · Bluetooth", "over the internet", "sending…"
  bearer dot = bearer::wifi;
  tone detail_tone = tone::dim;
};

struct nearby_model {
  std::vector<nearby_row> peers;
  // Remembered devices, when the internet and the DHT are on: a second
  // section, "Your devices · over the internet".
  std::vector<nearby_row> known;
  int selected = -1;          // into peers, then known; -1: none
  bool show_pointer = true;   // ↑↓ moves it (the prompt has no palette open)
  std::string empty_title = "No one nearby yet";
  std::string empty_how = "Open Driftwire on the other computer, on the same Wi-Fi network. It shows up here.";
  std::string note;           // what this computer cannot search (the window's reach note)
  int max_rows = 0;           // rows of computers at most (the height cap); 0: all
};
[[nodiscard]] ftxui::Element nearby(const theme& th, const nearby_model& m, int width);

// ---- the prompt, its hints, the palette -----------------------------------------------------

struct prompt_model {
  std::string title;        // "" for the main prompt; a field's title ("Send to a code")
  std::string text;         // the visible part of the line (line_editor::view)
  int cursor_col = 0;       // the cursor's column in `text`
  bool cut_left = false, cut_right = false;
  std::string placeholder;  // shown dim while `text` is empty
  bool focused = true;      // the terminal cursor goes here
  tone border = tone::dim;
};
// A frame holding one editable line; with `focused` the element carries the
// cursor (FTXUI focus), which inline_view places in the terminal. `text` is
// what line_editor::view(prompt_columns(width)) gives.
[[nodiscard]] ftxui::Element prompt(const theme& th, const prompt_model& m, int width);
// The columns the line has in a prompt `width` wide ("│ > " ... " │").
[[nodiscard]] int prompt_columns(int width);

struct hint_model {
  std::string left;   // "/ for commands · ↑↓ choose a computer · enter to send"
  std::string right;  // "3 files · 1.2 GB"
  tone left_tone = tone::dim;  // warning for "Press Ctrl+C again to quit"
};
[[nodiscard]] ftxui::Element hints(const theme& th, const hint_model& m, int width);

struct palette_row {
  std::string name;  // "/code"
  std::string args;  // "N on|off"
  std::string description;
};
struct palette_model {
  std::vector<palette_row> rows;  // already filtered, in palette order
  int selected = 0;
  int max_rows = 8;
  std::string empty = "No command matches";
};
[[nodiscard]] ftxui::Element palette(const theme& th, const palette_model& m, int width);

// ---- waiting and moving ------------------------------------------------------------------------

struct wait_model {
  std::string text;               // "Waiting for Ann's MacBook to accept…"
  double elapsed_s = -1;          // < 0: not shown
  std::string hint = "esc to cancel";
  tone t = tone::accent;
};
// One line: the spinner glyph (· ✢ ✳ ✶ ✻ ✽ and back, 120 ms a step) and the
// text, a brighter band sweeping across it every 2 s in truecolor.
[[nodiscard]] ftxui::Element waiting(const theme& th, const wait_model& m, int width, std::int64_t ms);

struct path_model {
  std::string here;     // this computer
  std::string there;    // the other
  bearer via = bearer::wifi;
  std::string badge;    // "" = the bearer's own ("Wi-Fi", "Bluetooth", "internet", "DHT"); a relay: its name
  bool sending = true;  // the dots travel here -> there; receiving: there -> here
  bool moving = true;   // the dots travel (bytes move)
  bool reconnecting = false;  // frozen, dimmed, "reconnecting…"
};
// "ThinkPad ━━━●━━━━●━━━━●━━━  Wi-Fi  ━━━━━━ Ann's MacBook": the line in the
// bearer's tint, dots travelling toward the receiver at 12 a second. When
// narrow the line shrinks first, then the names are cut.
[[nodiscard]] ftxui::Element path(const theme& th, const path_model& m, int width, std::int64_t ms);

// A gradient bar with eighth blocks for the part of a cell, and the percent.
[[nodiscard]] ftxui::Element bar(const theme& th, double fraction, int width);

struct progress_model {
  path_model link;
  std::uint64_t bytes = 0, total = 0;
  std::uint32_t files = 0, count = 0;
  double rate = 0, left_s = -1;  // the engine's (0, -1: not known yet)
  std::string hint = "esc to cancel";
};
// "1.2 GB of 4.8 GB · 14 MB/s · about 4 min left · esc to cancel", the
// engine's numbers only; parts drop from the least needed when narrow.
[[nodiscard]] ftxui::Element stats(const theme& th, const progress_model& m, int width);
// The path, the bar and the stats line.
[[nodiscard]] ftxui::Element progress(const theme& th, const progress_model& m, int width, std::int64_t ms);

// ---- the cards -------------------------------------------------------------------------------------

// The receiver, `in = connecting`: the code to type over there, in big
// digits; a remembered device's offer has none; an internet one shows the
// check to compare (never as a code to type).
struct code_card_model {
  std::string peer;
  std::string code;   // "123 456"
  bool known = false;
  bool internet = false;
  std::string hint = "esc to decline";
};
[[nodiscard]] ftxui::Element code_card(const theme& th, const code_card_model& m, int width);

// The receiver, `in = asking`.
struct offer_model {
  std::string peer;
  bool known = false;        // "(remembered)" in the title
  std::size_t count = 0;
  std::uint64_t bytes = 0;
  std::vector<std::string> names;
  std::string folder;        // the offer is this one folder
  std::string relay;         // through this computer of the person's own, or "your relay server"
  std::string check;         // an internet offer: the six digits to compare
  std::string save_to;       // "~/Downloads"
  int selected = 0;
};
[[nodiscard]] ftxui::Element offer_card(const theme& th, const offer_model& m, int width);
[[nodiscard]] std::string offer_hint(const theme& th);

// The sender, `out = confirming`.
struct code_entry_model {
  std::string peer;
  std::string typed;  // digits so far
  std::string note;   // the engine's out_entry_note ("That is not the code ..."), or ""
  bool wrong = false; // `note` says a try was wrong
};
[[nodiscard]] ftxui::Element code_entry_card(const theme& th, const code_entry_model& m, int width);
[[nodiscard]] std::string code_entry_hint(const theme& th);

// /code: receive from anywhere.
struct internet_code_model {
  std::string code;      // code_text: four words or base32 groups
  std::string link;      // code_link
  double left_s = -1;    // the life left
  std::string how;       // code_how ("router mapping, IPv6")
  std::string note;      // code_note
  std::string copied;    // "Code copied" for a moment after c / l
};
[[nodiscard]] ftxui::Element internet_code_card(const theme& th, const internet_code_model& m, int width);

// The sender over the internet, `out = naming`: compare the check, then send.
struct naming_model {
  std::string peer;
  std::string check;
  std::string relay;
  int selected = 0;
};
[[nodiscard]] ftxui::Element naming_card(const theme& th, const naming_model& m, int width);

// After a transfer that offers it: remember the other computer.
struct remember_model {
  enum class step : std::uint8_t {
    ask,         // "Remember Ann's MacBook?" 1. Remember 2. Not now
    show_check,  // the sender of a nearby transfer: the digits to type over there
    type_check,  // the receiver: type the digits the sender's screen shows
  } at = step::ask;
  std::string peer;
  std::string check;  // show_check
  std::string typed;  // type_check
  std::string note;   // type_check: a wrong entry
  int selected = 0;
};
[[nodiscard]] ftxui::Element remember_card(const theme& th, const remember_model& m, int width);

// ---- the transcript ---------------------------------------------------------------------------------

struct entry_model {
  tone bullet = tone::info;  // success, error, warning (cancelled, declined), info
  std::string text;          // the engine's own sentence where it has one
  std::vector<std::string> details;  // each under an elbow, dim
};
// "⏺ Received 24 files from Ann's MacBook" / "  ⎿ 412 MB in 31 s · Wi-Fi".
[[nodiscard]] ftxui::Element entry(const theme& th, const entry_model& m, int width);

// The live region: the blocks top to bottom, a blank row between them.
[[nodiscard]] ftxui::Element region(ftxui::Elements blocks);

}  // namespace mv::driftwire::cli
