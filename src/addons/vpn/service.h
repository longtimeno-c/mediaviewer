// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN add-on's engine (docs/plans/vpn-addon.md, owner 2026-11-19): one
// settings page with everything about the VPN, and its menu-bar / tray rows.
//
//   Connect   your VPN server (typed, or a VPN code pasted: Connect to a VPN
//             host…), the Tunnel (a SOCKS5 / HTTP proxy, tunnel.h), Use for
//             all apps (the system proxy, sysproxy.h), Look like a web site,
//             All traffic (VPN, vpn.h) and Block when the tunnel is down.
//   Host      this computer or the claimed server as the VPN's exit, which
//             Phone access's link owns: asked through the host, never
//             directly (service_host::ask_phone, Phone access's vpn_* requests).
//
// Moved out of Driftwire, whose engine ran the Tunnel until 2026-11-19, and
// Phone access, whose card had the Host rows. One control thread starts and
// stops the proxy or the VPN, writes vpn.json, posts the page and the menu,
// and asks Phone access; nothing here runs on the caller's (the host's
// worker's) thread but reading what was clicked.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "addons/vpn/prefs.h"
#include "addons/vpn/sysproxy.h"
#include "addons/vpn/tunnel.h"
#include "addons/vpn/vpn.h"
#include "wire/secrets.h"

namespace mv::driftwire::vpn {

// What the page and the menu show. Pure data; build_* below write the trees.
struct view {
  // ---- Connect
  std::string server;            // "Your VPN server", as typed (it holds the key)
  std::string server_shown;      // its address, without the key ("" when none)
  bool connect_open = false;     // the VPN code's field shows
  std::string connect_note;      // what the last code did, or why not
  bool tunnel = false;           // the switch (node "tunnel", also the menu row)
  std::string tunnel_address;    // "127.0.0.1:<port>" while the proxy listens, else ""
  std::string tunnel_note;       // why it does not run, or the server's last problem
  std::uint32_t tunnel_open = 0;     // connections through it now
  std::uint64_t tunnel_streams = 0;  // made this run (tests)
  bool all_apps = true;          // Use for all apps
  bool system = false;           // the system proxy points at the Tunnel now
  std::string system_note;
  std::string mode = "proxy";    // "proxy" or "vpn" (All traffic runs)
  bool tls = false, tls_available = false;
  bool all_traffic = false, all_traffic_available = false;
  std::string all_traffic_note;
  bool block = false, block_available = false;
  // ---- Host: Phone access's answer (vpn_state)
  bool phone = false;            // Phone access answered
  std::string host_where;        // "here", "server" or "none"
  std::string host_title, host_line, host_code, host_error;
  bool host_on = false, host_busy = false;
};

// The settings page (contribution API 8, place "settings"). Pure; tested.
[[nodiscard]] std::string build_settings_tree(const view& v);
// The menu-bar item / tray icon's rows (host table keep_menu) while the
// Tunnel is on: its switch and All traffic, under the page's ids. Pure; tested.
[[nodiscard]] std::string build_menu(const view& v);
// The kept item's first line ("VPN through <server>"), at most 120 bytes.
[[nodiscard]] std::string kept_label(const view& v);

class service_host {
 public:
  virtual ~service_host() = default;
  // The page's tree, while it shows (a no-op when it does not).
  virtual void post_settings(const std::string& tree_json) = 0;
  [[nodiscard]] virtual bool settings_shown() = 0;
  // Never a path, a name, an address or a key (rule 6).
  virtual void log(int level, const char* ascii) = 0;
  // While the Tunnel is on: this process stays, with a menu-bar item / tray
  // icon, after every window closes. False when the host did not take it (not
  // attached yet): the service tries again.
  virtual bool keep_alive(bool on, const std::string& label) {
    (void)on;
    (void)label;
    return true;
  }
  virtual bool post_menu(const std::string& menu_json) {
    (void)menu_json;
    return true;
  }
  // Phone access's answer to one of its vpn_* requests (docs/plans/vpn-addon.md),
  // through the host's addon_request; nullopt when it is not installed, too
  // old or did not answer. Blocks (at most the host's 30 s): the control
  // thread's call only.
  [[nodiscard]] virtual std::optional<std::string> ask_phone(const std::string& method, const std::string& json) {
    (void)method;
    (void)json;
    return std::nullopt;
  }
};

class service {
 public:
  struct options {
    std::string prefs_path;  // "" keeps nothing (the tests)
    std::string data_dir;    // the system proxy's record; "" keeps it in memory
    std::optional<prefs> initial;
    bool offline = false;  // the loader test: nothing starts
    std::function<std::unique_ptr<secrets::store>()> make_secrets;
    std::function<std::unique_ptr<sysproxy::port>()> make_sysproxy;
    std::function<std::unique_ptr<os_port>()> make_vpn;
    std::uint16_t tls_port = 0;  // the tests: the server's TLS port (0: the prefs')
    // How often Phone access is asked again while the page shows (and sooner
    // while it is busy changing the VPN).
    std::chrono::milliseconds phone_every{3000};
    std::chrono::milliseconds phone_busy_every{500};
  };

