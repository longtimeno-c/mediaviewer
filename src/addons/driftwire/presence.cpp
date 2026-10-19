// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// wifi_discovery: the UDP beacon (presence.h).
#include "addons/driftwire/presence.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace mv::driftwire {

namespace {

constexpr auto kBeaconEvery = std::chrono::milliseconds(1000);

class udp_discovery final : public discovery {
 public:
  explicit udp_discovery(std::uint16_t port) : port_(port) {}
  ~udp_discovery() override { stop(); }

  bool start(const beacon& self, seen_fn seen, tick_fn tick) override {
    stop();
    auto sock = net::open_beacon_socket(port_);
    if (!sock) return false;
    sock_ = std::move(*sock);
    stop_.store(false);
    const std::string datagram = encode_beacon(self);
    thread_ = std::thread([this, datagram, seen = std::move(seen), tick = std::move(tick)] {
      std::vector<std::uint8_t> buf(kMaxBeaconBytes + 1);
      auto next = std::chrono::steady_clock::now();
      while (!stop_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next) {
          (void)sock_->send({reinterpret_cast<const std::uint8_t*>(datagram.data()), datagram.size()});
          if (tick) tick();
          next = now + kBeaconEvery;
        }
        const int wait = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(next - std::chrono::steady_clock::now()).count());
        std::string from;
        auto n = sock_->receive(buf, wait < 1 ? 1 : wait, from);
        if (!n) {
          if (n.error() == status::timeout) continue;
          if (stop_.load()) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(200));  // a network change: try again
          continue;
        }
        if (auto b = decode_beacon({buf.data(), *n}); b && seen) seen(*b, from);
      }
    });
    return true;
  }

  void stop() noexcept override {
    stop_.store(true);
    if (sock_) sock_->shutdown();
    if (thread_.joinable()) thread_.join();
    sock_.reset();
  }

 private:
  std::uint16_t port_;
  std::unique_ptr<net::beacon_socket> sock_;
  std::atomic<bool> stop_{true};
  std::thread thread_;
};

}  // namespace

std::unique_ptr<discovery> wifi_discovery(std::uint16_t port) { return std::make_unique<udp_discovery>(port); }

}  // namespace mv::driftwire
