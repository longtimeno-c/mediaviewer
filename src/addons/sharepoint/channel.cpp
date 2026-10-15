// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/channel.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/json.h"

namespace mv::sharepoint {

namespace {

constexpr std::uint8_t kHelloMagic[4] = {'M', 'V', 'S', 'P'};
constexpr std::size_t kHelloBytes = 4 + 1 + crypto_kx_PUBLICKEYBYTES;
constexpr std::size_t kSealOverhead = crypto_secretstream_xchacha20poly1305_ABYTES;

void put_u32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v);
  p[1] = static_cast<std::uint8_t>(v >> 8);
  p[2] = static_cast<std::uint8_t>(v >> 16);
  p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::uint32_t get_u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

}  // namespace

struct channel::state {
  crypto_secretstream_xchacha20poly1305_state push{};
  crypto_secretstream_xchacha20poly1305_state pull{};
  ~state() { sodium_memzero(this, sizeof *this); }
};

// The client's commitment to its public key.
static void commitment(const std::uint8_t* pk, std::uint8_t out[32]) noexcept {
  static constexpr char kContext[] = "MediaViewer Sharepoint commit v1";
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 32);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1);
  crypto_generichash_update(&st, pk, crypto_kx_PUBLICKEYBYTES);
  crypto_generichash_final(&st, out, 32);
}

result<std::unique_ptr<channel>> channel::open(std::shared_ptr<link> l, bool client, std::string_view my_id,
                                               std::string_view my_name) {
  if (!l) return err(status::invalid_arg);
  if (sodium_init() < 0) return err(status::internal);
  std::unique_ptr<channel> c(new channel());
  c->via_ = l->via();
  c->link_ = std::move(l);
  c->st_ = std::make_unique<state>();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectMs);

  // 1-3. Commit, answer, reveal: this link's own key pair, never reused.
  std::uint8_t pk[crypto_kx_PUBLICKEYBYTES], sk[crypto_kx_SECRETKEYBYTES];
  crypto_kx_keypair(pk, sk);
  std::uint8_t rx[crypto_kx_SESSIONKEYBYTES]{}, tx[crypto_kx_SESSIONKEYBYTES]{};
  auto fail = [&](status s) -> result<std::unique_ptr<channel>> {
    sodium_memzero(sk, sizeof sk);
    sodium_memzero(rx, sizeof rx);
    sodium_memzero(tx, sizeof tx);
    c->link_->shutdown();
    return err(s);
  };
  auto hello = [](const std::uint8_t* payload, std::uint8_t out[kHelloBytes]) {
    std::memcpy(out, kHelloMagic, 4);
    out[4] = static_cast<std::uint8_t>(kProtocolVersion);
    std::memcpy(out + 5, payload, 32);
  };
  auto check = [](const std::uint8_t in[kHelloBytes]) {
    if (std::memcmp(in, kHelloMagic, 4) != 0) return status::corrupt;
    return in[4] == kProtocolVersion ? status::ok : status::unsupported_format;
  };
  std::uint8_t peer_pk[crypto_kx_PUBLICKEYBYTES];
  std::uint8_t mine_hello[kHelloBytes], theirs_hello[kHelloBytes];
  if (client) {
    std::uint8_t commit[32];
    commitment(pk, commit);
    hello(commit, mine_hello);
    if (!c->link_->write_all(mine_hello)) return fail(status::io);
    if (auto r = read_exact_until(*c->link_, theirs_hello, deadline); !r) return fail(r.error());
    if (const status s = check(theirs_hello); s != status::ok) return fail(s);
    std::memcpy(peer_pk, theirs_hello + 5, sizeof peer_pk);
    if (!c->link_->write_all({pk, sizeof pk})) return fail(status::io);  // the reveal
  } else {
    if (auto r = read_exact_until(*c->link_, theirs_hello, deadline); !r) return fail(r.error());
    if (const status s = check(theirs_hello); s != status::ok) return fail(s);
    hello(pk, mine_hello);  // only now: the client is already bound to its key
    if (!c->link_->write_all(mine_hello)) return fail(status::io);
    if (auto r = read_exact_until(*c->link_, {peer_pk, sizeof peer_pk}, deadline); !r) return fail(r.error());
    std::uint8_t expect[32];
    commitment(peer_pk, expect);
    if (sodium_memcmp(expect, theirs_hello + 5, 32) != 0) return fail(status::corrupt);
  }

  // 4. One key per direction.
  const int kx = client ? crypto_kx_client_session_keys(rx, tx, pk, sk, peer_pk)
                        : crypto_kx_server_session_keys(rx, tx, pk, sk, peer_pk);
  sodium_memzero(sk, sizeof sk);
  if (kx != 0) return fail(status::corrupt);  // a key that is not a usable point
  std::uint8_t header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
  crypto_secretstream_xchacha20poly1305_init_push(&c->st_->push, header, tx);
  sodium_memzero(tx, sizeof tx);
  if (!c->link_->write_all(header)) return fail(status::io);
  std::uint8_t peer_header[sizeof header];
  if (auto r = read_exact_until(*c->link_, peer_header, deadline); !r) return fail(r.error());
  const int pulled = crypto_secretstream_xchacha20poly1305_init_pull(&c->st_->pull, peer_header, rx);
  sodium_memzero(rx, sizeof rx);
  if (pulled != 0) return fail(status::corrupt);

  // 5. The code: the client's key first.
  const std::span<const std::uint8_t, 32> mine(pk, 32), theirs(peer_pk, 32);
  c->code_ = client ? pairing_code(mine, theirs) : pairing_code(theirs, mine);

  // 6. Who each side is, sealed.
  json::writer w;
  w.begin_object().key("id").string(my_id).key("name").string(clean_name(my_name)).end_object();
  c->last_send_ = std::chrono::steady_clock::now();
  if (auto r = c->send(msg::ident, w.str()); !r) return fail(r.error());
  message m;
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
  if (auto r = c->receive(m, static_cast<int>(std::max<long long>(0, left.count()))); !r) return fail(r.error());
  if (m.type != msg::ident) return fail(status::corrupt);
  const auto doc = json::parse(m.text(), 4);
  const std::string* id = doc ? doc->str("id") : nullptr;
  const std::string* name = doc ? doc->str("name") : nullptr;
  if (!id || !valid_id(*id) || !name) return fail(status::corrupt);
  c->peer_id_ = *id;
  c->peer_name_ = clean_name(*name);
  c->pinger_ = std::thread([ch = c.get()] { ch->ping_loop(); });
  return c;
}

