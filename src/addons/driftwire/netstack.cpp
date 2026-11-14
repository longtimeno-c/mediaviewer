// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/netstack.h"

#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace mv::driftwire::vpn {

namespace {

std::uint16_t rd16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] << 8 | p[1]); }
std::uint32_t rd32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
         static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}
void wr16(std::uint8_t* p, std::uint16_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 8);
  p[1] = static_cast<std::uint8_t>(v);
}
void wr32(std::uint8_t* p, std::uint32_t v) {
  wr16(p, static_cast<std::uint16_t>(v >> 16));
  wr16(p + 2, static_cast<std::uint16_t>(v));
}

std::uint32_t sum_words(std::span<const std::uint8_t> b, std::uint32_t sum) {
  std::size_t i = 0;
  for (; i + 1 < b.size(); i += 2) sum += rd16(b.data() + i);
  if (i < b.size()) sum += static_cast<std::uint32_t>(b[i]) << 8;
  return sum;
}

// The pseudo header's sum: addresses, protocol, length.
std::uint32_t pseudo(const flow& f, bool reply, std::uint8_t proto, std::size_t len) {
  const std::size_t n = f.v6 ? 16 : 4;
  const auto& a = reply ? f.dst : f.src;
  const auto& b = reply ? f.src : f.dst;
  std::uint32_t s = sum_words({a.bytes.data(), n}, 0);
  s = sum_words({b.bytes.data(), n}, s);
  s += proto;
  s += static_cast<std::uint32_t>(len & 0xffff);
  if (f.v6) s += static_cast<std::uint32_t>(len >> 16);
  return s;
}

// An IP header in front of `l4` (reply direction), returned whole.
std::vector<std::uint8_t> with_ip(const flow& f, std::uint8_t proto, std::vector<std::uint8_t> l4) {
  std::vector<std::uint8_t> out;
  if (f.v6) {
    out.assign(40, 0);
    out[0] = 0x60;
    wr16(&out[4], static_cast<std::uint16_t>(l4.size()));
    out[6] = proto;
    out[7] = 64;
    std::memcpy(&out[8], f.dst.bytes.data(), 16);
    std::memcpy(&out[24], f.src.bytes.data(), 16);
  } else {
    out.assign(20, 0);
    out[0] = 0x45;
    wr16(&out[2], static_cast<std::uint16_t>(20 + l4.size()));
    wr16(&out[6], 0x4000);  // don't fragment
    out[8] = 64;
    out[9] = proto;
    std::memcpy(&out[12], f.dst.bytes.data(), 4);
    std::memcpy(&out[16], f.src.bytes.data(), 4);
    wr16(&out[10], checksum(out));
  }
  out.insert(out.end(), l4.begin(), l4.end());
  return out;
}

bool after(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a - b) > 0; }  // a > b, mod 2^32

std::uint32_t random32() {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  return randombytes_random();
}

}  // namespace

std::uint16_t checksum(std::span<const std::uint8_t> bytes, std::uint32_t sum) noexcept {
  sum = sum_words(bytes, sum);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return static_cast<std::uint16_t>(~sum);
}

std::vector<std::uint8_t> make_tcp(const flow& f, const tcp_fields& t, std::span<const std::uint8_t> payload) {
  const std::size_t hl = t.mss ? 24 : 20;
  std::vector<std::uint8_t> l4(hl + payload.size(), 0);
  wr16(&l4[0], f.dport);
  wr16(&l4[2], f.sport);
  wr32(&l4[4], t.seq);
  wr32(&l4[8], t.ack);
  l4[12] = static_cast<std::uint8_t>((hl / 4) << 4);
  l4[13] = t.flags;
  wr16(&l4[14], t.window);
  if (t.mss) {
    l4[20] = 2;
    l4[21] = 4;
    wr16(&l4[22], t.mss);
  }
  if (!payload.empty()) std::memcpy(&l4[hl], payload.data(), payload.size());
  wr16(&l4[16], checksum(l4, pseudo(f, true, kTcp, l4.size())));
  return with_ip(f, kTcp, std::move(l4));
}

