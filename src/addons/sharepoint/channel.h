// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A sealed channel over a link (docs/design/28 "The wire").
//
// Handshake, commit then reveal (the pattern of ZRTP and Bluetooth numeric
// comparison), each side with an X25519 key made for this link alone:
//   1. the client (the side that connected, the sender) sends "MVSP", the
//      protocol version and a hash of its public key;
//   2. the server answers "MVSP", the version and its public key;
//   3. the client reveals its key, which must hash to what it sent.
// The client is bound to its key before it sees the server's, and the server
// shows its key only after that, so a machine in the middle cannot search for
// a pair of keys that gives both sides the same six digits: the code (a hash
// of both public keys) matches on both screens only if nobody is in between.
// Both derive one key per direction (crypto_kx) and open an XChaCha20-
// Poly1305 secretstream each way: every later message is encrypted,
// authenticated and in order. Names travel only sealed (msg::ident).
//
// Time: every wait is a deadline. A ping goes out whenever the channel has
// sent nothing for 2 s; receive() skips pings, which prove the link is alive
// but never extend a caller's deadline, and a frame must arrive whole within
// kSilenceMs of its first byte. Portable (D9).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "addons/sharepoint/link.h"
#include "addons/sharepoint/wire.h"
#include "core/result.h"

namespace mv::sharepoint {

struct message {
  msg type = msg::ping;
  std::vector<std::uint8_t> body;  // reused between receives
  [[nodiscard]] std::string_view text() const noexcept {
    return {reinterpret_cast<const char*>(body.data()), body.size()};
  }
};

class channel {
 public:
  ~channel();
  channel(const channel&) = delete;
  channel& operator=(const channel&) = delete;

  // Runs the handshake on the calling thread (a worker), within kConnectMs in
  // all. `client`: this side connected (the sender). The caller may keep its
  // own reference to `l` and shut it down to end a handshake that stalls.
  // Fails with status::unsupported_format for another protocol version,
  // status::corrupt for a bad hello or a key that does not match its
  // commitment, status::timeout when the other side is too slow.
  [[nodiscard]] static result<std::unique_ptr<channel>> open(std::shared_ptr<link> l, bool client,
                                                             std::string_view my_id, std::string_view my_name);

  [[nodiscard]] const std::string& code() const noexcept { return code_; }
  [[nodiscard]] const std::string& peer_id() const noexcept { return peer_id_; }
  [[nodiscard]] const std::string& peer_name() const noexcept { return peer_name_; }
  [[nodiscard]] transport via() const noexcept { return via_; }

  // Any thread; serialised.
  [[nodiscard]] expected send(msg type, std::span<const std::uint8_t> body);
  [[nodiscard]] expected send(msg type, std::string_view text) {
    return send(type, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  }
  // Sends only if no other send is under way (a cancel must never wait behind
  // a chunk on a slow link); false when it could not.
  bool try_send(msg type, std::string_view text) noexcept;
  // One reader thread. The next message that is not a ping. `timeout_ms` < 0:
  // no deadline of its own, the link only has to stay alive (a frame of any
  // kind at least every kSilenceMs). >= 0: status::timeout once that long has
  // passed without a message, pings or not (0 polls).
  [[nodiscard]] expected receive(message& out, int timeout_ms = -1);
  // Any thread: every blocked call returns; the pinger stops.
  void shutdown() noexcept;

 private:
  channel() = default;
  [[nodiscard]] expected send_locked(msg type, std::span<const std::uint8_t> body);
  void ping_loop();

  std::shared_ptr<link> link_;
  transport via_ = transport::wifi;
  std::string code_, peer_id_, peer_name_;
  struct state;
  std::unique_ptr<state> st_;
  std::mutex send_m_;
  std::vector<std::uint8_t> seal_buf_;               // under send_m_
  std::vector<std::uint8_t> open_buf_;               // the reader's
  std::chrono::steady_clock::time_point last_send_;  // under send_m_
  std::atomic<bool> down_{false};
  std::mutex ping_m_;
  std::condition_variable ping_cv_;
  std::thread pinger_;
};

}  // namespace mv::sharepoint
