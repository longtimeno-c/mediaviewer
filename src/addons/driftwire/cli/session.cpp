// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The session: the engine's flows as cli-engine-map §3 has them, with its
// gotchas (§4): the calls that silently do nothing are checked first and the
// person is told why; `dht` is followed by a call that reconciles; a code is
// asked for again until the receiver exists; settings that the engine saves
// are only ever changed by the person's own command; nothing calls the engine
// from a callback; one thread calls it.
#include "addons/driftwire/cli/session.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

#include <ftxui/dom/elements.hpp>

#include "addons/driftwire/cli/complete.h"
#include "addons/driftwire/cli/host.h"
#include "addons/driftwire/cli/inline_view.h"
#include "addons/driftwire/cli/keys.h"
#include "addons/driftwire/cli/line_editor.h"
#include "addons/driftwire/cli/render.h"
#include "addons/driftwire/cli/term.h"
#include "addons/driftwire/cli/text.h"
#include "addons/driftwire/wire.h"
#include "core/json.h"

namespace mv::driftwire::cli {

namespace {

using out_phase = view::out_phase;
using in_phase = view::in_phase;
using code_phase = internet_receiver::phase;
using ftxui::Element;
using ftxui::Elements;

constexpr int kFrameMs = 1000 / 12;    // what moves: ~12 a second, only while it moves
constexpr int kArmMs = 2000;           // "Press Ctrl+C again", "Press esc again"
constexpr int kNoteMs = 4000;          // a passing note under the prompt
constexpr int kBannerWaitMs = 1500;    // the banner waits this long for the network to come up
constexpr int kMakeCodeMs = 5000;      // make_code is asked again until the receiver exists
constexpr int kMakeCodeEveryMs = 100;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string json_str(std::string_view s) {
  json::writer w;
  w.string(s);
  return w.take();
}

std::string lower_ascii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

std::filesystem::path to_path(std::string_view utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

bool is_dir(const std::string& p) {
  std::error_code ec;
  return std::filesystem::is_directory(to_path(p), ec);
}

std::string leaf_of(std::string_view p) {
  while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.remove_suffix(1);
  const auto at = p.find_last_of("/\\");
  return std::string(at == std::string_view::npos ? p : p.substr(at + 1));
}

std::string digits_of(std::string_view s) {
  std::string d;
  for (const char c : s) {
    if (c >= '0' && c <= '9') d += c;
  }
  return d;
}

std::string plural(std::size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

// The path a transfer takes, for the picture: its bearer and its badge.
bearer bearer_of(const std::string& via, bool punched, const std::string& relay) {
  if (!relay.empty()) return bearer::relay;
  if (via == "the internet") return punched ? bearer::dht : bearer::internet;
  if (via == "Bluetooth") return bearer::bluetooth;
  return bearer::wifi;
}

std::string badge_of(const std::string& relay) {
  if (relay.empty()) return {};
  return relay == "your relay server" ? std::string("relay server") : "via " + relay;
}

// The bearer as the transcript says it: "Wi-Fi", "internet, met on the DHT",
// "through Studio Mac".
std::string via_words(const std::string& via, bool punched, const std::string& relay) {
  if (relay == "your relay server") return "through your relay server";
  if (!relay.empty()) return "through " + relay;
  if (via == "the internet") return punched ? "internet, met on the DHT" : "internet";
  return via;
}

// How an ended transfer's sentence is marked: a decline or a cancel is a
// warning, anything else that did not go through an error.
tone ended_tone(const std::string& note) {
  if (contains(note, "declined") || contains(note, "Declined") || contains(note, "ancelled")) return tone::warning;
  return tone::error;
}

std::string base64(std::string_view s) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < s.size(); i += 3) {
    const auto n = (static_cast<unsigned char>(s[i]) << 16) | (static_cast<unsigned char>(s[i + 1]) << 8) |
                   static_cast<unsigned char>(s[i + 2]);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }
  if (i + 1 == s.size()) {
    const auto n = static_cast<unsigned char>(s[i]) << 16;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += "==";
  } else if (i + 2 == s.size()) {
    const auto n = (static_cast<unsigned char>(s[i]) << 16) | (static_cast<unsigned char>(s[i + 1]) << 8);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

// The window's "what this computer cannot search" (tree.cpp reach_note); the
// command line has no Bluetooth bearer, so its note is "not used".
std::string reach_note(const view& v) {
  if (v.bluetooth_note == "Offline.") return "Offline: nothing nearby can be found.";
  if (v.wifi_up) return {};
  return "Not on a network, so nothing nearby can be found.";
}

// What each internet option means, the window's own words (tree.cpp), said
// the first time it is turned on in a session.
std::string meaning_of(std::string_view option) {
  if (option == "internet") {
    return "No server: the two computers connect straight to each other with a code that works once. "
           "Whoever has the code learns this computer\xE2\x80\x99s address.";
  }
  if (option == "dht") {
    return "For networks with no public address: a code is four words and both computers meet on the BitTorrent "
           "DHT while it waits. DHT nodes see that this computer takes part, never what is sent.";
  }
  if (option == "handover") {
    return "A nearby transfer whose Wi-Fi goes carries on over the internet. Both computers need this on.";
  }
  if (option == "relay") {
    return "When nothing answers directly, a send goes through another computer of yours that remembers this one "
           "(marked as yours with /mine, on both). It stays sealed end to end: the computer in between cannot read "
           "it.";
  }
  return {};
}

bool out_busy(const view& v) {
  return v.out == out_phase::waiting_wifi || v.out == out_phase::connecting || v.out == out_phase::confirming ||
         v.out == out_phase::waiting || v.out == out_phase::sending || v.out == out_phase::reaching ||
         v.out == out_phase::naming;
}

bool in_busy(const view& v) {
  return v.in == in_phase::connecting || v.in == in_phase::asking || v.in == in_phase::receiving;
}

bool out_waits(out_phase p) {
  return p == out_phase::waiting_wifi || p == out_phase::connecting || p == out_phase::waiting ||
         p == out_phase::reaching;
}

const char* out_name(out_phase p) {
  switch (p) {
    case out_phase::idle: return "idle";
    case out_phase::waiting_wifi: return "waiting_wifi";
    case out_phase::connecting: return "connecting";
    case out_phase::confirming: return "confirming";
    case out_phase::waiting: return "waiting";
    case out_phase::sending: return "sending";
    case out_phase::done: return "done";
    case out_phase::ended: return "ended";
    case out_phase::reaching: return "reaching";
    case out_phase::naming: return "naming";
  }
  return "?";
}

const char* in_name(in_phase p) {
  switch (p) {
    case in_phase::none: return "none";
    case in_phase::connecting: return "connecting";
    case in_phase::asking: return "asking";
    case in_phase::receiving: return "receiving";
    case in_phase::done: return "done";
    case in_phase::ended: return "ended";
  }
  return "?";
}

bool yes_word(std::string_view s) {
  const std::string l = lower_ascii(trim(s));
  return l == "y" || l == "yes";
}

}  // namespace

// ---- the controller ----------------------------------------------------------------------------------

struct session::impl {
  impl(terminal& t, cli_host& h, engine::options eo, session_options so, const theme& tt)
      : term(t), host(h), eopts(std::move(eo)), o(std::move(so)), th(tt), live(t, 80, 24) {}

  terminal& term;
  cli_host& host;
  engine::options eopts;
  session_options o;
  const theme& th;
  std::unique_ptr<engine> eng;

  view v;  // the last snapshot
  std::int64_t now = 0, started = 0;
  bool quitting = false;
  int width = 80, height = 24;

  // ---- the rich presenter
  inline_view live;
  key_decoder keys;
  std::int64_t bytes_at = 0;  // the last input, for the decoder's wait
  line_editor editor;  // the prompt's line
  std::string palette_closed_for;  // Esc closed the palette for this text
  int palette_sel = 0;
  std::vector<std::string> tab_choices;  // Tab's matches, under the prompt until the next key

  // ---- what this person is doing
  std::vector<std::string> files;  // the set, as added (the engine has them, plus pair halves and sidecars)
  std::string folder;              // or one folder
  std::string sel_id;              // Nearby's pointer: a peer's id, or "dev<fingerprint>"
  int sel_index = 0;
  std::string to_name;             // send --to: until heard
  bool sent_to_name = false;

  // A passing line under the prompt, and the two-press keys.
  struct passing {
    std::string text;
    tone t = tone::warning;
    std::int64_t until = 0;
  } note;
  enum class armed_key : std::uint8_t { none, ctrl_c, esc } armed = armed_key::none;
  std::int64_t armed_until = 0;

  // A modal field: a code, a folder, the relay server.
  enum class field_kind : std::uint8_t { none, to_code, save_elsewhere, relay_server } field = field_kind::none;
  line_editor field_ed;
  std::string field_note;
  std::vector<std::string> field_choices;

  // A question with numbered answers ("Forget Ann's MacBook?").
  struct question {
    std::string title;
    std::vector<std::string> body;
    std::vector<std::string> items;
    int selected = 0;
    std::string plain;                      // plain mode's line: "Forget Ann's MacBook? [y/N] "
    bool yes_no = true;                     // item 0 is yes, the rest no (plain: y / n)
    std::function<void(int)> answer;        // the item, or -1 for no / esc
  };
  std::optional<question> asked;

  // Choices on the cards.
  int offer_sel = 0, naming_sel = 0;
  // Which offer, check and Remember question this is (each new one counts
  // up): a key answers only the one drawn.
  std::uint64_t offer_serial = 0, naming_serial = 0, rem_serial = 0;
  std::string code_typed;      // the sender's code entry
  bool code_submitted = false; // six digits went: the engine answers
  std::string code_note_seen;  // out_entry_note when they went
  int code_tries_seen = kCodeTries;

  struct remember_ui {
    enum class step : std::uint8_t { none, ask, show_check, type_check } at = step::none;
    std::string peer;
    int selected = 0;
    std::string typed;
    bool submitted = false;
    std::string note_seen;
  } rem;

  // The episode under way, for the transcript.
  std::int64_t out_since = 0, in_since = 0, out_moving_since = -1, in_moving_since = -1;
  std::int64_t path_clock = 0, path_clock_at = 0;  // the dots' clock: still while nothing moves
  bool in_answered = false;    // the person answered this offer (else 180 s declined it)
  bool cancelled_here = false; // this person cancelled (for the transcript's tone)

  // The internet code (receive from anywhere).
  std::int64_t make_code_until = 0;  // asking for a code until the receiver exists
  std::int64_t code_ready_at = -1;
  bool code_stopped_here = false;

  // Once only.
  bool banner_done = false;
  bool unfinished_said = false;
  std::vector<std::string> meanings_said;

  // One-shots (send, receive --once, code): the transfer's result, and when to close.
  bool one_shot = false;
  bool shot_started = false;  // the send (or receive, or code) under way
  bool shot_ok = false;
  bool shot_over = false;     // close once nothing more is asked (Remember)

  bool destination_chosen = false;   // /save-to or "save somewhere else": kept on purpose

  // ---- plain mode
  std::string plain_key;     // what the last question / status was about
  std::string plain_prompt;  // what the current line starts with
  line_editor plain_line;
  bool prompt_on_line = false;
  std::string plain_nearby;  // the last Nearby line said

  // =========================================================================================
  // engine calls: all here, on this thread

  void ev(const std::string& node, std::uint32_t kind, const std::string& value) {
    if (eng) eng->on_event(node, kind, value);
  }

  void refresh_view() {
    if (!eng) return;
    view next = eng->snapshot();
    on_view(next);
  }

  // =========================================================================================
  // output

  int w() const { return width; }

  void event(const std::string& line) {
    if (o.events) o.events(line);
  }

  // " · " and "…" (or their ASCII), between and after words.
  std::string glue() const { return " " + std::string(th.g->sep) + " "; }
  std::string dots() const { return std::string(th.g->ellipsis); }

  // Plain mode's lines, each as written (a "\n" in one starts another), and
  // never a control character: plain mode writes no escape sequence at all.
  void plain_write_lines(const std::vector<std::string>& ls) {
    std::string s;
    if (prompt_on_line) s += "\r\n";
    for (const auto& l : ls) {
      for (std::size_t at = 0;;) {
        const std::size_t nl = l.find('\n', at);
        s += printable(std::string_view(l).substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        s += "\r\n";
        if (nl == std::string::npos) break;
        at = nl + 1;
      }
    }
    term.write(s);
    prompt_on_line = false;
  }

  void say(const entry_model& m) {
    if (o.plain) {
      std::vector<std::string> ls{m.text};
      for (const auto& d : m.details) ls.push_back("  " + d);
      plain_write_lines(ls);
      return;
    }
    live.print(entry(th, m, w()));
  }

  void say(tone t, std::string text, std::vector<std::string> details = {}) {
    say(entry_model{t, std::move(text), std::move(details)});
  }

  // A dim line in the transcript: the line typed, a log line. Cut into
  // rows of the terminal's width as they come (a path has no spaces to wrap at).
  void say_dim(const std::string& s) {
    if (o.plain) {
      plain_write_lines({s});
      return;
    }
    Elements rows;
    std::string row;
    int cols = 0;
    for (const glyph_span& g : glyph_spans(s)) {
      if (cols + g.width > w() && !row.empty()) {
        rows.push_back(paint(th, ftxui::text(std::move(row)), tone::dim));
        row.clear();
        cols = 0;
      }
      row.append(s, g.at, g.len);
      cols += g.width;
    }
    if (!row.empty() || rows.empty()) rows.push_back(paint(th, ftxui::text(std::move(row)), tone::dim));
    live.print(ftxui::vbox(std::move(rows)));
  }

  void flash(std::string text, tone t = tone::warning, int ms = kNoteMs) {
    if (o.plain) {
      plain_write_lines({text});
      return;
    }
    note = {std::move(text), t, now + ms};
  }

  std::string home_short_of(const std::string& p) const { return home_short(p, o.paths.home, o.paths.windows); }

  // =========================================================================================
  // the view: what changed, into the transcript

  void on_view(view next) {
    const view prev = std::move(v);
    v = std::move(next);

    // The banner, once the network had a moment to come up.
    if (!banner_done && (v.wifi_up || now - started >= kBannerWaitMs)) print_banner();

    for (const auto& l : host.take_log()) {
      if (o.verbose) say_dim("log: " + l);
    }

    // #530: receives that stopped, once.
    if (!unfinished_said && !v.unfinished.empty()) {
      unfinished_said = true;
      say(tone::info, plural(v.unfinished.size(), "receive", "receives") + " stopped part way" + glue() + "/unfinished",
          {"Each picks up where it stopped when its sender sends again."});
    }

    on_out(prev);
    on_in(prev);
    on_code(prev);
    on_remember(prev);
    keep_selection(prev);
    try_send_to_name();
    if (field == field_kind::save_elsewhere && v.in != in_phase::asking) close_field();

    if (o.plain) plain_nearby_update();
  }

  void print_banner() {
    banner_done = true;
    banner_model b;
    b.version = o.version;
    b.name = v.my_name;
    b.presence = v.bluetooth_note == "Offline." ? "offline" : v.wifi_up ? "visible on Wi-Fi" : "not on a network";
    b.internet = v.internet ? "internet on" : "internet off";
    b.saves_to = home_short_of(v.destination);
    if (o.plain) {
      plain_write_lines({"Driftwire " + b.version + glue() + b.name + glue() + b.presence + glue() + b.internet,
                         "Saves to " + b.saves_to + glue() + "/help for commands"});
    } else {
      live.print(banner(th, b, w()));
    }
    event("ready");
  }

  void on_out(const view& prev) {
    const out_phase was = prev.out, is = v.out;
    if (is == out_phase::naming && (was != is || v.out_check != prev.out_check)) ++naming_serial;
    if (was == is && was != out_phase::idle) {
      if (is == out_phase::confirming && code_submitted &&
          (v.out_entry_note != code_note_seen || v.out_tries_left != code_tries_seen)) {
        code_submitted = false;  // the engine answered the six digits
        code_typed.clear();
      }
      if (is == out_phase::sending && out_moving_since < 0) out_moving_since = now;
      return;
    }
    if (was == is) return;
    event(std::string("out ") + out_name(is) + (is == out_phase::naming ? " " + v.out_check : std::string()));
    if (was == out_phase::idle || was == out_phase::done || was == out_phase::ended) {
      // A send begins.
      out_since = now;
      out_moving_since = -1;
      cancelled_here = false;
      if (one_shot && o.cmd == command::send && !shot_over) shot_started = true;
    }
    if (is == out_phase::confirming) {
      code_typed.clear();
      code_submitted = false;
    }
    if (is == out_phase::naming) naming_sel = 0;
    if (is == out_phase::sending && out_moving_since < 0) out_moving_since = now;
    if (is == out_phase::done || is == out_phase::ended) {
      std::vector<std::string> details;
      if (is == out_phase::done) {
        std::string d = size_text(v.out_total);
        if (out_moving_since >= 0) d += " in " + elapsed_text(static_cast<double>(now - out_moving_since) / 1000.0);
        if (const std::string via = via_words(v.out_via, v.out_punched, v.out_relay); !via.empty()) d += glue() + via;
        details.push_back(std::move(d));
      }
      say(is == out_phase::done ? tone::success : cancelled_here ? tone::warning : ended_tone(v.out_note),
          v.out_note.empty() ? std::string("The send ended.") : v.out_note, std::move(details));
      // In a terminal the transcript is the OK.
      ev("out_dismiss", 1, "");
      if (one_shot && o.cmd == command::send && shot_started && !shot_over) {
        shot_ok = is == out_phase::done;
        shot_over = true;
      }
      out_moving_since = -1;
    }
  }

  void on_in(const view& prev) {
    const in_phase was = prev.in, is = v.in;
    // Another offer in place of the one drawn (seen only after the fact,
    // when this thread was busy) is another card.
    if (is == in_phase::asking && (was != is || v.in_code != prev.in_code || v.in_peer != prev.in_peer)) {
      ++offer_serial;
      offer_sel = 0;
    }
    if (was == is) {
      if (is == in_phase::receiving && in_moving_since < 0) in_moving_since = now;
      return;
    }
    event(std::string("in ") + in_name(is) +
          (is == in_phase::connecting && !v.in_known ? " " + v.in_code : std::string()));
    if (was == in_phase::none || was == in_phase::done || was == in_phase::ended) {
      in_since = now;
      in_moving_since = -1;
      in_answered = false;
      cancelled_here = false;
    }
    // A one-shot's transfer is one that offered something: anyone nearby can
    // knock and leave (connecting, then ended) without ending someone else's
    // `receive --once`. For `code`, the connection its code let in is the one
    // (the code works once).
    if (one_shot && (o.cmd == command::receive || o.cmd == command::code) && !shot_over && !shot_started &&
        (is == in_phase::asking || is == in_phase::receiving || is == in_phase::done ||
         (o.cmd == command::code && is == in_phase::connecting && v.in_internet && !v.in_known))) {
      shot_started = true;
    }
    if (is == in_phase::receiving && in_moving_since < 0) in_moving_since = now;
    if (is == in_phase::done || is == in_phase::ended) {
      std::vector<std::string> details;
      std::string text = v.in_note;
      if (is == in_phase::done) {
        // "Received 24 files from Ann's MacBook into /home/...": the folder
        // goes on the line under it, shortened.
        if (const std::string into = " into " + v.destination; !v.destination.empty()) {
          if (const auto at = text.find(into); at != std::string::npos) text.erase(at, into.size());
        }
        std::string d = size_text(v.in_total);
        if (in_moving_since >= 0) d += " in " + elapsed_text(static_cast<double>(now - in_moving_since) / 1000.0);
        if (const std::string via = via_words(v.in_via, v.in_punched, v.in_relay); !via.empty()) d += glue() + via;
        d += glue() + "every file checked" + glue() + home_short_of(v.destination);
        details.push_back(std::move(d));
      } else if (prev.in == in_phase::asking && !in_answered && contains(text, "Declined")) {
        details.emplace_back("Nobody answered for 3 minutes, so it was declined.");
      }
      if (text.empty()) text = "The receive ended.";
      say(is == in_phase::done ? tone::success : cancelled_here ? tone::warning : ended_tone(v.in_note), text,
          std::move(details));
      ev("in_dismiss", 1, "");
      if (one_shot && (o.cmd == command::receive || o.cmd == command::code) && shot_started && !shot_over) {
        shot_ok = is == in_phase::done;
        shot_over = true;
      }
      in_moving_since = -1;
    }
  }

  void on_code(const view& prev) {
    const auto was = static_cast<code_phase>(prev.code_phase), is = static_cast<code_phase>(v.code_phase);
    if (is != code_phase::none) make_code_until = 0;  // the receiver is there
    if (was == is) return;
    if (is == code_phase::ready) {
      code_ready_at = now;
      event("code " + v.code_text);
      if (o.plain) {
        std::vector<std::string> ls{"Receive from anywhere: give this code to the person sending. It works once, for " +
                                    std::to_string(eopts.code_lifetime_ms / 60000) + " minutes.",
                                    "  " + v.code_text};
        if (!v.code_link.empty()) ls.push_back("  Link: " + v.code_link);
        if (!v.code_how.empty()) ls.push_back("  Ways in: " + v.code_how + ".");
        plain_write_lines(ls);
      }
      return;
    }
    if (was == code_phase::ready || was == code_phase::preparing) {
      if (is == code_phase::used) {
        const bool wrong = contains(v.code_note, "wrong");
        say(wrong ? tone::warning : tone::info, v.code_note);
        if (wrong && one_shot && o.cmd == command::code && !shot_over && v.in == in_phase::none) {
          shot_ok = false;
          shot_over = true;
        }
      } else if (is == code_phase::expired || is == code_phase::failed) {
        say(is == code_phase::failed ? tone::error : tone::warning,
            v.code_note.empty() ? std::string("The code no longer works.") : v.code_note);
        if (one_shot && o.cmd == command::code && !shot_over && v.in == in_phase::none) {
          shot_ok = false;
          shot_over = true;
        }
      } else if (is == code_phase::none && code_stopped_here) {
        say(tone::info, "The code was stopped. It no longer works.");
        if (one_shot && o.cmd == command::code && !shot_over && v.in == in_phase::none) {
          shot_ok = false;
          shot_over = true;
        }
      }
      code_stopped_here = false;
    }
  }

  void on_remember(const view& prev) {
    // Offered after a transfer that went through; not asked again for a
    // device already remembered.
    if (v.remember_offer && !prev.remember_offer && v.can_remember && !v.remembered) {
      rem = {};
      rem.at = remember_ui::step::ask;
      rem.peer = v.remember_peer;
      ++rem_serial;
    }
    if (rem.at == remember_ui::step::type_check && rem.submitted && v.remember_check_note != rem.note_seen) {
      rem.submitted = false;
      rem.typed.clear();
    }
    if (v.remembered && !prev.remembered && rem.at != remember_ui::step::none) {
      say(tone::success, "Remembered " + rem.peer + ".",
          {"Send to each other later over the internet, with no code, once " + rem.peer +
           " remembers this computer too."});
      if (rem.at != remember_ui::step::show_check) rem = {};
    }
    // The offer goes with the next transfer, and with it the sender's shown
    // code (the engine clears it): the card goes too, Esc or not.
    if (!v.remember_offer && prev.remember_offer && rem.at != remember_ui::step::none) {
      if (rem.at == remember_ui::step::type_check && !v.remembered) {
        say(tone::warning, rem.peer + " is not remembered: the code was wrong 3 times.");
      }
      rem = {};
    }
  }

  // Nearby's pointer stays on the same computer as the list changes; if it
  // leaves, on the row nearest where it was.
  struct row_ref {
    std::string id;    // a peer's id, or "dev<fingerprint>"
    std::string name;
    bool known = false;
  };

  bool known_shown() const { return v.internet && v.dht && !v.known.empty(); }

  std::vector<row_ref> computers() const {
    std::vector<row_ref> out;
    for (const auto& p : v.peers) out.push_back({p.id, p.name, false});
    if (known_shown()) {
      for (std::size_t i = 0; i < v.known.size(); ++i) {
        out.push_back({"dev" + (i < v.known_ids.size() ? v.known_ids[i] : std::string()), v.known[i], true});
      }
    }
    return out;
  }

  void keep_selection(const view&) {
    const auto r = computers();
    if (r.empty()) {
      sel_id.clear();
      sel_index = 0;
      return;
    }
    for (std::size_t i = 0; i < r.size(); ++i) {
      if (r[i].id == sel_id) {
        sel_index = static_cast<int>(i);
        return;
      }
    }
    sel_index = std::clamp(sel_index, 0, static_cast<int>(r.size()) - 1);
    sel_id = r[static_cast<std::size_t>(sel_index)].id;
  }

  void move_selection(int by) {
    const auto r = computers();
    if (r.empty()) return;
    sel_index = std::clamp(sel_index + by, 0, static_cast<int>(r.size()) - 1);
    sel_id = r[static_cast<std::size_t>(sel_index)].id;
  }

  // =========================================================================================
  // sending

  bool files_ready() const { return v.file_count > 0 && !v.reading_folder; }

  // The checks start_send makes without a word (cli-engine-map §4), said.
  std::string why_not_send(const row_ref* to) const {
    if (out_busy(v) || in_busy(v)) return "A transfer is under way. Send again when it ends.";
    if (v.reading_folder) return "Still reading the files" + std::string(th.g->ellipsis);
    if (v.file_count == 0) return "Pick files first: type or drop them at the prompt.";
    if (!to) return "No one nearby yet: open Driftwire on the other computer, on this network.";
    if (to->known && (!v.internet || !v.dht)) return "Turn on /internet and /dht to send to a remembered device.";
    return {};
  }

  void send_to_row(const row_ref& to) {
    if (const std::string why = why_not_send(&to); !why.empty()) {
      flash(why);
      return;
    }
    if (to.known) {
      ev("known", 5, json_str(to.id));
    } else {
      ev("peers", 5, json_str(to.id));
    }
    refresh_view();
    if (v.out == out_phase::idle) {
      flash(!v.send_code_note.empty() ? v.send_code_note
                                      : to.name + " is not there any more. Pick another computer, or wait for it.");
    }
  }

  void send_selected() {
    const auto r = computers();
    if (r.empty() || sel_index < 0 || sel_index >= static_cast<int>(r.size())) {
      flash(why_not_send(nullptr));
      return;
    }
    if (o.plain && r.size() > 1) {
      ask_which_computer(r);
      return;
    }
    send_to_row(r[static_cast<std::size_t>(sel_index)]);
  }

  // send --to NAME: the first peer of that name (any case), else the one
  // whose name it begins, once heard and the files are read.
  void try_send_to_name() {
    if (to_name.empty() || sent_to_name || !files_ready() || out_busy(v) || in_busy(v)) return;
    const std::string want = lower_ascii(to_name);
    const view::peer_row* hit = nullptr;
    for (const auto& p : v.peers) {
      if (lower_ascii(p.name) == want) {
        hit = &p;
        break;
      }
    }
    if (!hit) {
      int n = 0;
      for (const auto& p : v.peers) {
        if (lower_ascii(p.name).rfind(want, 0) == 0) {
          hit = &p;
          ++n;
        }
      }
      if (n != 1) hit = nullptr;
    }
    if (!hit) return;
    sent_to_name = true;
    sel_id = hit->id;
    const row_ref to{hit->id, hit->name, false};
    send_to_row(to);
  }

  // Paths typed, pasted or dropped: added to the set.
  void add_paths(std::string_view line) {
    const parsed_paths r = parse_paths(line, o.paths);
    if (!r.missing.empty()) {
      std::string names;
      for (const auto& m : r.missing) names += (names.empty() ? "" : ", ") + m;
      say(tone::error, "Not found: " + names);
    }
    if (r.paths.empty()) return;
    add_existing(r.paths);
  }

  void add_existing(const std::vector<std::string>& paths) {
    if (out_busy(v)) {
      flash("A send is under way: add files when it ends.");
      return;
    }
    std::vector<std::string> dirs, plain_files;
    for (const auto& p : paths) (is_dir(p) ? dirs : plain_files).push_back(p);
    const std::string either = "A send is either files or one folder. /clear first.";
    if (!dirs.empty()) {
      if (files.empty() && folder.empty() && dirs.size() == 1 && plain_files.empty()) {
        folder = dirs[0];
        ev("send_folder", 3, json_str(folder));
        return;
      }
      say(tone::warning, either);
    }
    if (plain_files.empty()) return;
    if (!folder.empty()) {
      if (dirs.empty()) say(tone::warning, either);
      return;
    }
    for (const auto& p : plain_files) {
      if (std::find(files.begin(), files.end(), p) == files.end()) files.push_back(p);
    }
    // set_files replaces the set: the whole list each time.
    if (eng) eng->set_files(files);
  }

  void clear_files() {
    if (out_busy(v)) {
      flash("A send is under way: it keeps its files until it ends.");
      return;
    }
    files.clear();
    folder.clear();
    ev("clear", 1, "");
  }

  std::string files_summary() const {
    if (v.reading_folder) return "Reading the files" + std::string(th.g->ellipsis);
    if (v.file_count == 0) return {};
    if (!folder.empty()) return "the folder \"" + leaf_of(folder) + "\"" + glue() + size_text(v.file_bytes);
    return plural(v.file_count, "file", "files") + glue() + size_text(v.file_bytes);
  }

  // =========================================================================================
  // the internet

  // Turns on Over the internet for good after a yes, saying what it means.
  void ask_internet(std::string why, std::function<void()> then) {
    question q;
    q.title = std::move(why);
    q.body = {meaning_of("internet"), "It stays on for next time; /internet off turns it off."};
    q.items = {"Turn it on", "Not now"};
    q.plain = q.title + " Turn on Over the internet? [y/N] ";
    q.answer = [this, then = std::move(then)](int i) {
      if (i != 0) {
        if (one_shot && o.cmd == command::code && !shot_over) {
          shot_ok = false;
          shot_over = true;
        }
        return;
      }
      set_option("internet", true, false);
      refresh_view();  // the toggle is in the view at once; what follows reads it
      if (then) then();
    };
    asked = std::move(q);
  }

  void make_code() {
    if (!v.internet) {
      ask_internet("Receiving from anywhere needs Over the internet.", [this] { make_code(); });
      return;
    }
    if (static_cast<code_phase>(v.code_phase) == code_phase::ready ||
        static_cast<code_phase>(v.code_phase) == code_phase::preparing) {
      return;  // one is there (or coming): the card shows it
    }
    ev("make_code", 1, "");
    // The receiver is made on the engine's control thread after the toggle:
    // until it exists make_code does nothing, so it is asked again.
    make_code_until = now + kMakeCodeMs;
  }

  void retry_make_code() {
    if (make_code_until == 0) return;
    if (v.code_phase != 0) {
      make_code_until = 0;
      return;
    }
    if (now >= make_code_until) {
      make_code_until = 0;
      say(tone::error, "Could not make a code: the internet part of Driftwire did not start.");
      if (one_shot && o.cmd == command::code && !shot_over) {
        shot_ok = false;
        shot_over = true;
      }
      return;
    }
    ev("make_code", 1, "");
  }

  void stop_code() {
    code_stopped_here = true;
    ev("stop_code", 1, "");
  }

  void open_to_code() {
    if (const std::string why = why_not_code(); !why.empty()) {
      flash(why);
      return;
    }
    if (!v.internet) {
      ask_internet("Sending to a code needs Over the internet.", [this] { open_field(field_kind::to_code); });
      return;
    }
    open_field(field_kind::to_code);
  }

  std::string why_not_code() const {
    if (out_busy(v) || in_busy(v)) return "A transfer is under way. Send again when it ends.";
    if (v.reading_folder) return "Still reading the files" + std::string(th.g->ellipsis);
    if (v.file_count == 0) return "Pick files first: type or drop them at the prompt, then /to-code.";
    return {};
  }

  // A toggle the engine keeps (its prefs): only ever from the person's command.
  void set_option(const std::string& option, bool on, bool tell) {
    const bool was = option == "internet" ? v.internet
                     : option == "dht"    ? v.dht
                     : option == "handover" ? v.handover
                                            : v.relay;
    ev(option, 2, on ? "true" : "false");
    // `dht` does not reconcile by itself (cli-engine-map §4): the remembered
    // devices' wait starts at the next reconcile. Opening the window again
    // (it is open) reconciles and changes nothing else (its name stays).
    if (option == "dht" && eng) eng->screen_opened("");
    if (tell) {
      say(tone::info, std::string(on ? "On: " : "Off: ") + option_label(option) + (was == on ? " (it already was)" : ""));
    }
    if (on && !was && std::find(meanings_said.begin(), meanings_said.end(), option) == meanings_said.end()) {
      meanings_said.push_back(option);
      say(tone::info, option_label(option), {meaning_of(option)});
    }
    if (on && option != "internet" && !v.internet) flash("It takes effect with /internet on.", tone::info);
  }

  static std::string option_label(const std::string& option) {
    if (option == "internet") return "Send and receive over the internet";
    if (option == "dht") return "Find each other through the public DHT";
    if (option == "handover") return "Continue over the internet if Wi-Fi drops";
    return "Use my other computers to relay";
  }

  // =========================================================================================
  // questions and fields

  void ask_which_computer(const std::vector<row_ref>& r) {
    question q;
    q.title = "Send " + files_summary() + " to which computer?";
    for (const auto& x : r) q.items.push_back(x.name + (x.known ? " (over the internet)" : ""));
    q.plain = q.title + " [1-" + std::to_string(r.size()) + ", Enter alone to cancel] ";
    q.yes_no = false;
    q.answer = [this, r](int i) {
      if (i < 0 || i >= static_cast<int>(r.size())) return;
      sel_id = r[static_cast<std::size_t>(i)].id;
      sel_index = i;
      send_to_row(r[static_cast<std::size_t>(i)]);
    };
    asked = std::move(q);
  }

  void open_field(field_kind k) {
    field = k;
    field_ed.clear();
    field_ed.set_filter(nullptr);
    field_ed.set_paste_quote(0);
    field_note.clear();
    field_choices.clear();
    if (k == field_kind::save_elsewhere) {
      std::string start = home_short_of(v.destination);
      if (!start.empty() && start.back() != '/' && start.back() != '\\') start += o.paths.windows ? "\\" : "/";
      field_ed.set(start);
    }
  }

  void close_field() {
    field = field_kind::none;
    field_ed.clear();
    field_note.clear();
    field_choices.clear();
  }

  // A folder typed in a field: one existing folder, absolute.
  std::optional<std::string> folder_from(std::string_view text, std::string& why) const {
    const parsed_paths r = parse_paths(text, o.paths);
    if (r.paths.size() + r.missing.size() != 1) {
      why = "Type one folder.";
      return std::nullopt;
    }
    if (!r.missing.empty()) {
      why = "That folder is not there.";
      return std::nullopt;
    }
    if (!is_dir(r.paths[0])) {
      why = "That is a file, not a folder.";
      return std::nullopt;
    }
    return r.paths[0];
  }

  void submit_field() {
    const std::string text(trim(field_ed.text()));
    switch (field) {
      case field_kind::to_code: {
        if (text.empty()) return;
        ev("send_code", 3, json_str(text));
        refresh_view();
        if (v.out == out_phase::idle) {
          field_note = !v.send_code_note.empty() ? v.send_code_note : why_not_code();
          if (field_note.empty()) field_note = "That code did not start a send.";
          return;
        }
        close_field();
        break;
      }
      case field_kind::save_elsewhere: {
        std::string why;
        const auto dir = folder_from(text, why);
        if (!dir) {
          field_note = why;
          return;
        }
        ev("destination", 3, json_str(*dir));
        refresh_view();
        if (v.destination != *dir) {
          field_note = "That folder cannot be used.";
          return;
        }
        destination_chosen = true;
        close_field();
        in_answered = true;
        ev("accept", 1, "");
        break;
      }
      case field_kind::relay_server: {
        ev("relay_server", 3, json_str(text));
        close_field();
        // Never echoed: it holds the server's key.
        say(tone::info, text.empty() ? "No relay server: sends and codes go straight, or through your computers."
                                     : "Relay server set. Codes made from now on use it.");
        break;
      }
      case field_kind::none: break;
    }
  }

  // =========================================================================================
  // the cards' answers

  void answer_offer(int i) {
    if (v.in != in_phase::asking) return;
    if (i == 0) {
      in_answered = true;
      ev("accept", 1, "");
    } else if (i == 1) {
      open_field(field_kind::save_elsewhere);
    } else {
      in_answered = true;
      cancelled_here = true;
      ev("decline", 1, "");
    }
  }

  void answer_naming(bool send) {
    if (v.out != out_phase::naming) return;
    if (send) {
      ev("out_confirm", 1, "");
    } else {
      cancelled_here = true;
      ev("out_cancel", 1, "");
    }
  }

  void submit_code_entry() {
    if (code_typed.size() != 6 || code_submitted) return;
    code_submitted = true;
    code_note_seen = v.out_entry_note;
    code_tries_seen = v.out_tries_left;
    // One submit at the sixth digit: every change with six counts as a try.
    ev("out_code_entry", 3, json_str(code_typed.substr(0, 3) + " " + code_typed.substr(3)));
  }

  void answer_remember(bool yes) {
    if (rem.at != remember_ui::step::ask) return;
    if (!yes || !v.remember_offer) {
      rem = {};
      return;
    }
    if (v.remember_needs_check) {
      rem.at = remember_ui::step::type_check;  // #497: the sender's code first
      return;
    }
    // Nearby, the sender shows the digits for the receiver to type (#497);
    // over the internet both sides are proved already.
    const bool nearby = !v.remember_check.empty();
    rem.at = nearby ? remember_ui::step::show_check : remember_ui::step::ask;  // on_remember says it
    ev("remember", 2, "true");
    refresh_view();
    if (!nearby) {
      if (!v.remembered) say(tone::warning, "Could not remember " + rem.peer + ".");
      rem = {};
    }
  }

  void submit_remember_check() {
    if (rem.typed.size() != 6 || rem.submitted) return;
    rem.submitted = true;
    rem.note_seen = v.remember_check_note;
    ev("remember_check_entry", 3, json_str(rem.typed));
  }

  // =========================================================================================
  // commands

  std::optional<std::size_t> device_number(std::string_view s, std::size_t count) {
    const std::string d(trim(s));
    if (d.empty() || d.find_first_not_of("0123456789") != std::string::npos || d.size() > 4) return std::nullopt;
    const std::size_t n = static_cast<std::size_t>(std::stoul(d));
    if (n < 1 || n > count) return std::nullopt;
    return n - 1;
  }

  std::optional<bool> on_off(std::string_view s) {
    const std::string l = lower_ascii(trim(s));
    if (l == "on" || l == "yes" || l == "true") return true;
    if (l == "off" || l == "no" || l == "false") return false;
    return std::nullopt;
  }

  void run_command(const command_info& c, std::string_view rest) {
    const std::string_view name = c.name;
    rest = trim(rest);
    if (name == "/send") {
      if (rest.empty()) {
        flash("/send takes the files to send: /send PATH...");
        return;
      }
      add_paths(rest);
    } else if (name == "/folder") {
      std::string why;
      const auto dir = folder_from(rest, why);
      if (!dir) {
        flash(rest.empty() ? std::string("/folder takes a folder: /folder DIR") : why);
        return;
      }
      if (out_busy(v)) {
        flash("A send is under way: pick a folder when it ends.");
        return;
      }
      files.clear();
      folder = *dir;
      ev("send_folder", 3, json_str(folder));
    } else if (name == "/files") {
      list_files();
    } else if (name == "/clear") {
      clear_files();
    } else if (name == "/code") {
      make_code();
    } else if (name == "/copy") {
      copy_code(rest);
    } else if (name == "/to-code") {
      open_to_code();
    } else if (name == "/devices") {
      list_devices();
    } else if (name == "/forget") {
      const auto n = device_number(rest, v.known.size());
      if (!n) {
        flash(v.known.empty() ? std::string("No remembered devices.") : "/forget takes a device's number (/devices).");
        return;
      }
      const std::string peer = v.known[*n];
      const std::string fp = *n < v.known_ids.size() ? v.known_ids[*n] : std::string();
      question q;
      q.title = "Forget " + peer + "?";
      q.body = {"Sending to each other over the internet with no code stops. Send once nearby or with a code to "
                "remember it again."};
      q.items = {"Forget " + peer, "Keep it"};
      q.selected = 1;
      q.plain = "Forget " + peer + "? [y/N] ";
      q.answer = [this, peer, fp](int i) {
        if (i != 0 || fp.empty()) return;
        ev("forget" + fp, 1, "");
        say(tone::info, "Forgot " + peer + ".");
      };
      asked = std::move(q);
    } else if (name == "/mine") {
      const auto space = rest.find(' ');
      const auto n = device_number(rest.substr(0, space), v.known.size());
      const auto on = space == std::string_view::npos ? std::nullopt : on_off(rest.substr(space + 1));
      if (!n || !on) {
        flash("/mine takes a device's number and on or off: /mine 1 on");
        return;
      }
      const std::string fp = *n < v.known_ids.size() ? v.known_ids[*n] : std::string();
      if (fp.empty()) return;
      ev("own" + fp, 2, *on ? "true" : "false");
      say(tone::info, *on ? v.known[*n] + " is marked as your computer: it may relay for you, and you for it."
                          : v.known[*n] + " is no longer marked as your computer.",
          v.relay ? std::vector<std::string>{} : std::vector<std::string>{"It takes effect with /relay on."});
    } else if (name == "/unfinished") {
      list_unfinished();
    } else if (name == "/discard") {
      const auto n = device_number(rest, v.unfinished.size());
      if (!n) {
        flash(v.unfinished.empty() ? std::string("Nothing stopped part way.")
                                   : "/discard takes a receive's number (/unfinished).");
        return;
      }
      const auto u = v.unfinished[*n];
      ev("discard" + u.id, 1, "");
      say(tone::info, "Discarded what arrived from " + u.peer + " (" + size_text(u.bytes_here) + ").");
    } else if (name == "/save-to") {
      if (rest.empty()) {
        say(tone::info, "Received files go to " + home_short_of(v.destination) + ".");
        return;
      }
      std::string why;
      const auto dir = folder_from(rest, why);
      if (!dir) {
        flash(why);
        return;
      }
      ev("destination", 3, json_str(*dir));
      refresh_view();
      if (v.destination != *dir) {
        flash("That folder cannot be used.");
        return;
      }
      destination_chosen = true;
      say(tone::info, "Received files go to " + home_short_of(*dir) + " (kept for next time).");
    } else if (name == "/internet" || name == "/dht" || name == "/handover" || name == "/relay") {
      const std::string option(name.substr(1));
      const auto on = on_off(rest);
      if (!on) {
        const bool now_on = option == "internet" ? v.internet
                            : option == "dht"    ? v.dht
                            : option == "handover" ? v.handover
                                                   : v.relay;
        say(tone::info, option_label(option) + ": " + (now_on ? "on" : "off"),
            {std::string(name) + " on or " + std::string(name) + " off changes it (kept for next time)."});
        return;
      }
      set_option(option, *on, true);
    } else if (name == "/relay-server") {
      open_field(field_kind::relay_server);
    } else if (name == "/settings") {
      list_settings();
    } else if (name == "/help") {
      list_help();
    } else if (name == "/quit") {
      quitting = true;
    }
  }

  void list_files() {
    if (v.file_count == 0 && !v.reading_folder) {
      say(tone::info, "Nothing to send yet.", {"Type or drop files at the prompt, or /folder DIR."});
      return;
    }
    std::vector<std::string> details;
    if (!folder.empty()) {
      details.push_back(home_short_of(folder));
    } else {
      const std::size_t shown = std::min<std::size_t>(v.file_names.size(), 20);
      for (std::size_t i = 0; i < shown; ++i) details.push_back(v.file_names[i]);
      if (v.file_count > shown) details.push_back("and " + std::to_string(v.file_count - shown) + " more");
    }
    say(tone::info, files_summary() + " ready to send", std::move(details));
  }

  void list_devices() {
    if (!v.can_remember) {
      say(tone::info, "This computer remembers no devices.",
          {"There is no secret store here to keep its identity in (on Linux: a desktop keyring, the Secret Service)."});
      return;
    }
    if (v.known.empty()) {
      say(tone::info, "No remembered devices yet.",
          {"After a transfer, Driftwire asks whether to remember the other computer."});
      return;
    }
    std::vector<std::string> details;
    for (std::size_t i = 0; i < v.known.size(); ++i) {
      const bool own = i < v.known_own.size() && v.known_own[i];
      details.push_back(std::to_string(i + 1) + ". " + v.known[i] + (own ? glue() + "my computer" : std::string()));
    }
    details.emplace_back("/forget N forgets one; /mine N on|off marks one as yours.");
    say(tone::info, "Remembered devices", std::move(details));
  }

  void list_unfinished() {
    if (v.unfinished.empty()) {
      say(tone::info, "Nothing stopped part way.");
      return;
    }
    std::vector<std::string> details;
    for (std::size_t i = 0; i < v.unfinished.size(); ++i) {
      const auto& u = v.unfinished[i];
      details.push_back(std::to_string(i + 1) + ". From " + u.peer + ": " + size_text(u.bytes_here) + " of " +
                        size_text(u.bytes) + " is here.");
    }
    details.emplace_back("Each picks up where it stopped when its sender sends again. /discard N drops one.");
    say(tone::info, "Receives that stopped part way", std::move(details));
  }

  void list_settings() {
    const auto onoff = [](bool b) { return std::string(b ? "on" : "off"); };
    std::vector<std::string> details{
        "Name: " + v.my_name + (o.name_note.empty() ? std::string() : " (" + o.name_note + ")"),
        "Saves to: " + home_short_of(v.destination),
        "Over the internet: " + onoff(v.internet),
        "Public DHT: " + onoff(v.dht),
        "Continue over the internet if Wi-Fi drops: " + onoff(v.handover),
        "Relay through my computers: " + onoff(v.relay),
        "Relay server: " + std::string(v.relay_server.empty() ? "none" : "set"),
        "Remembers devices: " + std::string(v.can_remember ? "yes" : "no (no secret store here)"),
    };
    if (!o.data_dir.empty()) details.push_back("Settings kept in " + home_short_of(o.data_dir));
    say(tone::info, "Settings", std::move(details));
  }

  void list_help() {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const auto& c : commands()) {
      std::string head(c.name);
      if (!c.args.empty()) head += " " + std::string(c.args);
      rows.emplace_back(std::move(head), std::string(c.description));
    }
    say_table("Commands", rows);
    if (o.plain) {
      say(tone::info, "Keys",
          {"Enter on an empty line sends the files to a computer nearby.",
           "Esc cancels what is under way; Ctrl+C twice closes Driftwire."});
      return;
    }
    say_table("Keys", {{"enter", "send to the computer picked with " + std::string(th.g->arrows) + " (an empty line)"},
                       {"tab", "complete a path, or a command in the palette"},
                       {"ctrl+p  ctrl+n", "what was typed before"},
                       {"esc", "close the palette, clear the line, then cancel what is under way"},
                       {"ctrl+c", "clear the line; twice, close Driftwire"},
                       {"ctrl+d", "close Driftwire (an empty line)"},
                       {"ctrl+l", "draw everything again"},
                       {"ctrl+z", "suspend (fg comes back)"}});
  }

  // A titled list of two columns in the transcript: "/send PATH...  Add files ...".
  void say_table(const std::string& title, const std::vector<std::pair<std::string, std::string>>& rows) {
    int left = 0;
    for (const auto& row : rows) left = std::max(left, display_width(row.first));
    left = std::min(left + 2, std::max(8, w() / 2));
    if (o.plain) {
      std::vector<std::string> ls{title};
      for (const auto& [l, r] : rows) ls.push_back("  " + pad(l, left) + r);
      plain_write_lines(ls);
      return;
    }
    Elements e{entry(th, {tone::info, title, {}}, w())};
    const int room = std::max(1, w() - 2 - left);
    for (const auto& [l, r] : rows) {
      e.push_back(ftxui::hbox({ftxui::text("  "), paint(th, ftxui::text(pad(fit(l, left - 1, th), left)), tone::accent),
                               paint(th, ftxui::text(fit(r, room, th)), tone::dim)}));
    }
    live.print(ftxui::vbox(std::move(e)));
  }

  // The line typed at the prompt.
  void submit_prompt(std::string line) {
    const std::string_view t = trim(line);
    if (t.empty()) {
      send_selected();
      return;
    }
    editor.remember(line);
    if (!o.plain) say_dim("> " + std::string(t));
    if (t[0] == '/') {
      const auto space = t.find(' ');
      const std::string_view word = t.substr(0, space);
      if (const int c = find_command(word); c >= 0) {
        run_command(commands()[static_cast<std::size_t>(c)], space == std::string_view::npos ? "" : t.substr(space));
        return;
      }
      // A path dropped or pasted begins with "/" too.
      const parsed_paths r = parse_paths(t, o.paths);
      if (r.paths.empty()) {
        say(tone::warning, "Unknown command " + std::string(word), {"/help lists the commands."});
        return;
      }
    }
    add_paths(t);
  }

  // =========================================================================================
  // keys (the rich presenter)

  enum class focus : std::uint8_t { prompt, field, question, offer, naming, code_entry, remember };

  // What the keys go to. What the engine waits on with a clock running (an
  // offer, the code, the name) comes before a question or a field this
  // person opened, which wait their turn and come back after.
  focus focused() const {
    if (field == field_kind::save_elsewhere) return focus::field;  // the offer's own
    if (v.in == in_phase::asking) return focus::offer;
    if (v.out == out_phase::naming) return focus::naming;
    if (v.out == out_phase::confirming) return focus::code_entry;
    if (field != field_kind::none) return focus::field;
    if (asked) return focus::question;
    if (rem.at == remember_ui::step::ask || rem.at == remember_ui::step::type_check) return focus::remember;
    return focus::prompt;
  }

  // What has the keys, and which offer, check or question it is.
  struct card_ref {
    focus f = focus::prompt;
    std::uint64_t serial = 0;
    bool operator==(const card_ref&) const = default;
  };
  card_ref drawn;                  // what the last frame showed
  std::int64_t answerable_at = 0;  // a consent card takes answers from then (kAnswerSettleMs)

  card_ref card_now() const {
    const focus f = focused();
    if (f == focus::offer) return {f, offer_serial};
    if (f == focus::naming) return {f, naming_serial};
    if (f == focus::remember) return {f, rem_serial};
    return {f, 0};
  }

  // The cards that come unasked and say yes to someone else: accept these
  // files, send to this device, remember it.
  bool asks_consent(focus f) const {
    return f == focus::offer || f == focus::naming || (f == focus::remember && rem.at == remember_ui::step::ask);
  }

  // After a frame is drawn (rich or plain): a new card on screen starts its settle.
  void mark_drawn() {
    const card_ref c = card_now();
    if (c == drawn) return;
    drawn = c;
    answerable_at = now_ms() + kAnswerSettleMs;
  }

  bool transfer_running() const { return out_busy(v) || in_busy(v); }

  bool palette_open() const {
    const std::string& t = editor.text();
    return !t.empty() && t[0] == '/' && t.find(' ') == std::string::npos && t != palette_closed_for &&
           !(t.size() > 1 && t[1] == '/');
  }

  void on_ctrl_c() {
    if (field != field_kind::none && !field_ed.empty()) {
      field_ed.clear();
      return;
    }
    if (focused() == focus::prompt && !editor.empty()) {
      editor.clear();
      return;
    }
    if (o.plain && !plain_line.empty()) {
      plain_line.clear();
      plain_reprompt();
      return;
    }
    if (armed == armed_key::ctrl_c && now < armed_until) {
      quitting = true;
      return;
    }
    armed = armed_key::ctrl_c;
    armed_until = now + kArmMs;
    flash(transfer_running() ? "Press Ctrl+C again to cancel the transfer and quit" : "Press Ctrl+C again to quit",
          tone::warning, kArmMs);
  }

  // Esc with nothing typed: the card's cancel or decline (cli spec "Keys").
  void on_escape() {
    if (v.relaying_for.size() && !transfer_running()) {
      if (twice("Press esc again to stop relaying")) ev("in_cancel", 1, "");
      return;
    }
    if (v.out == out_phase::sending) {
      if (twice("Press esc again to cancel")) {
        cancelled_here = true;
        ev("out_cancel", 1, "");
      }
      return;
    }
    if (out_waits(v.out)) {
      cancelled_here = true;
      ev("out_cancel", 1, "");
      return;
    }
    if (v.in == in_phase::receiving) {
      if (twice("Press esc again to cancel")) {
        cancelled_here = true;
        ev("in_cancel", 1, "");
      }
      return;
    }
    if (v.in == in_phase::connecting) {
      cancelled_here = true;
      ev("decline", 1, "");
      return;
    }
    if (!to_name.empty() && !sent_to_name) {
      say(tone::info, "Stopped looking for " + to_name + ".");
      to_name.clear();
      return;
    }
    if (rem.at == remember_ui::step::show_check) {
      rem = {};
      return;
    }
    const auto cp = static_cast<code_phase>(v.code_phase);
    if (cp == code_phase::ready || cp == code_phase::preparing) {
      stop_code();
      return;
    }
  }

  // The second press within 2 s: true. The first: false, and the hint says so.
  bool twice(const std::string& say_first) {
    if (armed == armed_key::esc && now < armed_until) {
      armed = armed_key::none;
      note = {};
      return true;
    }
    armed = armed_key::esc;
    armed_until = now + kArmMs;
    flash(say_first, tone::warning, kArmMs);
    return false;
  }

  void copy(const std::string& what, const std::string& said) {
    // OSC 52: the terminal puts it on the clipboard (where it allows it).
    term.write("\x1B]52;c;" + base64(what) + "\x07");
    copied_text = said;
    copied_until = now + kArmMs;
  }
  std::string copied_text;
  std::int64_t copied_until = 0;

  // /copy, /copy link: the code card's code or link onto the clipboard. A
  // command, not a key: any letter typed at the prompt may begin a path.
  void copy_code(std::string_view what) {
    const std::string arg = lower_ascii(trim(what));
    if (!arg.empty() && arg != "link") {
      flash("/copy copies the code; /copy link copies its link.");
      return;
    }
    const bool the_link = arg == "link";
    if (static_cast<code_phase>(v.code_phase) != code_phase::ready) {
      flash("No code to copy: /code makes one.");
      return;
    }
    if (the_link && v.code_link.empty()) {
      flash("This code has no link.");
      return;
    }
    if (o.plain) {
      // Lines only, never a terminal sequence: the code is in a line above.
      say(tone::info, "Plain mode does not copy: copy the code from the line above.");
      return;
    }
    copy(the_link ? v.code_link : v.code_text, the_link ? "Link copied" : "Code copied");
  }

  // A choice list's keys: ↑↓, 1–n, Enter, y / n, Esc. -1: nothing chosen yet;
  // -2: no (n, Esc).
  int choose(const key_event& e, int& selected, int n, int yes_index = 0) {
    if (e.is(key::up)) {
      selected = std::max(0, selected - 1);
    } else if (e.is(key::down)) {
      selected = std::min(n - 1, selected + 1);
    } else if (e.is(key::enter)) {
      return selected;
    } else if (e.is(key::esc)) {
      return -2;
    } else if (e.is(key::text) && e.text.size() == 1) {
      const char c = e.text[0];
      if (c >= '1' && c < '1' + n) return c - '1';
      if (c == 'y' || c == 'Y') return yes_index;
      if (c == 'n' || c == 'N') return -2;
    }
    return -1;
  }

  // Digits typed into six boxes; true at the sixth.
  static bool type_digits(const key_event& e, std::string& typed) {
    if (e.is(key::backspace) && !typed.empty()) {
      typed.pop_back();
      return false;
    }
    if (e.is(key::text) || e.is(key::paste)) {
      for (const char c : digits_of(e.text)) {
        if (typed.size() < 6) typed += c;
      }
    }
    return typed.size() == 6;
  }

  void on_key(const key_event& e) {
    if (e.is_ctrl('c')) {
      on_ctrl_c();
      return;
    }
    if (armed != armed_key::none && !e.is(key::esc)) {
      armed = armed_key::none;
      note = {};
    }
    if (e.is_ctrl('z')) {
      suspend();
      return;
    }
    if (e.is_ctrl('l')) {
      live.invalidate();
      return;
    }
    // A key answers the card on screen: one the engine posted in the same
    // wake has not been seen (what a key of this person's opened has: loop()).
    if (card_now() != drawn) return;
    // A card that asks for consent takes no answer from typing that was
    // under way as it appeared: keys go nowhere until they have stopped for
    // kAnswerSettleMs with it on screen. Esc (no) always counts.
    if (asks_consent(drawn.f) && !e.is(key::esc) && now < answerable_at) {
      answerable_at = now + kAnswerSettleMs;
      return;
    }
    if (o.plain) {
      on_plain_key(e);
      return;
    }
    switch (focused()) {
      case focus::field: on_field_key(e); return;
      case focus::question: {
        const int n = static_cast<int>(asked->items.size());
        const int c = choose(e, asked->selected, n);
        if (c == -1) return;
        auto answer = std::move(asked->answer);
        asked.reset();
        if (answer) answer(c == -2 ? -1 : c);
        return;
      }
      case focus::offer: {
        const int c = choose(e, offer_sel, 3);
        if (c == -1) return;
        answer_offer(c == -2 ? 2 : c);
        return;
      }
      case focus::naming: {
        const int c = choose(e, naming_sel, 2);
        if (c == -1) return;
        answer_naming(c == 0);
        return;
      }
      case focus::code_entry:
        if (e.is(key::esc)) {
          cancelled_here = true;
          ev("out_cancel", 1, "");
          return;
        }
        if (!code_submitted && type_digits(e, code_typed)) submit_code_entry();
        return;
      case focus::remember:
        if (rem.at == remember_ui::step::ask) {
          const int c = choose(e, rem.selected, 2);
          if (c == -1) return;
          answer_remember(c == 0);
          return;
        }
        if (e.is(key::esc)) {
          rem = {};
          return;
        }
        if (!rem.submitted && type_digits(e, rem.typed)) submit_remember_check();
        return;
      case focus::prompt: on_prompt_key(e); return;
    }
  }

  void on_field_key(const key_event& e) {
    if (e.is(key::esc)) {
      close_field();
      return;
    }
    if (e.is(key::enter)) {
      submit_field();
      return;
    }
    if (e.is(key::tab) && field == field_kind::save_elsewhere) {
      const completion c = complete_path(field_ed.text(), field_ed.cursor(), o.paths, true);
      if (c.changed) field_ed.set(c.line, c.cursor);
      field_choices = c.choices;
      return;
    }
    field_choices.clear();
    if (field_ed.handle(e)) field_note.clear();
  }

  void on_prompt_key(const key_event& e) {
    const bool pal = palette_open();
    if (!e.is(key::tab)) tab_choices.clear();
    if (e.is(key::esc)) {
      if (pal) {
        palette_closed_for = editor.text();
      } else if (!editor.empty()) {
        editor.clear();
      } else {
        on_escape();
      }
      return;
    }
    if (pal && (e.is(key::up) || e.is(key::down))) {
      const auto n = static_cast<int>(filter_commands(std::string_view(editor.text()).substr(1)).size());
      palette_sel = std::clamp(palette_sel + (e.is(key::up) ? -1 : 1), 0, std::max(0, n - 1));
      return;
    }
    if (pal && (e.is(key::tab) || e.is(key::enter))) {
      const auto hits = filter_commands(std::string_view(editor.text()).substr(1));
      const int exact = find_command(editor.text());
      if (e.is(key::enter) && exact >= 0) {
        std::string line = editor.text();
        editor.clear();
        submit_prompt(std::move(line));
        return;
      }
      if (!hits.empty()) {
        const command_info& c = commands()[hits[static_cast<std::size_t>(std::clamp(palette_sel, 0, static_cast<int>(hits.size()) - 1))]];
        if (e.is(key::enter) && c.args.empty()) {
          editor.clear();
          submit_prompt(std::string(c.name));
          return;
        }
        editor.set(std::string(c.name) + " ");
        palette_sel = 0;
        return;
      }
      if (e.is(key::tab)) return;
      // No command matches: a path that begins with "/".
    }
    if (e.is(key::enter)) {
      std::string line = editor.text();
      editor.clear();
      palette_sel = 0;
      submit_prompt(std::move(line));
      return;
    }
    if (e.is(key::tab)) {
      const std::string& t = editor.text();
      const bool folders = t.rfind("/save-to ", 0) == 0 || t.rfind("/folder ", 0) == 0;
      const completion c = complete_path(t, editor.cursor(), o.paths, folders);
      if (c.changed) editor.set(c.line, c.cursor);
      tab_choices = c.choices;
      return;
    }
    if (e.is(key::up) || e.is(key::down)) {
      move_selection(e.is(key::up) ? -1 : 1);
      return;
    }
    if (e.is_ctrl('d') && editor.empty()) {
      if (transfer_running()) {
        on_ctrl_c();
      } else {
        quitting = true;
      }
      return;
    }
    const std::string before = editor.text();
    if (editor.handle(e) && editor.text() != before) palette_sel = 0;
  }

  // Ctrl+Z, or SIGTSTP from outside (POSIX job control; Windows has none).
  void suspend() {
#if !defined(_WIN32)
    if (!o.plain) live.erase();
    term.suspend_self();
    if (!o.plain) live.invalidate();
    prompt_on_line = false;  // plain: the prompt again, on a line of its own
#endif
  }

  // =========================================================================================
  // the live region (rich)

  bool animating() const {
    if (o.plain) return false;
    if (out_waits(v.out)) return true;
    if (v.out == out_phase::sending && !v.out_reconnecting) return true;
    if (v.in == in_phase::receiving && !v.in_reconnecting) return true;
    if (static_cast<code_phase>(v.code_phase) == code_phase::preparing || make_code_until) return true;
    if (!to_name.empty() && !sent_to_name) return true;
    if (!v.relaying_for.empty()) return true;
    return false;
  }

  std::int64_t dots_clock(bool moving) {
    if (moving) path_clock += now - path_clock_at;
    path_clock_at = now;
    return path_clock;
  }

  Element wait_line(std::string text, std::int64_t since, std::string hint = "esc to cancel") {
    wait_model m;
    m.text = std::move(text);
    m.elapsed_s = since > 0 ? static_cast<double>(now - since) / 1000.0 : -1;
    m.hint = std::move(hint);
    return waiting(th, m, w(), now - started);
  }

  Element dim_line(const std::string& s, int indent = 4) { return lines(th, s, w(), tone::dim, indent); }

  Element hints_for(std::vector<std::string> parts, std::string right = {}) {
    hint_model h;
    if (note.until > now && !note.text.empty()) {
      h.parts = {note.text};
      h.left_tone = note.t;
    } else {
      h.parts = std::move(parts);
    }
    h.right = std::move(right);
    return hints(th, h, w());
  }

  Element incoming_block() {
    switch (v.in) {
      case in_phase::connecting: {
        code_card_model m;
        m.peer = v.in_peer;
        m.code = v.in_code;
        m.known = v.in_known;
        m.internet = v.in_internet;
        return code_card(th, m, w());
      }
      case in_phase::receiving: {
        progress_model m;
        m.link.here = v.my_name;
        m.link.there = v.in_peer;
        m.link.via = bearer_of(v.in_via, v.in_punched, v.in_relay);
        m.link.badge = badge_of(v.in_relay);
        m.link.sending = false;
        m.link.moving = !v.in_reconnecting;
        m.link.reconnecting = v.in_reconnecting;
        m.bytes = v.in_bytes;
        m.total = v.in_total;
        m.files = v.in_files;
        m.count = static_cast<std::uint32_t>(v.in_count);
        m.rate = v.in_rate;
        m.left_s = v.in_left_s;
        return progress(th, m, w(), dots_clock(!v.in_reconnecting));
      }
      default: return nullptr;
    }
  }

  Element outgoing_block() {
    const std::string ell(th.g->ellipsis);
    switch (v.out) {
      case out_phase::connecting: return wait_line("Connecting to " + v.out_peer + ell, out_since);
      case out_phase::waiting: return wait_line("Waiting for " + v.out_peer + " to accept" + ell, out_since);
      case out_phase::waiting_wifi:
        return ftxui::vbox({wait_line("Waiting for " + v.out_peer + " on Wi-Fi" + ell, out_since),
                            dim_line(v.wifi_up ? "Put both computers on the same network; the send starts as soon as " +
                                                     v.out_peer + " is there."
                                               : "This computer is not on a network: join the same network as " +
                                                     v.out_peer + ". The send starts as soon as it is there.")});
      case out_phase::reaching: {
        if (v.out_known) return wait_line(v.out_note.empty() ? "Waiting for " + v.out_peer + ell : v.out_note, out_since);
        Elements rows{wait_line("Trying to reach the other computer" +
                                    (v.out_attempt > 1 ? " (attempt " + std::to_string(v.out_attempt) + ")" : std::string()) +
                                    ell,
                                out_since)};
        if (!v.out_note.empty()) rows.push_back(dim_line(v.out_note));
        return ftxui::vbox(std::move(rows));
      }
      case out_phase::sending: {
        progress_model m;
        m.link.here = v.my_name;
        m.link.there = v.out_peer;
        m.link.via = bearer_of(v.out_via, v.out_punched, v.out_relay);
        m.link.badge = badge_of(v.out_relay);
        m.link.sending = true;
        m.link.moving = !v.out_reconnecting;
        m.link.reconnecting = v.out_reconnecting;
        m.bytes = v.out_bytes;
        m.total = v.out_total;
        m.files = v.out_files;
        m.count = v.out_count;
        m.rate = v.out_rate;
        m.left_s = v.out_left_s;
        Elements rows{progress(th, m, w(), dots_clock(!v.out_reconnecting))};
        if (v.out_handover && v.out_via != "the internet") {
          rows.push_back(dim_line("If Wi-Fi drops, this continues over the internet.", 2));
        }
        return ftxui::vbox(std::move(rows));
      }
      default: return nullptr;
    }
  }

  Element code_block() {
    const auto cp = static_cast<code_phase>(v.code_phase);
    if (cp == code_phase::preparing || (cp == code_phase::none && make_code_until)) {
      return wait_line("Making a code" + std::string(th.g->ellipsis), -1, "esc to stop");
    }
    if (cp != code_phase::ready) return nullptr;
    internet_code_model m;
    m.code = v.code_text;
    m.link = v.code_link;
    m.how = v.code_how;
    if (code_ready_at >= 0) {
      m.left_s = std::max(0.0, static_cast<double>(eopts.code_lifetime_ms) / 1000.0 -
                                   static_cast<double>(now - code_ready_at) / 1000.0);
    }
    // The engine's note starts with the sentence the card already says;
    // only what follows it (the relay server's state) is shown.
    if (const auto at = v.code_note.find(" Your relay server"); at != std::string::npos) {
      m.note = v.code_note.substr(at + 1);
    }
    if (copied_until > now) m.copied = copied_text;
    return internet_code_card(th, m, w());
  }

  Element remember_block() {
    if (rem.at == remember_ui::step::none) return nullptr;
    remember_model m;
    m.peer = rem.peer;
    m.selected = rem.selected;
    switch (rem.at) {
      case remember_ui::step::ask: m.at = remember_model::step::ask; break;
      case remember_ui::step::show_check:
        m.at = remember_model::step::show_check;
        m.check = v.remember_check;
        break;
      case remember_ui::step::type_check:
        m.at = remember_model::step::type_check;
        m.typed = rem.typed;
        m.note = v.remember_check_note;
        break;
      case remember_ui::step::none: break;
    }
    Element card_e = remember_card(th, m, w());
    switch (rem.at) {
      case remember_ui::step::ask:
        return ftxui::vbox({card_e, hints_for({std::string(th.g->arrows) + " or 1" + std::string(th.g->dash) + "2",
                                               "enter to choose", "esc not now"})});
      case remember_ui::step::show_check: return ftxui::vbox({card_e, hints_for({"esc when it is typed there"})});
      case remember_ui::step::type_check:
        return ftxui::vbox({card_e, hints_for({"digits only", "esc not now"})});
      case remember_ui::step::none: break;
    }
    return card_e;
  }

  Element question_block() {
    Elements body;
    const int cw = w() - 4;
    for (const auto& b : asked->body) body.push_back(lines(th, b, cw, tone::dim, 2));
    if (!body.empty()) body.push_back(ftxui::text(""));
    body.push_back(choices(th, cw, asked->items, asked->selected));
    const int n = static_cast<int>(asked->items.size());
    return ftxui::vbox({card(th, w(), asked->title, std::move(body), tone::accent),
                        hints_for({std::string(th.g->arrows) + " or 1" + std::string(th.g->dash) + std::to_string(n),
                                   "enter to choose", "esc no"})});
  }

  Element field_block() {
    line_editor::window lw = field_ed.view(prompt_columns(w()));
    prompt_model p;
    p.text = lw.text;
    p.cursor_col = lw.cursor_col;
    p.cut_left = lw.cut_left;
    p.cut_right = lw.cut_right;
    p.border = tone::accent;
    std::vector<std::string> parts{"enter to " + std::string(field == field_kind::save_elsewhere ? "accept" : "set"),
                                   "esc to cancel"};
    switch (field) {
      case field_kind::to_code:
        p.title = "Send " + files_summary() + " to a code";
        p.placeholder = v.dht ? "Paste the code, or type the four words" : "Paste the code they sent";
        parts[0] = "enter to send";
        break;
      case field_kind::save_elsewhere:
        p.title = "Save somewhere else";
        p.placeholder = "A folder";
        parts.insert(parts.begin(), "tab completes");
        break;
      case field_kind::relay_server:
        p.title = "Your own relay server";
        p.placeholder = "address:port#key, as driftwire-relay prints it; empty for none";
        break;
      case field_kind::none: break;
    }
    Elements rows{prompt(th, p, w())};
    if (!field_note.empty()) rows.push_back(lines(th, field_note, w(), tone::error, 2));
    if (!field_choices.empty()) rows.push_back(choices_line(field_choices));
    rows.push_back(hints_for(std::move(parts)));
    return ftxui::vbox(std::move(rows));
  }

  Element choices_line(const std::vector<std::string>& cs) {
    std::string s;
    for (const auto& c : cs) s += (s.empty() ? "" : "  ") + c;
    return lines(th, s, w(), tone::dim, 2);
  }

  Element prompt_block() {
    line_editor::window lw = editor.view(prompt_columns(w()));
    prompt_model p;
    p.text = lw.text;
    p.cursor_col = lw.cursor_col;
    p.cut_left = lw.cut_left;
    p.cut_right = lw.cut_right;
    p.placeholder = "Type or drop files to send";
    Elements rows{prompt(th, p, w())};
    if (palette_open()) {
      palette_model pm;
      for (const std::size_t i : filter_commands(std::string_view(editor.text()).substr(1))) {
        const command_info& c = commands()[i];
        pm.rows.push_back({std::string(c.name), std::string(c.args), std::string(c.description)});
      }
      pm.selected = palette_sel;
      // A path pasted or dropped begins with "/" too: then no command matches,
      // and Enter adds it.
      pm.empty = "No command matches " + fit(editor.text(), std::max(8, w() / 2), th) + glue() + "enter adds it as a path";
      rows.push_back(command_palette(th, pm, w()));
      return ftxui::vbox(std::move(rows));
    }
    if (!tab_choices.empty()) rows.push_back(choices_line(tab_choices));
    std::vector<std::string> parts{"/ for commands"};
    const auto r = rows_count();
    if (r > 1) parts.push_back(std::string(th.g->arrows) + " choose a computer");
    if (r > 0 && files_ready() && !transfer_running()) parts.emplace_back("enter to send");
    rows.push_back(hints_for(std::move(parts), files_summary()));
    return ftxui::vbox(std::move(rows));
  }

  int rows_count() const { return static_cast<int>(v.peers.size() + (known_shown() ? v.known.size() : 0)); }

  Element nearby_block(int max_rows) {
    nearby_model m;
    for (const auto& p : v.peers) {
      nearby_row r;
      r.name = p.name;
      r.detail = p.wifi && p.bt ? "Wi-Fi" + glue() + "Bluetooth" : p.via;
      r.dot = p.wifi || !p.bt ? bearer::wifi : bearer::bluetooth;
      if (p.id == v.out_peer_id && out_busy(v)) {
        r.detail = v.out == out_phase::sending ? "sending" + std::string(th.g->ellipsis) : "connecting" + std::string(th.g->ellipsis);
        r.detail_tone = tone::accent;
      }
      m.peers.push_back(std::move(r));
    }
    if (known_shown()) {
      for (const auto& k : v.known) m.known.push_back({k, "over the internet", bearer::internet});
    }
    m.selected = rows_count() > 0 ? sel_index : -1;
    m.show_pointer = focused() == focus::prompt && !palette_open();
    m.note = reach_note(v);
    m.max_rows = max_rows;
    return nearby(th, m, w());
  }

  Element document(int nearby_rows) {
    Elements blocks;
    blocks.push_back(nearby_block(nearby_rows));
    if (!v.relaying_for.empty()) {
      blocks.push_back(wait_line("Relaying for " + v.relaying_for + ", sealed end to end", -1, "esc to stop"));
    }
    blocks.push_back(incoming_block());
    blocks.push_back(outgoing_block());
    if (!to_name.empty() && !sent_to_name) {
      blocks.push_back(wait_line("Looking for " + to_name + std::string(th.g->ellipsis), started,
                                 "esc to stop looking"));
    }
    blocks.push_back(code_block());
    if (rem.at == remember_ui::step::show_check) blocks.push_back(remember_block());
    switch (focused()) {
      case focus::field: blocks.push_back(field_block()); break;
      case focus::question: blocks.push_back(question_block()); break;
      case focus::offer: {
        offer_model m;
        m.peer = v.in_peer;
        m.known = v.in_known;
        m.count = v.in_count;
        m.bytes = v.in_total;
        m.names = v.in_names;
        m.folder = v.in_folder;
        m.relay = v.in_relay;
        m.check = v.in_internet ? v.in_code : std::string();
        m.save_to = home_short_of(v.destination);
        m.selected = offer_sel;
        blocks.push_back(ftxui::vbox({offer_card(th, m, w()), hints_for(offer_hint(th))}));
        break;
      }
      case focus::naming: {
        naming_model m;
        m.peer = v.out_peer;
        m.check = v.out_check;
        m.relay = v.out_relay;
        m.selected = naming_sel;
        blocks.push_back(ftxui::vbox(
            {naming_card(th, m, w()),
             hints_for({std::string(th.g->arrows) + " or 1" + std::string(th.g->dash) + "2", "enter to choose",
                        "y send", "n or esc cancel"})}));
        break;
      }
      case focus::code_entry: {
        code_entry_model m;
        m.peer = v.out_peer;
        m.typed = code_typed;
        m.note = v.out_entry_note;
        m.wrong = !v.out_entry_note.empty();
        blocks.push_back(ftxui::vbox({code_entry_card(th, m, w()), hints_for(code_entry_hint())}));
        break;
      }
      case focus::remember: blocks.push_back(remember_block()); break;
      case focus::prompt: blocks.push_back(prompt_block()); break;
    }
    return region(std::move(blocks));
  }

  void draw_rich() {
    if (!banner_done) return;  // the banner is the first thing on screen
    Element doc = document(0);
    // Taller than the terminal: Nearby shrinks first (inline_view cuts what is still over).
    const int n = rows_count();
    if (n > 1) {
      const int over = static_cast<int>(rasterize(doc, w()).rows.size()) - live.max_rows();
      if (over > 0) doc = document(std::max(1, n - over - 1));
    }
    live.present(doc);
    mark_drawn();
  }

  // =========================================================================================
  // plain mode: lines, and questions as lines

  struct plain_frame {
    std::string key;
    std::vector<std::string> said;
    std::string prompt = "driftwire> ";
  };

  plain_frame plain_now() {
    plain_frame f;
    switch (focused()) {
      case focus::field:
        f.key = "field" + std::to_string(static_cast<int>(field)) + field_note;
        if (!field_note.empty()) f.said.push_back(field_note);
        f.prompt = field == field_kind::to_code        ? "Code or four words (Enter alone to cancel): "
                   : field == field_kind::save_elsewhere ? "Folder to save into (Enter alone to cancel): "
                                                         : "Relay server, address:port#key (Enter alone for none): ";
        return f;
      case focus::question:
        f.key = "ask" + asked->title;
        f.said = asked->body;
        for (std::size_t i = 0; !asked->yes_no && i < asked->items.size(); ++i) {
          f.said.push_back("  " + std::to_string(i + 1) + ". " + asked->items[i]);
        }
        f.prompt = asked->plain;
        return f;
      case focus::offer: {
        f.key = "offer" + v.in_peer;
        f.said.push_back(v.in_folder.empty() ? names_text(v.in_names, v.in_count, 1 << 20, th)
                                             : "The folder \"" + v.in_folder + "\".");
        if (v.in_internet) f.said.push_back("Make sure " + v.in_peer + " shows " + v.in_code + ".");
        if (!v.in_relay.empty()) f.said.push_back("Through " + v.in_relay + ", sealed end to end.");
        f.said.push_back("It saves to " + home_short_of(v.destination) + " (s: somewhere else).");
        f.prompt = "Accept " + plural(v.in_count, "file", "files") + " (" + size_text(v.in_total) + ") from " +
                   v.in_peer + (v.in_known ? " (remembered)" : "") + "? [y/N/s] ";
        return f;
      }
      case focus::naming:
        f.key = "naming" + v.out_peer;
        f.said.push_back("Connected to " + v.out_peer + ". Check that " + v.out_peer + "'s screen shows " +
                         v.out_check + "; if not, someone else has the code.");
        f.prompt = "Send to " + v.out_peer + "? [y/N] ";
        return f;
      case focus::code_entry:
        f.key = "code" + v.out_peer + v.out_entry_note;
        if (!v.out_entry_note.empty()) f.said.push_back(v.out_entry_note);
        f.prompt = "Type the code on " + v.out_peer + "'s screen: ";
        return f;
      case focus::remember:
        if (rem.at == remember_ui::step::ask) {
          f.key = "rem" + rem.peer;
          f.said.push_back("Send to each other later over the internet, with no code, once " + rem.peer +
                           " remembers this computer too.");
          f.prompt = "Remember " + rem.peer + "? [y/N] ";
        } else {
          f.key = "remtype" + rem.peer + v.remember_check_note;
          if (!v.remember_check_note.empty()) f.said.push_back(v.remember_check_note);
          f.prompt = "Type the code " + rem.peer + " shows now: ";
        }
        return f;
      case focus::prompt: break;
    }
    // Statuses, said once each (progress at every tenth).
    const std::string ell = dots();
    const std::string sep = glue();
    if (v.in == in_phase::connecting) {
      f.key = "in" + v.in_code;
      if (v.in_known) {
        f.said.push_back(v.in_peer + " (remembered) wants to send you files. (Esc declines.)");
      } else if (v.in_internet) {
        f.said.push_back(v.in_peer + " (new device) wants to send you files. Make sure it shows " + v.in_code +
                         ". (Esc declines.)");
      } else {
        f.said.push_back(v.in_peer + " wants to send you files. Type this code on " + v.in_peer + ": " + v.in_code +
                         " (Esc declines.)");
      }
    } else if (v.in == in_phase::receiving || v.out == out_phase::sending) {
      const bool in = v.in == in_phase::receiving;
      const std::uint64_t b = in ? v.in_bytes : v.out_bytes, t = in ? v.in_total : v.out_total;
      const bool rc = in ? v.in_reconnecting : v.out_reconnecting;
      const int pct = t ? static_cast<int>(static_cast<double>(b) * 100.0 / static_cast<double>(t)) : 0;
      f.key = std::string(in ? "recv" : "send") + std::to_string(pct / 10) + (rc ? "rc" : "");
      std::string line = (rc ? "Reconnecting to " : in ? "Receiving from " : "Sending to ") +
                         (in ? v.in_peer : v.out_peer) + ": " + std::to_string(pct) + " %" + sep + size_text(b) +
                         " of " + size_text(t);
      if (!rc && (in ? v.in_rate : v.out_rate) > 0) line += sep + rate_text(in ? v.in_rate : v.out_rate);
      if (!rc) {
        if (const std::string left = time_left_text(in ? v.in_left_s : v.out_left_s); !left.empty()) line += sep + left;
      }
      f.said.push_back(line + " (Esc twice cancels.)");
    } else if (out_waits(v.out)) {
      std::string what = v.out == out_phase::connecting ? "Connecting to " + v.out_peer + ell
                         : v.out == out_phase::waiting  ? "Waiting for " + v.out_peer + " to accept" + ell
                         : v.out == out_phase::reaching ? (v.out_known ? v.out_note
                                                                       : "Trying to reach the other computer" +
                                                                             (v.out_attempt > 1
                                                                                  ? " (attempt " + std::to_string(v.out_attempt) + ")"
                                                                                  : std::string()) +
                                                                             ell)
                                                        : "Waiting for " + v.out_peer + " on Wi-Fi" + ell;
      f.key = "wait" + what + v.out_note;
      f.said.push_back(what + " (Esc cancels.)");
      if (v.out == out_phase::reaching && !v.out_known && !v.out_note.empty()) f.said.push_back("  " + v.out_note);
    } else if (!to_name.empty() && !sent_to_name) {
      f.key = "looking" + to_name;
      f.said.push_back("Looking for " + to_name + ell + " (Esc stops looking.)");
    } else if (static_cast<code_phase>(v.code_phase) == code_phase::preparing) {
      f.key = "making";
      f.said.push_back("Making a code" + ell);
    } else if (rem.at == remember_ui::step::show_check) {
      f.key = "remshow" + v.remember_check;
      f.said.push_back("Type this code on " + rem.peer + " so it remembers this computer: " + v.remember_check +
                       " (Esc when done.)");
    } else {
      f.key = "idle";
    }
    return f;
  }

  void plain_reprompt() {
    std::string s;
    if (prompt_on_line) s += "\r\n";
    s += printable(plain_prompt + plain_line.text());
    term.write(s);
    prompt_on_line = true;
  }

  void draw_plain() {
    if (!banner_done) return;
    plain_frame f = plain_now();
    const bool changed = f.key != plain_key || f.prompt != plain_prompt;
    if (changed) {
      plain_key = f.key;
      if (!f.said.empty()) plain_write_lines(f.said);
      if (f.prompt != plain_prompt) plain_line.clear();
      plain_prompt = f.prompt;
    }
    if (changed || !prompt_on_line) plain_reprompt();
    mark_drawn();
  }

  void plain_nearby_update() {
    if (!banner_done) return;
    std::string s;
    const auto r = computers();
    if (r.empty()) {
      s = "No one nearby yet. Open Driftwire on the other computer; it shows up when it is on this network.";
    } else {
      s = "Nearby:";
      for (std::size_t i = 0; i < r.size(); ++i) {
        s += (i ? ", " : " ") + std::to_string(i + 1) + ". " + r[i].name + (r[i].known ? " (over the internet)" : "");
      }
      if (files_ready()) s += ". Enter sends " + files_summary() + ".";
    }
    if (s == plain_nearby) return;
    plain_nearby = s;
    plain_write_lines({s});
    plain_reprompt();
  }

  void on_plain_key(const key_event& e) {
    if (e.is(key::enter)) {
      const std::string line = plain_line.text();
      plain_line.clear();
      term.write("\r\n");
      prompt_on_line = false;
      plain_submit(line);
      plain_key.clear();  // say where it stands again
      return;
    }
    if (e.is(key::esc)) {
      if (!plain_line.empty()) {
        plain_line.clear();
        plain_reprompt();
        return;
      }
      switch (focused()) {
        case focus::field: close_field(); break;
        case focus::question: {
          auto answer = std::move(asked->answer);
          asked.reset();
          if (answer) answer(-1);
          break;
        }
        case focus::offer: answer_offer(2); break;
        case focus::naming: answer_naming(false); break;
        case focus::code_entry:
          cancelled_here = true;
          ev("out_cancel", 1, "");
          break;
        case focus::remember: rem = {}; break;
        case focus::prompt: on_escape(); break;
      }
      return;
    }
    if (e.is_ctrl('d') && plain_line.empty() && focused() == focus::prompt) {
      if (transfer_running()) {
        on_ctrl_c();
      } else {
        quitting = true;
      }
      return;
    }
    if (e.is_ctrl('u')) {
      plain_echo_erase(plain_line.width());
      plain_line.clear();
      return;
    }
    if (e.is(key::backspace)) {
      if (plain_line.empty()) return;
      const int before = plain_line.width();
      plain_line.end();
      plain_line.backspace();
      plain_echo_erase(before - plain_line.width());
      return;
    }
    if (e.is(key::text) || e.is(key::paste)) {
      const int before = static_cast<int>(plain_line.text().size());
      plain_line.end();
      if (e.is(key::paste)) {
        plain_line.paste(e.text);
      } else {
        plain_line.insert(e.text);
      }
      if (o.echo) term.write(printable(std::string_view(plain_line.text()).substr(static_cast<std::size_t>(before))));
    }
  }

  void plain_echo_erase(int columns) {
    if (!o.echo || columns <= 0) return;
    std::string s;
    for (int i = 0; i < columns; ++i) s += "\b \b";
    term.write(s);
  }

  void plain_submit(const std::string& line) {
    const std::string t(trim(line));
    switch (focused()) {
      case focus::field:
        if (t.empty()) {
          close_field();
          return;
        }
        field_ed.set(t);
        submit_field();
        return;
      case focus::question: {
        int c = -1;
        if (asked->yes_no && yes_word(t)) c = 0;
        if (t.size() == 1 && t[0] >= '1' && t[0] < '1' + static_cast<int>(asked->items.size())) c = t[0] - '1';
        auto answer = std::move(asked->answer);
        asked.reset();
        if (answer) answer(c);
        return;
      }
      case focus::offer: {
        const std::string l = lower_ascii(t);
        answer_offer(yes_word(t) || l == "1" ? 0 : l == "s" || l == "2" ? 1 : 2);
        return;
      }
      case focus::naming: answer_naming(yes_word(t) || t == "1"); return;
      case focus::code_entry: {
        const std::string d = digits_of(t);
        if (d.size() != 6) {
          plain_write_lines({d.empty() ? std::string("Esc cancels.") : "The code has 6 digits."});
          return;
        }
        code_typed = d;
        submit_code_entry();
        return;
      }
      case focus::remember:
        if (rem.at == remember_ui::step::ask) {
          answer_remember(yes_word(t) || t == "1");
          return;
        }
        if (digits_of(t).size() == 6) {
          rem.typed = digits_of(t);
          submit_remember_check();
        } else {
          plain_write_lines({"The code has 6 digits."});
        }
        return;
      case focus::prompt: break;
    }
    if (rem.at == remember_ui::step::show_check && t.empty()) {
      rem = {};
      return;
    }
    submit_prompt(t);
  }

  // =========================================================================================
  // the loop

  void start() {
    started = now = now_ms();
    path_clock_at = now;
    const term_size s = term.size();
    width = std::max(1, s.columns);
    height = std::max(1, s.rows);
    live.resize(width, height);
    one_shot = o.cmd == command::send || o.cmd == command::code || (o.cmd == command::receive && o.once);
    to_name = o.to;
    editor.set_paste_quote(o.paths.windows ? '"' : '\'');

    eng = std::make_unique<engine>(host, eopts);
    // The window opens: presence, and this computer's name on the air (#501).
    eng->screen_opened("");
    if (!o.send_paths.empty()) add_existing(o.send_paths);
    refresh_view();
    if (o.cmd == command::code) make_code();
  }

  int timeout() const {
    std::int64_t t = -1;
    const auto take = [&](std::int64_t ms) {
      ms = std::max<std::int64_t>(0, ms);
      t = t < 0 ? ms : std::min(t, ms);
    };
    if (animating()) take(kFrameMs);
    if (const int p = keys.pending_ms(); p >= 0) take(p - (now - bytes_at));
    if (note.until > now) take(note.until - now);
    if (armed != armed_key::none && armed_until > now) take(armed_until - now);
    if (copied_until > now) take(copied_until - now);
    if (!banner_done) take(started + kBannerWaitMs - now);
    if (make_code_until) take(kMakeCodeEveryMs);
    if (static_cast<code_phase>(v.code_phase) == code_phase::ready && code_ready_at >= 0 && !o.plain) {
      // The life left is said in minutes: draw again as the minute turns.
      const std::int64_t gone = now - code_ready_at;
      take(60000 - gone % 60000 + 5);
    }
    return static_cast<int>(std::min<std::int64_t>(t, 1 << 30));
  }

  void loop() {
    std::vector<key_event> evs;
    term_input in;
    while (!quitting) {
      draw();
      if (shot_over && rem.at == remember_ui::step::none && !asked) {
        quitting = true;
        break;
      }
      term.wait(in, timeout());
      now = now_ms();
      evs.clear();
      if (!in.bytes.empty()) {
        keys.feed(in.bytes, evs);
        bytes_at = now;
      } else if (keys.pending_ms() >= 0 && now - bytes_at >= keys.pending_ms()) {
        keys.flush(evs);
      }
      if (in.resized) {
        const term_size s = term.size();
        width = std::max(1, s.columns);
        height = std::max(1, s.rows);
        live.resize(width, height);
      }
      if (in.resumed) live.invalidate();
      if (in.suspend) suspend();
      if (in.interrupt) on_ctrl_c();
      if (in.quit || in.eof) quitting = true;
      // The view first, so each key is checked against what the engine asks
      // now: a card that is not the one last drawn takes no key (on_key).
      refresh_view();
      for (const auto& e : evs) {
        if (quitting) break;
        const card_ref before = card_now();
        on_key(e);
        // What this person's own key opened (a field, a question) takes the
        // keys typed after it at once, even in the same read; a card that
        // asks for consent waits to be drawn.
        if (before == drawn) {
          if (const card_ref c = card_now(); c != drawn && !asks_consent(c.f)) drawn = c;
        }
      }
      if (note.until && note.until <= now) note = {};
      if (armed != armed_key::none && armed_until <= now) armed = armed_key::none;
      retry_make_code();
      refresh_view();
    }
  }

  void draw() {
    if (o.plain) {
      draw_plain();
    } else {
      draw_rich();
    }
  }

  void finish() {
    if (eng) {
      refresh_view();
      if (transfer_running()) {
        say(tone::warning, "Closed during a transfer: it was cancelled.",
            {v.in == in_phase::receiving ? "What arrived whole stays here." : "The other computer is told."});
      }
    }
    if (!o.plain) live.erase();
    // The engine stops on this thread, before the terminal is handed back (#530).
    eng.reset();
    put_back_destination();
    const std::string last = "Driftwire closed" + glue() + "this computer is no longer visible";
    if (o.plain) {
      plain_write_lines({last});
    } else {
      live.close(last);
    }
    event("quit " + std::to_string(exit_code()));
  }

  // --save-to is for this run: the engine writes its whole view whenever a
  // setting is saved, so the kept Save to is put back unless this person
  // chose one on purpose (/save-to, "save somewhere else").
  void put_back_destination() {
    if (eopts.prefs_path.empty() || destination_chosen || !o.restore_destination) return;
    auto now_kept = load_prefs(eopts.prefs_path);
    if (!now_kept || now_kept->destination == *o.restore_destination) return;
    now_kept->destination = *o.restore_destination;
    if (!save_prefs(eopts.prefs_path, *now_kept)) {
      // Nothing to do: the next run starts from the folder this one used.
    }
  }

  int exit_code() const {
    if (!one_shot) return 0;
    return shot_ok ? 0 : 1;
  }

  int run() {
    start();
    loop();
    finish();
    return exit_code();
  }
};

session::session(terminal& term, cli_host& host, engine::options engine_options, session_options o, const theme& th)
    : p_(std::make_unique<impl>(term, host, std::move(engine_options), std::move(o), th)) {}

session::~session() = default;

int session::run() { return p_->run(); }

}  // namespace mv::driftwire::cli
