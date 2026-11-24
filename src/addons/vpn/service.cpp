// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/vpn/service.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <span>

#include "core/json.h"
#include "io/file_port.h"
#include "wire/relay_field.h"
#include "wire/serve.h"
#include "wire/tls.h"

namespace mv::driftwire::vpn {

namespace {
// Space around what was typed, gone.
std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.remove_suffix(1);
  return std::string(s);
}
}  // namespace

namespace {

constexpr auto kPostEvery = std::chrono::milliseconds(100);
constexpr std::uint32_t kClick = 1, kChange = 2, kSubmit = 3;

void wipe(std::string& s) {
  if (!s.empty()) sodium_memzero(s.data(), s.size());
  s.clear();
}

}  // namespace

service::service(service_host& host, options o) : host_(host), opts_(std::move(o)) {
  if (sodium_init() < 0) {}  // safe to repeat
  if (opts_.make_secrets) secrets_ = opts_.make_secrets();
  const prefs kept = opts_.initial ? *opts_.initial : load_prefs(opts_.prefs_path).value_or(prefs{});
  v_.server = kept.server;
  // The key is in the secret store, the address in the file (one written
  // without a store keeps both, until the next save).
  if (secrets_ && !v_.server.empty() && v_.server.find('#') == std::string::npos) {
    if (auto k = secrets_->get(kServerKeyName)) {
      v_.server += '#';
      v_.server.append(k->begin(), k->end());
      sodium_memzero(k->data(), k->size());
    }
  }
  v_.server_shown = tunnel::relay_shown(v_.server);
  v_.tunnel = kept.tunnel;
  tunnel_port_ = kept.tunnel_port;
  v_.all_apps = kept.all_apps;
  v_.tls = kept.tls;
  v_.tls_available = tls::available();
  tls_port_ = kept.tls_port;
  v_.all_traffic = kept.all_traffic;
  v_.block = kept.block;
  if (opts_.make_vpn) vpn_port_ = opts_.make_vpn();
  if (vpn_port_) {
    v_.all_traffic_note = vpn_port_->unavailable();  // cheap by contract: no prompt, no wait
    v_.all_traffic_available = v_.all_traffic_note.empty();
    v_.block_available = vpn_port_->block_unavailable().empty();
  }
  if (opts_.make_sysproxy) sysproxy_ = opts_.make_sysproxy();
  // A setting this add-on (or Driftwire's Tunnel before it) made and did not
  // undo (a crash, a power cut): the first reconcile puts it back.
  if (!opts_.data_dir.empty()) {
    proxy_record_ = sysproxy::load_record(io::join_path(opts_.data_dir, kProxyRecordFile));
  }
  if (sysproxy_) proxy_thread_ = std::thread([this] { proxy_loop(); });
  control_thread_ = std::thread([this] { control_loop(); });
  if (v_.tunnel || proxy_record_) request(true);
}

service::~service() {
  {
    std::lock_guard lock(control_m_);
    stopping_.store(true);
    control_cv_.notify_all();
  }
  // A start waiting for the system (the Mac asks the person to allow the
  // extension) gives up now, so quitting never waits minutes for it.
  if (vpn_port_) vpn_port_->cancel();
  if (control_thread_.joinable()) control_thread_.join();
  {
    // A change in progress finishes (a dialog showing waits for its answer).
    std::lock_guard lock(proxy_m_);
    proxy_stop_ = true;
    proxy_want_.reset();
    proxy_cv_.notify_all();
  }
  if (proxy_thread_.joinable()) proxy_thread_.join();
  // The proxy or the VPN (the control thread has stopped: nothing starts
  // them again). Destroyed outside tunnel_m_: their threads take it to report.
  std::unique_ptr<tunnel::proxy> p;
  std::unique_ptr<runner> r;
  {
    std::lock_guard lock(tunnel_m_);
    p = std::move(proxy_);
    r = std::move(vpn_run_);
  }
  p.reset();
  if (r) {
    r.reset();  // every stream, then the interface, its routes and DNS
    if (vpn_port_) vpn_port_->stop();
  }
  // Quitting: the computer's proxy setting goes back to what it was.
  apply_system_proxy(0, false);
  if (keep_shown_) (void)host_.keep_alive(false, {});
  bool save = false;
  {
    std::lock_guard lock(control_m_);
    save = save_wanted_;
  }
  if (save) save_now();  // a change the control thread had not written yet
  std::lock_guard lock(m_);
  wipe(typed_code_);
  wipe(typed_server_);
  wipe(v_.server);
  wipe(v_.host_code);
}

