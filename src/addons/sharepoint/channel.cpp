// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/channel.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "addons/sharepoint/noise.h"
#include "core/json.h"

namespace mv::sharepoint {

namespace {

using clock = std::chrono::steady_clock;

constexpr std::uint8_t kHelloMagic[4] = {'M', 'V', 'S', 'P'};
constexpr std::size_t kHelloBytes = 4 + 1 + crypto_kx_PUBLICKEYBYTES;  // protocol 1
constexpr std::uint8_t kKindNew = 'N';
constexpr std::uint8_t kKindRejoin = 'R';
constexpr std::size_t kSealOverhead = crypto_secretstream_xchacha20poly1305_ABYTES;
constexpr std::size_t kStreamHeaderBytes = crypto_secretstream_xchacha20poly1305_HEADERBYTES;
// Protocol 2's plaintext header: type, sequence number, acknowledgement.
constexpr std::size_t kHeader2 = 1 + 8 + 8;
constexpr char kPrologue2[] = "MediaViewer Sharepoint 2";
constexpr char kPrologueRejoin[] = "MediaViewer Sharepoint 2 rejoin";
constexpr char kCommitContext[] = "MediaViewer Sharepoint commit v2";
constexpr char kTagContext[] = "MediaViewer Sharepoint tag v2";
// A rejoin's first sealed message arrives within this (it proves the client
// holds the keys, so a replayed first message cannot take the session over).
constexpr int kRejoinConfirmMs = 4000;
// Spare message buffers kept for reuse: a whole replay window's worth, so a
// steady send allocates nothing (a chunk is too big for the small-block heap).
constexpr std::size_t kSpareBodies = kReplayBytes / kChunkBytes + 4;

void put_u32(std::uint8_t* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get_u32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
void put_u64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t get_u64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

std::int64_t now_ns() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
}

std::vector<std::uint8_t> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }

// The prologue each side hashes first: what the hello said in the clear.
std::vector<std::uint8_t> prologue(const char* context, std::uint8_t kind, std::span<const std::uint8_t> tag = {}) {
  std::vector<std::uint8_t> p = bytes_of(context);
  p.insert(p.end(), std::begin(kHelloMagic), std::end(kHelloMagic));
  p.push_back(2);
  p.push_back(kind);
  p.insert(p.end(), tag.begin(), tag.end());
  return p;
}

// One Noise message as it goes on the link: its length (2 bytes) and itself.
expected write_noise(link& l, std::span<const std::uint8_t> prefix, const std::vector<std::uint8_t>& m) {
  std::vector<std::uint8_t> out(prefix.begin(), prefix.end());
  out.push_back(static_cast<std::uint8_t>(m.size()));
  out.push_back(static_cast<std::uint8_t>(m.size() >> 8));
  out.insert(out.end(), m.begin(), m.end());
  return l.write_all(out);
}

result<std::vector<std::uint8_t>> read_noise(link& l, clock::time_point deadline) {
  std::uint8_t len[2];
  if (auto r = read_exact_until(l, len, deadline); !r) return err(r.error());
  const std::size_t n = static_cast<std::size_t>(len[0]) | static_cast<std::size_t>(len[1]) << 8;
  if (n < noise::kDhBytes) return err(status::corrupt);
  std::vector<std::uint8_t> m(n);
  if (auto r = read_exact_until(l, m, deadline); !r) return err(r.error());
  return m;
}

void commitment_v1(const std::uint8_t* pk, std::uint8_t out[32]) noexcept {
  static constexpr char kContext[] = "MediaViewer Sharepoint commit v1";
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 32);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kContext), sizeof kContext - 1);
  crypto_generichash_update(&st, pk, crypto_kx_PUBLICKEYBYTES);
  crypto_generichash_final(&st, out, 32);
}

void commitment_v2(std::span<const std::uint8_t, 32> nonce, std::uint8_t out[32]) noexcept {
  crypto_generichash_state st;
  crypto_generichash_init(&st, nullptr, 0, 32);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kCommitContext), sizeof kCommitContext - 1);
  crypto_generichash_update(&st, nonce.data(), nonce.size());
  crypto_generichash_final(&st, out, 32);
}

// A rejoin's tag: 8 random bytes and 8 of a keyed hash over them. Only a side
// holding the resume secret makes or recognises one; no two look alike.
void tag_mac(std::span<const std::uint8_t, 32> secret, const std::uint8_t nonce[8], std::uint8_t out[8]) noexcept {
  crypto_generichash_state st;
  crypto_generichash_init(&st, secret.data(), secret.size(), 8);
  crypto_generichash_update(&st, reinterpret_cast<const unsigned char*>(kTagContext), sizeof kTagContext - 1);
  crypto_generichash_update(&st, nonce, 8);
  crypto_generichash_final(&st, out, 8);
}

}  // namespace

struct channel::stream_keys {
  crypto_secretstream_xchacha20poly1305_state push{};
  crypto_secretstream_xchacha20poly1305_state pull{};
  ~stream_keys() { sodium_memzero(this, sizeof *this); }
};

struct channel::state {
  crypto_secretstream_xchacha20poly1305_state push{};  // under send_m_
  crypto_secretstream_xchacha20poly1305_state pull{};  // the reader's
  ~state() { sodium_memzero(this, sizeof *this); }
};

