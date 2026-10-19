// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// build_tree: the Sharepoint window as a view tree (docs/design/25 §10). Both
// hosts draw it with their own controls, so it follows the theme, text size,
// VoiceOver and Narrator; every control carries a label.
//
// The design (#442) is AirDrop's: what is offered to this computer comes
// first, in a card of its own (who, the code to type over there, then one
// Accept / Decline, then progress); then what this computer sends (a strip of
// thumbnails and the count), and the computers nearby as large round tiles.
// One click on a tile sends; its ring fills while the bytes move.
//
// 2026-10-19: a transfer under way is a card at the top with a picture of its
// path (the `link` node: this computer, the other, the bearer's badge, dots
// moving while bytes do), so Bluetooth, Wi-Fi and each kind of internet path
// look different at a glance. The internet part reads in the order a person
// uses it: receive (a code to copy), send to a code, then the options.
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

// Text a person can select and copy (the code, its link).
void copyable(writer& w, const char* id, const std::string& s, const char* style) {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(s.empty() ? " " : s);
  w.key("style").string(style).key("copyable").boolean(true).end_object();
}

void button(writer& w, const char* id, const std::string& label, const char* style, bool disabled = false) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  w.key("style").string(style);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}

// A button whose click also puts `copy` on the clipboard (the host does it).
void copy_button(writer& w, const char* id, const std::string& label, const char* style, const std::string& copy) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  w.key("style").string(style).key("copy").string(copy).end_object();
}