void service::log_status(const char* what, status s) {
  char text[160];
  std::snprintf(text, sizeof text, "vpn: %s (%s)", what, status_name(s));
  host_.log(1, text);
}

// ---- the control thread ---------------------------------------------------------------

void service::request(bool reconcile, bool save) {
  std::lock_guard lock(control_m_);
  if (reconcile) reconcile_wanted_ = true;
  if (save && !opts_.prefs_path.empty()) save_wanted_ = true;
  ++asked_;
  control_cv_.notify_all();
}

void service::settle() {
  std::unique_lock lock(control_m_);
  const std::uint64_t want = ++asked_;
  control_cv_.notify_all();
  control_cv_.wait(lock, [&] { return done_ >= want || stopping_.load(); });
}

void service::post_throttled() {
  std::lock_guard lock(control_m_);
  if (!post_pending_) {
    post_pending_ = true;
    post_due_ = std::max(std::chrono::steady_clock::now(), last_post_.load() + kPostEvery);
    control_cv_.notify_all();
  }
}

void service::control_loop() {
  using clock = std::chrono::steady_clock;
  const clock::time_point none{};
  std::unique_lock lock(control_m_);
  while (!stopping_.load()) {
    clock::time_point due = none;
    for (const auto t : {post_pending_ ? post_due_ : none, phone_due_, keep_due_}) {
      if (t != none) due = due == none ? t : std::min(due, t);
    }
    const auto woken = [&] {
      return stopping_.load() || reconcile_wanted_ || save_wanted_ || phone_wanted_ || !phone_actions_.empty() ||
             done_ < asked_;
    };
    if (due != none) {
      control_cv_.wait_until(lock, due, woken);
    } else {
      control_cv_.wait(lock, [&] { return woken() || post_pending_; });
    }
    if (stopping_.load()) break;
    const std::uint64_t serving = asked_;
    const auto now = clock::now();
    if (save_wanted_) {
      lock.unlock();
      save_now();
      lock.lock();
    }
    if (reconcile_wanted_) {
      reconcile_wanted_ = false;
      lock.unlock();
      sync_tunnel();
      sync_keep();
      post();
      lock.lock();
    }
    if (keep_due_ != none && now >= keep_due_) {
      keep_due_ = none;
      lock.unlock();
      sync_keep();
      lock.lock();
    }
    // Phone access: what was clicked first, then a fresh look.
    bool refused = false;
    if (!phone_actions_.empty()) {
      const auto actions = std::move(phone_actions_);
      phone_actions_.clear();
      lock.unlock();
      for (const auto& a : actions) {
        json::writer w;
        w.begin_object().key("on").boolean(a.on);
        if (!a.domain.empty()) w.key("domain").string(a.domain);
        w.end_object();
        if (!host_.ask_phone(a.method, w.take())) refused = true;
      }
      lock.lock();
      phone_wanted_ = true;
    }
    if (phone_wanted_ || (phone_due_ != none && now >= phone_due_)) {
      phone_wanted_ = false;
      phone_due_ = none;
      lock.unlock();
      ask_phone_now();
      if (refused) {
        // After the fresh look, which would otherwise clear it.
        {
          std::lock_guard held(m_);
          v_.host_error = "Phone access didn't take that. Try again in a moment.";
        }
        post();
      }
      lock.lock();
    }
    if (post_pending_ && clock::now() >= post_due_) {
      post_pending_ = false;
      lock.unlock();
      post();
      sync_keep();  // the menu's label and rows follow (a mode or a problem changed)
      lock.lock();
    }
    done_ = std::max(done_, serving);
    control_cv_.notify_all();
  }
  done_ = asked_;
  control_cv_.notify_all();
}