namespace {

// Both directions' secretstreams over `l`: this side's header out, the
// peer's in.
expected open_streams(link& l, std::span<const std::uint8_t, 32> tx, std::span<const std::uint8_t, 32> rx,
                      clock::time_point deadline, crypto_secretstream_xchacha20poly1305_state& push,
                      crypto_secretstream_xchacha20poly1305_state& pull) {
  std::uint8_t header[kStreamHeaderBytes];
  crypto_secretstream_xchacha20poly1305_init_push(&push, header, tx.data());
  if (!l.write_all(header)) return err(status::io);
  std::uint8_t peer_header[kStreamHeaderBytes];
  if (auto r = read_exact_until(l, peer_header, deadline); !r) return err(r.error());
  if (crypto_secretstream_xchacha20poly1305_init_pull(&pull, peer_header, rx.data()) != 0) return err(status::corrupt);
  return {};
}

// One protocol 2 frame into `buf`: the length, then the sealed header and body.
std::span<const std::uint8_t> seal2(std::vector<std::uint8_t>& buf, crypto_secretstream_xchacha20poly1305_state& push,
                                    msg type, std::uint64_t seq, std::uint64_t ack,
                                    std::span<const std::uint8_t> body) {
  const std::size_t plain = kHeader2 + body.size();
  const std::size_t sealed = plain + kSealOverhead;
  buf.resize(4 + sealed + plain);
  std::uint8_t* frame = buf.data();
  std::uint8_t* in = frame + 4 + sealed;  // the plaintext, staged after the frame
  in[0] = static_cast<std::uint8_t>(type);
  put_u64(in + 1, seq);
  put_u64(in + 9, ack);
  if (!body.empty()) std::memcpy(in + kHeader2, body.data(), body.size());
  unsigned long long out_len = 0;
  crypto_secretstream_xchacha20poly1305_push(&push, frame + 4, &out_len, in, plain, nullptr, 0,
                                             crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
  sodium_memzero(in, plain);
  put_u32(frame, static_cast<std::uint32_t>(out_len));
  return {frame, 4 + static_cast<std::size_t>(out_len)};
}

// A kept message, already laid out as plaintext (its header in front), sealed
// into `buf` as one frame with the acknowledgement as of now. No copy of the
// body: it is sealed where it lies.
std::span<const std::uint8_t> seal_kept(std::vector<std::uint8_t>& buf, crypto_secretstream_xchacha20poly1305_state& push,
                                        std::vector<std::uint8_t>& plain, std::uint64_t ack) {
  put_u64(plain.data() + 9, ack);
  buf.resize(4 + plain.size() + kSealOverhead);
  unsigned long long out_len = 0;
  crypto_secretstream_xchacha20poly1305_push(&push, buf.data() + 4, &out_len, plain.data(), plain.size(), nullptr, 0,
                                             crypto_secretstream_xchacha20poly1305_TAG_MESSAGE);
  put_u32(buf.data(), static_cast<std::uint32_t>(out_len));
  return {buf.data(), 4 + static_cast<std::size_t>(out_len)};
}

bool valid_type(std::uint8_t raw, std::uint32_t version) noexcept {
  const auto last = version >= 2 ? msg::resume_at : msg::ping;
  return raw >= static_cast<std::uint8_t>(msg::ident) && raw <= static_cast<std::uint8_t>(last);
}

}  // namespace

// ---- opening ---------------------------------------------------------------------------

result<hello> channel::read_hello(link& l, clock::time_point deadline) {
  std::uint8_t head[5];
  if (auto r = read_exact_until(l, head, deadline); !r) return err(r.error());
  if (std::memcmp(head, kHelloMagic, 4) != 0) return err(status::corrupt);
  hello h;
  h.version = head[4];
  if (h.version == kProtocol1) {
    h.body.resize(crypto_kx_PUBLICKEYBYTES);
    if (auto r = read_exact_until(l, h.body, deadline); !r) return err(r.error());
    return h;
  }
  if (h.version != 2) return err(status::unsupported_format);
  std::uint8_t kind = 0;
  if (auto r = read_exact_until(l, {&kind, 1}, deadline); !r) return err(r.error());
  if (kind != kKindNew && kind != kKindRejoin) return err(status::corrupt);
  h.rejoin = kind == kKindRejoin;
  if (h.rejoin) {
    if (auto r = read_exact_until(l, h.tag, deadline); !r) return err(r.error());
  }
  auto m = read_noise(l, deadline);
  if (!m) return err(m.error());
  h.body = std::move(*m);
  return h;
}

result<std::unique_ptr<channel>> channel::open(std::shared_ptr<link> l, bool client, std::string_view my_id,
                                               std::string_view my_name, const options& opts) {
  if (!l) return err(status::invalid_arg);
  if (sodium_init() < 0) return err(status::internal);
  if (client) {
    const auto deadline = clock::now() + std::chrono::milliseconds(kConnectMs);
    std::unique_ptr<channel> c(new channel());
    c->via_ = l->via();
    c->link_ = c->write_link_ = c->read_link_ = l;
    c->st_ = std::make_unique<state>();
    c->version_ = opts.version >= 2 ? 2 : kProtocol1;
    auto opened = c->version_ >= 2 ? open_v2(std::move(c), true, nullptr, opts, deadline)
                                   : open_v1(std::move(c), true, nullptr, deadline);
    if (!opened) return err(opened.error());
    if (auto r = (*opened)->exchange_ident(my_id, my_name, deadline); !r) {
      l->shutdown();
      return err(r.error());
    }
    return opened;
  }
  auto h = read_hello(*l, clock::now() + std::chrono::milliseconds(kConnectMs));
  if (!h) {
    l->shutdown();
    return err(h.error());
  }
  return open_server(std::move(l), std::move(*h), my_id, my_name, opts);
}

result<std::unique_ptr<channel>> channel::open_server(std::shared_ptr<link> l, hello h, std::string_view my_id,
                                                      std::string_view my_name, const options& opts) {
  if (!l) return err(status::invalid_arg);
  if (sodium_init() < 0) return err(status::internal);
  if (h.rejoin) {
    // Names a session that is not here (it ended, or this is someone else's).
    l->shutdown();
    return err(status::not_found);
  }
  const auto deadline = clock::now() + std::chrono::milliseconds(kConnectMs);
  std::unique_ptr<channel> c(new channel());
  c->via_ = l->via();
  c->link_ = c->write_link_ = c->read_link_ = l;
  c->st_ = std::make_unique<state>();
  c->version_ = h.version >= 2 ? 2 : kProtocol1;
  auto opened = c->version_ >= 2 ? open_v2(std::move(c), false, &h, opts, deadline)
                                 : open_v1(std::move(c), false, &h, deadline);
  if (!opened) return err(opened.error());
  if (auto r = (*opened)->exchange_ident(my_id, my_name, deadline); !r) {
    l->shutdown();
    return err(r.error());
  }
  return opened;
}

result<std::unique_ptr<channel>> channel::open_v1(std::unique_ptr<channel> c, bool client, const hello* theirs,
                                                  clock::time_point deadline) {
  link& l = *c->link_;
  // 1-3. Commit, answer, reveal: this link's own key pair, never reused.
  std::uint8_t pk[crypto_kx_PUBLICKEYBYTES], sk[crypto_kx_SECRETKEYBYTES];
  crypto_kx_keypair(pk, sk);
  std::uint8_t rx[crypto_kx_SESSIONKEYBYTES]{}, tx[crypto_kx_SESSIONKEYBYTES]{};
  auto fail = [&](status s) -> result<std::unique_ptr<channel>> {
    sodium_memzero(sk, sizeof sk);
    sodium_memzero(rx, sizeof rx);
    sodium_memzero(tx, sizeof tx);
    l.shutdown();
    return err(s);
  };
  auto make_hello = [](const std::uint8_t* payload, std::uint8_t out[kHelloBytes]) {
    std::memcpy(out, kHelloMagic, 4);
    out[4] = static_cast<std::uint8_t>(kProtocol1);
    std::memcpy(out + 5, payload, 32);
  };
  std::uint8_t peer_pk[crypto_kx_PUBLICKEYBYTES];
  std::uint8_t mine_hello[kHelloBytes];
  if (client) {
    std::uint8_t commit[32];
    commitment_v1(pk, commit);
    make_hello(commit, mine_hello);
    if (!l.write_all(mine_hello)) return fail(status::io);
    std::uint8_t theirs_hello[kHelloBytes];
    if (auto r = read_exact_until(l, theirs_hello, deadline); !r) return fail(r.error());
    if (std::memcmp(theirs_hello, kHelloMagic, 4) != 0) return fail(status::corrupt);
    if (theirs_hello[4] != kProtocol1) return fail(status::unsupported_format);
    std::memcpy(peer_pk, theirs_hello + 5, sizeof peer_pk);
    if (!l.write_all({pk, sizeof pk})) return fail(status::io);  // the reveal
  } else {
    make_hello(pk, mine_hello);  // only now: the client is already bound to its key
    if (!l.write_all(mine_hello)) return fail(status::io);
    if (auto r = read_exact_until(l, {peer_pk, sizeof peer_pk}, deadline); !r) return fail(r.error());
    std::uint8_t expect[32];
    commitment_v1(peer_pk, expect);
    if (theirs->body.size() != 32 || sodium_memcmp(expect, theirs->body.data(), 32) != 0) {
      return fail(status::corrupt);
    }
  }

  // 4. One key per direction.
  const int kx = client ? crypto_kx_client_session_keys(rx, tx, pk, sk, peer_pk)
                        : crypto_kx_server_session_keys(rx, tx, pk, sk, peer_pk);
  sodium_memzero(sk, sizeof sk);
  if (kx != 0) return fail(status::corrupt);  // a key that is not a usable point
  const auto opened = open_streams(l, std::span<const std::uint8_t, 32>(tx, 32), std::span<const std::uint8_t, 32>(rx, 32),
                                   deadline, c->st_->push, c->st_->pull);
  sodium_memzero(rx, sizeof rx);
  sodium_memzero(tx, sizeof tx);
  if (!opened) return fail(opened.error());

  // 5. The code: the client's key first.
  const std::span<const std::uint8_t, 32> mine(pk, 32), peer(peer_pk, 32);
  c->code_ = client ? pairing_code(mine, peer) : pairing_code(peer, mine);
  return c;
}

result<std::unique_ptr<channel>> channel::open_v2(std::unique_ptr<channel> c, bool client, const hello* theirs,
                                                  const options& opts, clock::time_point deadline) {
  link& l = *c->link_;
  auto fail = [&](status s) -> result<std::unique_ptr<channel>> {
    l.shutdown();
    return err(s);
  };
  std::optional<std::array<std::uint8_t, 32>> s = opts.static_key;
  if (!s) {
    s.emplace();
    randombytes_buf(s->data(), s->size());
  }
  noise::handshake hs(noise::xx_hfs(), client, prologue(kPrologue2, kKindNew), s);
  sodium_memzero(s->data(), s->size());
  std::vector<std::uint8_t> out, payload;
  std::array<std::uint8_t, noise::kHashBytes> h2{};
  std::array<std::uint8_t, 32> nonce{};
  if (client) {
    // -> e, e1: and a commitment to a nonce revealed only in the last message.
    randombytes_buf(nonce.data(), nonce.size());
    std::uint8_t commit[32];
    commitment_v2(nonce, commit);
    if (!hs.write_message(commit, out)) return fail(status::internal);
    const std::uint8_t prefix[] = {'M', 'V', 'S', 'P', 2, kKindNew};
    if (!write_noise(l, prefix, out)) return fail(status::io);
    // <- e, ee, ekem1, s, es
    auto m2 = read_noise(l, deadline);
    if (!m2) return fail(m2.error());
    if (!hs.read_message(*m2, payload) || !payload.empty()) return fail(status::corrupt);
    h2 = hs.hash();
    // -> s, se: and the nonce.
    if (!hs.write_message(nonce, out)) return fail(status::internal);
    if (!write_noise(l, {}, out)) return fail(status::io);
  } else {
    if (!hs.read_message(theirs->body, payload) || payload.size() != 32) return fail(status::corrupt);
    std::array<std::uint8_t, 32> commit;
    std::memcpy(commit.data(), payload.data(), 32);
    if (!hs.write_message({}, out)) return fail(status::corrupt);  // e.g. a KEM key that is not well formed
    h2 = hs.hash();
    if (!write_noise(l, {}, out)) return fail(status::io);
    auto m3 = read_noise(l, deadline);
    if (!m3) return fail(m3.error());
    if (!hs.read_message(*m3, payload) || payload.size() != 32) return fail(status::corrupt);
    std::memcpy(nonce.data(), payload.data(), 32);
    sodium_memzero(payload.data(), payload.size());
    std::uint8_t expect[32];
    commitment_v2(nonce, expect);
    // The nonce is the one the client committed to before it saw anything of ours.
    if (sodium_memcmp(expect, commit.data(), 32) != 0) return fail(status::corrupt);
  }
  auto keys = hs.split();
  if (!keys) return fail(status::internal);
  const auto& tx = client ? keys->initiator_to_responder : keys->responder_to_initiator;
  const auto& rx = client ? keys->responder_to_initiator : keys->initiator_to_responder;
  if (auto r = open_streams(l, tx, rx, deadline, c->st_->push, c->st_->pull); !r) return fail(r.error());
  c->code_ = pairing_code_v2(h2, nonce);
  sodium_memzero(nonce.data(), nonce.size());
  {
    std::lock_guard lock(c->secret_m_);
    c->resume_ = keys->extra;  // the rejoin's psk: in memory only, for this session
  }
  c->last_heard_ns_ = now_ns();
  return c;
}

expected channel::exchange_ident(std::string_view my_id, std::string_view my_name, clock::time_point deadline) {
  // Who each side is, sealed.
  json::writer w;
  w.begin_object().key("id").string(my_id).key("name").string(clean_name(my_name)).end_object();
  last_send_ = clock::now();
  if (auto r = send(msg::ident, w.str()); !r) return r;
  message m;
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now());
  if (auto r = receive(m, static_cast<int>(std::max<long long>(0, left.count()))); !r) return r;
  if (m.type != msg::ident) return err(status::corrupt);
  const auto doc = json::parse(m.text(), 4);
  const std::string* id = doc ? doc->str("id") : nullptr;
  const std::string* name = doc ? doc->str("name") : nullptr;
  if (!id || !valid_id(*id) || !name) return err(status::corrupt);
  peer_id_ = *id;
  peer_name_ = clean_name(*name);
  opened_.store(true);
  pinger_ = std::thread([this] { ping_loop(); });
  return {};
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
  std::shared_ptr<link> l;
  {
    std::lock_guard lock(link_m_);
    l = link_;
  }
  link_cv_.notify_all();
  {
    std::lock_guard lock(replay_m_);
  }
  replay_cv_.notify_all();
  if (l) l->shutdown();
}