std::vector<std::uint8_t> make_udp(const flow& f, std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> l4(8 + payload.size(), 0);
  wr16(&l4[0], f.dport);
  wr16(&l4[2], f.sport);
  wr16(&l4[4], static_cast<std::uint16_t>(l4.size()));
  if (!payload.empty()) std::memcpy(&l4[8], payload.data(), payload.size());
  std::uint16_t c = checksum(l4, pseudo(f, true, kUdp, l4.size()));
  if (c == 0) c = 0xffff;
  wr16(&l4[6], c);
  return with_ip(f, kUdp, std::move(l4));
}

std::optional<parsed> parse(std::span<const std::uint8_t> p) {
  if (p.size() < 20) return std::nullopt;
  parsed out;
  std::span<const std::uint8_t> l4;
  const int ver = p[0] >> 4;
  if (ver == 4) {
    const std::size_t ihl = static_cast<std::size_t>(p[0] & 15) * 4;
    const std::size_t total = rd16(&p[2]);
    if (ihl < 20 || total < ihl || total > p.size()) return std::nullopt;
    if ((rd16(&p[6]) & 0x3fff) != 0) return std::nullopt;  // a fragment
    if (checksum(p.first(ihl)) != 0) return std::nullopt;
    out.f.proto = p[9];
    std::memcpy(out.f.src.bytes.data(), &p[12], 4);
    std::memcpy(out.f.dst.bytes.data(), &p[16], 4);
    l4 = p.subspan(ihl, total - ihl);
  } else if (ver == 6) {
    if (p.size() < 40) return std::nullopt;
    const std::size_t len = rd16(&p[4]);
    if (40 + len > p.size()) return std::nullopt;
    out.f.v6 = true;
    out.f.src.v6 = out.f.dst.v6 = true;
    out.f.proto = p[6];  // no extension headers taken
    std::memcpy(out.f.src.bytes.data(), &p[8], 16);
    std::memcpy(out.f.dst.bytes.data(), &p[24], 16);
    l4 = p.subspan(40, len);
  } else {
    return std::nullopt;
  }
  if (out.f.proto == kTcp) {
    if (l4.size() < 20) return std::nullopt;
    const std::size_t off = static_cast<std::size_t>(l4[12] >> 4) * 4;
    if (off < 20 || off > l4.size()) return std::nullopt;
    if (checksum(l4, pseudo(out.f, false, kTcp, l4.size())) != 0) return std::nullopt;
    out.f.sport = rd16(&l4[0]);
    out.f.dport = rd16(&l4[2]);
    out.tcp.seq = rd32(&l4[4]);
    out.tcp.ack = rd32(&l4[8]);
    out.tcp.flags = l4[13];
    out.tcp.window = rd16(&l4[14]);
    for (std::size_t i = 20; i < off;) {  // options: MSS only
      const std::uint8_t kind = l4[i];
      if (kind == 0) break;
      if (kind == 1) {
        ++i;
        continue;
      }
      if (i + 1 >= off || l4[i + 1] < 2) break;
      if (kind == 2 && l4[i + 1] == 4 && i + 4 <= off) out.tcp.mss = rd16(&l4[i + 2]);
      i += l4[i + 1];
    }
    out.payload = l4.subspan(off);
    return out;
  }
  if (out.f.proto == kUdp) {
    if (l4.size() < 8) return std::nullopt;
    const std::size_t len = rd16(&l4[4]);
    if (len < 8 || len > l4.size()) return std::nullopt;
    l4 = l4.first(len);
    if ((rd16(&l4[6]) != 0 || out.f.v6) && checksum(l4, pseudo(out.f, false, kUdp, l4.size())) != 0) {
      return std::nullopt;
    }
    out.f.sport = rd16(&l4[0]);
    out.f.dport = rd16(&l4[2]);
    out.payload = l4.subspan(8);
    return out;
  }
  return std::nullopt;
}

// ---- the stack ------------------------------------------------------------------------------

