// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/udp_hub.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

#include "addons/driftwire/wire.h"

namespace mv::driftwire {

namespace {

using clock = std::chrono::steady_clock;

constexpr std::uint8_t kData = 1, kAck = 2, kFin = 3;
constexpr std::size_t kHead = 1 + 1 + 4 + 4;             // magic, type, id, seq / cum
constexpr std::size_t kSackWords = 4;                    // 256 packets past the cumulative ack
constexpr std::size_t kAckPacket = kHead + 8 * kSackWords + 4;  // + sack bitmap, window
constexpr double kMss = static_cast<double>(kStreamPayload);
constexpr double kMaxCwnd = 8.0 * 1024 * 1024;
constexpr std::size_t kMaxRx = 16u * 1024 * 1024;       // the receiver's buffer, at most
constexpr std::uint32_t kMaxAhead = 16384;               // packets held out of order, at most
constexpr int kInitialRtoMs = 1000, kMinRtoMs = 200, kMaxRtoMs = 8000;
constexpr int kExtraBuffers = 64 * 1024;  // a birthday socket carries probes only, until a stream adopts it

void put32(std::uint8_t* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
void put64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint64_t get64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
// Sequence numbers wrap; "a before b" within half the space.
bool before(std::uint32_t a, std::uint32_t b) noexcept { return static_cast<std::int32_t>(a - b) < 0; }

}  // namespace

struct udp_hub::stream_state {
  std::weak_ptr<udp_hub> hub;  // a stream that outlives its hub just fails
  std::shared_ptr<net::udp_socket> sock;  // under m: the socket it runs on
  std::uint32_t extra = 0;                // the hub's extra it owns (0: the main socket)
  net::endpoint peer;
  std::uint32_t id = 0;
  bool learn = false;  // under m: the first packet's sender becomes the peer
  std::mutex m;
  std::condition_variable cv;
  bool down = false;
  // Sending.
  struct packet {
    std::vector<std::uint8_t> wire;  // the whole datagram
    std::size_t payload = 0;
    clock::time_point sent{};
    int tries = 0;
    bool fin = false;
  };
  std::map<std::uint32_t, packet> inflight;
  std::uint32_t next_seq = 0;
  std::size_t inflight_bytes = 0;
  double cwnd = 10 * kMss, ssthresh = kMaxCwnd;
  double srtt = 0, rttvar = 0;
  int rto_ms = kInitialRtoMs;
  std::uint32_t recover = 0;  // the end of the last loss episode
  bool in_recovery = false;
  // RACK (RFC 8985, simplified): the latest send time among packets known
  // delivered. A packet sent before it, by more than the reordering window,
  // is lost: sent again at once, a lost retransmission included.
  clock::time_point rack_sent{};
  // Tail loss probe (RFC 8985 §7): nothing heard for two round trips while
  // packets are out, the newest is sent again so its ack shows what is missing.
  clock::time_point last_ack{};
  bool probed = false;
  std::size_t peer_window = kMaxRx;
  // Receiving.
  std::uint32_t rcv_next = 0;
  struct held {
    std::vector<std::uint8_t> bytes;
    bool fin = false;
  };
  std::map<std::uint32_t, held> ahead;
  std::deque<std::uint8_t> rx;
  bool fin_in = false;
  int unacked = 0;
  clock::time_point ack_due{};

  void send_wire(std::span<const std::uint8_t> w) {
    if (auto h = hub.lock()) (void)h->send_on(*sock, w, peer);
  }

  // Under m.
  void send_ack() {
    std::uint8_t a[kAckPacket];
    a[0] = kStream;
    a[1] = kAck;
    put32(a + 2, id);
    put32(a + 6, rcv_next);
    std::uint64_t sack[kSackWords] = {};
    for (const auto& [seq, unused] : ahead) {
      const std::uint32_t off = seq - rcv_next - 1;
      if (off < 64 * kSackWords) sack[off / 64] |= std::uint64_t{1} << (off % 64);
    }
    for (std::size_t w = 0; w < kSackWords; ++w) put64(a + 10 + 8 * w, sack[w]);
    const std::size_t room = rx.size() >= kMaxRx ? 0 : kMaxRx - rx.size();
    put32(a + 10 + 8 * kSackWords, static_cast<std::uint32_t>(std::min<std::size_t>(room, 0xffffffffu)));
    unacked = 0;
    ack_due = {};
    send_wire(a);
  }

