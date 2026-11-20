// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/vpn/prefs.h"

#include <sodium.h>

#include <cstdint>
#include <span>
#include <vector>

#include "core/json.h"
#include "io/file_port.h"
#include "wire/known.h"

namespace mv::driftwire::vpn {

namespace {

// A prefs file is a few hundred bytes; anything larger is not ours.
constexpr std::uint64_t kMaxPrefsBytes = 64 * 1024;

std::optional<std::string> read_small(const std::string& path) {
  io::file_reader in;
  if (!in.open(path, io::read_mode::sequential) || in.size() > kMaxPrefsBytes) return std::nullopt;
  std::string text(static_cast<std::size_t>(in.size()), '\0');
  std::size_t got = 0;
  while (got < text.size()) {
    auto n = in.read({reinterpret_cast<std::uint8_t*>(text.data()) + got, text.size() - got});
    if (!n || *n == 0) return std::nullopt;
    got += *n;
  }
  return text;
}

std::optional<std::string> read_file(const std::string& path) {
  if (auto text = read_small(path)) return text;
  // A write stopped between removing the old file and renaming the new one.
  return read_small(path + ".new");
}

std::optional<std::uint16_t> port_of(const json::value& doc, std::string_view key) {
  if (const auto p = doc.integer(key); p && *p >= 1 && *p <= 65535) return static_cast<std::uint16_t>(*p);
  return std::nullopt;
}

std::optional<prefs> parse(const std::string& text) {
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  prefs p;
  if (const std::string* s = doc->str("server")) p.server = *s;
  p.tunnel = doc->boolean("tunnel").value_or(false);
  if (const auto port = port_of(*doc, "tunnel_port")) p.tunnel_port = *port;
  p.all_apps = doc->boolean("all_apps").value_or(true);
  p.tls = doc->boolean("tls").value_or(false);
  if (const auto port = port_of(*doc, "tls_port")) p.tls_port = *port;
  p.all_traffic = doc->boolean("all_traffic").value_or(false);
  p.block = doc->boolean("block").value_or(false);
  return p;
}

// Driftwire's driftwire.json, its Tunnel's keys (src/addons/driftwire/prefs.cpp),
// when the Tunnel was ever used there: switched on, or its proxy given a port.
std::optional<prefs> parse_driftwire(const std::string& text) {
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const bool on = doc->boolean("tunnel").value_or(false);
  const auto port = port_of(*doc, "tunnel_port");
  if (!on && !port) return std::nullopt;
  prefs p;
  if (const std::string* s = doc->str("relay_server")) p.server = *s;
  p.tunnel = on;
  p.tunnel_port = port.value_or(0);
  p.all_apps = doc->boolean("tunnel_all_apps").value_or(true);
  p.tls = doc->boolean("tunnel_tls").value_or(false);
  if (const auto tp = port_of(*doc, "tunnel_tls_port")) p.tls_port = *tp;
  p.all_traffic = doc->boolean("tunnel_vpn").value_or(false);
  p.block = doc->boolean("tunnel_block").value_or(false);
  return p;
}

bool write_whole(const std::string& path, const std::string& text) {
  const std::string fresh = path + ".new";
  (void)io::remove_file(fresh);  // a leftover from a write that stopped
  {
    io::file_writer out;
    auto made = out.create_new(fresh);
    if (!made || *made != io::rename_outcome::renamed) return false;
    const bool ok = out.write({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}) &&
                    out.flush_durable() && out.close();
    if (!ok) {
      (void)io::remove_file(fresh);
      return false;
    }
  }
  (void)io::remove_file(path);
  auto r = io::rename_no_replace(fresh, path);
  return r && *r == io::rename_outcome::renamed;
}

}  // namespace

std::optional<prefs> load_prefs(const std::string& path) {
  if (path.empty()) return std::nullopt;
  if (auto text = read_file(path)) return parse(*text);
  return std::nullopt;
}

bool save_prefs(const std::string& path, const prefs& p) {
  if (path.empty()) return false;
  json::writer w;
  w.begin_object().key("v").integer(1).key("server").string(p.server);
  w.key("tunnel").boolean(p.tunnel).key("tunnel_port").integer(p.tunnel_port);
  w.key("all_apps").boolean(p.all_apps);
  w.key("tls").boolean(p.tls).key("tls_port").integer(p.tls_port);
  w.key("all_traffic").boolean(p.all_traffic).key("block").boolean(p.block).end_object();
  return write_whole(path, w.take());
}

std::string driftwire_data_dir(const std::string& data_dir) {
  const std::string root(io::parent_of(io::parent_of(data_dir)));
  if (data_dir.empty() || root.empty()) return {};
  // The host names the folder after the add-on: its name on the Mac, its id elsewhere (addon/store.cpp).
#if defined(__APPLE__)
  return io::join_path(io::join_path(root, "Driftwire"), "data");
#else
  return io::join_path(io::join_path(root, "driftwire"), "data");
#endif
}

bool adopt_driftwire(const std::string& driftwire_dir, const std::string& data_dir, const std::string& prefs_path,
                     secrets::store* driftwire, secrets::store* mine) {
  if (driftwire_dir.empty() || data_dir.empty() || prefs_path.empty() || read_file(prefs_path)) return false;
  const std::string& theirs = driftwire_dir;
  const auto text = read_file(io::join_path(theirs, "driftwire.json"));
  if (!text) return false;
  auto p = parse_driftwire(*text);
  if (!p) return false;
  // The key: Driftwire's secret item when its file holds the address alone (#500).
  if (!p->server.empty() && p->server.find('#') == std::string::npos && driftwire) {
    if (auto k = driftwire->get(known::kRelayKeyName)) {
      if (mine && mine->put(kServerKeyName, *k)) {
        // the file keeps the address, as save_prefs is given it
      } else {
        p->server += '#';
        p->server.append(k->begin(), k->end());
      }
      sodium_memzero(k->data(), k->size());
    }
  }
  // A key still in Driftwire's file (no store there): into ours, the file keeps the address.
  if (const auto hash = p->server.rfind('#'); hash != std::string::npos && mine) {
    const std::span<const std::uint8_t> key(reinterpret_cast<const std::uint8_t*>(p->server.data()) + hash + 1,
                                            p->server.size() - hash - 1);
    if (mine->put(kServerKeyName, key)) {
      sodium_memzero(p->server.data() + hash, p->server.size() - hash);
      p->server.resize(hash);
    }
  }
  // The system proxy Driftwire's Tunnel set and a crash left: ours to put back now.
  const std::string record = io::join_path(theirs, "driftwire_proxy.json");
  if (io::stat_path(record)) {
    if (auto moved = io::rename_no_replace(record, io::join_path(data_dir, kProxyRecordFile));
        !moved || *moved != io::rename_outcome::renamed) {
      // Left where it was; nothing else reads it.
    }
  }
  const bool saved = save_prefs(prefs_path, *p);
  if (!p->server.empty()) sodium_memzero(p->server.data(), p->server.size());
  return saved;
}

}  // namespace mv::driftwire::vpn