struct stack::conn {
  flow f;
  bool established = false;
  std::uint32_t iss = 0, snd_una = 0, snd_nxt = 0, rcv_nxt = 0;
  std::uint32_t snd_wnd = 0;
  std::uint16_t mss = 0;
  std::deque<std::uint8_t> sendbuf;  // from snd_una: sent and not acked, then not sent
  std::deque<std::uint8_t> recvbuf;  // from the app, not yet read
  bool peer_fin = false;             // the app sent FIN (after recvbuf)
  bool fin_queued = false, fin_sent = false, fin_acked = false;
  bool reset = false, gone = false;  // RST either way; the link let go
  clock::time_point last_send{}, last_seen{};
  int rto = kRtoMs, retries = 0;
  std::size_t window_shown = kRecvWindow;  // the window last advertised
};

namespace {
// Just past each limit: tick() acts on "more than", so at the limit itself it
// would find nothing due and the timer would spin until it passed.
constexpr auto kPastDue = std::chrono::milliseconds(1);
}  // namespace

// When this connection's resend (data, FIN or SYN-ACK) falls due, for tick().
stack::clock::time_point stack::resend_due(const conn& c) {
  return c.last_send + std::chrono::milliseconds(c.rto) + kPastDue;
}

class stack::tcp_link final : public link {
 public:
  tcp_link(stack& s, std::shared_ptr<conn> c) : s_(s), c_(std::move(c)) {}
  ~tcp_link() override { s_.close(c_, false); }
  expected write_all(std::span<const std::uint8_t> b) override { return s_.write(c_, b); }
  result<std::size_t> read_some(std::span<std::uint8_t> into, int ms) override { return s_.read(c_, into, ms); }
  void shutdown() noexcept override { s_.close(c_, false); }
  transport via() const noexcept override { return transport::internet; }

 private:
  stack& s_;
  std::shared_ptr<conn> c_;
};

stack::stack(callbacks cb) : cb_(std::move(cb)) {}

stack::~stack() {
  std::vector<std::vector<std::uint8_t>> out;
  {
    std::lock_guard lock(m_);
    stopping_ = true;
    for (auto& [f, c] : conns_) {
      c->reset = true;
      out.push_back(make_tcp(c->f, {c->snd_nxt, c->rcv_nxt, static_cast<std::uint8_t>(kRst | kAck), 0, 0}, {}));
    }
    conns_.clear();
    cv_.notify_all();
  }
  emit(out);
}

void stack::emit(std::vector<std::vector<std::uint8_t>>& out) {
  if (cb_.out) {
    for (auto& p : out) cb_.out(std::move(p));
  }
  out.clear();
}

std::size_t stack::connections() const {
  std::lock_guard lock(m_);
  return conns_.size();
}

void stack::input(std::span<const std::uint8_t> packet) {
  const auto p = parse(packet);
  if (!p) return;
  if (p->f.proto == kUdp) {
    if (cb_.on_udp) cb_.on_udp(p->f, p->payload);
    return;
  }
  std::vector<std::vector<std::uint8_t>> out;
  std::unique_ptr<link> accepted;
  {
    std::lock_guard lock(m_);
    if (stopping_) return;
    on_tcp_packet(*p, out, accepted);
  }
  emit(out);
  if (accepted && cb_.on_tcp) cb_.on_tcp(std::move(accepted), p->f);
}