  void on_data(std::uint32_t seq, std::span<const std::uint8_t> payload, bool fin) {
    if (before(seq, rcv_next)) {
      send_ack();  // a copy of one already here: its ack was lost
      return;
    }
    if (seq != rcv_next) {
      if (seq - rcv_next < kMaxAhead && !ahead.count(seq)) {
        ahead.emplace(seq, held{std::vector<std::uint8_t>(payload.begin(), payload.end()), fin});
      }
      send_ack();  // a gap: say so at once (fast retransmit)
      return;
    }
    auto take = [&](std::span<const std::uint8_t> p, bool f) {
      if (f) {
        fin_in = true;
      } else {
        rx.insert(rx.end(), p.begin(), p.end());
      }
      ++rcv_next;
    };
    take(payload, fin);
    for (auto it = ahead.find(rcv_next); it != ahead.end(); it = ahead.find(rcv_next)) {
      held h = std::move(it->second);
      ahead.erase(it);
      take(h.bytes, h.fin);
    }
    cv.notify_all();
    // Every second packet at once, else within 5 ms.
    if (++unacked >= 2 || fin) {
      send_ack();
    } else if (ack_due == clock::time_point{}) {
      ack_due = clock::now() + std::chrono::milliseconds(5);
    }
  }

  void on_ack(std::uint32_t cum, const std::uint64_t (&sack)[kSackWords], std::uint32_t window) {
    peer_window = window;
    const auto now = clock::now();
    last_ack = now;
    probed = false;
    std::size_t newly = 0;
    bool sampled = false;
    auto acked = [&](std::map<std::uint32_t, packet>::iterator it) {
      if (!sampled && it->second.tries == 1) {
        // Karn: an RTT only from a packet sent once.
        const double r = std::chrono::duration<double, std::milli>(now - it->second.sent).count();
        if (srtt == 0) {
          srtt = r;
          rttvar = r / 2;
        } else {
          rttvar = 0.75 * rttvar + 0.25 * std::abs(srtt - r);
          srtt = 0.875 * srtt + 0.125 * r;
        }
        rto_ms = std::clamp(static_cast<int>(srtt + 4 * rttvar), kMinRtoMs, kMaxRtoMs);
        sampled = true;
      }
      newly += it->second.payload;
      inflight_bytes -= it->second.payload;
      rack_sent = std::max(rack_sent, it->second.sent);
      return inflight.erase(it);
    };
    for (auto it = inflight.begin(); it != inflight.end() && before(it->first, cum);) it = acked(it);
    for (std::size_t b = 0; b < 64 * kSackWords; ++b) {
      if (!(sack[b / 64] >> (b % 64) & 1)) continue;
      const std::uint32_t seq = cum + 1 + static_cast<std::uint32_t>(b);
      if (auto it = inflight.find(seq); it != inflight.end()) acked(it);
    }
    if (in_recovery && !before(cum, recover)) in_recovery = false;
    // Growth: slow start, then about a packet per round trip.
    if (newly > 0 && !in_recovery) cwnd += cwnd < ssthresh ? static_cast<double>(newly) : kMss * newly / cwnd;
    cwnd = std::min(cwnd, kMaxCwnd);
    // RACK: sent before something since delivered, by more than a quarter of
    // a round trip (reordering): lost, sent again now.
    const auto reorder = std::chrono::microseconds(static_cast<long long>(std::max(250.0, srtt * 250.0)));
    for (auto& [seq, p] : inflight) {
      if (rack_sent == clock::time_point{} || p.sent + reorder >= rack_sent) continue;
      if (!in_recovery) {
        ssthresh = std::max(cwnd / 2, 2 * kMss);
        cwnd = ssthresh;
        in_recovery = true;
        recover = next_seq;
      }
      p.sent = now;
      ++p.tries;
      send_wire(p.wire);
    }
    cv.notify_all();
  }

  // The timer: a packet unacknowledged past the RTO is sent again; a
  // delayed ack goes out.
  void tick(clock::time_point now) {
    if (ack_due != clock::time_point{} && now >= ack_due) send_ack();
    if (inflight.empty()) return;
    const auto quiet = std::chrono::microseconds(static_cast<long long>(std::max(10000.0, srtt * 2000.0)));
    if (!probed && last_ack != clock::time_point{} && now - last_ack > quiet) {
      probed = true;
      auto& newest = inflight.rbegin()->second;
      newest.sent = now;
      ++newest.tries;
      send_wire(newest.wire);
      return;
    }
    auto& [seq, p] = *inflight.begin();
    if (now - p.sent < std::chrono::milliseconds(rto_ms)) return;
    ssthresh = std::max(static_cast<double>(inflight_bytes) / 2, 2 * kMss);
    cwnd = kMss;
    rto_ms = std::min(rto_ms * 2, kMaxRtoMs);
    in_recovery = true;
    recover = next_seq;
    p.sent = now;
    ++p.tries;
    send_wire(p.wire);
  }
};

namespace {

class udp_stream final : public link {
 public:
  explicit udp_stream(std::shared_ptr<udp_hub::stream_state> s) : s_(std::move(s)) {}
  ~udp_stream() override { shutdown(); }