void service::save_now() {
  // The flag first: a change made while this copies asks again, and is written next.
  {
    std::lock_guard lock(control_m_);
    save_wanted_ = false;
  }
  prefs p;
  {
    std::lock_guard lock(m_);
    p.server = v_.server;
    p.tunnel = v_.tunnel;
    p.tunnel_port = tunnel_port_;
    p.all_apps = v_.all_apps;
    p.tls = v_.tls;
    p.tls_port = tls_port_;
    p.all_traffic = v_.all_traffic;
    p.block = v_.block;
  }
  // The key after '#' goes to the secret store and the file keeps the
  // address; with no store (the portable build) the file keeps both.
  {
    std::lock_guard s(secrets_m_);
    if (secrets_) {
      const auto hash = p.server.rfind('#');
      if (hash != std::string::npos) {
        const std::span<const std::uint8_t> key(reinterpret_cast<const std::uint8_t*>(p.server.data()) + hash + 1,
                                                p.server.size() - hash - 1);
        auto kept = secrets_->get(kServerKeyName);
        const bool same = kept && std::equal(kept->begin(), kept->end(), key.begin(), key.end());
        if (kept) sodium_memzero(kept->data(), kept->size());
        if (same || secrets_->put(kServerKeyName, key)) {
          sodium_memzero(p.server.data() + hash, p.server.size() - hash);
          p.server.resize(hash);
        }
      } else {
        (void)secrets_->erase(kServerKeyName);
      }
    }
  }
  if (!save_prefs(opts_.prefs_path, p)) host_.log(1, "vpn: could not keep the settings");
  wipe(p.server);
}

void service::post() {
  std::lock_guard serial(post_m_);
  last_post_ = std::chrono::steady_clock::now();
  if (!host_.settings_shown()) return;
  std::string tree;
  {
    std::lock_guard lock(m_);
    tree = build_settings_tree(v_);
  }
  host_.post_settings(tree);
}

// The kept menu-bar item / tray icon: while the Tunnel is on, so closing every
// window does not take the VPN down with it.
void service::sync_keep() {
  bool keep = false;
  std::string label, menu;
  {
    std::lock_guard lock(m_);
    keep = v_.tunnel && !stopping_.load();
    if (keep) {
      label = kept_label(v_);
      menu = build_menu(v_);
    }
  }
  bool taken = true;
  if (keep != keep_shown_ || (keep && label != keep_label_shown_)) {
    if (host_.keep_alive(keep, label)) {
      keep_shown_ = keep;
      keep_label_shown_ = keep ? label : std::string();
      if (!keep) menu_shown_.clear();
    } else {
      taken = false;
    }
  }
  if (taken && keep && menu != menu_shown_) {
    if (host_.post_menu(menu)) {
      menu_shown_ = menu;
    } else {
      taken = false;
    }
  }
  std::lock_guard lock(control_m_);
  if (taken) {
    keep_tries_ = 0;
    keep_due_ = {};
    return;
  }
  // The add-on loads before the host attaches its items: again shortly, for a while.
  keep_due_ = ++keep_tries_ < 40 ? std::chrono::steady_clock::now() + std::chrono::milliseconds(250)
                                 : std::chrono::steady_clock::time_point{};
}

// ---- Phone access (the Host section) ------------------------------------------------------

void service::ask_phone_now() {
  const auto answer = host_.ask_phone("vpn_state", "{}");
  const auto doc = answer ? json::parse(*answer, 3) : std::nullopt;
  bool busy = false;
  {
    std::lock_guard lock(m_);
    v_.phone = doc && doc->k == json::kind::object;
    const auto s = [&](const char* k) {
      const std::string* p = v_.phone ? doc->str(k) : nullptr;
      return p ? *p : std::string();
    };
    v_.host_where = v_.phone ? s("where") : std::string();
    v_.host_title = s("title");
    v_.host_line = s("line");
    wipe(v_.host_code);
    v_.host_code = s("code");
    v_.host_on = v_.phone && doc->boolean("on").value_or(false);
    v_.host_busy = v_.phone && doc->boolean("busy").value_or(false);
    v_.host_domain = s("domain");
    v_.host_domain_line = s("domain_line");
    if (v_.phone) v_.host_error = s("error");
    busy = v_.host_busy;
  }
  if (answer) {
    std::string a = *answer;
    wipe(a);
  }
  post();
  // While the page shows: again soon while Phone access is changing it.
  if (!host_.settings_shown()) return;
  std::lock_guard lock(control_m_);
  phone_due_ = std::chrono::steady_clock::now() + (busy ? opts_.phone_busy_every : opts_.phone_every);
}

void service::settings_opened() {
  {
    std::lock_guard lock(control_m_);
    phone_wanted_ = true;
    control_cv_.notify_all();
  }
  post();  // the page now, never waiting on the control thread
}

