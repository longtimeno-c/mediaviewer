// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// build_tree: the Sharepoint window as a view tree (docs/design/25 §10). Both
// hosts draw it with their own controls, so it follows the theme, text size,
// VoiceOver and Narrator; every control carries a label.
//
// The design (#442) is AirDrop's: what is offered to this computer comes
// first, in a card of its own (who, the code to type over there, then one
// Accept / Decline, then a progress ring); then what this computer sends (a
// strip of thumbnails and the count), and the computers nearby as large round
// tiles. One click on a tile sends; its ring fills while the bytes move.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "addons/sharepoint/engine.h"
#include "core/json.h"

namespace mv::sharepoint {

namespace {

using json::writer;

void text(writer& w, const char* id, const std::string& s, const char* style = "body") {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(s.empty() ? " " : s);
  w.key("style").string(style).end_object();
}

void button(writer& w, const char* id, const std::string& label, const char* style, bool disabled = false) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  w.key("style").string(style);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}

void ring(writer& w, const char* id, const std::string& label, std::uint64_t done, std::uint64_t total, int size) {
  const double v = total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : -1.0;
  w.begin_object().key("type").string("progress").key("id").string(id).key("label").string(label);
  w.key("value").number(v).key("look").string("ring").key("size").integer(size).end_object();
}

void folder_field(writer& w, const char* id, const char* label, const std::string& value, const char* placeholder) {
  w.begin_object().key("type").string("text_field").key("id").string(id).key("label").string(label);
  w.key("value").string(value).key("placeholder").string(placeholder).key("pick").string("folder").end_object();
}

void stack(writer& w, const char* id, int spacing, const char* axis = "vertical", const char* look = nullptr,
           const char* align = nullptr) {
  w.begin_object().key("type").string("stack").key("id").string(id).key("axis").string(axis);
  w.key("spacing").integer(spacing);
  if (look) w.key("look").string(look);
  if (align) w.key("align").string(align);
  w.key("children").begin_array();
}

void row(writer& w, const char* id) { stack(w, id, 8, "horizontal"); }

void end(writer& w) { w.end_array().end_object(); }

std::string count_text(std::size_t n, std::uint64_t bytes) {
  return std::to_string(n) + (n == 1 ? " file, " : " files, ") + size_text(bytes);
}

std::string names_line(const std::vector<std::string>& names, std::size_t count) {
  std::string out;
  const std::size_t shown = std::min<std::size_t>(names.size(), 4);
  for (std::size_t i = 0; i < shown; ++i) {
    if (i) out += ", ";
    out += names[i];
  }
  if (count > shown) out += " and " + std::to_string(count - shown) + " more";
  return out;
}

// "Studio Mac" -> "SM", "Ann" -> "A": the monogram in a tile's circle, the
// first character of the first two words (ASCII letters upper-cased).
std::string initials(const std::string& name) {
  std::string out;
  int words = 0;
  bool start = true;
  for (std::size_t i = 0; i < name.size() && words < 2;) {
    const auto c = static_cast<unsigned char>(name[i]);
    const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    const bool gap = c == ' ' || c == '-' || c == '_' || c == '.';
    if (start && !gap && i + len <= name.size()) {
      std::string piece = name.substr(i, len);
      if (len == 1 && c >= 'a' && c <= 'z') piece[0] = static_cast<char>(c - 'a' + 'A');
      out += piece;
      ++words;
    }
    start = gap;
    i += len;
  }
  return out.empty() ? std::string("?") : out;
}

// Why Send waits for Wi-Fi, and what to do about it.
std::string wifi_hint(const view& v, const std::string& peer) {
  const std::string why = "These files are " + size_text(v.file_bytes) +
                          " and Bluetooth carries 32 MB at most. ";
  if (!v.wifi_up) {
    return why + "This computer is not on a network: turn on Wi-Fi here and join the same network as " + peer +
           ". Click " + peer + " to send over Wi-Fi; it waits, and starts as soon as " + peer + " is there.";
  }
  return why + peer + " is only in Bluetooth range: put both computers on the same Wi-Fi network. Click " +
         peer + " to send over Wi-Fi; it waits, and starts as soon as " + peer + " is there.";
}

std::string moved_text(std::uint32_t files, std::uint32_t count, std::uint64_t bytes, std::uint64_t total) {
  return std::to_string(files) + " of " + std::to_string(count) + " files, " + size_text(bytes) + " of " +
         size_text(total);
}

bool out_busy(const view& v) {
  return v.out == view::out_phase::waiting_wifi || v.out == view::out_phase::connecting ||
         v.out == view::out_phase::confirming || v.out == view::out_phase::waiting ||
         v.out == view::out_phase::sending || v.out == view::out_phase::reaching || v.out == view::out_phase::naming;
}

// ---- what is offered to this computer -------------------------------------------------

void incoming(writer& w, const view& v) {
  stack(w, "incoming", 10, "vertical", "card", "center");
  // Who: the sender's tile, shown not chosen.
  w.begin_object().key("type").string("tiles").key("id").string("in_from").key("label").string("From " + v.in_peer);
  w.key("static").boolean(true).key("size").integer(64).key("items").begin_array();
  w.begin_object().key("id").string("sender").key("text").string(v.in_peer).key("detail").string(v.in_via);
  w.key("initials").string(initials(v.in_peer));
  if (v.in == view::in_phase::done) w.key("mark").string("done");
  if (v.in == view::in_phase::ended) w.key("mark").string("failed");
  w.end_object().end_array().end_object();
  switch (v.in) {
    case view::in_phase::connecting:
      if (v.in_internet) {
        // #472 PR 2: the code already proved them; the check is to compare.
        text(w, "in_title", v.in_peer + " (new device) wants to send you files", "heading");
        text(w, "in_code", v.in_code, "display");
        text(w, "in_check",
             "The same six digits show on " + v.in_peer + "\xE2\x80\x99s screen. If they differ, decline.",
             "secondary");
      } else {
        // #442: the code shows here only; the person at the sender types it.
        text(w, "in_title", v.in_peer + " wants to send you files", "heading");
        text(w, "in_code", v.in_code, "display");
        text(w, "in_check", "Type this code on " + v.in_peer + " to continue.", "secondary");
      }
      button(w, "decline", "Decline", "plain");
      break;
    case view::in_phase::asking:
      text(w, "in_title", v.in_peer + " wants to send you " + count_text(v.in_count, v.in_total), "heading");
      text(w, "in_files", names_line(v.in_names, v.in_count), "secondary");
      folder_field(w, "destination", "Save to", v.destination, "Choose a folder");
      row(w, "in_actions");
      button(w, "decline", "Decline", "plain");
      button(w, "accept", "Accept", "primary", v.destination.empty());
      end(w);
      break;
    case view::in_phase::receiving: {
      // #472: between links the ring holds where it is and says so; it never restarts.
      const std::string what = v.in_reconnecting ? "Reconnecting to " + v.in_peer + "\xE2\x80\xA6"
                                                 : "Receiving from " + v.in_peer;
      ring(w, "in_progress", what, v.in_bytes, v.in_total, 88);
      text(w, "in_title", what, "heading");
      text(w, "in_moved", moved_text(v.in_files, static_cast<std::uint32_t>(v.in_count), v.in_bytes, v.in_total),
           "secondary");
      button(w, "in_cancel", "Cancel", "destructive");
      break;
    }
    case view::in_phase::done:
    case view::in_phase::ended:
      text(w, "in_note", v.in_note);
      button(w, "in_dismiss", "OK", "plain");
      break;
    case view::in_phase::none:
      break;
  }
  end(w);
}

// ---- what this computer sends ----------------------------------------------------------

void files_card(writer& w, const view& v, bool sending) {
  stack(w, "send", 10, "vertical", "card", "fill");
  text(w, "send_title", "Send", "heading");
  if (v.reading_folder) {
    text(w, "files_summary", "Reading the files\xE2\x80\xA6", "secondary");
  } else if (v.file_count == 0) {
    text(w, "files_summary", "Nothing to send yet. Select photos in the gallery and press Send, or pick a folder.",
         "secondary");
  } else {
    // A strip of the first files, from the viewer's thumbnail cache.
    row(w, "files_strip");
    for (std::size_t i = 0; i < v.file_paths.size(); ++i) {
      char id[24];
      std::snprintf(id, sizeof id, "thumb%zu", i);
      const std::string label = i < v.file_names.size() ? v.file_names[i] : std::string("File");
      w.begin_object().key("type").string("thumbnail").key("id").string(id).key("path").string(v.file_paths[i]);
      w.key("label").string(label).key("size").integer(56).end_object();
    }
    end(w);
    text(w, "files_summary", count_text(v.file_count, v.file_bytes), "title");
    text(w, "files_names", names_line(v.file_names, v.file_count), "secondary");
  }
  row(w, "file_actions");
  folder_field(w, "send_folder", "Send a folder", "", "Pick a folder to send");
  button(w, "clear", "Clear", "plain", v.file_count == 0 || sending);
  end(w);
  end(w);
}

void nearby(writer& w, const view& v, bool sending) {
  stack(w, "nearby", 10, "vertical", nullptr, "fill");
  text(w, "peers_title", "Nearby", "heading");
  if (v.peers.empty()) {
    text(w, "peers_none", "Looking for MediaViewers nearby\xE2\x80\xA6 Open Sharepoint on the other computer too.",
         "secondary");
  } else {
    w.begin_object().key("type").string("tiles").key("id").string("peers").key("label").string("Nearby MediaViewers");
    w.key("size").integer(72).key("items").begin_array();
    for (const auto& p : v.peers) {
      w.begin_object().key("id").string(p.id).key("text").string(p.name).key("detail").string(p.via);
      w.key("initials").string(initials(p.name));
      if (p.id == v.out_peer_id) {
        if (v.out == view::out_phase::sending) {
          w.key("progress").number(v.out_total > 0 ? std::min(1.0, static_cast<double>(v.out_bytes) /
                                                                       static_cast<double>(v.out_total))
                                                   : 0.0);
        } else if (v.out == view::out_phase::connecting || v.out == view::out_phase::confirming ||
                   v.out == view::out_phase::waiting || v.out == view::out_phase::waiting_wifi) {
          w.key("progress").number(0.0);
        } else if (v.out == view::out_phase::done) {
          w.key("mark").string("done");
        } else if (v.out == view::out_phase::ended) {
          w.key("mark").string("failed");
        }
      }
      w.end_object();
    }
    w.end_array();
    w.key("selection").begin_array();
    if (!v.selected_peer.empty()) w.string(v.selected_peer);
    w.end_array();
    // Nothing to send yet: the tiles are shown, not clicked. While a send
    // runs they stay live (its ring fills); the engine takes no second one.
    if (v.file_count == 0 || v.reading_folder) w.key("disabled").boolean(true);
    w.end_object();
  }
  std::string target;
  for (const auto& p : v.peers) {
    if (p.id == v.selected_peer) target = p.name;
  }
  // Too big for Bluetooth and the peer is only in Bluetooth range: say what
  // to do before the click, which then waits for the peer on Wi-Fi.
  if (needs_wifi(v) && !sending) text(w, "send_hint", wifi_hint(v, target), "secondary");

  switch (v.out) {
    case view::out_phase::idle:
      if (!v.peers.empty() && v.file_count > 0 && !v.reading_folder) {
        text(w, "out_note", "Click a computer to send it these files.", "secondary");
      }
      break;
    case view::out_phase::waiting_wifi:
      text(w, "out_note",
           "Waiting for " + v.out_peer + " on Wi-Fi. " +
               (v.wifi_up ? "Put both computers on the same network; the send starts as soon as " + v.out_peer +
                                " is there."
                          : std::string("This computer is not on a network: turn on Wi-Fi here and join the same "
                                        "network as ") +
                                v.out_peer + ". The send starts as soon as it is there."),
           "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::connecting:
      text(w, "out_note", "Connecting to " + v.out_peer + "\xE2\x80\xA6", "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::confirming:
      // #442: typed, not compared. This screen never shows the code.
      stack(w, "out_code", 8, "vertical", "card", "center");
      text(w, "out_note", "Type the code shown on " + v.out_peer, "heading");
      w.begin_object().key("type").string("text_field").key("id").string("out_code_entry");
      w.key("label").string("Code shown on " + v.out_peer).key("value").string("");
      w.key("placeholder").string("000 000").key("look").string("code").key("digits").integer(6).end_object();
      text(w, "out_entry_note",
           v.out_entry_note.empty() ? std::string("Nothing has been sent yet.") : v.out_entry_note, "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      end(w);
      break;
    case view::out_phase::waiting:
      text(w, "out_note", "Waiting for " + v.out_peer + " to accept\xE2\x80\xA6", "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::sending:
      text(w, "out_moved",
           (v.out_reconnecting ? "Reconnecting to " + v.out_peer + "\xE2\x80\xA6 "
                               : "Sending to " + v.out_peer + (v.out_via.empty() ? "" : " over " + v.out_via) + ": ") +
               moved_text(v.out_files, v.out_count, v.out_bytes, v.out_total),
           "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::reaching:
      // #472 PR 2: round after round until one place answers, or Cancel.
      text(w, "out_reaching",
           "Trying to reach the other computer" +
               (v.out_attempt > 1 ? " (attempt " + std::to_string(v.out_attempt) + ")" : std::string()) +
               "\xE2\x80\xA6",
           "secondary");
      if (!v.out_note.empty()) text(w, "out_note", v.out_note, "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::naming:
      stack(w, "out_name", 8, "vertical", "card", "center");
      text(w, "out_note", "Connected to " + v.out_peer + " (new device)", "heading");
      text(w, "out_check", v.out_check, "display");
      text(w, "out_check_note",
           "The same six digits show on their screen. If they differ, someone else has the code: cancel. Nothing has "
           "been sent yet.",
           "secondary");
      row(w, "out_name_actions");
      button(w, "out_cancel", "Cancel", "plain");
      button(w, "out_confirm", "Send to " + v.out_peer, "primary");
      end(w);
      end(w);
      break;
    case view::out_phase::done:
    case view::out_phase::ended:
      text(w, "out_note", v.out_note);
      break;
  }
  end(w);
}

// ---- over the internet (#472 PR 2) -------------------------------------------------------

void internet(writer& w, const view& v, bool sending) {
  stack(w, "internet_section", 10, "vertical", nullptr, "fill");
  w.begin_object().key("type").string("toggle").key("id").string("internet");
  w.key("label").string("Send and receive over the internet").key("value").boolean(v.internet).end_object();
  if (!v.internet) {
    end(w);
    return;
  }
  text(w, "internet_intro",
       "No server: the other computer connects straight to this one with a code that works once. Your public "
       "address goes to whoever you give the code to.",
       "caption");
  // Receive: one code at a time.
  text(w, "code_title", "Receive from anywhere", "heading");
  using phase = internet_receiver::phase;
  const auto p = static_cast<phase>(v.code_phase);
  if (p == phase::ready) {
    stack(w, "code_card", 8, "vertical", "card", "center");
    text(w, "code_text", v.code_text, "display");
    text(w, "code_link", v.code_link, "caption");
    if (!v.code_note.empty()) text(w, "code_note", v.code_note, "secondary");
    if (!v.code_how.empty()) text(w, "code_how", "Ways in: " + v.code_how + ".", "caption");
    button(w, "stop_code", "Stop", "plain");
    end(w);
  } else {
    if (!v.code_note.empty()) text(w, "code_note", v.code_note, "secondary");
    button(w, "make_code", p == phase::preparing ? "Making a code\xE2\x80\xA6" : "Make a code", "primary",
           p == phase::preparing);
  }
  w.begin_object().key("type").string("text_field").key("id").string("forwarded");
  w.key("label").string("Forwarded port (advanced)").key("value").string(v.forwarded);
  w.key("placeholder").string("public address:port").end_object();
  // Send to a code.
  text(w, "send_code_title", "Send to a code", "heading");
  row(w, "send_code_row");
  w.begin_object().key("type").string("text_field").key("id").string("send_code");
  w.key("label").string("Code or link").key("value").string("");
  w.key("placeholder").string("Paste the code they sent").end_object();
  button(w, "send_to_code", "Send", "primary", v.file_count == 0 || v.reading_folder || sending);
  end(w);
  if (!v.send_code_note.empty()) text(w, "send_code_note", v.send_code_note, "secondary");
  end(w);
}

}  // namespace

std::string build_tree(const view& v) {
  writer w;
  w.begin_object().key("type").string("scroll").key("id").string("root").key("children").begin_array();
  stack(w, "page", 16, "vertical", nullptr, "fill");

  // Who this computer is, and whether it stays discoverable.
  stack(w, "me", 6, "vertical", nullptr, "fill");
  text(w, "intro",
       "Nearby MediaViewers see this computer as \xE2\x80\x9C" + v.my_name +
           "\xE2\x80\x9D while this window is open" +
           (v.background ? ", and while MediaViewer is closed."
                         : v.receiving ? " and while MediaViewer runs." : "."),
       "secondary");
  w.begin_object().key("type").string("toggle").key("id").string("receiving");
  w.key("label").string("Keep receiving when this window is closed").key("value").boolean(v.receiving).end_object();
  // #452: opt-in, off by default. MediaViewer then stays in the menu bar /
  // notification area with every window closed, and an offer is a system
  // notification that opens this window (never the code itself).
  w.begin_object().key("type").string("toggle").key("id").string("background");
  w.key("label").string(kBackgroundLabel).key("value").boolean(v.background).end_object();
  {
    std::string where = v.wifi_up ? "Looking on the local network" : "The local network is not available";
    where += v.bluetooth_note.empty() ? (v.wifi_up ? " and Bluetooth." : "; looking on Bluetooth.")
                                      : ". " + v.bluetooth_note;
    text(w, "presence", where, "caption");
  }
  end(w);

  if (v.in != view::in_phase::none) incoming(w, v);
  const bool sending = out_busy(v);
  files_card(w, v, sending);
  nearby(w, v, sending);
  internet(w, v, sending);

  end(w);                      // page
  w.end_array().end_object();  // root
  return w.take();
}

}  // namespace mv::sharepoint