void channel::set_on_link(std::function<void(link_event)> fn) {
  std::lock_guard lock(link_m_);
  on_link_ = std::move(fn);
}

void channel::notify(link_event e) {
  std::function<void(link_event)> fn;
  {
    std::lock_guard lock(link_m_);
    fn = on_link_;
  }
  if (!fn) return;
  try {
    fn(e);
  } catch (...) {
  }
}

bool channel::link_up() const noexcept {
  if (down_.load()) return false;
  if (version_ < 2) return true;
  std::lock_guard lock(link_m_);
  return up_ && now_ns() - last_heard_ns_.load() < std::int64_t{kSuspectMs} * 1000000;
}

// ---- sending ------------------------------------------------------------------------------

expected channel::send(msg type, std::span<const std::uint8_t> body) {
  if (down_.load()) return err(status::cancelled);
  if (body.size() + kHeader2 > kMaxMessageBytes) return err(status::invalid_arg);
  if (version_ >= 2) return send_v2(type, body);
  std::lock_guard lock(send_m_);
  return send_locked(type, body);
}

expected channel::send_locked(msg type, std::span<const std::uint8_t> body) {
  // Protocol 1: the type byte and the body, sealed.
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
  last_send_ = clock::now();
  if (auto r = write_link_->write_all({frame, 4 + static_cast<std::size_t>(out_len)}); !r) {
    return err(down_.load() ? status::cancelled : r.error());
  }
  return {};
}

