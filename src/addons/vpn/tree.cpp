// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// build_settings_tree: the VPN add-on's page in Settings -> Add-ons as a view
// tree (docs/design/25 §10), drawn by both hosts with their own controls, so
// it follows the theme, text size, VoiceOver and Narrator. Two cards:
// Connect (this computer's traffic through your server) and Host (this
// computer or your server as the exit, Phone access's link). build_menu: the
// menu-bar item / tray icon's rows while the Tunnel is on.
#include <string>

#include "addons/vpn/service.h"
#include "core/json.h"

namespace mv::driftwire::vpn {

namespace {

using json::writer;

void text(writer& w, const char* id, const std::string& s, const char* style = "body") {
  w.begin_object().key("type").string("text").key("id").string(id).key("text").string(s.empty() ? " " : s);
  w.key("style").string(style).end_object();
}

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

void copy_button(writer& w, const char* id, const std::string& label, const char* style, const std::string& copy) {
  w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label);
  w.key("style").string(style).key("copy").string(copy).end_object();
}

void toggle(writer& w, const char* id, const std::string& label, bool value, bool disabled = false) {
  w.begin_object().key("type").string("toggle").key("id").string(id).key("label").string(label);
  w.key("value").boolean(value);
  if (disabled) w.key("disabled").boolean(true);
  w.end_object();
}

void stack(writer& w, const char* id, int spacing, const char* axis = "vertical", const char* look = nullptr,
           const char* align = nullptr) {
  w.begin_object().key("type").string("stack").key("id").string(id).key("axis").string(axis);
  w.key("spacing").integer(spacing);
  if (look) w.key("look").string(look);
  if (align) w.key("align").string(align);
  w.key("children").begin_array();
}

void end(writer& w) { w.end_array().end_object(); }

std::string through(const view& v) {
  std::string t = v.server_shown.empty() ? std::string("your VPN server") : v.server_shown;
  if (t.size() > 64) t.resize(64);
  return t;
}

std::string tunnel_label(const view& v) {
  return "Tunnel through " + (v.server_shown.empty() ? std::string("your VPN server") : v.server_shown);
}

// A label cut at a character to at most `max` bytes, with an ellipsis.
std::string cut(std::string s, std::size_t max) {
  if (s.size() <= max) return s;
  std::size_t n = max - 3;
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  return s.substr(0, n) + "\xE2\x80\xA6";
}

void path_node(writer& w, const view& v, const std::string& label, const char* to, const std::string& caption) {
  const char* state = v.tunnel_open > 0 ? "moving" : !v.tunnel_note.empty() ? "failed" : "done";
  w.begin_object().key("type").string("link").key("id").string("tunnel_path");
  w.key("label").string(label + " " + caption + ".");
  w.key("via").string("tunnel").key("state").string(state).key("direction").string("send");
  w.key("from").string("This computer").key("to").string(to).key("through").string(through(v));
  w.key("caption").string(caption).end_object();
}

// The switch (node "tunnel": the menu row has the same id), and while it is
// on, the path, the proxy's address with Copy, and how to use it.
void tunnel_controls(writer& w, const view& v) {
  toggle(w, "tunnel", tunnel_label(v), v.tunnel);
  if (!v.tunnel) {
    text(w, "tunnel_note",
         v.server.empty() ? "For a network that blocks sites, or away from home: your traffic leaves through your own "
                            "server. Set your VPN server first."
                          : "For a network that blocks sites, or away from home: your traffic leaves through your own "
                            "server, sealed on the way there.",
         "caption");
    return;
  }
  if (!v.tunnel_note.empty()) text(w, "tunnel_problem", v.tunnel_note, "secondary");
  // Every app, not only those that follow the proxy (vpn.h).
  if (v.all_traffic_available) {
    toggle(w, "all_traffic", "All traffic (VPN)", v.all_traffic);
    if (v.all_traffic && v.block_available) toggle(w, "block", "Block when the tunnel is down", v.block);
  } else if (!v.all_traffic_note.empty()) {
    text(w, "all_traffic_note", v.all_traffic_note, "caption");
  }
  if (v.mode == "vpn") {
    // The VPN carries everything: no proxy address, no system proxy.
    const std::string caption =
        "All traffic through your VPN server" +
        (v.tunnel_open > 0 ? ": " + std::to_string(v.tunnel_open) + (v.tunnel_open == 1 ? " connection" : " connections")
                           : std::string());
    path_node(w, v, "All of this computer's traffic goes through " + through(v) + ".", "The internet", caption);
    text(w, "all_traffic_how",
         v.block ? "Every app goes through it, apart from this network's own devices. If it goes down, nothing "
                   "reaches the internet until it is back or you turn this off."
                 : "Every app goes through it, apart from this network's own devices. If it goes down, apps use the "
                   "network as it is.",
         "caption");
    return;
  }
  // The server's TLS port, for networks that block what does not look like a
  // web site. Only in a build with TLS.
  if (v.tls_available) {
    toggle(w, "tls", "Look like a web site (port 443)", v.tls);
    if (v.tls) {
      text(w, "tls_note",
           "Your server needs its relay's web-site port for this (and ideally a real certificate). To anyone "
           "without your key it looks like an ordinary web site.",
           "caption");
    }
  }
  toggle(w, "all_apps", "Use for all apps", v.all_apps);
  if (v.all_apps && v.system_asking) {
    text(w, "system_note", "Waiting for you to allow the proxy change\xE2\x80\xA6", "caption");
  } else if (v.all_apps && !v.system_note.empty()) {
    text(w, "system_note", v.system_note, "caption");
  }
  if (v.tunnel_address.empty()) return;
  const std::string caption =
      v.tunnel_open > 0 ? "Through your VPN server: " + std::to_string(v.tunnel_open) +
                              (v.tunnel_open == 1 ? " connection" : " connections")
                        : std::string("Through your VPN server: ready");
  path_node(w, v, "This computer's web traffic goes through " + through(v) + ".", "The web", caption);
  stack(w, "tunnel_row", 8, "horizontal");
  copyable(w, "tunnel_address", v.tunnel_address, "body");
  copy_button(w, "copy_tunnel", "Copy", "plain", v.tunnel_address);
  end(w);
  text(w, "tunnel_how",
       std::string(v.system ? "For apps that ignore the system setting: " : "In your apps: ") +
           "SOCKS5 or HTTP proxy " + v.tunnel_address + " (in a terminal, HTTPS_PROXY=http://" + v.tunnel_address +
           "). Not carried: apps that ignore proxies, and anything over UDP. The sites see your server's address; "
           "the server sees which sites, never what you send.",
       "caption");
}

void connect_card(writer& w, const view& v) {
  stack(w, "connect_card", 10, "vertical", "card", "fill");
  text(w, "connect_title", "Connect", "heading");
  text(w, "connect_intro",
       v.server_shown.empty()
           ? "Away from home, send this computer's traffic through your own server: a Windows PC, a Mac or a "
             "Synology running MediaViewer, or a relay server you run."
           : "Your VPN server is " + v.server_shown + ".",
       "caption");
  if (v.connect_open) {
    text(w, "connect_how",
         "On your server, in MediaViewer: Settings \xE2\x86\x92 Add-ons \xE2\x86\x92 VPN, under Host: Use as VPN, "
         "then VPN code\xE2\x80\xA6 Copy the code and paste it here.",
         "caption");
    w.begin_object().key("type").string("text_field").key("id").string("connect_code").key("label").string("VPN code");
    w.key("value").string("").key("secure").boolean(true).key("placeholder").string("mediaviewer://relay#\xE2\x80\xA6");
    w.end_object();
    stack(w, "connect_buttons", 8, "horizontal");
    button(w, "connect_go", "Connect", "primary");
    button(w, "connect_cancel", "Cancel", "plain");
    end(w);
  } else {
    button(w, "connect_open",
           v.server_shown.empty() ? "Connect to a VPN host\xE2\x80\xA6" : "Connect to another\xE2\x80\xA6", "plain");
  }
  if (!v.connect_note.empty()) text(w, "connect_note", v.connect_note, "secondary");
  // The server typed by hand, for a relay server of one's own (tools/driftwire-relay):
  // a secure field (the key), never shown back; the intro names the address.
  if (v.server.empty() || v.server_editing) {
    w.begin_object().key("type").string("text_field").key("id").string("server");
    w.key("label").string("Your VPN server").key("value").string("").key("secure").boolean(true);
    w.key("placeholder").string("address:port#key, as your relay server prints it").end_object();
    if (v.server_editing) {
      stack(w, "server_buttons", 8, "horizontal");
      button(w, "server_done", "Done", "plain");
      end(w);
    }
  } else {
    stack(w, "server_buttons", 8, "horizontal");
    button(w, "server_change", "Change server\xE2\x80\xA6", "plain");
    button(w, "server_clear", "Remove", "plain");
    end(w);
  }
  tunnel_controls(w, v);
  end(w);
}

void host_card(writer& w, const view& v) {
  stack(w, "host_card", 10, "vertical", "card", "fill");
  text(w, "host_heading", "Host", "heading");
  if (!v.phone) {
    text(w, "host_none",
         "To use this computer or your server as your laptop's VPN, add Phone access in Settings \xE2\x86\x92 "
         "Add-ons: it sets up and runs the server.",
         "caption");
  } else if (v.host_where != "here" && v.host_where != "server") {
    text(w, "host_none",
         "To use this computer or your server as your laptop's VPN, set it up in Settings \xE2\x86\x92 Add-ons \xE2\x86\x92 "
         "Phone access: "
         "this computer as your server, or a server that offers a VPN.",
         "caption");
  } else {
    text(w, "host_title", v.host_title, "body");
    text(w, "host_line", v.host_line, "caption");
    toggle(w, "host_on", "Use as VPN", v.host_on, v.host_busy);
    if (v.host_busy) {
      text(w, "host_busy", "Changing the VPN\xE2\x80\xA6", "secondary");
    } else if (v.host_on && v.host_code.empty()) {
      button(w, "host_code", "VPN code\xE2\x80\xA6", "plain");
    }
    if (v.host_on && !v.host_code.empty()) {
      stack(w, "host_code_box", 8);
      text(w, "host_code_note",
           "On your laptop, in MediaViewer: Settings \xE2\x86\x92 Add-ons \xE2\x86\x92 VPN \xE2\x86\x92 Connect to a VPN "
           "host\xE2\x80\xA6, and paste this. It holds the relay's key: keep it to yourself.",
           "secondary");
      copyable(w, "host_code_text", v.host_code, "mono");
      button(w, "host_code_hide", "Hide", "plain");
      end(w);
    }
  }
  if (!v.host_error.empty()) text(w, "host_error", v.host_error, "secondary");
  end(w);
}

void menu_toggle(writer& w, const char* id, const std::string& label, bool value) {
  w.begin_object().key("kind").string("toggle").key("id").string(id).key("label").string(label);
  w.key("value").boolean(value).end_object();
}

}  // namespace

std::string build_settings_tree(const view& v) {
  writer w;
  w.begin_object().key("type").string("stack").key("id").string("root").key("axis").string("vertical");
  w.key("spacing").integer(16).key("align").string("fill").key("children").begin_array();
  connect_card(w, v);
  host_card(w, v);
  w.end_array().end_object();
  return w.take();
}

std::string kept_label(const view& v) {
  return cut((v.mode == "vpn" ? "VPN: all traffic through " : "VPN: Tunnel through ") + through(v), 120);
}

std::string build_menu(const view& v) {
  writer w;
  w.begin_object().key("items").begin_array();
  if (!v.tunnel_note.empty()) {
    w.begin_object().key("kind").string("text").key("label").string(cut(v.tunnel_note, 120)).end_object();
  }
  menu_toggle(w, "tunnel", cut(tunnel_label(v), 100), v.tunnel);
  if (v.all_traffic_available) menu_toggle(w, "all_traffic", "All Traffic (VPN)", v.all_traffic);
  w.end_array().end_object();
  return w.take();
}

}  // namespace mv::driftwire::vpn