  service(service_host& host, options o);
  ~service();
  service(const service&) = delete;
  service& operator=(const service&) = delete;

  void settings_opened();
  void settings_closed();
  // A control on the page or a menu row (screen::event_kind, the value as JSON).
  void on_event(const std::string& node, std::uint32_t kind, const std::string& value_json);

  // ---- for tests
  [[nodiscard]] view snapshot() const;
  // Blocks until the control thread has done what was asked before this call.
  void settle();

 private:
  void control_loop();
  void request(bool reconcile, bool save = false);
  void save_now();
  void post();
  void post_throttled();
  void sync_tunnel();
  void sync_system_proxy(std::uint16_t port, bool want);
  bool sync_vpn(bool on, const std::string& relay, bool tls, std::uint16_t tls_port, bool block);
  void tunnel_changed();
  void sync_keep();
  void ask_phone_now();
  void write_proxy_record();
  void log_status(const char* what, status s);

  service_host& host_;
  options opts_;

  mutable std::mutex m_;
  view v_;                        // under m_
  std::uint16_t tunnel_port_ = 0;  // under m_: the port to try first (kept in the prefs)
  std::uint16_t tls_port_ = 443;   // under m_: the prefs'
  std::string typed_code_;         // under m_: the VPN code as typed (it holds a key), wiped after use

  std::mutex secrets_m_;
  std::unique_ptr<secrets::store> secrets_;

  // The proxy or the VPN, made and dropped on the control thread; read by
  // tunnel_changed under tunnel_m_, since their threads report through it.
  std::mutex tunnel_m_;
  std::unique_ptr<tunnel::proxy> proxy_;  // under tunnel_m_
  std::string proxy_for_;                 // under tunnel_m_
  std::unique_ptr<runner> vpn_run_;       // under tunnel_m_
  std::string vpn_for_;                   // under tunnel_m_
  std::unique_ptr<os_port> vpn_port_;

  // The system proxy (the control thread's, and the destructor's after it).
  std::unique_ptr<sysproxy::port> sysproxy_;
  std::optional<sysproxy::record> proxy_record_;
  std::atomic<bool> proxy_retry_{false};
  std::string proxy_gave_up_for_, proxy_gave_up_note_;

  // The control thread and what it was asked.
  std::mutex control_m_;
  std::condition_variable control_cv_;
  std::thread control_thread_;
  std::atomic<bool> stopping_{false};
  bool reconcile_wanted_ = false, save_wanted_ = false, post_pending_ = false;  // under control_m_
  bool phone_wanted_ = false;                                                  // under control_m_
  // Under control_m_: what was clicked for Phone access, in order ("vpn_set"
  // with its value, "vpn_code", "vpn_code_hide"), sent by the control thread.
  std::vector<std::pair<std::string, bool>> phone_actions_;
  std::uint64_t asked_ = 0, done_ = 0;       // under control_m_ (settle)
  std::chrono::steady_clock::time_point post_due_{}, phone_due_{}, keep_due_{};  // under control_m_
  std::atomic<std::chrono::steady_clock::time_point> last_post_{};
  std::mutex post_m_;

  // The kept item and its menu, as the host last took them (control thread).
  bool keep_shown_ = false;
  std::string keep_label_shown_, menu_shown_;
  int keep_tries_ = 0;
};

}  // namespace mv::driftwire::vpn
