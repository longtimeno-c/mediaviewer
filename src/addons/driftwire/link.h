// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A reliable, ordered byte pipe between two MediaViewers: a TCP connection
// (net.h) or the Bluetooth pipe (bt.h). The channel (channel.h) seals what
// goes through it; a link only moves bytes. Portable (D9).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "addons/driftwire/wire.h"
#include "core/result.h"

namespace mv::driftwire {

class link {
 public:
  virtual ~link() = default;
  // Every byte, or an error (io, timeout, cancelled after shutdown). Thread-safe
  // against read_some on another thread; one writer at a time.
  [[nodiscard]] virtual expected write_all(std::span<const std::uint8_t> bytes) = 0;
  // At least one byte; 0 when the other side closed. `timeout_ms` < 0 waits
  // until data, close or shutdown(); status::timeout when it passes.
  [[nodiscard]] virtual result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) = 0;
  // Any thread, never blocks long: a read or write in progress returns, and
  // every later one fails.
  virtual void shutdown() noexcept = 0;
  [[nodiscard]] virtual transport via() const noexcept = 0;
  // #472 relays: what the bytes go through when it is not a straight path
  // ("relay": the person's own computer or server, splice.h), for the
  // window; nullptr for a direct link.
  [[nodiscard]] virtual const char* through() const noexcept { return nullptr; }
  // #472 PR 3: a stream on a hole punched after meeting on the DHT
  // (udp_hub), for the window's picture of the path.
  [[nodiscard]] virtual bool punched() const noexcept { return false; }
};

// A link that says it is another bearer than its own (#472 PR 2: a TCP link
// over the internet is not "Wi-Fi" on the window).
class bearer_as final : public link {
 public:
  bearer_as(std::unique_ptr<link> inner, transport via) : inner_(std::move(inner)), via_(via) {}
  [[nodiscard]] expected write_all(std::span<const std::uint8_t> bytes) override { return inner_->write_all(bytes); }
  [[nodiscard]] result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    return inner_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override { inner_->shutdown(); }
  [[nodiscard]] transport via() const noexcept override { return via_; }
  [[nodiscard]] const char* through() const noexcept override { return inner_->through(); }
  [[nodiscard]] bool punched() const noexcept override { return inner_->punched(); }

 private:
  std::unique_ptr<link> inner_;
  transport via_;
};

// Reads exactly `into.size()` bytes by `deadline` (the whole span, however the
// peer slices it). A clean close part-way is status::io. What is left is
// rounded up to whole milliseconds: rounded down, a deadline under 1 ms away
// (channel::receive's poll) is 0 ms and times out without reading at all.
[[nodiscard]] inline expected read_exact_until(link& l, std::span<std::uint8_t> into,
                                               std::chrono::steady_clock::time_point deadline) {
  std::size_t got = 0;
  while (got < into.size()) {
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) return err(status::timeout);
    auto n = l.read_some(into.subspan(got), static_cast<int>(left.count()));
    if (!n) return err(n.error());
    if (*n == 0) return err(status::io);
    got += *n;
  }
  return {};
}

[[nodiscard]] inline expected read_exact(link& l, std::span<std::uint8_t> into, int timeout_ms) {
  return read_exact_until(l, into, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
}

}  // namespace mv::driftwire
