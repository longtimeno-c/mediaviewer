// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A sealed channel over a link (docs/design/28 "The wire").
//
// Protocol 1 (a peer from before #472): commit then reveal (the pattern of
// ZRTP and Bluetooth numeric comparison), each side with an X25519 key made
// for this link alone:
//   1. the client (the side that connected, the sender) sends "MVSP", 1 and a
//      hash of its public key;
//   2. the server answers "MVSP", 1 and its public key;
//   3. the client reveals its key, which must hash to what it sent.
// Both derive one key per direction (crypto_kx) and open an XChaCha20-
// Poly1305 secretstream each way. The code is a hash of both public keys.
// The channel is its link: when the link drops, the channel ends.
//
// Protocol 2 (#472): a Noise handshake, Noise_XXhfs_25519+MLKEM768_ChaChaPoly_
// BLAKE2b (noise.h), so a recording of the link today needs both X25519 and
// ML-KEM-768 broken to be read later. The client's first payload commits to a
// random nonce it reveals only in its last message, after the server's every
// contribution is fixed; the code is a hash of the handshake hash after the
// server's message and that nonce, so a machine in the middle cannot search
// for keys that give both people the same digits. The secretstreams are keyed
// from the handshake's Split.
//
// And the channel is a session above the link. Every message has a sequence
// number and carries an acknowledgement; what the peer has not acknowledged
// is kept (at most kReplayBytes; a sender waits past it). When the link drops,
// or a faster one appears, the client opens a new link on any bearer and
// rejoins: a fresh Noise_NNhfs+psk0 handshake keyed by a resume secret the
// last handshake left (in memory only, replaced at every rejoin). Each side
// then says what it has received (resume_at) and the other re-sends the rest:
// a message is never lost, never repeated, and never re-sent once it arrived.
// Callers (transfer.h) see none of this: a send or receive waits through a
// rejoin. A link silent for kSuspectMs is suspect (the engine races the other
// bearers); kRejoinMs without a link ends the session; so do three rejoins in
// kFlapMs.
//
// Time: every wait is a deadline. A ping goes out whenever the channel has
// sent nothing for a while (protocol 2: kPing2Ms, carrying the acknowledgement);
// receive() skips pings, which prove the link is alive but never extend a
// caller's deadline. Portable (D9).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "addons/driftwire/link.h"
#include "addons/driftwire/wire.h"
#include "core/result.h"

namespace mv::driftwire {

struct message {
  msg type = msg::ping;
  std::vector<std::uint8_t> body;  // reused between receives
  [[nodiscard]] std::string_view text() const noexcept {
    return {reinterpret_cast<const char*>(body.data()), body.size()};
  }
};

// The first thing a link says, read before the receiver knows whether it is a
// new transfer or a rejoin of one under way (protocol 2).
struct hello {
  std::uint32_t version = 0;
  bool rejoin = false;
  bool internet = false;               // #472 PR 2: a sender with an internet code
  std::array<std::uint8_t, 16> tag{};  // rejoin: which session; internet: CPace's session id
  std::vector<std::uint8_t> body;      // protocol 1: the commitment; 2: the first Noise message;
                                       // internet: the sender's CPace message
};

// What happened to a protocol 2 channel's link (channel::set_on_link).
enum class link_event : std::uint8_t {
  suspect,  // nothing heard for kSuspectMs: worth racing the other bearers
  down,     // the link failed; the session waits kRejoinMs for another
  up,       // a rejoin moved the session onto a new link (via() says which)
};

struct channel_options {
  // Client: the protocol to speak (the peer's beacon or INFO says which it
  // has). The server answers whichever the hello is.
  std::uint32_t version = kProtocolVersion;
  // Protocol 2: this side's X25519 key in the XX handshake. Empty: one is
  // made for this link. The engine gives its identity (known.h) only behind
  // CPace (an internet code, a remembered device's meeting): a plain XX
  // handshake hands the responder's static to whoever connects (#501).
  std::optional<std::array<std::uint8_t, 32>> static_key;
  // #472 PR 2, the receiver: the secret of the code it showed, which an
  // internet hello must know. Empty: an internet hello is refused.
  std::optional<std::vector<std::uint8_t>> internet_secret;  // a direct code's 6 bytes, a word code's 32
};

class channel {
 public:
  ~channel();
  channel(const channel&) = delete;
  channel& operator=(const channel&) = delete;