channel::~channel() {
  shutdown();
  if (pinger_.joinable()) pinger_.join();
}

void channel::shutdown() noexcept {
  {
    std::lock_guard lock(ping_m_);
    if (down_.exchange(true)) return;
  }
  ping_cv_.notify_all();
  if (link_) link_->shutdown();
}

expected channel::send(msg type, std::span<const std::uint8_t> body) {
  if (down_.load()) return err(status::cancelled);
  if (body.size() + 1 > kMaxMessageBytes) return err(status::invalid_arg);
  std::lock_guard lock(send_m_);
  return send_locked(type, body);
}

expected channel::send_locked(msg type, std::span<const std::uint8_t> body) {
  const std::size_t plain = body.size() + 1;
  const std::size_t sealed = plain + kSealOverhead;
  seal_buf_.resize(4 + sealed + plain);
  std::uint8_t* frame = seal_buf_.data();
  std::uint8_t* in = frame + 4 + sealed;  // the plaintext, staged after the frame
  in[0] = static_cast<std::uint8_t>(type);
  if (!body.empty()) std::memcpy(in + 1, body.data(), body.size());
  unsigned long long out_len = 0;
  crypto_secretstream_xchacha20poly1305_push(&st_->push, frame + 4, &out_len, in, plain, nullptr, 0,
                                             crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
  sodium_memzero(in, plain);
  put_u32(frame, static_cast<std::uint32_t>(out_len));
  last_send_ = std::chrono::steady_clock::now();
  if (auto r = link_->write_all({frame, 4 + static_cast<std::size_t>(out_len)}); !r) {
    return err(down_.load() ? status::cancelled : r.error());
  }
  return {};
}

bool channel::try_send(msg type, std::string_view text) noexcept {
  if (down_.load()) return false;
  std::unique_lock lock(send_m_, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  try {
    return send_locked(type, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}).has_value();
  } catch (...) {
    return false;
  }
}

expected channel::receive(message& out, int timeout_ms) {
  using clock = std::chrono::steady_clock;
  const bool limited = timeout_ms >= 0;
  const auto deadline = clock::now() + std::chrono::milliseconds(limited ? timeout_ms : 0);
  for (;;) {
    if (down_.load()) return err(status::cancelled);
    // The first byte of the next frame: by the caller's deadline (pings do not
    // move it), and in any case within kSilenceMs.
    auto first = clock::now() + std::chrono::milliseconds(kSilenceMs);
    if (limited && deadline < first) first = deadline;
    std::uint8_t len_bytes[4];
    if (auto r = read_exact_until(*link_, {len_bytes, 1}, std::max(first, clock::now() + std::chrono::milliseconds(1)));
        !r) {
      return err(down_.load() ? status::cancelled : r.error());
    }
    // Once a frame has begun, all of it within kSilenceMs: a timeout never
    // leaves the stream part-way through one, and a peer cannot dribble it.
    const auto frame = clock::now() + std::chrono::milliseconds(kSilenceMs);
    if (auto r = read_exact_until(*link_, {len_bytes + 1, 3}, frame); !r) {
      return err(down_.load() ? status::cancelled : r.error());
    }
    const std::uint32_t len = get_u32(len_bytes);
    if (len <= kSealOverhead || len > kMaxMessageBytes + kSealOverhead) return err(status::corrupt);
    open_buf_.resize(len);
    if (auto r = read_exact_until(*link_, open_buf_, frame); !r) return err(down_.load() ? status::cancelled : r.error());
    out.body.resize(len - kSealOverhead);
    unsigned long long plain = 0;
    unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(&st_->pull, out.body.data(), &plain, &tag, open_buf_.data(),
                                                   open_buf_.size(), nullptr, 0) != 0 ||
        plain == 0) {
      return err(status::corrupt);
    }
    const auto raw = out.body[0];
    if (raw < static_cast<std::uint8_t>(msg::ident) || raw > static_cast<std::uint8_t>(msg::ping)) {
      return err(status::corrupt);
    }
    out.type = static_cast<msg>(raw);
    out.body.erase(out.body.begin());
    out.body.resize(static_cast<std::size_t>(plain) - 1);
    if (out.type != msg::ping) return {};
    if (limited && clock::now() >= deadline) return err(status::timeout);
  }
}

void channel::ping_loop() {
  std::unique_lock lock(ping_m_);
  while (!down_.load()) {
    ping_cv_.wait_for(lock, std::chrono::milliseconds(kPingMs / 2));
    if (down_.load()) break;
    lock.unlock();
    {
      std::lock_guard s(send_m_);
      if (std::chrono::steady_clock::now() - last_send_ >= std::chrono::milliseconds(kPingMs / 2)) {
        (void)send_locked(msg::ping, {});
      }
    }
    lock.lock();
  }
}

}  // namespace mv::sharepoint
