// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// build_tree: the Sharepoint window as a view tree (docs/design/25 §10). Both
// hosts draw it with their own controls, so it follows the theme, text size,
// VoiceOver and Narrator; every control carries a label.
#include <algorithm>
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

void separator(writer& w, const char* id) { w.begin_object().key("type").string("separator").key("id").string(id).end_object(); }

void progress(writer& w, const char* id, const char* label, std::uint64_t done, std::uint64_t total) {
  const double v = total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : -1.0;
  w.begin_object().key("type").string("progress").key("id").string(id).key("label").string(label);
  w.key("value").number(v).end_object();
}

void folder_field(writer& w, const char* id, const char* label, const std::string& value, const char* placeholder) {
  w.begin_object().key("type").string("text_field").key("id").string(id).key("label").string(label);
  w.key("value").string(value).key("placeholder").string(placeholder).key("pick").string("folder").end_object();
}

void row(writer& w, const char* id) {
  w.begin_object().key("type").string("stack").key("id").string(id).key("axis").string("horizontal");
  w.key("spacing").integer(8).key("children").begin_array();
}

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

// Why Send waits for Wi-Fi, and what to do about it.
std::string wifi_hint(const view& v, const std::string& peer) {
  const std::string why = "These files are " + size_text(v.file_bytes) +
                          " and Bluetooth carries 32 MB at most. ";
  if (!v.wifi_up) {
    return why + "This computer is not on a network: turn on Wi-Fi here and join the same network as " + peer +
           ". Send waits, and starts as soon as " + peer + " is there.";
  }
  return why + peer + " is only in Bluetooth range: put both computers on the same Wi-Fi network. Send waits, " +
         "and starts as soon as " + peer + " is there.";
}

std::string moved_text(std::uint32_t files, std::uint32_t count, std::uint64_t bytes, std::uint64_t total) {
  return std::to_string(files) + " of " + std::to_string(count) + " files, " + size_text(bytes) + " of " +
         size_text(total);
}

}  // namespace