expected channel::write_frame2(msg type, std::uint64_t seq, std::span<const std::uint8_t> body) {
  // send_m_ held.
  const std::uint64_t ack = recv_seq_.load();
  const std::uint64_t heard = rx_bytes_.load();
  const auto frame = seal2(seal_buf_, st_->push, type, seq, ack, body);
  last_send_ = clock::now();
  if (auto r = write_link_->write_all(frame); !r) {
    mark_down(write_gen_);
    return r;
  }
  rx_bytes_at_ack_.store(heard);
  return {};
}

bool channel::rejoin_expired() const noexcept {
  return now_ns() - last_heard_ns_.load() >= std::int64_t{kRejoinMs} * 1000000;
}

expected channel::send_v2(msg type, std::span<const std::uint8_t> body) {
  // Room among what is kept unacknowledged, waited for before send_m_: a
  // rejoin takes send_m_ to move the session, and it is what frees room.
  {
    std::unique_lock lock(replay_m_);
    while (!down_.load() && !replay_.empty() && replay_bytes_ + body.size() > kReplayBytes) {
      replay_cv_.wait_for(lock, std::chrono::milliseconds(250));
      if (rejoin_expired()) {
        lock.unlock();
        shutdown();
        return err(status::io);
      }
    }
  }
  if (down_.load()) return err(status::cancelled);
  std::lock_guard lock(send_m_);
  (void)keep_and_write(type, body);
  // A link that failed under it is not this send's failure: the message is
  // kept, and a rejoin sends it again.
  return {};
}

