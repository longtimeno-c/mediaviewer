// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_driftwire_cli_gallery: every card and state the driftwire command line
// draws (docs/design/28 "From a terminal"), with made-up data, at the
// terminal's width, in colour, one after another; or one animation played
// through the live-region renderer for a few seconds. A test tool for
// reviewing and screenshotting the look; never packaged, and nothing in it
// talks to the engine or the network.
//
//   mv_driftwire_cli_gallery [--width N] [--ascii] [--no-color] [--depth 16|256|true]
//   mv_driftwire_cli_gallery --animate STATE [--seconds S] [...]
//     STATE: waiting, sending, receiving, bluetooth, internet, dht, relay,
//            reconnecting, code
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "addons/driftwire/cli/inline_view.h"
#include "addons/driftwire/cli/render.h"
#include "addons/driftwire/cli/term.h"
#include "addons/driftwire/cli/theme.h"

namespace cli = mv::driftwire::cli;
using ftxui::Element;

namespace {

struct args {
  int width = 0;
  bool ascii = false;
  bool no_color = false;
  int depth = 0;  // 16, 256, 24 (truecolor); 0: the terminal's
  std::string animate;
  double seconds = 4;
};

int usage() {
  std::fprintf(stderr,
               "usage: mv_driftwire_cli_gallery [--width N] [--ascii] [--no-color] [--depth 16|256|true]\n"
               "       mv_driftwire_cli_gallery --animate STATE [--seconds S]\n"
               "  STATE: waiting, sending, receiving, bluetooth, internet, dht, relay, reconnecting, code\n");
  return 2;
}

cli::nearby_model nearby_full() {
  cli::nearby_model m;
  m.peers = {{"Ann's MacBook", "Wi-Fi"}, {"Living-room PC", "Wi-Fi \xC2\xB7 Bluetooth"}};
  m.selected = 0;
  return m;
}

Element idle_prompt(const cli::theme& th, int w, std::string_view right) {
  cli::prompt_model p;
  p.placeholder = "Type or drop files to send";
  cli::hint_model h;
  h.parts = {"/ for commands", std::string(th.g->arrows) + " choose a computer", "enter to send"};
  h.right = std::string(right);
  return ftxui::vbox({cli::prompt(th, p, w), cli::hints(th, h, w)});
}

cli::progress_model moving(cli::bearer via, bool sending, double fraction) {
  cli::progress_model m;
  m.link.here = "ThinkPad";
  m.link.there = "Ann's MacBook";
  m.link.via = via;
  m.link.sending = sending;
  m.total = 4'800'000'000ULL;
  m.bytes = static_cast<std::uint64_t>(fraction * static_cast<double>(m.total));
  m.count = 24;
  m.files = static_cast<std::uint32_t>(fraction * 24);
  m.rate = 14'000'000;
  m.left_s = static_cast<double>(m.total - m.bytes) / m.rate;
  if (via == cli::bearer::relay) m.link.badge = "via Studio Mac";
  return m;
}

class stdout_sink final : public cli::output {
 public:
  explicit stdout_sink(cli::console* c) : c_(c) {}
  void write(std::string_view bytes) override {
    if (c_) {
      c_->write(bytes);
    } else {
      std::fwrite(bytes.data(), 1, bytes.size(), stdout);
      std::fflush(stdout);
    }
  }

