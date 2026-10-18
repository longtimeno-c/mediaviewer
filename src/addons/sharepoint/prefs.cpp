// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/prefs.h"

#include <cstdint>
#include <span>
#include <vector>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::sharepoint {

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
  if (const std::string* f = doc->str("forwarded")) p.forwarded = *f;
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

bool save_prefs(const std::string& path, const prefs& p) {
  if (path.empty()) return false;
  json::writer w;
  w.begin_object().key("v").integer(1).key("receiving").boolean(p.receiving);
  w.key("background").boolean(p.background);
  w.key("destination").string(p.destination);
  w.key("internet").boolean(p.internet).key("forwarded").string(p.forwarded);
  w.key("dht").boolean(p.dht).end_object();
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

}  // namespace mv::sharepoint
