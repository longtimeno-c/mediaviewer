// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The driftwire command line as the engine's host (engine_host, engine.h).
//
// The engine calls its host from any of its threads, sometimes under its own
// locks (cli-engine-map §2). So nothing here calls back into the engine or
// waits on anything the session holds: a new view only bumps a counter and
// wakes the session's wait (term.h); the session takes the engine's
// snapshot on its own thread. The engine's log lines go into a small ring
// the session shows with --verbose; the engine writes no path, name or
// address into them (rule 6), and they are only ever printed here.
//
// The window's extras have nothing to do in a terminal: no window to show,
// no notifications (the terminal shows the offer itself, so the engine is
// told it was taken), no status text, no staying alive after closing (the
// command line has no background receiving), and nothing presents frames, so
// a transfer never yields.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "addons/driftwire/engine.h"

namespace mv::driftwire::cli {

class terminal;

class cli_host final : public engine_host {
 public:
  static constexpr std::size_t kLogLines = 64;

  // `destination`: where received files go unless the person picks another
  // (--save-to for this run, else the Downloads folder). Read once, here:
  // the engine asks for it under its lock.
  cli_host(terminal& wake, std::string destination);

  // The number of posts so far (the session compares it with the last it saw).
  [[nodiscard]] std::uint64_t posts() const noexcept { return posts_.load(); }
  // The log lines since the last call, oldest first.
  [[nodiscard]] std::vector<std::string> take_log();

  // ---- engine_host
  void post_tree(const std::string& tree_json) override;
  void show_screen(bool show) override;
  [[nodiscard]] std::string default_destination() override { return destination_; }
  [[nodiscard]] std::optional<std::uint64_t> free_bytes(const std::string& dir) override;
  [[nodiscard]] bool should_yield() override { return false; }
  [[nodiscard]] std::vector<std::uint32_t> pair_partners(const std::vector<std::string>& names) override;
  void log(int level, const char* ascii) override;
  bool post_status(const std::string& text) override;
  bool post_notice(const std::string& title, const std::string& body) override;
  bool post_offer_notice(const std::string& title, const std::string& body, std::uint64_t offer) override;
  void ask_to_notify() override {}
  bool keep_alive(bool on) override;

 private:
  terminal& wake_;
  const std::string destination_;
  std::atomic<std::uint64_t> posts_{0};
  std::mutex log_m_;
  std::deque<std::string> log_;
};

// The RAW + JPEG and Live Photo halves of one folder's names, as the viewer
// pairs them (io/pairing.h): each name's partner, or UINT32_MAX.
[[nodiscard]] std::vector<std::uint32_t> pair_names(const std::vector<std::string>& names);

}  // namespace mv::driftwire::cli