void toggle(writer& w, const char* id, const std::string& label, bool value) {
  w.begin_object().key("type").string("toggle").key("id").string(id).key("label").string(label);
  w.key("value").boolean(value).end_object();
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

// #472 relays: the line under a send that goes through one.
std::string relay_line(const std::string& through) {
  return "Through " + (through == "your relay server" ? through : through + " (your computer)") +
         ", sealed end to end: it passes the bytes on and cannot read them.";
}

// #472 PR 4: under a transfer's result over protocol 2.
void remember_toggle(writer& w, const view& v) {
  if (!v.remember_offer) return;
  toggle(w, "remember", "Remember " + v.remember_peer, v.remembered);
  text(w, "remember_note",
       "Send to each other later over the internet, with no code. It works once " + v.remember_peer +
           " remembers this computer too.",
       "caption");
}

// ---- the path a transfer takes (the `link` node) ----------------------------------------

struct path_kind {
  const char* via;      // the node's "via"
  std::string caption;  // what the picture says under it
  std::string through;  // the computer in between, for "relay"
};

// From what the engine knows of the link: its bearer, whether it was punched
// after meeting on the DHT, and the relay it goes through (#472).
path_kind path_of(const std::string& via, bool punched, const std::string& relay) {
  if (relay == "your relay server") return {"server", "Internet, through your relay server", ""};
  if (!relay.empty()) return {"relay", "Internet, through " + relay, relay};
  if (via == "the internet") {
    return punched ? path_kind{"dht", "Internet, met through the public DHT", ""}
                   : path_kind{"internet", "Internet, straight to each other", ""};
  }
  if (via == "Bluetooth") return {"bluetooth", "Bluetooth", ""};
  return {"wifi", "Wi-Fi, on this network", ""};
}

double fraction(std::uint64_t done, std::uint64_t total) {
  return total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : -1.0;
}

// state: connecting, moving, paused, done, failed. `value` < 0: none yet.
void link_picture(writer& w, const char* id, const path_kind& k, const char* state, bool sending,
                  const std::string& peer, double value) {
  const std::string other = peer.empty() ? std::string("The other computer") : peer;
  w.begin_object().key("type").string("link").key("id").string(id);
  w.key("label").string((sending ? "Sending to " : "Receiving from ") + other + ". " + k.caption + ".");
  w.key("via").string(k.via).key("state").string(state).key("direction").string(sending ? "send" : "receive");
  w.key("from").string("This computer").key("to").string(other);
  if (!k.through.empty()) w.key("through").string(k.through);
  w.key("caption").string(k.caption);
  if (value >= 0.0) w.key("value").number(value);
  w.end_object();
}

// ---- what is offered to this computer -------------------------------------------------

void incoming(writer& w, const view& v) {
  stack(w, "incoming", 10, "vertical", "card", "center");
  {
    const char* state = v.in == view::in_phase::receiving ? (v.in_reconnecting ? "paused" : "moving")
                        : v.in == view::in_phase::done    ? "done"
                        : v.in == view::in_phase::ended   ? "failed"
                                                          : "connecting";
    link_picture(w, "in_path", path_of(v.in_via, v.in_punched, v.in_relay), state, false, v.in_peer,
                 v.in == view::in_phase::receiving ? fraction(v.in_bytes, v.in_total) : -1.0);
  }
  switch (v.in) {
    case view::in_phase::connecting:
      if (v.in_known) {
        // #472 PR 4: a remembered device; its key is the one remembered.
        text(w, "in_title", v.in_peer + " (remembered) wants to send you files", "heading");
        text(w, "in_check", "Its key is the one you remembered.", "secondary");
      } else if (v.in_internet) {
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
      if (!v.in_relay.empty()) text(w, "in_relay", "Through " + v.in_relay + ", sealed end to end.", "caption");
      folder_field(w, "destination", "Save to", v.destination, "Choose a folder");
      row(w, "in_actions");
      button(w, "decline", "Decline", "plain");
      button(w, "accept", "Accept", "primary", v.destination.empty());
      end(w);
      break;
    case view::in_phase::receiving: {
      // #472: between links the picture pauses where it is and says so; it never restarts.
      const std::string what = v.in_reconnecting ? "Reconnecting to " + v.in_peer + "\xE2\x80\xA6"
                                                 : "Receiving from " + v.in_peer;
      text(w, "in_title", what, "heading");
      text(w, "in_moved", moved_text(v.in_files, static_cast<std::uint32_t>(v.in_count), v.in_bytes, v.in_total),
           "secondary");
      button(w, "in_cancel", "Cancel", "destructive");
      break;
    }
    case view::in_phase::done:
    case view::in_phase::ended:
      text(w, "in_note", v.in_note);
      if (v.in == view::in_phase::done) remember_toggle(w, v);
      button(w, "in_dismiss", "OK", "plain");
      break;
    case view::in_phase::none:
      break;
  }
  end(w);
}

// ---- a send under way ---------------------------------------------------------------------

// The path of the send: before its link is open, the bearer it will try.
path_kind out_path(const view& v) {
  if (!v.out_via.empty() && v.out != view::out_phase::waiting_wifi) {
    return path_of(v.out_via, v.out_punched, v.out_relay);
  }
  if (v.out_internet || v.out_known || v.out == view::out_phase::reaching) return path_of("the internet", false, "");
  if (v.out == view::out_phase::waiting_wifi) return path_of("Wi-Fi", false, "");
  for (const auto& p : v.peers) {
    if (p.id == v.out_peer_id) return path_of(p.wifi ? "Wi-Fi" : "Bluetooth", false, "");
  }
  return path_of("Wi-Fi", false, "");
}

void outgoing(writer& w, const view& v) {
  stack(w, "outgoing", 10, "vertical", "card", "center");
  const char* state = v.out == view::out_phase::sending ? (v.out_reconnecting ? "paused" : "moving")
                      : v.out == view::out_phase::done  ? "done"
                      : v.out == view::out_phase::ended ? "failed"
                      : v.out == view::out_phase::waiting_wifi ? "paused"
                                                               : "connecting";
  const std::string peer = v.out == view::out_phase::reaching && v.out_peer.empty() ? std::string() : v.out_peer;
  link_picture(w, "out_path", out_path(v), state, true, peer,
               v.out == view::out_phase::sending ? fraction(v.out_bytes, v.out_total) : -1.0);
  switch (v.out) {
    case view::out_phase::idle:
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
      text(w, "out_note", "Connecting to " + v.out_peer + "\xE2\x80\xA6", "heading");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::confirming:
      // #442: typed, not compared. This screen never shows the code.
      text(w, "out_note", "Type the code shown on " + v.out_peer, "heading");
      w.begin_object().key("type").string("text_field").key("id").string("out_code_entry");
      w.key("label").string("Code shown on " + v.out_peer).key("value").string("");
      w.key("placeholder").string("000 000").key("look").string("code").key("digits").integer(6).end_object();
      text(w, "out_entry_note",
           v.out_entry_note.empty() ? std::string("Nothing has been sent yet.") : v.out_entry_note, "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::waiting:
      text(w, "out_note", "Waiting for " + v.out_peer + " to accept\xE2\x80\xA6", "heading");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::sending:
      text(w, "out_title",
           v.out_reconnecting ? "Reconnecting to " + v.out_peer + "\xE2\x80\xA6" : "Sending to " + v.out_peer,
           "heading");
      text(w, "out_moved", moved_text(v.out_files, v.out_count, v.out_bytes, v.out_total), "secondary");
      if (!v.out_relay.empty()) text(w, "out_relay", relay_line(v.out_relay), "caption");
      if (v.out_handover && v.out_via != "the internet") {
        text(w, "out_handover", "If Wi-Fi drops, this continues over the internet.", "caption");
      }
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::reaching:
      if (v.out_known) {
        // #472 PR 4: waiting for a remembered device, as long as it takes.
        text(w, "out_note", v.out_note, "secondary");
        button(w, "out_cancel", "Cancel", "destructive");
        break;
      }
      // #472 PR 2: round after round until one place answers, or Cancel.
      text(w, "out_reaching",
           "Trying to reach the other computer" +
               (v.out_attempt > 1 ? " (attempt " + std::to_string(v.out_attempt) + ")" : std::string()) +
               "\xE2\x80\xA6",
           "heading");
      if (!v.out_note.empty()) text(w, "out_note", v.out_note, "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::naming:
      text(w, "out_note", "Connected to " + v.out_peer + " (new device)", "heading");
      if (!v.out_relay.empty()) text(w, "out_relay", relay_line(v.out_relay), "caption");
      text(w, "out_check", v.out_check, "display");
      text(w, "out_check_note",
           "The same six digits show on their screen. If they differ, someone else has the code: cancel. Nothing has "
           "been sent yet.",
           "secondary");
      row(w, "out_name_actions");
      button(w, "out_cancel", "Cancel", "plain");
      button(w, "out_confirm", "Send to " + v.out_peer, "primary");
      end(w);
      break;
    case view::out_phase::done:
    case view::out_phase::ended:
      text(w, "out_note", v.out_note);
      if (v.out == view::out_phase::done && v.in == view::in_phase::none) remember_toggle(w, v);
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

void nearby(writer& w, const view& v) {
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
          w.key("progress").number(std::max(0.0, fraction(v.out_bytes, v.out_total)));
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
  if (needs_wifi(v) && !out_busy(v)) text(w, "send_hint", wifi_hint(v, target), "secondary");
  if (v.out == view::out_phase::idle && !v.peers.empty() && v.file_count > 0 && !v.reading_folder) {
    text(w, "out_hint", "Click a computer to send it these files.", "secondary");
  }
  end(w);
}

// ---- over the internet (#472 PR 2) -------------------------------------------------------

void receive_card(writer& w, const view& v) {
  stack(w, "code_card", 10, "vertical", "card", "center");
  text(w, "code_title", "Receive from anywhere", "heading");
  using phase = internet_receiver::phase;
  const auto p = static_cast<phase>(v.code_phase);
  if (p == phase::ready) {
    text(w, "code_intro", "Give this code to the person sending. It works once, for 10 minutes.", "secondary");
    copyable(w, "code_text", v.code_text, "display");
    row(w, "code_actions");
    copy_button(w, "copy_code", "Copy code", "primary", v.code_text);
    if (!v.code_link.empty()) copy_button(w, "copy_link", "Copy link", "plain", v.code_link);
    button(w, "stop_code", "Stop", "plain");
    end(w);
    if (!v.code_note.empty()) text(w, "code_note", v.code_note, "secondary");
    if (!v.code_how.empty()) text(w, "code_how", "Ways in: " + v.code_how + ".", "caption");
  } else {
    text(w, "code_intro", "Make a code, then give it to the person sending. No account and no server.",
         "secondary");
    if (!v.code_note.empty()) text(w, "code_note", v.code_note, "secondary");
    button(w, "make_code", p == phase::preparing ? "Making a code\xE2\x80\xA6" : "Make a code", "primary",
           p == phase::preparing);
  }
  end(w);
}

void send_code_card(writer& w, const view& v, bool sending) {
  stack(w, "send_code_card", 10, "vertical", "card", "fill");
  text(w, "send_code_title", "Send to a code", "heading");
  text(w, "send_code_intro",
       v.file_count == 0 ? std::string("Pick what to send above, then paste the code you were given.")
                         : "Paste the code you were given to send " + count_text(v.file_count, v.file_bytes) + ".",
       "secondary");
  row(w, "send_code_row");
  w.begin_object().key("type").string("text_field").key("id").string("send_code");
  w.key("label").string("Code or link").key("value").string("");
  w.key("placeholder").string(v.dht ? "Paste the code, or type the four words" : "Paste the code they sent")
      .end_object();
  button(w, "send_to_code", "Send", "primary", v.file_count == 0 || v.reading_folder || sending);
  end(w);
  if (!v.send_code_note.empty()) text(w, "send_code_note", v.send_code_note, "secondary");
  end(w);
}

void known_devices(writer& w, const view& v, bool sending) {
  if (v.known.empty()) return;
  stack(w, "known_section", 8, "vertical", nullptr, "fill");
  text(w, "known_title", "Remembered devices", "heading");
  w.begin_object().key("type").string("tiles").key("id").string("known").key("label").string("Remembered devices");
  w.key("size").integer(64).key("items").begin_array();
  for (std::size_t i = 0; i < v.known.size(); ++i) {
    w.begin_object().key("id").string("dev" + std::to_string(i)).key("text").string(v.known[i]);
    w.key("detail").string("Over the internet").key("initials").string(initials(v.known[i])).end_object();
  }
  w.end_array();
  if (v.file_count == 0 || v.reading_folder || sending || !v.dht) w.key("disabled").boolean(true);
  w.end_object();
  text(w, "known_note",
       v.dht ? std::string("Click one to send it these files. It waits until that computer is online.")
             : std::string("Turn on \xE2\x80\x9C" "Find each other through the public DHT\xE2\x80\x9D below to send "
                           "to these."),
       "caption");
  row(w, "known_forget");
  for (std::size_t i = 0; i < v.known.size(); ++i) {
    const std::string id = "forget" + std::to_string(i);
    w.begin_object().key("type").string("button").key("id").string(id).key("label").string("Forget " + v.known[i]);
    w.key("style").string("plain").end_object();
  }
  end(w);
  if (v.relay) {
    // #472 relays: which of them are this person's own (relay for each other).
    for (std::size_t i = 0; i < v.known.size(); ++i) {
      const std::string id = "own" + (i < v.known_ids.size() ? v.known_ids[i] : std::string());
      toggle(w, id.c_str(), v.known[i] + " is my computer", i < v.known_own.size() && v.known_own[i]);
    }
  }
  end(w);
}

void internet_options(writer& w, const view& v) {
  stack(w, "internet_options", 8, "vertical", nullptr, "fill");
  text(w, "options_title", "Internet options", "heading");
  // #472 PR 3: opt-in, off by default.
  toggle(w, "dht", "Find each other through the public DHT", v.dht);
  text(w, "dht_note",
       v.dht ? "For networks with no public address: a code is four words and both computers meet on the BitTorrent "
               "DHT while it waits. DHT nodes see that this computer takes part, never what is sent."
             : "Turn on if Make a code says no way in was found.",
       "caption");
  // #472 PR 5: opt-in, off by default.
  toggle(w, "handover", "Continue over the internet if Wi-Fi drops", v.handover);
  if (v.handover) {
    text(w, "handover_note", "A nearby transfer whose Wi-Fi goes carries on over the internet. Both computers need "
                             "this on.",
         "caption");
  }
  // #472 relays: opt-in, off by default.
  toggle(w, "relay", "Use my other computers to relay", v.relay);
  if (v.relay) {
    text(w, "relay_note",
         "When nothing answers directly, a send goes through another computer of yours that remembers this one "
         "(marked as yours under Remembered devices, on both). It stays sealed end to end: the computer in between "
         "cannot read it.",
         "caption");
  }
  // #472 relays: the person's own relay server (tools/sharepoint-relay).
  w.begin_object().key("type").string("text_field").key("id").string("relay_server");
  w.key("label").string("Your own relay server (optional)").key("value").string(v.relay_server);
  w.key("placeholder").string("Only if you run one: address:port#key").end_object();
  end(w);
}

void internet(writer& w, const view& v, bool sending) {
  stack(w, "internet_section", 12, "vertical", nullptr, "fill");
  text(w, "internet_title", "Over the internet", "heading");
  toggle(w, "internet", "Send and receive over the internet", v.internet);
  if (!v.internet) {
    text(w, "internet_intro", "For a computer that is not nearby. Off until you turn it on.", "caption");
    end(w);
    return;
  }
  text(w, "internet_intro",
       "No server: the two computers connect straight to each other with a code that works once. Whoever has the "
       "code learns this computer\xE2\x80\x99s address.",
       "caption");
  receive_card(w, v);
  send_code_card(w, v, sending);
  known_devices(w, v, sending);
  internet_options(w, v);
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
  {
    std::string where = v.wifi_up ? "Looking on the local network" : "The local network is not available";
    where += v.bluetooth_note.empty() ? (v.wifi_up ? " and Bluetooth." : "; looking on Bluetooth.")
                                      : ". " + v.bluetooth_note;
    text(w, "presence", where, "caption");
  }
  toggle(w, "receiving", "Keep receiving when this window is closed", v.receiving);
  // #452: opt-in, off by default. MediaViewer then stays in the menu bar /
  // notification area with every window closed, and an offer is a system
  // notification that opens this window (never the code itself).
  toggle(w, "background", kBackgroundLabel, v.background);
  end(w);

  if (!v.relaying_for.empty()) {
    // #472 relays: this computer carries another of the person's own sends.
    stack(w, "relaying", 8, "vertical", "card", "center");
    text(w, "relaying_title", "Relaying for " + v.relaying_for, "heading");
    text(w, "relaying_note",
         "Its transfer passes through this computer sealed end to end: this computer cannot read or change it.",
         "secondary");
    button(w, "in_cancel", "Stop relaying", "destructive");
    end(w);
  }
  // What is happening now comes first: an offer or receive, then a send.
  if (v.in != view::in_phase::none) incoming(w, v);
  const bool sending = out_busy(v);
  if (v.out != view::out_phase::idle) outgoing(w, v);
  files_card(w, v, sending);
  nearby(w, v);
  internet(w, v, sending);

  end(w);                      // page
  w.end_array().end_object();  // root
  return w.take();
}

}  // namespace mv::sharepoint