  expected write_all(std::span<const std::uint8_t> bytes) override {
    std::unique_lock lock(s_->m);
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t n = std::min(kStreamPayload, bytes.size() - at);
      // Room in the congestion window and the peer's buffer; a peer silent
      // this long is gone (the session above notices sooner).
      const auto until = clock::now() + std::chrono::milliseconds(kSilenceMs);
      while (!s_->down && (static_cast<double>(s_->inflight_bytes + n) > s_->cwnd ||
                           s_->inflight_bytes + n > s_->peer_window)) {
        if (s_->cv.wait_until(lock, until) == std::cv_status::timeout) return err(status::timeout);
        // A zero window must not stall forever: one packet may probe it.
        if (s_->inflight.empty()) break;
      }
      if (s_->down) return err(status::cancelled);
      udp_hub::stream_state::packet p;
      const std::uint32_t seq = s_->next_seq++;
      p.wire.resize(kHead + n);
      p.wire[0] = kStream;
      p.wire[1] = kData;
      put32(p.wire.data() + 2, s_->id);
      put32(p.wire.data() + 6, seq);
      std::memcpy(p.wire.data() + kHead, bytes.data() + at, n);
      p.payload = n;
      p.sent = clock::now();
      p.tries = 1;
      if (s_->inflight.empty()) s_->last_ack = p.sent;  // the probe's clock starts with the flight
      s_->inflight_bytes += n;
      s_->send_wire(p.wire);
      s_->inflight.emplace(seq, std::move(p));
      at += n;
    }
    return {};
  }

  result<std::size_t> read_some(std::span<std::uint8_t> into, int timeout_ms) override {
    std::unique_lock lock(s_->m);
    const auto ready = [&] { return s_->down || !s_->rx.empty() || s_->fin_in; };
    if (timeout_ms < 0) {
      s_->cv.wait(lock, ready);
    } else if (!s_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
      return err(status::timeout);
    }
    if (!s_->rx.empty()) {
      const bool was_full = s_->rx.size() >= kMaxRx / 2;
      const std::size_t n = std::min(into.size(), s_->rx.size());
      std::copy_n(s_->rx.begin(), n, into.begin());
      s_->rx.erase(s_->rx.begin(), s_->rx.begin() + static_cast<std::ptrdiff_t>(n));
      if (was_full && s_->rx.size() < kMaxRx / 2) s_->send_ack();  // the window opened
      return n;
    }
    if (s_->down) return err(status::cancelled);
    return std::size_t{0};  // the peer finished
  }

  void shutdown() noexcept override {
    std::lock_guard lock(s_->m);
    if (s_->down) return;
    s_->down = true;
    std::uint8_t fin[kHead];
    fin[0] = kStream;
    fin[1] = kFin;
    put32(fin + 2, s_->id);
    put32(fin + 6, s_->next_seq);
    try {
      s_->send_wire(fin);  // best effort: the peer's session sees a closed link sooner
    } catch (...) {
    }
    s_->cv.notify_all();
    release_extra();
  }

  transport via() const noexcept override { return transport::internet; }
  bool punched() const noexcept override { return true; }

 private:
  // Under s_->m. The birthday socket this stream ran on goes with it: the hub
  // is back to its one socket (and its reader to its idle wait).
  void release_extra() noexcept {
    const std::uint32_t id = std::exchange(s_->extra, 0);
    if (id == 0) return;
    try {
      if (auto h = s_->hub.lock()) h->close_extras(std::span<const std::uint32_t>(&id, 1));
    } catch (...) {
    }
  }