void service::settings_closed() {
  bool code = false;
  {
    std::lock_guard lock(m_);
    v_.connect_open = false;
    v_.connect_note.clear();
    wipe(typed_code_);
    wipe(typed_server_);
    v_.server_editing = false;
    code = !v_.host_code.empty();
    wipe(v_.host_code);
  }
  std::lock_guard lock(control_m_);
  phone_due_ = {};
  // The VPN code shown (it holds the relay's key) goes when the page does.
  if (code) {
    phone_actions_.push_back({"vpn_code_hide", false, {}});
    control_cv_.notify_all();
  }
}

// ---- the Tunnel (tunnel.h) and the VPN (vpn.h) ------------------------------------------

void service::sync_tunnel() {
  std::string relay;
  bool want = false, tls = false;
  std::uint16_t port = 0, tls_port = 0;
  bool vpn_wanted = false, block = false;
  {
    std::lock_guard lock(m_);
    want = v_.tunnel && !stopping_.load() && !opts_.offline;
    relay = v_.server;
    port = tunnel_port_;
    tls = v_.tls && tls::available();
    tls_port = opts_.tls_port ? opts_.tls_port : tls_port_;
    // All traffic chosen and the interface able to start: the VPN runs and
    // the proxy (and the system proxy) step aside.
    vpn_wanted = want && v_.all_traffic && v_.all_traffic_available && !relay.empty();
    block = v_.block && v_.block_available;
  }
  const bool vpn_runs = sync_vpn(vpn_wanted, relay, tls, tls_port, block);
  if (vpn_runs) want = false;
  // What the proxy runs with: a change of any restarts it.
  const std::string made_for = relay + (tls ? "\n" + std::to_string(tls_port) : std::string());
  std::unique_ptr<tunnel::proxy> drop;
  {
    std::lock_guard lock(tunnel_m_);
    if (proxy_ && (!want || proxy_for_ != made_for)) {
      drop = std::move(proxy_);
      proxy_for_.clear();
    }
  }
  if (drop) {
    if (const auto p = drop->port()) port = p;  // the next one takes the same port, once this has let go
    // The system setting first, on the proxy thread: apps stop asking for the
    // script about when the port that serves it closes (the script's DIRECT
    // covers the gap).
    if (!want) want_system_proxy(0, false);
    drop.reset();  // its streams close and its threads end, on this thread
  }
  bool running = false;
  {
    std::lock_guard lock(tunnel_m_);
    running = static_cast<bool>(proxy_);
  }
  std::string note;
  if (want && !running) {
    std::string why;
    if (relay.empty()) {
      note = "Set your VPN server first: type it, or paste a VPN code with Connect to a VPN host\xE2\x80\xA6";
    } else if (!relay_server_of(relay, why)) {
      note = why;
    } else {
      tunnel::proxy::options o;
      o.relay = relay;
      o.port = port;
      o.tls = tls;
      o.tls_port = tls_port;
      o.changed = [this] { tunnel_changed(); };
      auto made = tunnel::proxy::start(std::move(o));
      if (made) {
        const auto got = (*made)->port();
        {
          std::lock_guard lock(tunnel_m_);
          proxy_ = std::move(*made);
          proxy_for_ = made_for;
        }
        bool save = false;
        {
          std::lock_guard lock(m_);
          save = tunnel_port_ != got;
          tunnel_port_ = got;
        }
        if (save) request(false, true);
        host_.log(2, "vpn: tunnel on");
      } else {
        note = "The Tunnel could not listen on this computer.";
        log_status("tunnel", made.error());
      }
    }
  }
  wipe(relay);
  {
    std::lock_guard lock(m_);
    if (!want) {
      v_.tunnel_address.clear();
      if (!vpn_runs) v_.tunnel_note.clear();
      if (!vpn_runs) v_.tunnel_open = 0;
    } else if (!note.empty()) {
      v_.tunnel_address.clear();
      v_.tunnel_note = note;
      v_.tunnel_open = 0;
    }
  }
  std::uint16_t live = 0;
  {
    std::lock_guard lock(tunnel_m_);
    if (proxy_) live = proxy_->port();
  }
  bool all_apps = false;
  {
    std::lock_guard lock(m_);
    all_apps = v_.all_apps;
  }
  want_system_proxy(live, want && all_apps && live != 0);
  tunnel_changed();
}

void service::write_proxy_record() {
  if (opts_.data_dir.empty()) return;  // the tests: kept in memory only
  const std::string path = io::join_path(opts_.data_dir, kProxyRecordFile);
  if (proxy_record_) {
    if (!sysproxy::save_record(path, *proxy_record_)) host_.log(1, "vpn: could not keep the proxy record");
  } else {
    (void)io::remove_file(path);
  }
}

