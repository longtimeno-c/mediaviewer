// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Bluetooth byte pipe both bearers share (docs/design/28 "Bluetooth").
//
// Bluetooth LE between a Mac and a Windows PC has one common way to move
// bytes: GATT. Windows offers apps no connection-oriented LE channel, so the
// pipe is a GATT service of MediaViewer's own:
//
//   INFO  read    {"v":1,"p":2,"id":"<16 hex>","name":"..."}: who advertises
//                 ("p", #472: the protocol it speaks; none is protocol 1)
//   C2P   write without response: bytes from the sender (the central)
//   P2C   notify: bytes from the receiver (the peripheral)
//
// Every value is one packet: a type byte (0 data, 1 close) and up to the
// connection's packet size less one. GATT keeps them in order on one
// connection, and the sealed channel above (channel.h) checks every byte, so
// the pipe adds no checksum of its own. LE moves tens of kilobytes a second:
// a protocol 1 peer gets small sends only (kBluetoothMaxBytes); with protocol
// 2 a send starts here and moves to Wi-Fi as soon as both are on one network.
//
// bt_pipe is the link a bearer hands the engine: the bearer feeds it packets
// from its callbacks (deliver, closed_by_peer: never blocking) and gives it a
// way to send one (blocking on the engine's thread until the radio takes it).
// Portable (D9).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/sharepoint/link.h"

namespace mv::sharepoint::bt {

inline constexpr const char* kServiceUuid = "C01014FB-2640-4F63-BCB5-464F6177B271";
inline constexpr const char* kInfoUuid = "4AE794CF-3EF9-4789-8F27-5C9FD8F0A94C";
inline constexpr const char* kC2PUuid = "0E0C922D-E539-491E-B24E-A68505F01F08";
inline constexpr const char* kP2CUuid = "951908A3-83AA-4B6E-AFE3-923D5F3897C6";
inline constexpr std::uint8_t kPacketData = 0;
inline constexpr std::uint8_t kPacketClose = 1;
// Received bytes held for a reader that is behind; past it the link fails
// (a peer that ignores flow control).
inline constexpr std::size_t kMaxBuffered = 4u * 1024 * 1024;

// The INFO value for (id, name), and back.
[[nodiscard]] std::string info_json(const std::string& id, const std::string& name);
[[nodiscard]] bool parse_info(std::span<const std::uint8_t> value, std::string& id, std::string& name,
                              std::uint32_t& protocol);

class pipe final : public link {
 public:
  // Sends one packet (type byte included); blocks until the radio takes it.
  // False when the connection is gone. Never called on the bearer's queue.
  using send_fn = std::function<bool(std::span<const std::uint8_t> packet)>;
  // Called once, on the thread that shut the pipe down, after it is down:
  // the bearer sends a close packet (without waiting) and disconnects.
  using closed_fn = std::function<void()>;

  pipe(std::size_t packet_bytes, send_fn send, closed_fn on_shutdown);
  ~pipe() override;

  // ---- from the bearer's callbacks (never block) -------------------------------
  void deliver(std::span<const std::uint8_t> packet);
  void closed_by_peer() noexcept;
  void set_packet_bytes(std::size_t n) noexcept;

  // ---- link ------------------------------------------------------------------------
  expected write_all(std::span<const std::uint8_t> bytes) override;
  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override;
  void shutdown() noexcept override;
  transport via() const noexcept override { return transport::bluetooth; }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<std::uint8_t> in_;
  bool peer_closed_ = false;
  bool down_ = false;
  bool told_ = false;
  std::size_t packet_bytes_;
  send_fn send_;
  closed_fn on_shutdown_;
  std::mutex write_m_;
  std::vector<std::uint8_t> packet_;  // under write_m_
};

// Calls into the engine from a bearer's own threads, closed at stop(): after
// close() returns no call is running and none will start, so the engine can
// go away under a late radio callback.
class callback_gate {
 public:
  template <typename Fn>
  void call(Fn&& fn) {
    {
      std::lock_guard lock(m_);
      if (!open_) return;
      ++running_;
    }
    try {
      fn();
    } catch (...) {
    }
    std::lock_guard lock(m_);
    if (--running_ == 0) cv_.notify_all();
  }
  void open() {
    std::lock_guard lock(m_);
    open_ = true;
  }
  void close() {
    std::unique_lock lock(m_);
    open_ = false;
    cv_.wait(lock, [&] { return running_ == 0; });
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  bool open_ = false;
  int running_ = 0;
};

// "C01014FB-2640-..." into its 16 bytes as a GUID lays them out (Data1-3
// native-endian integers, Data4 bytes). False if it is not that shape.
struct guid_parts {
  std::uint32_t d1 = 0;
  std::uint16_t d2 = 0, d3 = 0;
  std::uint8_t d4[8] = {};
};
[[nodiscard]] bool parse_guid(std::string_view text, guid_parts& out) noexcept;

// A pipe the bearer still references, handed to the engine as the link it
// owns: destroying it shuts the pipe (the bearer's close and disconnect).
class pipe_ref final : public link {
 public:
  explicit pipe_ref(std::shared_ptr<pipe> p) : p_(std::move(p)) {}
  ~pipe_ref() override { p_->shutdown(); }
  expected write_all(std::span<const std::uint8_t> b) override { return p_->write_all(b); }
  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    return p_->read_some(into, timeout_ms);
  }
  void shutdown() noexcept override { p_->shutdown(); }
  transport via() const noexcept override { return transport::bluetooth; }

 private:
  std::shared_ptr<pipe> p_;
};

}  // namespace mv::sharepoint::bt
