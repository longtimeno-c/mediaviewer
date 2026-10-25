// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/cli/host.h"

#include <filesystem>
#include <system_error>

#include "addons/driftwire/cli/term.h"
#include "io/pairing.h"

namespace mv::driftwire::cli {

cli_host::cli_host(terminal& wake, std::string destination) : wake_(wake), destination_(std::move(destination)) {}

std::vector<std::string> cli_host::take_log() {
  std::lock_guard lock(log_m_);
  std::vector<std::string> out(log_.begin(), log_.end());
  log_.clear();
  return out;
}

void cli_host::post_tree(const std::string&) {
  posts_.fetch_add(1);
  wake_.wake();
}

// The terminal shows an offer as it comes; nothing to bring forward.
void cli_host::show_screen(bool) { wake_.wake(); }

std::optional<std::uint64_t> cli_host::free_bytes(const std::string& dir) {
  std::error_code ec;
  const auto p = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(dir.data()), dir.size()));
  const auto s = std::filesystem::space(p, ec);
  if (ec) return std::nullopt;
  return static_cast<std::uint64_t>(s.available);
}

std::vector<std::uint32_t> cli_host::pair_partners(const std::vector<std::string>& names) { return pair_names(names); }

void cli_host::log(int, const char* ascii) {
  // May run under the engine's locks: the ring's own lock only, and a wake.
  {
    std::lock_guard lock(log_m_);
    if (log_.size() >= kLogLines) log_.pop_front();
    log_.emplace_back(ascii ? ascii : "");
  }
  wake_.wake();
}

bool cli_host::post_status(const std::string&) { return true; }

// Taken: the session shows the offer in the terminal (the engine would
// otherwise ask for a window it does not have).
bool cli_host::post_notice(const std::string&, const std::string&) {
  wake_.wake();
  return true;
}

bool cli_host::post_offer_notice(const std::string&, const std::string&, std::uint64_t) {
  wake_.wake();
  return true;
}

bool cli_host::keep_alive(bool) { return true; }

std::vector<std::uint32_t> pair_names(const std::vector<std::string>& names) {
  // The viewer's own pairing; the index rides in the path so each pair maps
  // back to the caller's positions (as the host's t_pair does).
  std::vector<io::dir_entry> entries(names.size());
  std::vector<std::uint32_t> partner(names.size(), UINT32_MAX);
  for (std::size_t i = 0; i < names.size(); ++i) {
    entries[i].name_utf8 = names[i];
    entries[i].path_utf8 = std::to_string(i);
  }
  for (const io::listed_item& item : io::pair_listing(std::move(entries))) {
    if (item.kind == io::pair_kind::none) continue;
    const auto a = static_cast<std::uint32_t>(std::stoul(item.primary.path_utf8));
    const auto b = static_cast<std::uint32_t>(std::stoul(item.secondary.path_utf8));
    partner[a] = b;
    partner[b] = a;
  }
  return partner;
}

}  // namespace mv::driftwire::cli
