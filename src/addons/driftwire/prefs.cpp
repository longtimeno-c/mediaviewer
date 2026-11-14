// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/prefs.h"

#include <cstdint>
#include <span>
#include <vector>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::driftwire {

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

std::optional<prefs> parse(const std::string& text) {
  const auto doc = json::parse(text, 4);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  prefs p;
  p.receiving = doc->boolean("receiving").value_or(false);
  p.background = doc->boolean("background").value_or(false);
  if (p.background) p.receiving = true;
  if (const std::string* d = doc->str("destination")) p.destination = *d;
  p.internet = doc->boolean("internet").value_or(false);
  p.dht = doc->boolean("dht").value_or(false);
  p.handover = doc->boolean("handover").value_or(false);
  p.relay = doc->boolean("relay").value_or(false);
  if (const std::string* r = doc->str("relay_server")) p.relay_server = *r;
  p.tunnel = doc->boolean("tunnel").value_or(false);
  p.tunnel_all_apps = doc->boolean("tunnel_all_apps").value_or(true);
  p.tunnel_tls = doc->boolean("tunnel_tls").value_or(false);
  if (const auto tp = doc->integer("tunnel_tls_port"); tp && *tp >= 1 && *tp <= 65535) {
    p.tunnel_tls_port = static_cast<std::uint16_t>(*tp);
  }
  if (const auto port = doc->integer("tunnel_port"); port && *port >= 1 && *port <= 65535) {
    p.tunnel_port = static_cast<std::uint16_t>(*port);
  }
  return p;
}

}  // namespace

std::optional<prefs> load_prefs(const std::string& path) {
  if (path.empty()) return std::nullopt;
  if (auto text = read_small(path)) return parse(*text);
  // A write stopped between removing the old file and renaming the new one.
  if (auto text = read_small(path + ".new")) return parse(*text);
  return std::nullopt;
}

void adopt_old_prefs(const std::string& data_dir, const std::string& prefs_path) {
  if (data_dir.empty() || prefs_path.empty() || load_prefs(prefs_path)) return;
  // <root>/driftwire/data -> <root>/sharepoint/data/sharepoint.json
  const std::string root(io::parent_of(io::parent_of(data_dir)));
  if (root.empty()) return;
  const auto old = load_prefs(io::join_path(io::join_path(io::join_path(root, "sharepoint"), "data"), "sharepoint.json"));
  if (old) (void)save_prefs(prefs_path, *old);
}

bool save_prefs(const std::string& path, const prefs& p) {
  if (path.empty()) return false;
  json::writer w;
  w.begin_object().key("v").integer(1).key("receiving").boolean(p.receiving);
  w.key("background").boolean(p.background);
  w.key("destination").string(p.destination);
  w.key("internet").boolean(p.internet);
  w.key("dht").boolean(p.dht).key("handover").boolean(p.handover);
  w.key("relay").boolean(p.relay).key("relay_server").string(p.relay_server);
  w.key("tunnel").boolean(p.tunnel).key("tunnel_port").integer(p.tunnel_port);
  w.key("tunnel_all_apps").boolean(p.tunnel_all_apps);
  w.key("tunnel_tls").boolean(p.tunnel_tls).key("tunnel_tls_port").integer(p.tunnel_tls_port).end_object();
  const std::string text = w.take();
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

}  // namespace mv::driftwire
