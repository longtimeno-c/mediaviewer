// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The system proxy's record and the port where an OS has none. Portable.
#include "addons/driftwire/sysproxy.h"

#include <cstdint>

#include "core/json.h"
#include "io/file_port.h"

namespace mv::driftwire::sysproxy {

std::string record_json(const record& r) {
  json::writer w;
  w.begin_object().key("v").integer(1).key("pac_url").string(r.pac_url).key("saved").string(r.saved).end_object();
  return w.take();
}

std::optional<record> parse_record(std::string_view text) {
  const auto doc = json::parse(std::string(text), 4);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const std::string* url = doc->str("pac_url");
  const std::string* saved = doc->str("saved");
  if (!url || !saved || url->empty()) return std::nullopt;
  return record{*url, *saved};
}

std::optional<record> load_record(const std::string& path) {
  if (path.empty()) return std::nullopt;
  io::file_reader in;
  if (!in.open(path, io::read_mode::sequential) || in.size() > 64 * 1024) return std::nullopt;
  std::string text(static_cast<std::size_t>(in.size()), '\0');
  std::size_t got = 0;
  while (got < text.size()) {
    auto n = in.read({reinterpret_cast<std::uint8_t*>(text.data()) + got, text.size() - got});
    if (!n || *n == 0) return std::nullopt;
    got += *n;
  }
  return parse_record(text);
}

bool save_record(const std::string& path, const record& r) {
  if (path.empty()) return false;
  const std::string text = record_json(r);
  (void)io::remove_file(path);
  io::file_writer out;
  auto made = out.create_new(path);
  if (!made || *made != io::rename_outcome::renamed) return false;
  return out.write({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}) && out.flush_durable() &&
         out.close();
}

}  // namespace mv::driftwire::sysproxy