void stack::on_tcp_packet(const parsed& p, std::vector<std::vector<std::uint8_t>>& out,
                          std::unique_ptr<link>& accepted) {
  const auto& t = p.tcp;
  const auto now = clock::now();
  auto it = conns_.find(p.f);
  if (it == conns_.end()) {
    if (t.flags & kRst) return;
    if ((t.flags & (kSyn | kAck)) != kSyn) {
      // Not ours (a connection from before, or one reset): RST it.
      out.push_back(make_tcp(p.f, {t.ack, t.seq + static_cast<std::uint32_t>(p.payload.size()) + ((t.flags & kFin) ? 1 : 0),
                                   static_cast<std::uint8_t>(kRst | kAck), 0, 0},
                             {}));
      return;
    }
    if (conns_.size() >= kMaxConnections) {
      out.push_back(make_tcp(p.f, {0, t.seq + 1, static_cast<std::uint8_t>(kRst | kAck), 0, 0}, {}));
      return;
    }
    auto c = std::make_shared<conn>();
    c->f = p.f;
    c->iss = random32();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->rcv_nxt = t.seq + 1;
    c->snd_wnd = t.window;
    const std::uint16_t ours = static_cast<std::uint16_t>(kMtu - (p.f.v6 ? 60 : 40));
    c->mss = t.mss ? std::min(t.mss, ours) : static_cast<std::uint16_t>(p.f.v6 ? 1220 : 536);
    c->last_send = c->last_seen = now;
    conns_.emplace(p.f, c);
    if (cb_.timer) cb_.timer(resend_due(*c));  // the SYN-ACK's resend
    out.push_back(make_tcp(p.f, {c->iss, c->rcv_nxt, static_cast<std::uint8_t>(kSyn | kAck),
                                 static_cast<std::uint16_t>(kRecvWindow), ours},
                           {}));
    accepted = std::make_unique<tcp_link>(*this, c);
    return;
  }
  conn& c = *it->second;
  c.last_seen = now;
  if (t.flags & kRst) {
    c.reset = true;
    conns_.erase(it);
    cv_.notify_all();
    return;
  }
  if (t.flags & kSyn) {
    // Our SYN-ACK was lost: say it again.
    if (!c.established && t.seq + 1 == c.rcv_nxt) {
      out.push_back(make_tcp(c.f, {c.iss, c.rcv_nxt, static_cast<std::uint8_t>(kSyn | kAck),
                                   static_cast<std::uint16_t>(kRecvWindow),
                                   static_cast<std::uint16_t>(kMtu - (c.f.v6 ? 60 : 40))},
                             {}));
    }
    return;
  }
  if (t.flags & kAck) {
    if (!c.established && t.ack == c.iss + 1) {
      c.established = true;
      c.snd_una = c.iss + 1;
      c.rto = kRtoMs;  // the data's own timer starts fresh, not with the SYN-ACK's backoff
      c.retries = 0;
    }
    if (c.established && after(t.ack, c.snd_una) && !after(t.ack, c.snd_nxt)) {
      std::uint32_t acked = t.ack - c.snd_una;
      if (c.fin_sent && t.ack == c.snd_nxt) {
        c.fin_acked = true;
        --acked;  // the FIN's own sequence number
      }
      const std::size_t drop = std::min<std::size_t>(acked, c.sendbuf.size());
      c.sendbuf.erase(c.sendbuf.begin(), c.sendbuf.begin() + static_cast<std::ptrdiff_t>(drop));
      c.snd_una = t.ack;
      c.retries = 0;
      const bool backed_off = c.rto != kRtoMs;
      c.rto = kRtoMs;
      c.last_send = now;
      // A backed-off timer, reset with data still out, falls due sooner than the ticker expects.
      if (backed_off && c.snd_nxt != c.snd_una && cb_.timer) cb_.timer(resend_due(c));
      cv_.notify_all();  // room in the send buffer
    }
    c.snd_wnd = t.window;
  }
  bool ack_now = false;
  if (!p.payload.empty()) {
    if (t.seq == c.rcv_nxt && !c.peer_fin) {
      const std::size_t room = kRecvWindow > c.recvbuf.size() ? kRecvWindow - c.recvbuf.size() : 0;
      const std::size_t take = std::min(room, p.payload.size());
      c.recvbuf.insert(c.recvbuf.end(), p.payload.begin(), p.payload.begin() + static_cast<std::ptrdiff_t>(take));
      c.rcv_nxt += static_cast<std::uint32_t>(take);
      if (take) cv_.notify_all();
    }
    ack_now = true;  // in order or not: what we have so far
  }
  if ((t.flags & kFin) && !c.peer_fin && t.seq + static_cast<std::uint32_t>(p.payload.size()) == c.rcv_nxt) {
    c.peer_fin = true;
    c.rcv_nxt += 1;
    ack_now = true;
    cv_.notify_all();
  }
  if (ack_now) {
    const std::size_t room = kRecvWindow > c.recvbuf.size() ? kRecvWindow - c.recvbuf.size() : 0;
    c.window_shown = room;
    out.push_back(make_tcp(c.f, {c.snd_nxt, c.rcv_nxt, kAck, static_cast<std::uint16_t>(room), 0}, {}));
  }
  send_data_locked(c, out, false);
  if (c.peer_fin && c.fin_acked) conns_.erase(it);  // both closed (the link keeps what it read)
}