expected channel::keep_and_write(msg type, std::span<const std::uint8_t> body) {
  // send_m_ held. The message goes into the replay as plaintext (one copy),
  // and is sealed from there, under replay_m_ so an acknowledgement cannot
  // recycle its buffer meanwhile; the write is outside it.
  bool current = false;
  {
    std::lock_guard l(link_m_);
    current = up_ && write_gen_ == gen_;
  }
  const std::uint64_t seq = ++send_seq_;
  std::span<const std::uint8_t> frame;
  {
    std::lock_guard r(replay_m_);
    entry e;
    e.seq = seq;
    if (!spare_.empty()) {
      e.plain = std::move(spare_.back());  // its old size kept: resizing to a chunk again fills nothing
      spare_.pop_back();
    }
    e.plain.resize(kHeader2 + body.size());
    e.plain[0] = static_cast<std::uint8_t>(type);
    put_u64(e.plain.data() + 1, seq);
    if (!body.empty()) std::memcpy(e.plain.data() + kHeader2, body.data(), body.size());
    replay_bytes_ += body.size();
    replay_.push_back(std::move(e));
    sent_seq_.store(seq);
    if (current) frame = seal_kept(seal_buf_, st_->push, replay_.back().plain, recv_seq_.load());
  }
  if (!current) return err(status::io);
  const std::uint64_t heard = rx_bytes_.load();
  last_send_ = clock::now();
  if (auto r = write_link_->write_all(frame); !r) {
    mark_down(write_gen_);
    return r;
  }
  rx_bytes_at_ack_.store(heard);
  return {};
}

bool channel::try_send(msg type, std::string_view text) noexcept {
  if (down_.load()) return false;
  std::unique_lock lock(send_m_, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  try {
    const std::span<const std::uint8_t> body(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    if (version_ < 2) return send_locked(type, body).has_value();
    // Said if the link is free now, never waited for (kept, like any other).
    return keep_and_write(type, body).has_value();
  } catch (...) {
    return false;
  }
}

void channel::flush(int timeout_ms) {
  if (version_ < 2) return;
  const auto until = clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    {
      std::lock_guard lock(replay_m_);
      if (replay_.empty()) return;
    }
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - clock::now()).count();
    if (left <= 0 || down_.load()) return;
    // The acknowledgement rides on the peer's pings, which receive() reads;
    // it returns the moment nothing is left unacknowledged.
    message m;
    if (auto r = receive_v2(m, static_cast<int>(left), true); !r && r.error() != status::timeout) return;
  }
}

void channel::trim(std::uint64_t acked) {
  std::lock_guard lock(replay_m_);
  if (acked <= peer_acked_) return;
  peer_acked_ = acked;
  while (!replay_.empty() && replay_.front().seq <= acked) {
    replay_bytes_ -= replay_.front().plain.size() - kHeader2;
    if (spare_.size() < kSpareBodies) spare_.push_back(std::move(replay_.front().plain));
    replay_.pop_front();
  }
  replay_cv_.notify_all();
}

// ---- receiving ----------------------------------------------------------------------------

expected channel::receive(message& out, int timeout_ms) {
  return version_ >= 2 ? receive_v2(out, timeout_ms, false) : receive_v1(out, timeout_ms);
}

expected channel::receive_v1(message& out, int timeout_ms) {
  const bool limited = timeout_ms >= 0;
  const auto deadline = clock::now() + std::chrono::milliseconds(limited ? timeout_ms : 0);
  link& l = *read_link_;
  for (;;) {
    if (down_.load()) return err(status::cancelled);
    // The first byte of the next frame: by the caller's deadline (pings do not
    // move it), and in any case within kSilenceMs.
    auto first = clock::now() + std::chrono::milliseconds(kSilenceMs);
    if (limited && deadline < first) first = deadline;
    std::uint8_t len_bytes[4];
    if (auto r = read_exact_until(l, {len_bytes, 1}, std::max(first, clock::now() + std::chrono::milliseconds(1))); !r) {
      return err(down_.load() ? status::cancelled : r.error());
    }
    // Once a frame has begun, all of it within kSilenceMs: a timeout never
    // leaves the stream part-way through one, and a peer cannot dribble it.
    const auto frame = clock::now() + std::chrono::milliseconds(kSilenceMs);
    if (auto r = read_exact_until(l, {len_bytes + 1, 3}, frame); !r) {
      return err(down_.load() ? status::cancelled : r.error());
    }
    const std::uint32_t len = get_u32(len_bytes);
    if (len <= kSealOverhead || len > kMaxMessageBytes + kSealOverhead) return err(status::corrupt);
    open_buf_.resize(len);
    if (auto r = read_exact_until(l, open_buf_, frame); !r) return err(down_.load() ? status::cancelled : r.error());
    out.body.resize(len - kSealOverhead);
    unsigned long long plain = 0;
    unsigned char tag = 0;
    if (crypto_secretstream_xchacha20poly1305_pull(&st_->pull, out.body.data(), &plain, &tag, open_buf_.data(),
                                                   open_buf_.size(), nullptr, 0) != 0 ||
        plain == 0) {
      return err(status::corrupt);
    }
    const auto raw = out.body[0];
    if (!valid_type(raw, kProtocol1)) return err(status::corrupt);
    out.type = static_cast<msg>(raw);
    out.body.erase(out.body.begin());
    out.body.resize(static_cast<std::size_t>(plain) - 1);
    if (out.type != msg::ping) return {};
    if (limited && clock::now() >= deadline) return err(status::timeout);
  }
}