  using options = channel_options;

  // Runs the handshake on the calling thread (a worker), within kConnectMs in
  // all. `client`: this side connected (the sender). The caller may keep its
  // own reference to `l` and shut it down to end a handshake that stalls.
  // Fails with status::unsupported_format for another protocol version,
  // status::corrupt for a bad hello or a key that does not match its
  // commitment, status::timeout when the other side is too slow, and
  // status::not_found for a rejoin (the server; see read_hello).
  [[nodiscard]] static result<std::unique_ptr<channel>> open(std::shared_ptr<link> l, bool client,
                                                             std::string_view my_id, std::string_view my_name,
                                                             const options& opts);
  [[nodiscard]] static result<std::unique_ptr<channel>> open(std::shared_ptr<link> l, bool client,
                                                             std::string_view my_id, std::string_view my_name) {
    return open(std::move(l), client, my_id, my_name, options{});
  }
  // #472 PR 2, the sender with an internet code: CPace on `secret`, then
  // Noise_XXhfs+psk0 keyed by it (noise.h). A wrong or spent code fails
  // (status::io or ::corrupt: the receiver hangs up). code() is then the
  // six-digit check both windows may show (it differs whenever someone is in
  // the middle), not a code to type.
  [[nodiscard]] static result<std::unique_ptr<channel>> open_internet(std::shared_ptr<link> l,
                                                                      std::span<const std::uint8_t> secret,
                                                                      std::string_view my_id, std::string_view my_name,
                                                                      const options& opts = {});
  // The server in two steps: the hello first (a rejoin goes to the live
  // channel it names, accept_rejoin), then a new channel from it. An
  // internet hello needs opts.internet_secret; a wrong code is
  // status::permission_denied.
  [[nodiscard]] static result<hello> read_hello(link& l, std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] static result<std::unique_ptr<channel>> open_server(std::shared_ptr<link> l, hello h,
                                                                    std::string_view my_id, std::string_view my_name,
                                                                    const options& opts = {});