std::string build_tree(const view& v) {
  writer w;
  w.begin_object().key("type").string("scroll").key("id").string("root").key("children").begin_array();
  w.begin_object().key("type").string("stack").key("id").string("page").key("spacing").integer(12);
  w.key("align").string("fill").key("children").begin_array();

  text(w, "intro",
       "Nearby MediaViewers see this computer as \xE2\x80\x9C" + v.my_name +
           "\xE2\x80\x9D while this window is open. Nothing moves until the person on the other side accepts.",
       "secondary");
  w.begin_object().key("type").string("toggle").key("id").string("receiving");
  w.key("label").string("Keep receiving when this window is closed").key("value").boolean(v.receiving).end_object();
  {
    std::string where = v.wifi_up ? "Looking on the local network" : "The local network is not available";
    where += v.bluetooth_note.empty() ? (v.wifi_up ? " and Bluetooth." : "; looking on Bluetooth.")
                                      : ". " + v.bluetooth_note;
    text(w, "presence", where, "caption");
  }

  // ---- an offer to this computer
  if (v.in != view::in_phase::none) {
    separator(w, "in_sep");
    text(w, "in_title", "From " + v.in_peer, "heading");
    switch (v.in) {
      case view::in_phase::connecting:
        text(w, "in_code", "Code " + v.in_code, "title");
        text(w, "in_check",
             v.in_peer + " is checking that this code is on its screen too. The files come next. Over " + v.in_via +
                 ".",
             "secondary");
        button(w, "decline", "Decline", "plain");
        break;
      case view::in_phase::asking:
        text(w, "in_files", count_text(v.in_count, v.in_total) + ": " + names_line(v.in_names, v.in_count));
        text(w, "in_code", "Code " + v.in_code, "title");
        text(w, "in_check",
             "Accept only if " + v.in_peer + " shows the same code. Over " + v.in_via + ".", "secondary");
        folder_field(w, "destination", "Save to", v.destination, "Choose a folder");
        row(w, "in_actions");
        button(w, "accept", "Accept", "primary", v.destination.empty());
        button(w, "decline", "Decline", "plain");
        end(w);
        break;
      case view::in_phase::receiving:
        text(w, "in_code", "Code " + v.in_code, "secondary");
        progress(w, "in_progress", "Receiving", v.in_bytes, v.in_total);
        text(w, "in_moved", moved_text(v.in_files, static_cast<std::uint32_t>(v.in_count), v.in_bytes, v.in_total),
             "secondary");
        button(w, "in_cancel", "Cancel", "destructive");
        break;
      case view::in_phase::done:
      case view::in_phase::ended:
        text(w, "in_note", v.in_note);
        button(w, "in_dismiss", "OK", "plain");
        break;
      case view::in_phase::none:
        break;
    }
  }

  // ---- what this computer sends
  separator(w, "send_sep");
  text(w, "send_title", "Send", "heading");
  if (v.reading_folder) {
    text(w, "files_summary", "Reading the folder\xE2\x80\xA6", "secondary");
  } else if (v.file_count == 0) {
    text(w, "files_summary", "Nothing to send yet. Select photos in the gallery, or pick a folder.", "secondary");
  } else {
    text(w, "files_summary", count_text(v.file_count, v.file_bytes));
    w.begin_object().key("type").string("list").key("id").string("files").key("label").string("Files to send");
    w.key("items").begin_array();
    for (std::size_t i = 0; i < v.file_names.size(); ++i) {
      char id[24];
      std::snprintf(id, sizeof id, "f%zu", i);
      w.begin_object().key("id").string(id).key("text").string(v.file_names[i]).end_object();
    }
    w.end_array().end_object();
    if (v.file_count > v.file_names.size()) {
      text(w, "files_more", "and " + std::to_string(v.file_count - v.file_names.size()) + " more", "caption");
    }
  }
  const bool sending = v.out == view::out_phase::waiting_wifi || v.out == view::out_phase::connecting ||
                       v.out == view::out_phase::confirming || v.out == view::out_phase::waiting ||
                       v.out == view::out_phase::sending;
  row(w, "file_actions");
  folder_field(w, "send_folder", "Send a folder", "", "Pick a folder to send");
  button(w, "clear", "Clear", "plain", v.file_count == 0 || sending);
  end(w);

  text(w, "peers_title", "Nearby", "heading");
  if (v.peers.empty()) {
    text(w, "peers_none", "Looking for MediaViewers nearby. Open Sharepoint on the other computer too.", "secondary");
  } else {
    w.begin_object().key("type").string("list").key("id").string("peers").key("label").string("Nearby MediaViewers");
    w.key("items").begin_array();
    for (const auto& p : v.peers) {
      w.begin_object().key("id").string(p.id).key("text").string(p.name).key("detail").string(p.via).end_object();
    }
    w.end_array();
    w.key("selection").begin_array();
    if (!v.selected_peer.empty()) w.string(v.selected_peer);
    w.end_array().end_object();
  }
  std::string target;
  for (const auto& p : v.peers) {
    if (p.id == v.selected_peer) target = p.name;
  }
  // Too big for Bluetooth and the peer is only in Bluetooth range: say what
  // to do before Send, and let Send wait for the peer on Wi-Fi.
  const bool over_wifi = needs_wifi(v);
  if (over_wifi && !sending) {
    text(w, "send_hint", wifi_hint(v, target), "secondary");
  }
  button(w, "send",
         target.empty() ? std::string("Send") : over_wifi ? "Send to " + target + " over Wi-Fi" : "Send to " + target,
         "primary", sending || target.empty() || v.file_count == 0 || v.reading_folder);

  switch (v.out) {
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
      text(w, "out_note", "Connecting to " + v.out_peer + "\xE2\x80\xA6");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::confirming:
      text(w, "out_code", "Code " + v.out_code, "title");
      text(w, "out_note",
           "Check that " + v.out_peer + " shows this code too. If it does not, cancel: another computer is "
           "answering. Nothing has been sent yet.",
           "secondary");
      row(w, "out_actions");
      button(w, "out_confirm", "Same code, send", "primary");
      button(w, "out_cancel", "Cancel", "destructive");
      end(w);
      break;
    case view::out_phase::waiting:
      text(w, "out_code", "Code " + v.out_code, "title");
      text(w, "out_note", "Waiting for " + v.out_peer + " to accept.", "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::sending:
      text(w, "out_code", "Code " + v.out_code, "secondary");
      progress(w, "out_progress", "Sending", v.out_bytes, v.out_total);
      text(w, "out_moved", moved_text(v.out_files, v.out_count, v.out_bytes, v.out_total), "secondary");
      button(w, "out_cancel", "Cancel", "destructive");
      break;
    case view::out_phase::done:
    case view::out_phase::ended:
      text(w, "out_note", v.out_note);
      break;
    case view::out_phase::idle:
      break;
  }

  w.end_array().end_object();  // page
  w.end_array().end_object();  // root
  return w.take();
}

}  // namespace mv::sharepoint