// Reads `into` whole from `l`. The first byte by `first`; after it, each
// byte within kRejoinMs of the one before (a slow Bluetooth chunk is not a
// dead link). Every byte is a sign of life.
expected channel::read_alive(link& l, std::span<std::uint8_t> into, clock::time_point first) {
  std::size_t got = 0;
  while (got < into.size()) {
    const auto until = got == 0 ? first : clock::now() + std::chrono::milliseconds(kRejoinMs);
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(until - clock::now());
    if (left.count() <= 0) return err(status::timeout);
    auto n = l.read_some(into.subspan(got), static_cast<int>(left.count()));
    if (!n) return err(n.error());
    if (*n == 0) return err(status::io);
    got += *n;
    last_heard_ns_.store(now_ns());
  }
  return {};
}

expected channel::receive_v2(message& out, int timeout_ms, bool until_acked) {
  const bool limited = timeout_ms >= 0;
  const auto deadline = clock::now() + std::chrono::milliseconds(limited ? timeout_ms : 0);
  for (;;) {
    if (down_.load()) return err(status::cancelled);
    if (rejoin_expired()) {
      shutdown();
      return err(status::io);
    }
    // The link to read: a rejoin may have moved the session.
    bool up = false;
    {
      std::unique_lock lock(link_m_);
      if (gen_ != read_gen_ && next_pull_) {
        st_->pull = next_pull_->pull;
        next_pull_.reset();
        read_link_ = link_;
        read_gen_ = gen_;
      }
      up = up_ && read_gen_ == gen_;
      if (!up) {
        // Down: wait for a rejoin (the pinger ends the session at kRejoinMs).
        auto until = clock::now() + std::chrono::milliseconds(250);
        if (limited && deadline < until) until = deadline;
        link_cv_.wait_until(lock, until, [&] { return down_.load() || (gen_ != read_gen_ && next_pull_); });
      }
    }
    if (!up) {
      if (limited && clock::now() >= deadline) return err(status::timeout);
      continue;
    }
    // One frame. Its first byte by the caller's deadline, or within a second
    // (then round again: the link may have been replaced meanwhile).
    auto first = clock::now() + std::chrono::milliseconds(1000);
    if (limited && deadline < first) first = std::max(deadline, clock::now() + std::chrono::milliseconds(1));
    link& l = *read_link_;
    std::uint8_t len_bytes[4];
    auto r = read_alive(l, {len_bytes, 1}, first);
    if (!r && r.error() == status::timeout) {
      if (limited && clock::now() >= deadline) return err(status::timeout);
      continue;
    }
    // The rest of the frame: each byte within kRejoinMs of the last.
    const auto alive = [] { return clock::now() + std::chrono::milliseconds(kRejoinMs); };
    if (r) r = read_alive(l, {len_bytes + 1, 3}, alive());
    std::uint32_t len = 0;
    if (r) {
      len = get_u32(len_bytes);
      if (len <= kSealOverhead + kHeader2 - 1 || len > kMaxMessageBytes + kSealOverhead) r = err(status::corrupt);
    }
    if (r) {
      open_buf_.resize(len);
      r = read_alive(l, open_buf_, alive());
    }
    unsigned long long plain = 0;
    if (r) {
      out.body.resize(len - kSealOverhead);
      unsigned char tag = 0;
      if (crypto_secretstream_xchacha20poly1305_pull(&st_->pull, out.body.data(), &plain, &tag, open_buf_.data(),
                                                     open_buf_.size(), nullptr, 0) != 0 ||
          plain < kHeader2 || !valid_type(out.body[0], 2)) {
        r = err(status::corrupt);
      }
    }
    if (!r) {
      if (down_.load()) return err(status::cancelled);
      if (!opened_.load()) return err(r.error());
      // A failed link, or one bad byte on it: the link ends, the session
      // waits for a rejoin. What did not arrive is sent again.
      mark_down(read_gen_);
      continue;
    }
    const std::uint8_t* head = out.body.data();
    out.type = static_cast<msg>(head[0]);
    const std::uint64_t seq = get_u64(head + 1);
    const std::uint64_t ack = get_u64(head + 9);
    if (ack > sent_seq_.load()) return err(status::corrupt);  // acknowledges what was never sent
    trim(ack);
    // A ping never extends the deadline: past it, the next round only polls
    // (what has already arrived is still read).
    if (out.type == msg::ping) {
      if (until_acked) {
        std::lock_guard lock(replay_m_);
        if (replay_.empty()) return err(status::timeout);  // flush(): all acknowledged
      }
      continue;
    }
    if (out.type == msg::resume_at) return err(status::corrupt);  // only ever first on a rejoined link
    const std::uint64_t have = recv_seq_.load();
    if (seq <= have) continue;                         // sent again after a rejoin; already here
    if (seq != have + 1) return err(status::corrupt);  // a gap: never, on an ordered link
    out.body.erase(out.body.begin(), out.body.begin() + kHeader2);
    out.body.resize(static_cast<std::size_t>(plain) - kHeader2);
    recv_seq_.store(seq);
    if (out.type == msg::done) {
      // The last message of a transfer: acknowledged now, from here, before
      // the caller shuts the channel down (the peer flushes until it has this).
      std::lock_guard sending(send_m_);
      bool current = false;
      {
        std::lock_guard on_link(link_m_);
        current = up_ && write_gen_ == gen_;
      }
      if (current) (void)write_frame2(msg::ping, 0, {});
    }
    const std::uint64_t heard = rx_bytes_.fetch_add(out.body.size() + kHeader2) + out.body.size() + kHeader2;
    // Acknowledge at least every kAckBytes, and at once for anything but
    // file bytes (rare, and the last of a transfer must not wait).
    if (out.type != msg::data || heard - rx_bytes_at_ack_.load() >= kAckBytes) {
      {
        std::lock_guard lock(ping_m_);
        ack_wanted_ = true;
      }
      ping_cv_.notify_all();
    }
    return {};
  }
}