void service::want_system_proxy(std::uint16_t port, bool want) {
  if (!sysproxy_) return;  // no port (the tests without a stand-in, an offline load): nothing to touch
  std::lock_guard lock(proxy_m_);
  proxy_want_ = std::make_pair(port, want);
  proxy_cv_.notify_all();
}

void service::proxy_loop() {
  std::unique_lock lock(proxy_m_);
  for (;;) {
    proxy_cv_.wait(lock, [&] { return proxy_stop_ || proxy_want_.has_value(); });
    if (proxy_stop_) return;
    const auto [port, want] = *proxy_want_;
    proxy_want_.reset();
    lock.unlock();
    apply_system_proxy(port, want);
    post();  // what came of it, at once: not through the control thread
    lock.lock();
  }
}

void service::apply_system_proxy(std::uint16_t port, bool want) {
  const std::string url = want && port != 0 ? tunnel::pac_url(port) : std::string();
  std::string note;
  bool ours = false;
  if (!sysproxy_) {
    // No port (the tests without a stand-in, an offline load): nothing to touch.
  } else if (!sysproxy_->unsupported().empty()) {
    if (want) note = sysproxy_->unsupported();
  } else {
    // What is set and no longer wanted (off, another port, a crash's leftover) goes back first.
    if (proxy_record_ && proxy_record_->pac_url != url) {
      {
        std::lock_guard lock(m_);
        v_.system_asking = "restore";
      }
      if (!stopping_.load()) post();
      const bool back = sysproxy_->restore(proxy_record_->saved, proxy_record_->pac_url);
      {
        std::lock_guard lock(m_);
        v_.system_asking.clear();
      }
      if (back) {
        proxy_record_.reset();
        write_proxy_record();
        host_.log(2, "vpn: system proxy restored");
      } else {
        // Declined (or it failed): said plainly, with how to undo it by hand.
        note = "This computer's proxy setting still points at the Tunnel: putting it back was not allowed. Apps "
               "that follow it go direct while the Tunnel is off. It is put back the next time you allow it (turn "
               "Use for all apps off, or start MediaViewer again); or turn off the automatic proxy in this "
               "computer's network settings.";
        host_.log(1, "vpn: system proxy not restored");
      }
    }
    if (proxy_retry_.exchange(false)) proxy_gave_up_for_.clear();
    if (!url.empty() && !proxy_record_ && url == proxy_gave_up_for_) {
      note = proxy_gave_up_note_;
    } else if (!url.empty() && !proxy_record_) {
      // The OS may ask the person now (a Mac's administrator dialog): say so while it does.
      {
        std::lock_guard lock(m_);
        v_.system_asking = "set";
      }
      post();
      auto saved = sysproxy_->set(url);
      {
        std::lock_guard lock(m_);
        v_.system_asking.clear();
      }
      if (saved) {
        proxy_record_ = sysproxy::record{url, *saved};
        write_proxy_record();
        host_.log(2, "vpn: system proxy set");
      } else {
        proxy_gave_up_for_ = url;
        if (saved.error() == status::permission_denied) {
          note = "This computer's proxy setting was not changed (it needs an administrator): set the address "
                 "below in your apps, or turn Use for all apps off and on to be asked again.";
        } else {
          note = "This computer's proxy setting could not be changed: set the address below in your apps.";
          log_status("system proxy", saved.error());
        }
        proxy_gave_up_note_ = note;
      }
    }
    ours = proxy_record_ && proxy_record_->pac_url == url && !url.empty();
  }
  if (stopping_.load()) return;  // the destructor: no view to show it in
  std::lock_guard lock(m_);
  v_.system = ours;
  v_.system_note = ours ? "Apps that follow this computer's proxy setting use the Tunnel. Others can use the "
                          "address below."
                        : note;
}