  std::shared_ptr<udp_hub::stream_state> s_;
};

}  // namespace

result<std::shared_ptr<udp_hub>> udp_hub::open(std::uint16_t port, bool loopback_only, socket_wrap wrap) {
  auto sock = net::open_udp_dual(port, loopback_only);
  if (!sock) return err(sock.error());
  std::shared_ptr<udp_hub> h(new udp_hub());
  h->port_ = (*sock)->port();
  h->sock_ = wrap ? wrap(std::move(*sock)) : std::move(*sock);
  h->loopback_ = loopback_only;
  h->wrap_ = std::move(wrap);
  h->reader_ = std::thread([raw = h.get()] { raw->read_loop(); });
  h->timer_ = std::thread([raw = h.get()] { raw->timer_loop(); });
  return h;
}

udp_hub::~udp_hub() { stop(); }

void udp_hub::stop() noexcept {
  if (stop_.exchange(true)) return;
  if (sock_) sock_->shutdown();
  if (reader_.joinable()) reader_.join();
  if (timer_.joinable()) timer_.join();
  std::map<std::uint32_t, std::weak_ptr<stream_state>> all;
  std::map<std::uint32_t, std::shared_ptr<net::udp_socket>> extras;
  {
    std::lock_guard lock(m_);
    all.swap(streams_);
    extras.swap(extras_);
    has_extras_.store(false);
  }
  for (auto& [id, w] : all) {
    if (auto s = w.lock()) {
      std::lock_guard l(s->m);
      s->down = true;
      s->cv.notify_all();
    }
  }
}

expected udp_hub::send(std::span<const std::uint8_t> datagram, const net::endpoint& to) {
  return send_on(*sock_, datagram, to);
}

expected udp_hub::send(std::span<const std::uint8_t> datagram, const net::endpoint& to, std::uint32_t sock) {
  if (sock == 0) return send_on(*sock_, datagram, to);
  std::shared_ptr<net::udp_socket> s;
  {
    std::lock_guard lock(m_);
    if (auto it = extras_.find(sock); it != extras_.end()) s = it->second;
  }
  if (!s) return err(status::not_found);
  return send_on(*s, datagram, to);
}

expected udp_hub::send_on(net::udp_socket& sock, std::span<const std::uint8_t> datagram, const net::endpoint& to) {
  if (stop_.load()) return err(status::cancelled);
  if (const int loss = loss_.load(); loss > 0 && !datagram.empty() && datagram[0] == kStream &&
                                     static_cast<int>(randombytes_uniform(1000)) < loss) {
    return {};  // the tests' lossy network
  }
  return sock.send_to(datagram, to);
}

std::vector<std::uint32_t> udp_hub::open_extras(int n) {
  net::reserve_sockets(n);
  std::vector<std::uint32_t> ids;
  for (int i = 0; i < n && !stop_.load(); ++i) {
    auto s = net::open_udp_dual(0, loopback_, kExtraBuffers);
    if (!s) break;  // out of sockets: the punch runs with fewer
    std::shared_ptr<net::udp_socket> sock = wrap_ ? wrap_(std::move(*s)) : std::move(*s);
    std::lock_guard lock(m_);
    const std::uint32_t id = next_extra_++;
    extras_.emplace(id, std::move(sock));
    ids.push_back(id);
  }
  std::lock_guard lock(m_);
  has_extras_.store(!extras_.empty());
  return ids;
}

void udp_hub::close_extras(std::span<const std::uint32_t> ids) {
  std::vector<std::shared_ptr<net::udp_socket>> gone;  // closed outside the lock
  std::lock_guard lock(m_);
  for (const auto id : ids) {
    if (auto it = extras_.find(id); it != extras_.end()) {
      gone.push_back(std::move(it->second));
      extras_.erase(it);
    }
  }
  has_extras_.store(!extras_.empty());
}

std::size_t udp_hub::extra_count() {
  std::lock_guard lock(m_);
  return extras_.size();
}

void udp_hub::on_dht(datagram_fn fn) {
  std::lock_guard calling(calling_m_);  // no call of the old one is running
  std::lock_guard lock(m_);
  dht_ = std::move(fn);
}

void udp_hub::on_probe(probe_fn fn) {
  std::lock_guard calling(calling_m_);
  std::lock_guard lock(m_);
  probe_ = std::move(fn);
}

std::unique_ptr<link> udp_hub::stream(const net::endpoint& peer, std::uint32_t id, bool learn, std::uint32_t sock) {
  auto s = std::make_shared<stream_state>();
  s->hub = weak_from_this();
  s->peer = peer;
  s->id = id;
  s->learn = learn;
  {
    std::lock_guard lock(m_);
    if (sock == 0) {
      s->sock = sock_;
    } else if (auto it = extras_.find(sock); it != extras_.end()) {
      s->sock = it->second;
      s->sock->set_buffers(net::kUdpBuffers);  // a transfer now runs on it
      s->extra = sock;
    }
    if (s->sock) streams_[id] = s;
  }
  if (!s->sock) {
    s->sock = sock_;  // that socket is closed: a stream that has failed
    s->down = true;
  }
  return std::make_unique<udp_stream>(std::move(s));
}

void udp_hub::read_loop() {
  std::vector<std::uint8_t> buf(65536);
  std::vector<std::pair<std::uint32_t, std::shared_ptr<net::udp_socket>>> socks;
  std::vector<net::udp_socket*> raw;
  std::unique_ptr<bool[]> ready;
  std::size_t ready_cap = 0;
  while (!stop_.load()) {
    net::endpoint from;
    if (!has_extras_.load()) {
      // The usual case: the one socket.
      auto n = sock_->receive_from(buf, 200, from);
      if (n && *n > 0) take({buf.data(), *n}, from, 0, sock_);
      continue;
    }
    // A birthday punch: every socket, polled together. Short waits, so an
    // extra opened meanwhile is read soon.
    socks.clear();
    socks.emplace_back(0, sock_);
    {
      std::lock_guard lock(m_);
      for (const auto& [id, s] : extras_) socks.emplace_back(id, s);
    }
    raw.clear();
    for (const auto& [id, s] : socks) raw.push_back(s.get());
    if (ready_cap < raw.size()) {
      ready_cap = raw.size();
      ready.reset(new bool[ready_cap]);  // a vector<bool> has no span
    }
    auto any = net::wait_readable(raw, std::span<bool>(ready.get(), raw.size()), 20);
    if (!any) continue;
    for (std::size_t i = 0; i < raw.size() && !stop_.load(); ++i) {
      if (!ready[i]) continue;
      auto n = raw[i]->receive_from(buf, 0, from);
      if (n && *n > 0) take({buf.data(), *n}, from, socks[i].first, socks[i].second);
    }
  }
}

void udp_hub::take(std::span<const std::uint8_t> d, const net::endpoint& from, std::uint32_t sock_id,
                   const std::shared_ptr<net::udp_socket>& sock) {
  if (d[0] == 'd') {
    // KRPC on the main socket only: the DHT's queries never go out on an extra.
    if (sock_id != 0) return;
    std::lock_guard calling(calling_m_);
    datagram_fn fn;
    {
      std::lock_guard lock(m_);
      fn = dht_;
    }
    if (fn) fn(d, from);
    return;
  }
  if (d[0] == kProbe) {
    std::lock_guard calling(calling_m_);
    probe_fn fn;
    {
      std::lock_guard lock(m_);
      fn = probe_;
    }
    if (fn) fn(d, from, sock_id);
    return;
  }
  if (d[0] != kStream || d.size() < kHead) return;
  const std::uint32_t id = get32(d.data() + 2);
  std::shared_ptr<stream_state> s;
  {
    std::lock_guard lock(m_);
    if (auto it = streams_.find(id); it != streams_.end()) s = it->second.lock();
  }
  if (!s) return;
  std::lock_guard l(s->m);
  if (s->learn) {
    s->peer = from;
    s->sock = sock;
    s->learn = false;
  }
  if (s->down || s->sock != sock || s->peer.address != from.address || s->peer.port != from.port) return;
  const std::uint8_t type = d[1];
  if (type == kData) {
    s->on_data(get32(d.data() + 6), d.subspan(kHead), false);
  } else if (type == kFin) {
    s->on_data(get32(d.data() + 6), {}, true);
  } else if (type == kAck && d.size() >= kAckPacket) {
    std::uint64_t sack[kSackWords];
    for (std::size_t w = 0; w < kSackWords; ++w) sack[w] = get64(d.data() + 10 + 8 * w);
    s->on_ack(get32(d.data() + 6), sack, get32(d.data() + 10 + 8 * kSackWords));
  }
}

void udp_hub::timer_loop() {
  while (!stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::vector<std::shared_ptr<stream_state>> live;
    {
      std::lock_guard lock(m_);
      for (auto it = streams_.begin(); it != streams_.end();) {
        if (auto s = it->second.lock()) {
          live.push_back(std::move(s));
          ++it;
        } else {
          it = streams_.erase(it);
        }
      }
    }
    const auto now = clock::now();
    for (auto& s : live) {
      std::lock_guard l(s->m);
      if (!s->down) s->tick(now);
    }
  }
}

}  // namespace mv::driftwire