// ---- the pinger, and the session's health ---------------------------------------------------

void channel::ping_loop() {
  const bool v2 = version_ >= 2;
  const auto every = std::chrono::milliseconds(v2 ? 250 : kPingMs / 2);
  const auto idle = std::chrono::milliseconds(v2 ? kPing2Ms : kPingMs / 2);
  std::unique_lock lock(ping_m_);
  while (!down_.load()) {
    ping_cv_.wait_for(lock, every, [&] { return down_.load() || ack_wanted_; });
    if (down_.load()) break;
    const bool want = std::exchange(ack_wanted_, false);
    lock.unlock();
    if (!v2) {
      std::lock_guard s(send_m_);
      if (clock::now() - last_send_ >= idle) (void)send_locked(msg::ping, {});
    } else {
      if (rejoin_expired()) {
        shutdown();  // kRejoinMs without a link, or without a sign of life
        return;
      }
      std::uint64_t gen = 0;
      bool up = false;
      {
        std::lock_guard l(link_m_);
        gen = gen_;
        up = up_;
      }
      if (up && now_ns() - last_heard_ns_.load() >= std::int64_t{kSuspectMs} * 1000000 &&
          suspect_gen_.exchange(gen) != gen) {
        notify(link_event::suspect);
      }
      if (up) {
        std::lock_guard s(send_m_);
        if ((want || clock::now() - last_send_ >= idle) && write_gen_ == gen) (void)write_frame2(msg::ping, 0, {});
      }
    }
    lock.lock();
  }
}

// ---- moving the session (protocol 2) -----------------------------------------------------------

void channel::mark_down(std::uint64_t gen) noexcept {
  std::shared_ptr<link> old;
  {
    std::lock_guard lock(link_m_);
    if (gen != gen_ || !up_) return;
    up_ = false;
    old = link_;
  }
  link_cv_.notify_all();
  if (old) old->shutdown();
  if (!opened_.load()) return;
  notify(link_event::down);
}

bool channel::names_this(const hello& h) const noexcept {
  if (version_ < 2 || !h.rejoin || h.version != 2) return false;
  std::uint8_t mac[8];
  {
    std::lock_guard lock(secret_m_);
    tag_mac(resume_, h.tag.data(), mac);
  }
  return sodium_memcmp(mac, h.tag.data() + 8, 8) == 0;
}

expected channel::finish_rejoin(link& l, std::span<const std::uint8_t, 32> tx, std::span<const std::uint8_t, 32> rx,
                                 clock::time_point deadline, std::uint64_t& peer_got, stream_keys& keys) {
  // The secretstreams, then each side's resume_at, sealed: proof the other
  // side holds the keys, and where to re-send from.
  if (auto r = open_streams(l, tx, rx, deadline, keys.push, keys.pull); !r) return r;
  std::vector<std::uint8_t> buf;
  if (auto r = l.write_all(seal2(buf, keys.push, msg::resume_at, 0, recv_seq_.load(), {})); !r) return r;
  std::uint8_t len_bytes[4];
  if (auto r = read_exact_until(l, len_bytes, deadline); !r) return r;
  const std::uint32_t len = get_u32(len_bytes);
  if (len != kSealOverhead + kHeader2) return err(status::corrupt);
  std::vector<std::uint8_t> sealed(len), plain(kHeader2);
  if (auto r = read_exact_until(l, sealed, deadline); !r) return r;
  unsigned long long n = 0;
  unsigned char tag = 0;
  if (crypto_secretstream_xchacha20poly1305_pull(&keys.pull, plain.data(), &n, &tag, sealed.data(), sealed.size(),
                                                 nullptr, 0) != 0 ||
      n != kHeader2 || plain[0] != static_cast<std::uint8_t>(msg::resume_at)) {
    return err(status::corrupt);
  }
  peer_got = get_u64(plain.data() + 9);
  if (peer_got > sent_seq_.load()) return err(status::corrupt);
  return {};
}