bool service::sync_vpn(bool on, const std::string& relay, bool tls, std::uint16_t tls_port, bool block) {
  const std::string made_for =
      relay + "\n" + (tls ? std::to_string(tls_port) : std::string("-")) + (block ? "\nblock" : "");
  std::unique_ptr<runner> drop;
  {
    std::lock_guard lock(tunnel_m_);
    if (vpn_run_ && (!on || vpn_for_ != made_for)) {
      drop = std::move(vpn_run_);
      vpn_for_.clear();
    }
  }
  if (drop) {
    drop.reset();  // every stream reset, its threads joined
    if (vpn_port_) vpn_port_->stop();  // the interface, its routes and DNS go
    host_.log(2, "vpn: all traffic off");
  }
  {
    std::lock_guard lock(tunnel_m_);
    if (vpn_run_) return true;
  }
  const auto proxy_mode = [&] {
    std::lock_guard lock(m_);
    v_.mode = "proxy";
    return false;
  };
  if (!on || !vpn_port_) return proxy_mode();  // a failed start's words stay until the person flips the switch
  {
    std::lock_guard lock(m_);
    if (vpn_gave_up_for_ == made_for) {  // failed this run for these settings: its note says so
      v_.mode = "proxy";
      return false;
    }
  }
  std::string why;
  const auto field = relay_server_of(relay, why);
  if (!field) {
    std::lock_guard lock(m_);
    if (v_.all_traffic_available) v_.all_traffic_note.clear();  // the proxy's notes say why; no older words
    v_.mode = "proxy";
    return false;
  }
  settings s;
  s.kill_switch = block;
  s.relay_name = field->at.substr(0, field->at.rfind(':'));
  // Left out of the tunnel: the server itself at every address it has, IPv4
  // and IPv6 (the dialer may reach it at any), and every local network.
  s.exclude = relay_addresses(field->at);
  if (s.exclude.empty()) return proxy_mode();  // the server's name did not resolve: routes would loop
  for (const auto& n : net::local_subnets()) {
    if (!net::is_public(n.base) || n.prefix >= 8) s.exclude_nets.push_back(n);
  }
  // The first start may wait for the person (the system asks to allow the
  // extension and to add a VPN configuration): the page says so meanwhile.
  {
    std::lock_guard lock(m_);
    v_.all_traffic_note =
        "Starting All traffic\xE2\x80\xA6 The first time, your system asks you to allow MediaViewer's network "
        "extension and to add its VPN configuration.";
  }
  post();
  std::string note;
  auto dev = vpn_port_->start(s);
  bool failed = false;   // turns the switch off: the person has to act (allow it, restart)
  bool gave_up = false;  // a passing failure: kept on, not tried again this run
  if (!dev) {
    failed = dev.error() == status::permission_denied || dev.error() == status::busy;
    gave_up = !failed && dev.error() != status::cancelled;  // quitting: nothing to say
    if (dev.error() == status::permission_denied) {
      note = "All traffic (VPN) wasn't allowed. Allow MediaViewer's network extension and its VPN configuration "
             "(on a Mac: System Settings \xE2\x86\x92 General \xE2\x86\x92 Login Items & Extensions), then turn "
             "All traffic on again. Meanwhile the proxy carries the apps that follow it.";
    } else if (dev.error() == status::busy) {
      note = "Restart this computer to finish installing MediaViewer's network extension, then turn All traffic on "
             "again. Meanwhile the proxy carries the apps that follow it.";
    } else if (gave_up) {
      note = "All traffic (VPN) could not start: the proxy carries the apps that follow it. Turn All traffic off "
             "and on again to try once more.";
    }
    log_status("all traffic", dev.error());
  } else {
    runner::options o;
    o.dial = {relay, tls, tls_port, {}, std::nullopt};  // the knock key derived here
    o.dev = std::move(*dev);
    o.s = s;
    o.changed = [this] { tunnel_changed(); };
    auto r = runner::start(std::move(o));
    if (r) {
      std::lock_guard lock(tunnel_m_);
      vpn_run_ = std::move(*r);
      vpn_for_ = made_for;
    } else {
      vpn_port_->stop();
      gave_up = true;
      note = "All traffic (VPN) could not start: the proxy carries the apps that follow it. Turn All traffic off "
             "and on again to try once more.";
      log_status("all traffic", r.error());
    }
  }
  bool runs = false;
  {
    std::lock_guard lock(tunnel_m_);
    runs = static_cast<bool>(vpn_run_);
  }
  if (runs) host_.log(2, "vpn: all traffic on");
  {
    std::lock_guard lock(m_);
    v_.mode = runs ? "vpn" : "proxy";
    // The starting words go either way; never over a reason it isn't offered.
    if (v_.all_traffic_available) v_.all_traffic_note = note;
    // A start that failed turns the switch off: nothing else (the server, the
    // web-site look, the next launch) asks the person again until they do.
    if (failed) v_.all_traffic = false;
    vpn_gave_up_for_ = gave_up ? made_for : std::string();
  }
  if (failed) request(false, true);  // kept off
  return runs;
}