 private:
  cli::console* c_;
};

void show(cli::output& out, const cli::theme& th, int w, std::string_view label, const Element& e) {
  std::string s = "\r\n";
  s += "\x1B[0;2m";
  s += std::string(th.g->sep) + " " + std::string(label);
  s += "\x1B[0m\r\n";
  for (const auto& row : cli::rasterize(e, w).rows) {
    s += row;
    s += "\r\n";
  }
  out.write(s);
}

void gallery(cli::output& out, const cli::theme& th, int w) {
  cli::banner_model b{"1.0.0", "Tristan's ThinkPad", "visible on Wi-Fi", "internet off", "~/Downloads"};
  show(out, th, w, "start banner (printed once)", cli::banner(th, b, w));

  show(out, th, w, "idle: Nearby, the prompt, its hints",
       cli::region({cli::nearby(th, nearby_full(), w), idle_prompt(th, w, "3 files \xC2\xB7 1.2 GB")}));
  show(out, th, w, "idle: no one nearby", cli::region({cli::nearby(th, cli::nearby_model{}, w), idle_prompt(th, w, "")}));
  {
    cli::nearby_model m = nearby_full();
    m.peers.push_back({"\xE6\x9D\x8E\xE6\x98\x8E\xE7\x9A\x84\xE7\x94\xB5\xE8\x84\x91 \xF0\x9F\x8C\xB8 (a very long name indeed)",
                       "Bluetooth", cli::bearer::bluetooth});
    m.known = {{"Studio Mac", "over the internet", cli::bearer::internet}};
    m.selected = 3;
    m.note = "Searching this network only. Bluetooth is not used.";
    show(out, th, w, "Nearby with your devices over the internet, a CJK + emoji name", cli::nearby(th, m, w));
  }
  {
    cli::prompt_model p;
    p.text = "/co";
    p.cursor_col = 3;
    cli::palette_model pal;
    pal.rows = {{"/code", "", "Receive from anywhere: make a code to give someone"},
                {"/send-to", "", "Send these files to a code someone gave you"}};
    show(out, th, w, "the slash palette", ftxui::vbox({cli::prompt(th, p, w), cli::command_palette(th, pal, w)}));
  }
  {
    cli::code_card_model m{"Ann's MacBook", "342 917"};
    show(out, th, w, "receive: the code to type over there", cli::code_card(th, m, w));
    m.known = true;
    show(out, th, w, "receive: a remembered device (no code)", cli::code_card(th, m, w));
    m.known = false;
    m.internet = true;
    m.code = "482 917";
    show(out, th, w, "receive over the internet: the check to compare", cli::code_card(th, m, w));
  }
  {
    cli::offer_model m;
    m.peer = "Ann's MacBook";
    m.count = 24;
    m.bytes = 412'000'000;
    m.names = {"IMG_4471.HEIC", "IMG_4472.HEIC", "IMG_4473.MOV", "IMG_4474.HEIC"};
    m.save_to = "~/Downloads";
    show(out, th, w, "the offer",
         ftxui::vbox({cli::offer_card(th, m, w), cli::hints(th, {cli::offer_hint(th), ""}, w)}));
    m.folder = "Trip";
    m.known = true;
    m.selected = 1;
    show(out, th, w, "the offer: a folder, from a remembered device", cli::offer_card(th, m, w));
    m.folder.clear();
    m.known = false;
    m.check = "482 917";
    m.relay = "Studio Mac";
    m.selected = 2;
    show(out, th, w, "the offer over the internet, through a relay", cli::offer_card(th, m, w));
  }
  {
    cli::code_entry_model m;
    m.peer = "Ann's MacBook";
    show(out, th, w, "send: type the code (empty)",
         ftxui::vbox({cli::code_entry_card(th, m, w), cli::hints(th, {cli::code_entry_hint(), ""}, w)}));
    m.typed = "4829";
    m.note = "That is not the code on Ann's MacBook's screen. 2 tries left.";
    m.wrong = true;
    show(out, th, w, "send: a wrong code", cli::code_entry_card(th, m, w));
  }
  show(out, th, w, "waiting lines",
       ftxui::vbox({cli::waiting(th, {"Connecting to Ann's MacBook" + std::string(th.g->ellipsis), 1}, w, 0),
                    cli::waiting(th, {"Waiting for Ann's MacBook to accept" + std::string(th.g->ellipsis), 12}, w, 360),
                    cli::waiting(th, {"Trying to reach the other computer (attempt 2)" + std::string(th.g->ellipsis), 41},
                                 w, 720),
                    cli::waiting(th, {"Looking for Ann's MacBook" + std::string(th.g->ellipsis), -1}, w, 960)}));
  for (const auto& [via, name] : std::vector<std::pair<cli::bearer, const char*>>{
           {cli::bearer::wifi, "Wi-Fi, 42 %"},
           {cli::bearer::bluetooth, "Bluetooth"},
           {cli::bearer::internet, "the internet"},
           {cli::bearer::dht, "met on the DHT"},
           {cli::bearer::relay, "through a relay"}}) {
    show(out, th, w, std::string("sending: ") + name, cli::progress(th, moving(via, true, 0.42), w, 700));
  }
  show(out, th, w, "receiving: 0 %", cli::progress(th, moving(cli::bearer::wifi, false, 0), w, 0));
  {
    cli::progress_model m = moving(cli::bearer::wifi, true, 0.42);
    m.link.reconnecting = true;
    show(out, th, w, "sending: reconnecting", cli::progress(th, m, w, 700));
    m = moving(cli::bearer::wifi, true, 1.0);
    m.left_s = 0;
    show(out, th, w, "sending: 100 %", cli::progress(th, m, w, 700));
  }
  {
    cli::internet_code_model m;
    m.code = "amber river quiet falcon";
    m.link = "driftwire://c/amber-river-quiet-falcon";
    m.left_s = 540;
    m.how = "router mapping, IPv6";
    show(out, th, w, "/code: four words", cli::internet_code_card(th, m, w));
    m.code = "KX7P 2M9Q 4TND 8WBA";
    m.copied = "Code copied";
    show(out, th, w, "/code: base32 groups, copied", cli::internet_code_card(th, m, w));
  }
  show(out, th, w, "send to a code: compare the check", cli::naming_card(th, {"Ann's MacBook", "482 917", ""}, w));
  {
    cli::remember_model m;
    m.peer = "Ann's MacBook";
    show(out, th, w, "remember: ask", cli::remember_card(th, m, w));
    m.at = cli::remember_model::step::show_check;
    m.check = "615 204";
    show(out, th, w, "remember: the sender shows the code", cli::remember_card(th, m, w));
    m.at = cli::remember_model::step::type_check;
    m.typed = "61";
    show(out, th, w, "remember: the receiver types it", cli::remember_card(th, m, w));
  }
  {
    cli::prompt_model p;
    p.title = "Send to a code";
    p.placeholder = "Paste the code, or type the four words";
    p.border = cli::tone::accent;
    show(out, th, w, "a field: /send-to",
         ftxui::vbox({cli::prompt(th, p, w),
                      cli::hints(th, {{"enter to send", "esc to cancel"}, ""}, w)}));
    p.title = "Save somewhere else";
    p.text = "~/Pictures/Trip 2026/";
    p.cursor_col = cli::display_width(p.text);
    show(out, th, w, "a field: a folder (tab completes)", cli::prompt(th, p, w));
  }
  show(out, th, w, "transcript",
       ftxui::vbox({cli::entry(th, {cli::tone::success, "Received 24 files from Ann's MacBook",
                                    {"412 MB in 31 s \xC2\xB7 Wi-Fi \xC2\xB7 every file checked \xC2\xB7 ~/Downloads"}},
                               w),
                    cli::entry(th, {cli::tone::success, "Sent 3 files to Ann's MacBook.", {"1.2 GB in 1 min 32 s \xC2\xB7 Wi-Fi"}},
                               w),
                    cli::entry(th, {cli::tone::warning, "Ann's MacBook declined.", {}}, w),
                    cli::entry(th, {cli::tone::error, "The code was wrong 3 times, so nothing was sent.", {}}, w),
                    cli::entry(th, {cli::tone::info, "2 receives stopped part way", {"/unfinished to see them"}}, w)}));
  show(out, th, w, "a hint that asks",
       cli::hints(th, {{"Press Ctrl+C again to quit"}, "3 files", cli::tone::warning}, w));
}

int animate(cli::console& con, cli::output& out, const cli::theme& th, int w, int h, const args& a) {
  cli::inline_view view(out, w, h);
  const auto start = std::chrono::steady_clock::now();
  const auto ms_since = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
  };
  const double total_ms = a.seconds * 1000;
  cli::term_input in;
  std::int64_t frozen_at = -1;
  while (true) {
    const std::int64_t ms = ms_since();
    if (static_cast<double>(ms) >= total_ms) break;
    const double f = std::min(1.0, static_cast<double>(ms) / total_ms);
    Element live;
    if (a.animate == "waiting") {
      live = cli::waiting(th, {"Waiting for Ann's MacBook to accept" + std::string(th.g->ellipsis),
                               static_cast<double>(ms) / 1000.0},
                          w, ms);
    } else if (a.animate == "code") {
      live = cli::code_card(th, {"Ann's MacBook", "342 917"}, w);
    } else {
      const cli::bearer via = a.animate == "bluetooth"  ? cli::bearer::bluetooth
                              : a.animate == "internet" ? cli::bearer::internet
                              : a.animate == "dht"      ? cli::bearer::dht
                              : a.animate == "relay"    ? cli::bearer::relay
                                                        : cli::bearer::wifi;
      cli::progress_model m = moving(via, a.animate != "receiving", f);
      std::int64_t clock = ms;
      if (a.animate == "reconnecting" && f > 0.4) {
        // Frozen where it was from 40 %: the clock the dots read stops.
        m.link.reconnecting = true;
        if (frozen_at < 0) frozen_at = ms;
        clock = frozen_at;
      }
      live = cli::progress(th, m, w, clock);
    }
    view.present(cli::region({live, idle_prompt(th, w, "")}));
    con.wait(in, 1000 / 12);
    if (in.interrupt || in.quit || in.eof || !in.bytes.empty()) break;
    if (in.resized) {
      const cli::term_size s = con.size();
      w = a.width > 0 ? a.width : s.columns;
      view.resize(w, s.rows);
    }
  }
  view.print(cli::entry(th, {cli::tone::success, "Sent 24 files to Ann's MacBook.", {"4.8 GB in 5 min 43 s"}}, w));
  view.close("Driftwire closed " + std::string(th.g->sep) + " this computer is no longer visible");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  args a;
  for (int i = 1; i < argc; ++i) {
    const std::string_view s = argv[i];
    const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    if (s == "--width") {
      const char* v = value();
      if (!v) return usage();
      a.width = std::atoi(v);
    } else if (s == "--ascii") {
      a.ascii = true;
    } else if (s == "--no-color") {
      a.no_color = true;
    } else if (s == "--depth") {
      const char* v = value();
      if (!v) return usage();
      a.depth = std::string_view(v) == "16" ? 16 : std::string_view(v) == "256" ? 256 : 24;
    } else if (s == "--animate") {
      const char* v = value();
      if (!v) return usage();
      a.animate = v;
    } else if (s == "--seconds") {
      const char* v = value();
      if (!v) return usage();
      a.seconds = std::atof(v);
    } else {
      return usage();
    }
  }
  const bool tty = cli::console::is_terminal();
  cli::console con;
#if defined(_WIN32)
  // VT sequences and UTF-8 in the Windows console.
  if (tty) con.enter_raw();
#else
  // An animation stops at any key; typed keys do not echo into it.
  if (tty && !a.animate.empty()) con.enter_raw();
#endif
  const cli::term_size size = tty ? con.size() : cli::term_size{};
  const int w = a.width > 0 ? a.width : size.columns;
  cli::color_depth depth = cli::detect_color_depth(a.no_color);
  if (!a.no_color && a.depth) {
    depth = a.depth == 16 ? cli::color_depth::c16 : a.depth == 256 ? cli::color_depth::c256 : cli::color_depth::truecolor;
  }
  const cli::theme th = cli::make_theme(depth, !a.ascii && (cli::console::utf8() || !tty));
  stdout_sink out(tty ? &con : nullptr);
  if (!a.animate.empty()) return animate(con, out, th, w, size.rows, a);
  gallery(out, th, w);
  return 0;
}