expected channel::rejoin(std::shared_ptr<link> l) {
  if (!l) return err(status::invalid_arg);
  if (version_ < 2) return err(status::unsupported_format);
  std::lock_guard one(rejoin_m_);
  if (down_.load()) {
    l->shutdown();
    return err(status::cancelled);
  }
  const auto deadline = clock::now() + std::chrono::milliseconds(kConnectMs);
  auto fail = [&](status s) -> expected {
    l->shutdown();
    return err(down_.load() ? status::cancelled : s);
  };
  std::array<std::uint8_t, 32> psk;
  std::array<std::uint8_t, 16> tag;
  randombytes_buf(tag.data(), 8);
  {
    std::lock_guard lock(secret_m_);
    psk = resume_;
    tag_mac(resume_, tag.data(), tag.data() + 8);
  }
  noise::handshake hs(noise::nn_hfs_psk0(), true, prologue(kPrologueRejoin, kKindRejoin, tag), std::nullopt, psk);
  sodium_memzero(psk.data(), psk.size());
  std::vector<std::uint8_t> out, payload;
  if (!hs.write_message({}, out)) return fail(status::internal);
  std::vector<std::uint8_t> prefix = {'M', 'V', 'S', 'P', 2, kKindRejoin};
  prefix.insert(prefix.end(), tag.begin(), tag.end());
  if (!write_noise(*l, prefix, out)) return fail(status::io);
  auto m2 = read_noise(*l, deadline);
  if (!m2) return fail(m2.error());
  if (!hs.read_message(*m2, payload) || !payload.empty()) return fail(status::corrupt);
  auto keys = hs.split();
  if (!keys) return fail(status::internal);
  auto streams = std::make_unique<stream_keys>();
  std::uint64_t peer_got = 0;
  if (auto r = finish_rejoin(*l, keys->initiator_to_responder, keys->responder_to_initiator, deadline, peer_got, *streams);
      !r) {
    return fail(r.error());
  }
  return adopt(std::move(l), std::move(streams), peer_got);
}

expected channel::accept_rejoin(std::shared_ptr<link> l, const hello& h) {
  if (!l) return err(status::invalid_arg);
  std::lock_guard one(rejoin_m_);
  auto fail = [&](status s) -> expected {
    l->shutdown();
    return err(down_.load() ? status::cancelled : s);
  };
  if (down_.load()) return fail(status::cancelled);
  if (!names_this(h)) return fail(status::not_found);
  const auto deadline = clock::now() + std::chrono::milliseconds(kRejoinConfirmMs);
  std::array<std::uint8_t, 32> psk;
  {
    std::lock_guard lock(secret_m_);
    psk = resume_;
  }
  noise::handshake hs(noise::nn_hfs_psk0(), false, prologue(kPrologueRejoin, kKindRejoin, h.tag), std::nullopt, psk);
  sodium_memzero(psk.data(), psk.size());
  std::vector<std::uint8_t> out, payload;
  // A first message made without the secret does not open here.
  if (!hs.read_message(h.body, payload) || !payload.empty()) return fail(status::corrupt);
  if (!hs.write_message({}, out)) return fail(status::corrupt);
  if (!write_noise(*l, {}, out)) return fail(status::io);
  auto keys = hs.split();
  if (!keys) return fail(status::internal);
  auto streams = std::make_unique<stream_keys>();
  std::uint64_t peer_got = 0;
  // A replayed first message gets this far, and no further: it has no keys.
  if (auto r = finish_rejoin(*l, keys->responder_to_initiator, keys->initiator_to_responder, deadline, peer_got, *streams);
      !r) {
    return fail(r.error());
  }
  return adopt(std::move(l), std::move(streams), peer_got);
}

expected channel::adopt(std::shared_ptr<link> l, std::unique_ptr<stream_keys> keys, std::uint64_t peer_got) {
  const auto now = clock::now();
  std::shared_ptr<link> old;
  std::uint64_t gen = 0;
  bool flapping = false;
  crypto_secretstream_xchacha20poly1305_state push = keys->push;
  {
    std::lock_guard lock(link_m_);
    if (down_.load()) {
      sodium_memzero(&push, sizeof push);
      l->shutdown();
      return err(status::cancelled);
    }
    while (!rejoined_at_.empty() && now - rejoined_at_.front() > std::chrono::milliseconds(kFlapMs)) {
      rejoined_at_.pop_front();
    }
    rejoined_at_.push_back(now);
    flapping = static_cast<int>(rejoined_at_.size()) >= kFlapRejoins;
    if (!flapping) {
      old = link_;
      link_ = l;
      gen = ++gen_;
      up_ = true;
      next_pull_ = std::move(keys);
      via_ = l->via();
      last_heard_ns_.store(now_ns());
    }
  }
  if (flapping) {
    // Three rejoins in kFlapMs: the bearers are not holding; the session ends.
    sodium_memzero(&push, sizeof push);
    l->shutdown();
    shutdown();
    return err(status::busy);
  }
  link_cv_.notify_all();
  if (old && old != l) old->shutdown();  // a writer or the reader blocked on it returns
  rejoins_.fetch_add(1);
  {
    std::lock_guard s(send_m_);
    st_->push = push;
    sodium_memzero(&push, sizeof push);
    write_link_ = l;
    write_gen_ = gen;
    trim(peer_got);
    // What the peer has not got, again, in order, sealed for this link.
    std::uint64_t next = peer_got + 1;
    for (;;) {
      std::span<const std::uint8_t> frame;
      {
        std::lock_guard r(replay_m_);
        if (replay_.empty() || next < replay_.front().seq) break;
        const std::size_t at = static_cast<std::size_t>(next - replay_.front().seq);
        if (at >= replay_.size()) break;
        frame = seal_kept(seal_buf_, st_->push, replay_[at].plain, recv_seq_.load());
      }
      last_send_ = clock::now();
      if (!l->write_all(frame)) {
        mark_down(gen);
        break;
      }
      ++next;
    }
  }
  notify(link_event::up);
  return {};
}

}  // namespace mv::sharepoint