void service::tunnel_changed() {
  std::optional<tunnel::proxy::snapshot> s;
  std::optional<runner::snapshot> vs;
  {
    std::lock_guard lock(tunnel_m_);
    if (proxy_) s = proxy_->state();
    if (vpn_run_) vs = vpn_run_->state();
  }
  if (vs) {
    std::lock_guard lock(m_);
    v_.mode = "vpn";
    v_.tunnel_address.clear();
    v_.tunnel_note = vs->problem;
    v_.tunnel_open = vs->tcp_open;
    v_.tunnel_streams = vs->tcp_made;
  }
  if (s && !vs) {  // the VPN's state wins while both are briefly alive
    std::lock_guard lock(m_);
    v_.tunnel_address = "127.0.0.1:" + std::to_string(s->port);
    v_.tunnel_note = s->problem;
    v_.tunnel_open = s->open;
    v_.tunnel_streams = s->streams;
  }
  post_throttled();
}

// ---- events ----------------------------------------------------------------------------

void service::on_event(const std::string& node, std::uint32_t kind, const std::string& value_json) {
  const auto value = json::parse(value_json, 2);
  const std::string* text = value && value->k == json::kind::string ? &value->s : nullptr;
  const bool is_bool = value && value->k == json::kind::boolean;
  const auto set_flag = [&](bool view::*field, bool reconcile, bool retry_proxy = false) {
    {
      std::lock_guard lock(m_);
      v_.*field = value->b;
      if (field == &view::all_traffic && vpn_port_) {
        // The person flipped it: an earlier failure's words go, and on tries afresh.
        vpn_gave_up_for_.clear();
        if (value->b) {
          v_.all_traffic_note = vpn_port_->unavailable();
          v_.all_traffic_available = v_.all_traffic_note.empty();
        } else if (v_.all_traffic_available) {
          v_.all_traffic_note.clear();
        }
      }
    }
    if (retry_proxy) proxy_retry_.store(true);
    request(reconcile, true);
    post();  // at once: the page shows what the click did
  };
  // The switches: the page's and the menu's rows (the same ids) come here.
  if (node == "tunnel" && kind == kChange && is_bool) return set_flag(&view::tunnel, true, true);
  if (node == "all_traffic" && kind == kChange && is_bool) return set_flag(&view::all_traffic, true);
  if (node == "block" && kind == kChange && is_bool) return set_flag(&view::block, true);
  if (node == "tls" && kind == kChange && is_bool) return set_flag(&view::tls, true);
  if (node == "all_apps" && kind == kChange && is_bool) return set_flag(&view::all_apps, true, true);
  // Your VPN server: a secure field (it holds the key), kept here as typed
  // until Return or Done; then the Tunnel follows it. Change server… opens it
  // again, Remove clears it (and its key).
  const auto set_server = [&](std::string field) {
    {
      std::lock_guard lock(m_);
      wipe(v_.server);
      v_.server = std::move(field);
      v_.server_shown = tunnel::relay_shown(v_.server);
      v_.server_editing = false;
      wipe(typed_server_);
    }
    request(true, true);
    post();  // at once: the page shows what the click did
  };
  if (node == "server" && kind == kChange && text) {
    std::lock_guard lock(m_);
    wipe(typed_server_);
    typed_server_ = *text;
    return;
  }
  if (node == "server" && kind == kSubmit && text) return set_server(*text);
  if (node == "server_done" && kind == kClick) {
    std::string typed;
    {
      std::lock_guard lock(m_);
      typed = std::move(typed_server_);
      typed_server_.clear();
      if (typed.empty()) {  // nothing typed: the server stays as it was
        v_.server_editing = false;
      }
    }
    if (!typed.empty()) return set_server(std::move(typed));
    post();
    return;
  }
  if (node == "server_change" && kind == kClick) {
    {
      std::lock_guard lock(m_);
      v_.server_editing = true;
    }
    post();
    return;
  }
  if (node == "server_clear" && kind == kClick) return set_server({});
  // Connect to a VPN host: the code from the server's VPN page, in one step.
  if (node == "connect_open" && kind == kClick) {
    {
      std::lock_guard lock(m_);
      v_.connect_open = true;
      v_.connect_note.clear();
    }
    post();  // at once: the page shows what the click did
    return;
  }
  if (node == "connect_cancel" && kind == kClick) {
    {
      std::lock_guard lock(m_);
      v_.connect_open = false;
      wipe(typed_code_);
    }
    post();  // at once: the page shows what the click did
    return;
  }
  if (node == "connect_code" && kind == kChange && text) {
    std::lock_guard lock(m_);
    wipe(typed_code_);
    typed_code_ = *text;
    return;
  }
  if ((node == "connect_go" && kind == kClick) || (node == "connect_code" && kind == kSubmit)) {
    std::optional<std::string> line;
    {
      std::lock_guard lock(m_);
      if (node == "connect_code" && text) {
        wipe(typed_code_);
        typed_code_ = *text;
      }
      bool look = false;
      line = serve::parse_relay_connect_code(typed_code_, &look);
      wipe(typed_code_);
      if (!line) {
        v_.connect_note = "That isn't a VPN code. Copy it again from the VPN page on your server (VPN code\xE2\x80\xA6).";
      } else {
        // As the server field would take it, and the Tunnel on.
        wipe(v_.server);
        v_.server = *line;
        v_.server_shown = tunnel::relay_shown(*line);
        v_.tunnel = true;
        // The code says whether the host looks like a web site (docs/plans/vpn-stealth.md): the
        // Tunnel's look follows it, on the code's port, so nothing else is set by hand.
        v_.tls = look && v_.tls_available;
        if (v_.tls) {
          const auto colon = line->rfind(':', line->find('#'));
          tls_port_ = static_cast<std::uint16_t>(std::stoi(line->substr(colon + 1)));
        }
        v_.connect_open = false;
        v_.connect_note = look && !v_.tls_available
                              ? "This VPN code needs Look like a web site, which this build lacks."
                              : "Connected to " + v_.server_shown + ". The Tunnel is on" +
                                    (v_.all_traffic_available ? "; turn on All traffic (VPN) for every app." : ".");
      }
    }
    if (line) {
      wipe(*line);
      proxy_retry_.store(true);
      request(true, true);
    }
    post();  // at once: the page shows what the click did
    return;
  }
  // Host: Phone access does it (its link owns this computer's server and the NAS).
  if (node == "host_on" && kind == kChange && is_bool) {
    {
      std::lock_guard lock(m_);
      v_.host_busy = true;
      v_.host_error.clear();
    }
    {
      std::lock_guard lock(control_m_);
      phone_actions_.push_back({"vpn_set", value->b, {}});
      control_cv_.notify_all();
    }
    post();  // at once: the page shows what the click did
    return;
  }
  if ((node == "host_code" || node == "host_code_hide") && kind == kClick) {
    std::lock_guard lock(control_m_);
    phone_actions_.push_back({node == "host_code" ? "vpn_code" : "vpn_code_hide", false, {}});
    control_cv_.notify_all();
    return;
  }
  // This computer's own domain for the VPN: Phone access runs `host name domain`.
  if (node == "domain_name" && kind == kChange && text) {
    std::lock_guard lock(m_);
    v_.domain_typed = *text;
    return;
  }
  if ((node == "domain_change" || node == "domain_cancel") && kind == kClick) {
    {
      std::lock_guard lock(m_);
      v_.domain_open = node == "domain_change";
      v_.domain_typed.clear();
    }
    post_throttled();
    return;
  }
  if ((node == "domain_save" || node == "domain_clear" || (node == "domain_name" && kind == kSubmit)) &&
      (kind == kClick || kind == kSubmit)) {
    std::string domain;
    {
      std::lock_guard lock(m_);
      if (node == "domain_name" && text) v_.domain_typed = *text;
      domain = node == "domain_clear" ? std::string("none") : trim(v_.domain_typed);
      if (domain.empty()) {
        v_.host_error = "Type your domain, like vpn.example.com.";
      } else {
        v_.domain_open = false;
        v_.domain_typed.clear();
        v_.host_busy = true;
        v_.host_error.clear();
      }
    }
    if (!domain.empty()) {
      std::lock_guard lock(control_m_);
      phone_actions_.push_back({"vpn_domain", false, domain});
      control_cv_.notify_all();
    }
    post_throttled();
    return;
  }
}

view service::snapshot() const {
  std::lock_guard lock(m_);
  return v_;
}

}  // namespace mv::driftwire::vpn