void stack::send_data_locked(conn& c, std::vector<std::vector<std::uint8_t>>& out, bool resend) {
  if (!c.established || c.reset) return;
  const bool was_quiet = c.snd_nxt == c.snd_una;  // no resend timer running for it
  if (resend) {
    c.snd_nxt = c.snd_una;
    if (c.fin_sent && !c.fin_acked) c.fin_sent = false;
  }
  const std::size_t wnd = std::min<std::size_t>(c.snd_wnd, kSendBuffer);
  bool sent = false;
  for (;;) {
    const std::size_t offset = c.snd_nxt - c.snd_una;
    if (offset >= c.sendbuf.size() || offset >= wnd) break;
    const std::size_t n = std::min({static_cast<std::size_t>(c.mss), c.sendbuf.size() - offset, wnd - offset});
    std::vector<std::uint8_t> seg(c.sendbuf.begin() + static_cast<std::ptrdiff_t>(offset),
                                  c.sendbuf.begin() + static_cast<std::ptrdiff_t>(offset + n));
    const std::size_t room = kRecvWindow > c.recvbuf.size() ? kRecvWindow - c.recvbuf.size() : 0;
    out.push_back(make_tcp(c.f, {c.snd_nxt, c.rcv_nxt, static_cast<std::uint8_t>(kAck | kPsh),
                                 static_cast<std::uint16_t>(room), 0},
                           seg));
    c.snd_nxt += static_cast<std::uint32_t>(n);
    sent = true;
  }
  if (c.fin_queued && !c.fin_sent && c.snd_nxt - c.snd_una == c.sendbuf.size()) {
    out.push_back(make_tcp(c.f, {c.snd_nxt, c.rcv_nxt, static_cast<std::uint8_t>(kFin | kAck), 0, 0}, {}));
    c.snd_nxt += 1;
    c.fin_sent = true;
    sent = true;
  }
  if (sent && (resend || c.snd_nxt != c.snd_una)) c.last_send = clock::now();
  if (sent && was_quiet && !resend && c.snd_nxt != c.snd_una && cb_.timer) cb_.timer(resend_due(c));  // its timer starts
}

result<std::size_t> stack::read(const std::shared_ptr<conn>& cp, std::span<std::uint8_t> into, int timeout_ms) {
  std::vector<std::vector<std::uint8_t>> out;
  std::unique_lock lock(m_);
  conn& c = *cp;
  const auto ready = [&] { return !c.recvbuf.empty() || c.peer_fin || c.reset || c.gone || stopping_; };
  if (timeout_ms < 0) {
    cv_.wait(lock, ready);
  } else if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
    return err(status::timeout);
  }
  if (!c.recvbuf.empty()) {
    const std::size_t n = std::min(into.size(), c.recvbuf.size());
    std::copy_n(c.recvbuf.begin(), n, into.begin());
    c.recvbuf.erase(c.recvbuf.begin(), c.recvbuf.begin() + static_cast<std::ptrdiff_t>(n));
    // The window opened by a segment or more since it was last shown: say so.
    const std::size_t room = kRecvWindow - c.recvbuf.size();
    if (room >= c.window_shown + c.mss && !c.reset && !c.peer_fin) {
      c.window_shown = room;
      out.push_back(make_tcp(c.f, {c.snd_nxt, c.rcv_nxt, kAck, static_cast<std::uint16_t>(room), 0}, {}));
    }
    lock.unlock();
    emit(out);
    return n;
  }
  if (c.peer_fin) return std::size_t{0};
  if (c.gone) return err(status::cancelled);
  return err(status::io);
}

