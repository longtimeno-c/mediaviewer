// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the Driftwire tests share (tests/test_driftwire*.cpp): engines over
// loopback TCP found through an in-memory discovery bus, a host that records
// what the engine asks of it, and links that can be tampered with.
#pragma once

#include "catch_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/screen.h"
#include "addons/driftwire/channel.h"
#include "addons/driftwire/engine.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/prefs.h"
#include "addons/driftwire/secrets.h"
#include "addons/driftwire/transfer.h"
#include "addons/driftwire/wire.h"
#include "import_fixture.h"
#include "io/file_port.h"

using namespace mv::test;
namespace sp = mv::driftwire;

namespace {


template <typename F>
bool eventually(F f, int seconds = 15) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < until) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return f();
}

// Discovery on one machine, in memory: every member hears every other one,
// three times a second, at 127.0.0.1.
struct bus {
  std::mutex m;
  struct member {
    sp::beacon self;
    sp::discovery::seen_fn seen;
  };
  std::map<int, member> members;
  int next = 0;
  // Every beacon says this protocol instead of its own (0: as sent), as a
  // peer from before #472 would.
  std::atomic<std::uint32_t> force_protocol{0};
};

class bus_discovery final : public sp::discovery {
 public:
  explicit bus_discovery(bus& b) : bus_(b) {}
  ~bus_discovery() override { stop(); }
  bool start(const sp::beacon& self, seen_fn seen, tick_fn tick) override {
    {
      std::lock_guard lock(bus_.m);
      key_ = bus_.next++;
      bus_.members[key_] = {self, std::move(seen)};
    }
    stop_.store(false);
    thread_ = std::thread([this, tick = std::move(tick)] {
      while (!stop_.load()) {
        std::vector<std::pair<sp::beacon, sp::discovery::seen_fn>> calls;
        {
          std::lock_guard lock(bus_.m);
          const auto me = bus_.members.find(key_);
          if (me == bus_.members.end()) break;
          for (const auto& [k, other] : bus_.members) {
            if (k != key_) calls.emplace_back(me->second.self, other.seen);
          }
        }
        for (auto& [b, seen] : calls) {
          if (const std::uint32_t p = bus_.force_protocol.load()) b.protocol = p;
          seen(b, "127.0.0.1");
        }
        if (tick) tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
    });
    return true;
  }
  void stop() noexcept override {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(bus_.m);
    bus_.members.erase(key_);
  }

 private:
  bus& bus_;
  int key_ = -1;
  std::atomic<bool> stop_{true};
  std::thread thread_;
};

class fake_host final : public sp::engine_host {
 public:
  explicit fake_host(fs::path received) : received_(std::move(received)) {}
  void post_tree(const std::string& tree) override {
    std::lock_guard lock(m);
    last_tree = tree;
    ++posts;
  }
  void show_screen(bool show) override {
    std::lock_guard lock(m);
    shown += show ? 1 : 0;
  }
  std::string default_destination() override { return utf8(received_); }
  std::optional<std::uint64_t> free_bytes(const std::string&) override { return room; }
  bool should_yield() override { return slow.load(); }
  std::vector<std::uint32_t> pair_partners(const std::vector<std::string>& names) override {
    ++pair_calls;
    // A slow folder (a large selection, a share): the reader is held here.
    while (hold_pairs.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // RAW + JPEG by stem, as io::pairing does for the cases tested here.
    std::vector<std::uint32_t> out(names.size(), UINT32_MAX);
    for (std::size_t i = 0; i < names.size(); ++i) {
      for (std::size_t j = 0; j < names.size(); ++j) {
        if (i == j) continue;
        const auto stem = [](const std::string& n) { return n.substr(0, n.rfind('.')); };
        const auto ext = [](const std::string& n) { return n.substr(n.rfind('.') + 1); };
        if (stem(names[i]) == stem(names[j]) && ((ext(names[i]) == "CR2" && ext(names[j]) == "JPG") ||
                                                 (ext(names[i]) == "JPG" && ext(names[j]) == "CR2"))) {
          out[i] = static_cast<std::uint32_t>(j);
        }
      }
    }
    return out;
  }
  // Shown with a failure (ctest prints the output): where a transfer ended.
  void log(int, const char* ascii) override { std::fprintf(stderr, "[log] %s\n", ascii); }
  bool post_status(const std::string& text) override {
    std::lock_guard lock(m);
    status = text;
    return true;
  }
  // #452: a host with notifications records them; without, the engine opens the window.
  bool post_notice(const std::string& title, const std::string& body) override {
    if (!notify.load()) return false;
    std::lock_guard lock(m);
    notices.push_back(title.empty() ? std::string() : title + "|" + body);
    return true;
  }
  void ask_to_notify() override { ++asks; }
  bool keep_alive(bool on) override {
    keep = on;
    return true;
  }
  // #510: every post_busy, in order ("" = ended).
  void post_busy(const std::string& what) override {
    std::lock_guard lock(m);
    busy.push_back(what);
  }
  std::vector<std::string> busy_posts() {
    std::lock_guard lock(m);
    return busy;
  }
  std::string last_notice() {
    std::lock_guard lock(m);
    return notices.empty() ? std::string("(none)") : notices.back();
  }

  std::mutex m;
  std::vector<std::string> notices;
  std::atomic<bool> notify{false};
  std::atomic<int> asks{0};
  std::atomic<bool> keep{false};
  std::vector<std::string> busy;
  std::string last_tree;
  std::string status;
  int posts = 0;
  int shown = 0;
  std::optional<std::uint64_t> room;
  std::atomic<bool> slow{false};
  std::atomic<bool> hold_pairs{false};
  std::atomic<int> pair_calls{0};

 private:
  fs::path received_;
};

// One MediaViewer: its host, its engine, its window open.
struct client {
  fake_host host;
  sp::engine eng;
  // `secrets`: a folder for the secret store (Remember, #472 PR 4); empty: none.
  // `data`: the add-on's data folder (prefs, #530's unfinished receives); empty: none.
  client(bus& b, const fs::path& received, const std::string& name, const fs::path& secrets = {},
         const fs::path& data = {})
      : host(received), eng(host, options(b, name, secrets, data)) {
    eng.screen_opened("");
  }
  static sp::engine::options options(bus& b, const std::string& name, const fs::path& secrets = {},
                                     const fs::path& data = {}) {
    sp::engine::options o;
    if (!secrets.empty()) {
      fs::create_directories(secrets);
      o.make_secrets = [dir = utf8(secrets)] { return sp::secrets::file_store(dir); };
    }
    if (!data.empty()) {
      fs::create_directories(data);
      o.prefs_path = utf8(data / sp::kPrefsFile);
    }
    o.my_name = name;
    o.use_bluetooth = false;
    o.loopback_only = true;
    o.make_discovery = [&b] { return std::make_unique<bus_discovery>(b); };
    return o;
  }
  bool sees(const client& other) const {
    const auto v = eng.snapshot();
    return std::any_of(v.peers.begin(), v.peers.end(), [&](const auto& p) { return p.id == other.eng.id(); });
  }
  void pick(const std::vector<fs::path>& files) {
    std::vector<std::string> paths;
    for (const auto& f : files) paths.push_back(utf8(f));
    eng.set_files(paths);
    REQUIRE(eventually([&] {
      const auto v = eng.snapshot();
      return !v.reading_folder && v.file_count >= files.size();
    }));
  }
  // Picks `other` and sends; with `confirm`, types the code `other`'s
  // screen shows (what the person at this end reads off the other screen).
  void send_to(const client& other, bool confirm = true) {
    REQUIRE(eventually([&] { return sees(other); }));
    // One click on its tile sends (#442).
    eng.on_event("peers", 5, "\"" + other.eng.id() + "\"");
    if (!confirm) return;
    REQUIRE(eventually([&] {
      const auto o = eng.snapshot().out;
      return o == sp::view::out_phase::confirming || o == sp::view::out_phase::ended;
    }));
    if (eng.snapshot().out == sp::view::out_phase::ended) return;
    std::string code;
    if (!eventually([&] {
          const auto v = other.eng.snapshot();
          code = v.in_code;
          return v.in == sp::view::in_phase::connecting && !code.empty();
        }, 3)) {
      return;  // busy there: this side ends on its own
    }
    type_code(code);
  }
  void type_code(const std::string& code) { eng.on_event("out_code_entry", 3, "\"" + code + "\""); }
  void save_into(const fs::path& dir) { eng.on_event("destination", 2, "\"" + json_path(dir) + "\""); }
  std::string tree() {
    std::lock_guard lock(host.m);
    return host.last_tree;
  }
};

inline std::vector<std::string> files_in(const fs::path& dir) {
  std::vector<std::string> out;
  if (!fs::exists(dir)) return out;
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    // Relative paths with "/" on every platform, as the sender's names travel.
    std::string rel = utf8(fs::relative(e.path(), dir));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    out.push_back(std::move(rel));
  }
  std::sort(out.begin(), out.end());
  return out;
}

inline bool any_temporary(const fs::path& dir) {
  const auto all = files_in(dir);
  return std::any_of(all.begin(), all.end(), [](const std::string& n) { return n.find(".mvtmp") != std::string::npos; });
}

// A link over loopback: one end connected, the other accepted.
inline std::pair<std::unique_ptr<sp::link>, std::unique_ptr<sp::link>> loopback_pair() {
  auto l = sp::net::listen(true);
  REQUIRE(l);
  std::unique_ptr<sp::link> server;
  std::thread t([&] {
    auto a = (*l)->accept(5000);
    if (a) server = std::move(*a);
  });
  auto c = sp::net::connect({"127.0.0.1", (*l)->port()}, 5000);
  t.join();
  REQUIRE(c);
  REQUIRE(server);
  return {std::move(*c), std::move(server)};
}

// Both ends of a channel, opened at once.
inline std::pair<std::unique_ptr<sp::channel>, std::unique_ptr<sp::channel>> channel_pair() {
  auto [a, b] = loopback_pair();
  mv::result<std::unique_ptr<sp::channel>> server = mv::err(mv::status::internal);
  std::thread t([&, link = std::move(b)]() mutable { server = sp::channel::open(std::move(link), false, "bbbbbbbbbbbbbbbb", "Bea"); });
  auto opened = sp::channel::open(std::move(a), true, "aaaaaaaaaaaaaaaa", "Ann");
  t.join();
  REQUIRE(opened);
  REQUIRE(server);
  return {std::move(*opened), std::move(*server)};
}

// A link that passes bytes through and can flip one, at a byte count.
class tamper_link final : public sp::link {
 public:
  tamper_link(std::unique_ptr<sp::link> inner, std::size_t flip_at) : inner_(std::move(inner)), flip_at_(flip_at) {}
  mv::expected write_all(std::span<const std::uint8_t> bytes) override {
    std::vector<std::uint8_t> copy(bytes.begin(), bytes.end());
    for (std::size_t i = 0; i < copy.size(); ++i) {
      if (written_ + i == flip_at_) copy[i] ^= 0x40;
    }
    written_ += copy.size();
    return inner_->write_all(copy);
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    return inner_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override { inner_->shutdown(); }
  sp::transport via() const noexcept override { return inner_->via(); }

 private:
  std::unique_ptr<sp::link> inner_;
  std::size_t flip_at_;
  std::size_t written_ = 0;
};


// Beacons the test makes up, as a flood or a spoof would.
// The engine starts it on its control thread; `ready` publishes `seen` to the
// test's thread.
struct inject_discovery final : sp::discovery {
  seen_fn seen;
  std::atomic<bool> ready{false};
  bool start(const sp::beacon&, seen_fn s, tick_fn) override {
    seen = std::move(s);
    ready.store(true);
    return true;
  }
  void stop() noexcept override {}
};

}  // namespace