  [[nodiscard]] const std::string& code() const noexcept { return code_; }
  [[nodiscard]] const std::string& peer_id() const noexcept { return peer_id_; }
  [[nodiscard]] const std::string& peer_name() const noexcept { return peer_name_; }
  [[nodiscard]] transport via() const noexcept { return via_.load(); }
  // The link now carrying it was punched after meeting on the DHT (link::punched).
  [[nodiscard]] bool punched() const noexcept { return punched_.load(); }
  [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
  // Protocol 2: the session can move to another link.
  [[nodiscard]] bool resumable() const noexcept { return version_ >= 2; }
  // Opened with an internet code (#472 PR 2): the peer is a new device,
  // known only by the code; code() is the check, not a code to type.
  [[nodiscard]] bool internet() const noexcept { return internet_; }
  // Protocol 2 (#472 PR 4): the peer's static X25519 key from the XX
  // handshake (its device identity over the internet; nearby, from #501, a
  // key for that link alone), and a secret only the two ends of this
  // session hold, for "Remember this device".
  [[nodiscard]] const std::optional<std::array<std::uint8_t, 32>>& peer_key() const noexcept { return peer_key_; }
  [[nodiscard]] std::array<std::uint8_t, 32> pairing_secret() const;
  // Protocol 2: a secret of this session (fixed for its life), apart from
  // pairing_secret(), for meeting on the DHT when both sides lose the local
  // link (dht_handover, #472, which mixes in rejoins() for each drop).
  [[nodiscard]] std::array<std::uint8_t, 32> handover_secret() const;

  // Any thread; serialised. Protocol 2: the message is kept until the peer
  // acknowledges it, so a link that drops under it does not fail the send;
  // past kReplayBytes unacknowledged this waits (as a full socket would).
  [[nodiscard]] expected send(msg type, std::span<const std::uint8_t> body);
  [[nodiscard]] expected send(msg type, std::string_view text) {
    return send(type, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
  }
  // Sends only if no other send is under way (a cancel must never wait behind
  // a chunk on a slow link); false when it could not.
  bool try_send(msg type, std::string_view text) noexcept;
  // One reader thread. The next message that is not a ping. `timeout_ms` < 0:
  // no deadline of its own, the link only has to stay alive (protocol 2: or
  // be replaced within kRejoinMs). >= 0: status::timeout once that long has
  // passed without a message, pings or not (0 polls).
  [[nodiscard]] expected receive(message& out, int timeout_ms = -1);
  // Protocol 2: reads (skipping pings) for up to `timeout_ms` until the peer
  // has acknowledged everything sent: the last message of a transfer, before
  // shutdown(). The reader thread's call; nothing else may be read meanwhile.
  void flush(int timeout_ms);
  // Any thread: every blocked call returns; the pinger stops.
  void shutdown() noexcept;

  // ---- protocol 2: moving the session -------------------------------------------------------
  // Called (any thread, briefly, never with a lock of the channel held) when
  // the link is suspect, fails, or is replaced.
  void set_on_link(std::function<void(link_event)> fn);
  // The client: a rejoin over `l`, a link to the same peer on any bearer. On
  // success the session continues there and the old link is closed. Fails
  // with status::cancelled once the session has ended, status::corrupt when
  // the peer does not hold this session's secret, status::busy after three
  // rejoins in kFlapMs (which ends the session).
  [[nodiscard]] expected rejoin(std::shared_ptr<link> l);
  // The server: `h` (from read_hello) names this session.
  [[nodiscard]] bool names_this(const hello& h) const noexcept;
  // The server: the rest of a rejoin that names_this().
  [[nodiscard]] expected accept_rejoin(std::shared_ptr<link> l, const hello& h);
  // The link is up (not suspect, not down).
  [[nodiscard]] bool link_up() const noexcept;
  // The session is over (shut down, or no link for kRejoinMs).
  [[nodiscard]] bool closed() const noexcept { return down_.load(); }
  // Rejoins so far (tests, the window).
  [[nodiscard]] std::uint32_t rejoins() const noexcept { return rejoins_.load(); }

 private:
  channel() = default;
  struct state;
  struct stream_keys;
  struct entry {
    std::uint64_t seq = 0;
    std::vector<std::uint8_t> plain;  // as sealed: type, sequence number, acknowledgement, body
  };

  [[nodiscard]] static result<std::unique_ptr<channel>> open_v1(std::unique_ptr<channel> c, bool client,
                                                                const hello* theirs,
                                                                std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] static result<std::unique_ptr<channel>> open_v2(std::unique_ptr<channel> c, bool client,
                                                                const hello* theirs, const options& opts,
                                                                std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] static result<std::unique_ptr<channel>> open_internet_v2(std::unique_ptr<channel> c, bool client,
                                                                         const hello* theirs,
                                                                         std::span<const std::uint8_t> secret,
                                                                         const options& opts,
                                                                         std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] expected exchange_ident(std::string_view my_id, std::string_view my_name,
                                        std::chrono::steady_clock::time_point deadline);
  // Protocol 1: one message, sealed, on the link. send_m_ held.
  [[nodiscard]] expected send_locked(msg type, std::span<const std::uint8_t> body);
  // Protocol 2: an unkept frame (a ping, which carries the acknowledgement)
  // on the current link. send_m_ held.
  [[nodiscard]] expected write_frame2(msg type, std::uint64_t seq, std::span<const std::uint8_t> body);
  [[nodiscard]] expected send_v2(msg type, std::span<const std::uint8_t> body);
  // Protocol 2: keeps the message and writes it on the current link (if
  // there is one up). send_m_ held.
  [[nodiscard]] expected keep_and_write(msg type, std::span<const std::uint8_t> body);
  [[nodiscard]] expected receive_v1(message& out, int timeout_ms);
  // `until_acked` (flush): status::timeout as soon as a ping leaves nothing
  // unacknowledged.
  [[nodiscard]] expected receive_v2(message& out, int timeout_ms, bool until_acked);
  [[nodiscard]] expected read_alive(link& l, std::span<std::uint8_t> into, std::chrono::steady_clock::time_point first);
  void ping_loop();
  // Protocol 2.
  [[nodiscard]] bool rejoin_expired() const noexcept;
  void mark_down(std::uint64_t gen) noexcept;
  void trim(std::uint64_t acked);
  [[nodiscard]] expected adopt(std::shared_ptr<link> l, std::unique_ptr<stream_keys> keys, std::uint64_t peer_got);
  [[nodiscard]] expected finish_rejoin(link& l, std::span<const std::uint8_t, 32> tx,
                                       std::span<const std::uint8_t, 32> rx,
                                       std::chrono::steady_clock::time_point deadline, std::uint64_t& peer_got,
                                       stream_keys& keys);
  void notify(link_event e);

  std::uint32_t version_ = kProtocolVersion;
  bool internet_ = false;
  std::optional<std::array<std::uint8_t, 32>> peer_key_;
  std::atomic<transport> via_{transport::wifi};
  std::atomic<bool> punched_{false};
  std::string code_, peer_id_, peer_name_;
  std::unique_ptr<state> st_;  // the keys: push under send_m_, pull the reader's
  std::atomic<bool> down_{false};
  std::atomic<bool> opened_{false};  // the handshake and ident are done

  // Writing: one sender at a time (send_m_), never waiting on a lock the
  // reader holds. Lock order: send_m_, then link_m_ or replay_m_.
  std::mutex send_m_;
  std::shared_ptr<link> write_link_;                 // under send_m_
  std::uint64_t write_gen_ = 1;                      // under send_m_
  std::vector<std::uint8_t> seal_buf_;               // under send_m_
  std::chrono::steady_clock::time_point last_send_;  // under send_m_
  std::uint64_t send_seq_ = 0;                       // under send_m_
  std::atomic<std::uint64_t> sent_seq_{0};           // the last sequence number given out

  // Kept until acknowledged (protocol 2). Short holds only, never across I/O.
  mutable std::mutex replay_m_;
  std::condition_variable replay_cv_;
  std::deque<entry> replay_;                      // under replay_m_
  std::vector<std::vector<std::uint8_t>> spare_;  // under replay_m_: buffers to reuse
  std::size_t replay_bytes_ = 0;                  // under replay_m_
  std::uint64_t peer_acked_ = 0;                  // under replay_m_

  // Reading: the reader thread's own.
  std::shared_ptr<link> read_link_;
  std::uint64_t read_gen_ = 1;
  std::vector<std::uint8_t> open_buf_;
  std::atomic<std::uint64_t> recv_seq_{0};         // messages received (what an ack says)
  std::atomic<std::uint64_t> rx_bytes_{0};         // plaintext received
  std::atomic<std::uint64_t> rx_bytes_at_ack_{0};  // rx_bytes_ when an ack last went out

  // The link the session is on (protocol 2): changed by a rejoin.
  mutable std::mutex link_m_;
  std::condition_variable link_cv_;
  std::shared_ptr<link> link_;                     // under link_m_
  std::uint64_t gen_ = 1;                          // under link_m_
  bool up_ = true;                                 // under link_m_
  std::unique_ptr<stream_keys> next_pull_;         // under link_m_: for the reader, at gen_
  std::function<void(link_event)> on_link_;        // under link_m_
  std::deque<std::chrono::steady_clock::time_point> rejoined_at_;  // under link_m_
  std::atomic<std::int64_t> last_heard_ns_{0};     // steady_clock: the last byte from the peer, or a rejoin
  std::atomic<std::uint64_t> suspect_gen_{0};      // the link already reported suspect

  // The resume secret (protocol 2): the psk of every rejoin of this session.
  // From the first handshake's Split; never leaves memory.
  std::mutex rejoin_m_;  // one rejoin at a time
  mutable std::mutex secret_m_;
  std::array<std::uint8_t, 32> resume_{};  // under secret_m_
  std::atomic<std::uint32_t> rejoins_{0};

  std::mutex ping_m_;
  std::condition_variable ping_cv_;
  bool ack_wanted_ = false;  // under ping_m_
  std::thread pinger_;
};

}  // namespace mv::driftwire