expected stack::write(const std::shared_ptr<conn>& cp, std::span<const std::uint8_t> bytes) {
  conn& c = *cp;
  while (!bytes.empty()) {
    std::vector<std::vector<std::uint8_t>> out;
    {
      std::unique_lock lock(m_);
      cv_.wait(lock, [&] { return c.sendbuf.size() < kSendBuffer || c.reset || c.gone || stopping_; });
      if (c.reset || stopping_) return err(status::io);
      if (c.gone || c.fin_queued) return err(status::cancelled);
      const std::size_t n = std::min(bytes.size(), kSendBuffer - c.sendbuf.size());
      c.sendbuf.insert(c.sendbuf.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n));
      bytes = bytes.subspan(n);
      send_data_locked(c, out, false);
    }
    emit(out);
  }
  return {};
}

void stack::close(const std::shared_ptr<conn>& cp, bool reset) {
  std::vector<std::vector<std::uint8_t>> out;
  {
    std::lock_guard lock(m_);
    conn& c = *cp;
    if (c.gone) return;
    c.gone = true;
    cv_.notify_all();
    if (c.reset || stopping_) return;
    if (reset) {
      c.reset = true;
      out.push_back(make_tcp(c.f, {c.snd_nxt, c.rcv_nxt, static_cast<std::uint8_t>(kRst | kAck), 0, 0}, {}));
      conns_.erase(c.f);
    } else {
      // What was written still goes; then FIN.
      c.fin_queued = true;
      send_data_locked(c, out, false);
    }
  }
  emit(out);
}

void stack::udp_reply(const flow& f, std::span<const std::uint8_t> payload) {
  if (cb_.out && payload.size() + 48 <= kMtu) cb_.out(make_udp(f, payload));
}

void stack::tick() {
  std::vector<std::vector<std::uint8_t>> out;
  {
    std::lock_guard lock(m_);
    const auto now = clock::now();
    for (auto it = conns_.begin(); it != conns_.end();) {
      conn& c = *it->second;
      const bool idle = now - c.last_seen > std::chrono::milliseconds(kIdleMs);
      bool drop = idle;
      if (!drop && c.established && c.snd_nxt != c.snd_una &&
          now - c.last_send > std::chrono::milliseconds(c.rto)) {
        if (++c.retries > kMaxRetries) {
          drop = true;
        } else {
          c.rto = std::min(c.rto * 2, 5000);
          send_data_locked(c, out, true);
        }
      }
      if (!drop && !c.established && now - c.last_send > std::chrono::milliseconds(c.rto)) {
        // The SYN-ACK went unanswered: again, then give up.
        if (++c.retries > kMaxRetries) {
          drop = true;
        } else {
          c.rto = std::min(c.rto * 2, 5000);
          c.last_send = now;
          out.push_back(make_tcp(c.f, {c.iss, c.rcv_nxt, static_cast<std::uint8_t>(kSyn | kAck),
                                       static_cast<std::uint16_t>(kRecvWindow),
                                       static_cast<std::uint16_t>(kMtu - (c.f.v6 ? 60 : 40))},
                                 {}));
        }
      }
      if (drop) {
        c.reset = true;
        out.push_back(make_tcp(c.f, {c.snd_nxt, c.rcv_nxt, static_cast<std::uint8_t>(kRst | kAck), 0, 0}, {}));
        it = conns_.erase(it);
        cv_.notify_all();
        continue;
      }
      if (c.gone && c.fin_acked && c.peer_fin) {
        it = conns_.erase(it);
        continue;
      }
      ++it;
    }
  }
  emit(out);
}

std::optional<std::chrono::steady_clock::time_point> stack::next_deadline() const {
  std::lock_guard lock(m_);
  std::optional<clock::time_point> next;
  const auto take = [&](clock::time_point t) {
    if (!next || t < *next) next = t;
  };
  for (const auto& [f, cp] : conns_) {
    const conn& c = *cp;
    take(c.last_seen + std::chrono::milliseconds(kIdleMs) + kPastDue);
    if (!c.established || c.snd_nxt != c.snd_una) take(resend_due(c));
    // A connection closed both ways is erased where that happens
    // (on_tcp_packet), so nothing here waits to forget one.
  }
  return next;
}

}  // namespace mv::driftwire::vpn
