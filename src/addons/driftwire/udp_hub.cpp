// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/driftwire/udp_hub.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <set>
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
// #529: past the bitmap, an ack may add a count and up to this many runs of
// packets held ([start, end), 4 + 4 bytes each). A path's window is often
// more than 256 packets, and a retransmission lost again past the bitmap was
// seen only by a timeout, which sent the whole window again. A peer that
// predates them reads the fixed part and ignores the rest.
constexpr std::size_t kMaxRuns = 16;
constexpr double kMss = static_cast<double>(kStreamPayload);
constexpr double kMaxCwnd = 8.0 * 1024 * 1024;
constexpr std::size_t kMaxRx = kStreamMaxRx;           // the receiver's buffer, at most
constexpr std::uint32_t kMaxAhead = kMaxRx / kStreamPayload + 1;  // packets held out of order, at most
constexpr int kInitialRtoMs = 1000, kMinRtoMs = 200, kMaxRtoMs = 8000;
// #529: the sender's model of the path (BBR v1, draft-cardwell-iccrg-bbr-
// congestion-control, simplified): pacing and cwnd follow the bandwidth it
// measures and the smallest RTT it has seen, not loss, so a random loss on
// a long path costs one packet sent again rather than half the window.
constexpr double kStartupGain = 2.885;  // 2 / ln 2: the rate doubles each round trip
constexpr double kCwndGain = 2.0;
constexpr double kProbeGains[] = {1.25, 0.75, 1, 1, 1, 1, 1, 1};
constexpr std::size_t kBwRounds = 10;  // the bandwidth filter, in round trips
constexpr auto kMinRttWindow = std::chrono::seconds(10);
constexpr auto kProbeRttTime = std::chrono::milliseconds(200);
constexpr double kMinCwnd = 4 * kMss;
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
    bool lost = false;  // marked lost, waiting to go again: not in the pipe
    // Delivery-rate sampling (draft-cheng-iccrg-delivery-rate-estimation):
    // the stream's counts when this copy left.
    std::uint64_t delivered = 0;
    clock::time_point delivered_at{}, first_sent{};
    bool app_limited = false;
  };
  std::map<std::uint32_t, packet> inflight;  // sent and not acknowledged, the lost ones too
  std::set<std::uint32_t> lost;              // of those, the ones to send again, oldest first
  std::uint32_t next_seq = 0;
  std::size_t inflight_bytes = 0;            // the pipe: neither acknowledged nor marked lost
  double cwnd = 10 * kMss;
  double srtt = 0, rttvar = 0;
  int rto_ms = kInitialRtoMs;
  // RACK (RFC 8985, simplified): the latest send time among packets known
  // delivered. A packet sent before it, by more than the reordering window,
  // is lost: marked, and sent again as the pace allows (#529: not all at
  // once, which a router's queue dropped again), a lost retransmission
  // included.
  clock::time_point rack_sent{};
  // Tail loss probe (RFC 8985 §7): nothing heard for two round trips while
  // packets are out, the newest is sent again so its ack shows what is missing.
  clock::time_point last_ack{};
  bool probed = false;
  std::size_t peer_window = kMaxRx;
  udp_hub::stream_stats stats;  // under m (#529)
  // The model (#529).
  enum class phase : std::uint8_t { startup, drain, probe_bw, probe_rtt };
  phase mode = phase::startup;
  std::uint64_t delivered = 0;               // payload bytes acknowledged, ever
  clock::time_point delivered_at{}, first_sent{};
  std::uint64_t app_limited_until = 0;       // 0: not; else samples until `delivered` passes it are
  std::uint64_t rounds = 0, round_end = 0;   // round trips, counted by what was delivered
  struct bw_sample {
    std::uint64_t round = 0;
    double bytes_per_s = 0;
  };
  std::array<bw_sample, kBwRounds> bw{};     // the largest sample of each recent round
  std::uint64_t bw_round = 0;                // the round of the latest sample the filter took
  std::uint64_t round_lost = 0, round_acked = 0;  // bytes taken for lost and delivered this round
  double round_max_inflight = 0;
  // BBR v2's inflight_hi, simplified: a round that loses more than 2 % caps
  // the pipe near what it held then; each clean round raises the cap 2 %.
  // Two bandwidth-delay products in flight overflow a router queue that is
  // shallower than one, round after round.
  double inflight_hi = kMaxCwnd;
  double min_rtt = 0;                        // ms, over kMinRttWindow
  clock::time_point min_rtt_at{};
  double full_bw = 0;                        // startup: the pipe is full when the bandwidth
  int full_bw_rounds = 0;                    // stops growing by a quarter for three rounds
  bool filled = false;
  double pacing_gain = kStartupGain, cwnd_gain = kStartupGain;
  std::size_t cycle = 0;                     // probe_bw: which of kProbeGains
  clock::time_point cycle_at{};
  clock::time_point probe_rtt_until{};       // probe_rtt: held at kMinCwnd until then
  std::uint64_t probe_rtt_round = 0;
  double prior_cwnd = 0;
  // Pacing: packets leave at pace_rate(), a few at a time. The burst a wake
  // may send grows with how late this OS wakes a waiter.
  double tokens = 0;
  clock::time_point tokens_at{};
  double oversleep_ms = 0;
  // Receiving.
  std::uint32_t rcv_next = 0;
  struct held {
    std::vector<std::uint8_t> bytes;
    bool fin = false;
  };
  std::map<std::uint32_t, held> ahead;
  std::size_t ahead_bytes = 0;  // the payloads in `ahead`
  std::map<std::uint32_t, std::uint32_t> runs;  // `ahead` as runs of numbers, [start, end)
  std::deque<std::uint8_t> rx;
  bool fin_in = false;
  int unacked = 0;
  clock::time_point ack_due{};

  void send_wire(std::span<const std::uint8_t> w) {
    if (auto h = hub.lock()) (void)h->send_on(*sock, w, peer);
  }

  // ---- the model; all under m -------------------------------------------------------------

  double btl_bw() const {
    // The window is counted back from the latest sample, not from now: rounds
    // with no sample worth taking (a recovery, a writer that paused) leave the
    // estimate where it was.
    double best = 0;
    for (const auto& b : bw)
      if (b.round + kBwRounds > bw_round) best = std::max(best, b.bytes_per_s);
    return best;
  }
  double bdp() const { return btl_bw() * min_rtt / 1000.0; }
  double pace_rate() const {
    if (const double b = btl_bw(); b > 0) return pacing_gain * b;
    if (srtt > 0) return pacing_gain * cwnd / (srtt / 1000.0);
    return 0;  // nothing measured yet: the first window goes as it is
  }
  // Tokens for what pace_rate() allows by `now`, at most a burst.
  void refill(clock::time_point now) {
    const double rate = pace_rate();
    if (rate <= 0) {
      tokens = cwnd;
      tokens_at = now;
      return;
    }
    const double burst = std::max(2 * kMss, rate * std::max(1.0, 0.5 + oversleep_ms) / 1000.0);
    if (tokens_at != clock::time_point{}) tokens += rate * std::chrono::duration<double>(now - tokens_at).count();
    tokens = std::min(tokens, burst);
    tokens_at = now;
  }
  bool paced_out(clock::time_point now, std::size_t bytes) {
    refill(now);
    return pace_rate() > 0 && tokens < static_cast<double>(bytes);
  }
  // A copy of `p` leaves now: what its ack will measure the rate against.
  void stamp(packet& p, clock::time_point now) {
    if (inflight_bytes == 0) first_sent = delivered_at = now;
    p.delivered = delivered;
    p.delivered_at = delivered_at;
    p.first_sent = first_sent;
    // Limited by this side, not the path: the writer, packets still to repair,
    // or a window regrowing after a timeout. Such a sample may raise the
    // estimate, never lower it.
    p.app_limited = app_limited_until != 0 || !lost.empty() || cwnd < bdp() ||
                    static_cast<double>(inflight_bytes) < 0.8 * bdp();  // the pipe was refilling
    p.sent = now;
    tokens -= static_cast<double>(p.wire.size());
    round_max_inflight = std::max(round_max_inflight, static_cast<double>(inflight_bytes + p.payload));
  }
  void enter_probe_bw(clock::time_point now) {
    mode = phase::probe_bw;
    cwnd_gain = kCwndGain;
    cycle = 2 + randombytes_uniform(6);  // anywhere but the drain right after the probe
    cycle_at = now;
    pacing_gain = kProbeGains[cycle];
  }
  // Packets marked lost go again, oldest first, as the window and the pace
  // allow.
  void pump(clock::time_point now) {
    while (!lost.empty() && !down) {
      auto it = inflight.find(*lost.begin());
      if (it == inflight.end()) {
        lost.erase(lost.begin());
        continue;
      }
      packet& p = it->second;
      if (static_cast<double>(inflight_bytes + p.payload) > cwnd || paced_out(now, p.wire.size())) return;
      lost.erase(lost.begin());
      p.lost = false;
      stamp(p, now);
      inflight_bytes += p.payload;
      ++p.tries;
      ++stats.retransmits;
      send_wire(p.wire);
    }
  }

  // Under m.
  void send_ack() {
    std::uint8_t a[kAckPacket + 1 + 8 * kMaxRuns];
    a[0] = kStream;
    a[1] = kAck;
    put32(a + 2, id);
    put32(a + 6, rcv_next);
    // Only the packets the bitmap covers are visited (#499: not every packet
    // held, on every gap). The map is ordered by the raw number, so a window
    // that wraps past 2^32 is read in two pieces.
    std::uint64_t sack[kSackWords] = {};
    constexpr std::uint32_t kSpan = 64 * kSackWords;
    const std::uint32_t first = rcv_next + 1;
    auto mark = [&](std::map<std::uint32_t, held>::const_iterator it) {
      for (; it != ahead.end(); ++it) {
        const std::uint32_t off = it->first - first;
        if (off >= kSpan) return;
        sack[off / 64] |= std::uint64_t{1} << (off % 64);
      }
    };
    mark(ahead.lower_bound(first));
    if (first + kSpan < first) mark(ahead.begin());  // wrapped
    for (std::size_t w = 0; w < kSackWords; ++w) put64(a + 10 + 8 * w, sack[w]);
    // What may still arrive: the buffer less what waits to be read and what
    // is held out of order (a sender forgets a packet once it is SACKed).
    const std::size_t used = rx.size() + ahead_bytes;
    const std::size_t room = used >= kMaxRx ? 0 : kMaxRx - used;
    put32(a + 10 + 8 * kSackWords, static_cast<std::uint32_t>(std::min<std::size_t>(room, 0xffffffffu)));
    // The runs that reach past the bitmap, the first kMaxRuns of them.
    std::size_t len = kAckPacket, count = 0;
    if (!runs.empty()) {
      const std::uint32_t past = first + kSpan;
      auto it = runs.upper_bound(past);
      if (it != runs.begin() && before(past, std::prev(it)->second)) --it;
      for (; it != runs.end() && count < kMaxRuns; ++it, ++count) {
        put32(a + kAckPacket + 1 + 8 * count, it->first);
        put32(a + kAckPacket + 5 + 8 * count, it->second);
      }
      if (count > 0) {
        a[kAckPacket] = static_cast<std::uint8_t>(count);
        len = kAckPacket + 1 + 8 * count;
      }
    }
    unacked = 0;
    ack_due = {};
    send_wire({a, len});
  }

  void on_data(std::uint32_t seq, std::span<const std::uint8_t> payload, bool fin) {
    if (before(seq, rcv_next)) {
      send_ack();  // a copy of one already here: its ack was lost
      return;
    }
    // Past the window this side advertised: dropped (an honest sender never
    // sends it; it is sent again once the window opens). The next packet in
    // order may use one packet more, so held packets can always drain.
    const std::size_t limit = kMaxRx + (seq == rcv_next ? kStreamPayload : 0);
    if (rx.size() + ahead_bytes + payload.size() > limit) {
      send_ack();
      return;
    }
    if (seq != rcv_next) {
      if (seq - rcv_next < kMaxAhead && !ahead.count(seq)) {
        ahead.emplace(seq, held{std::vector<std::uint8_t>(payload.begin(), payload.end()), fin});
        ahead_bytes += payload.size();
        // Join the run that ends at seq and the one that starts after it.
        auto next = runs.upper_bound(seq);
        std::uint32_t start = seq, end = seq + 1;
        if (next != runs.end() && next->first == end) {
          end = next->second;
          next = runs.erase(next);
        }
        if (next != runs.begin() && std::prev(next)->second == seq) {
          std::prev(next)->second = end;
        } else {
          runs.emplace(start, end);
        }
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
    // What follows in order is one run, drained whole.
    if (auto r = runs.find(rcv_next); r != runs.end()) runs.erase(r);
    for (auto it = ahead.find(rcv_next); it != ahead.end(); it = ahead.find(rcv_next)) {
      held h = std::move(it->second);
      ahead.erase(it);
      ahead_bytes -= h.bytes.size();
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

  void on_ack(std::uint32_t cum, const std::uint64_t (&sack)[kSackWords], std::uint32_t window,
              std::span<const std::uint8_t> ack_runs) {
    // An ack of packets never sent is not this stream's peer speaking (#499:
    // it would drop every packet in flight for good).
    if (before(next_seq, cum)) return;
    peer_window = window;
    const auto now = clock::now();
    last_ack = now;
    probed = false;
    // Expired before this ack's sample refreshes it: then probe_rtt (BBR).
    const bool rtt_expired = min_rtt_at != clock::time_point{} && now - min_rtt_at > kMinRttWindow;
    std::size_t newly = 0;
    bool sampled = false;
    // The rate sample comes from the most recently sent packet this ack covers.
    bool have = false, sample_limited = false;
    std::uint64_t prior_delivered = 0;
    double send_elapsed = 0, ack_elapsed = 0;
    auto acked = [&](std::map<std::uint32_t, packet>::iterator it) {
      packet& p = it->second;
      if (!sampled && p.tries == 1 && !p.lost) {
        // Karn: an RTT only from a packet sent once.
        const double r = std::chrono::duration<double, std::milli>(now - p.sent).count();
        if (srtt == 0) {
          srtt = r;
          rttvar = r / 2;
        } else {
          rttvar = 0.75 * rttvar + 0.25 * std::abs(srtt - r);
          srtt = 0.875 * srtt + 0.125 * r;
        }
        // The floor goes under the variance, not the sum (as Linux's): a
        // steady 160 ms path has almost none, and srtt + a few ms timed out
        // on any queue at all, sending a whole window again (#529).
        rto_ms = std::min(static_cast<int>(srtt + std::max(4 * rttvar, static_cast<double>(kMinRtoMs))), kMaxRtoMs);
        if (min_rtt == 0 || r <= min_rtt || rtt_expired) {
          min_rtt = r;
          min_rtt_at = now;
        }
        sampled = true;
      }
      if (p.lost) {
        lost.erase(it->first);  // marked lost too soon: it arrived after all
      } else {
        inflight_bytes -= p.payload;
      }
      newly += p.payload;
      delivered += p.payload;
      delivered_at = now;
      if (!have || p.delivered > prior_delivered) {
        have = true;
        prior_delivered = p.delivered;
        send_elapsed = std::chrono::duration<double>(p.sent - p.first_sent).count();
        ack_elapsed = std::chrono::duration<double>(now - p.delivered_at).count();
        sample_limited = p.app_limited;
        first_sent = p.sent;
      }
      rack_sent = std::max(rack_sent, p.sent);
      return inflight.erase(it);
    };
    for (auto it = inflight.begin(); it != inflight.end() && before(it->first, cum);) it = acked(it);
    for (std::size_t b = 0; b < 64 * kSackWords; ++b) {
      if (!(sack[b / 64] >> (b % 64) & 1)) continue;
      const std::uint32_t seq = cum + 1 + static_cast<std::uint32_t>(b);
      if (auto it = inflight.find(seq); it != inflight.end()) acked(it);
    }
    // The runs past the bitmap: only packets still in flight are visited, and
    // each visit forgets one, so no ack costs more than what was sent (#499).
    if (!ack_runs.empty()) {
      const std::size_t count = std::min<std::size_t>(ack_runs[0], kMaxRuns);
      for (std::size_t r = 0; r < count && ack_runs.size() >= 1 + 8 * (r + 1); ++r) {
        const std::uint32_t a = get32(ack_runs.data() + 1 + 8 * r), b = get32(ack_runs.data() + 5 + 8 * r);
        if (!before(a, b) || before(next_seq, b) || !before(cum, a)) continue;  // not a run of packets sent
        for (auto it = inflight.lower_bound(a); it != inflight.end() && before(it->first, b);) it = acked(it);
      }
    }
    if (app_limited_until != 0 && delivered > app_limited_until) app_limited_until = 0;
    // A round trip ends when a packet sent after the last one ended is acknowledged.
    bool round_start = false;
    double round_loss = 0;  // the share of the round that ended taken for lost
    round_acked += newly;
    if (have && prior_delivered >= round_end) {
      round_end = delivered;
      ++rounds;
      round_start = true;
      if (round_acked + round_lost > 0)
        round_loss = static_cast<double>(round_lost) / static_cast<double>(round_acked + round_lost);
      if (filled && round_loss > 0.02) {
        inflight_hi = std::max({kMinCwnd, bdp(), 0.85 * round_max_inflight});
      } else if (filled) {
        inflight_hi = std::min(kMaxCwnd, inflight_hi + std::max(kMss, 0.02 * inflight_hi));
      }
      round_acked = round_lost = 0;
      round_max_inflight = 0;
    }
    // The delivery rate over the longer of the send and ack spans; one
    // shorter than a round trip says more about bunched acks than the path.
    const double interval = std::max(send_elapsed, ack_elapsed);
    if (have && interval > 0 && interval * 1000.0 >= min_rtt) {
      const double rate = static_cast<double>(delivered - prior_delivered) / interval;
      if (!sample_limited || rate >= btl_bw()) {
        auto& slot = bw[rounds % kBwRounds];
        if (slot.round != rounds) slot = {rounds, 0};
        slot.bytes_per_s = std::max(slot.bytes_per_s, rate);
        bw_round = rounds;
      }
    }
    // RACK: sent before something since delivered, by more than a quarter of
    // a round trip (reordering): lost. In sequence order the first copy that
    // is not yet late ends the walk: every packet after it left later.
    const auto reorder = std::chrono::microseconds(static_cast<long long>(std::max(250.0, srtt * 250.0)));
    bool loss = false;
    for (auto& [seq, p] : inflight) {
      if (rack_sent == clock::time_point{} || p.sent + reorder >= rack_sent) {
        if (p.tries == 1 && !p.lost) break;
        continue;
      }
      if (p.lost) continue;
      p.lost = true;
      inflight_bytes -= p.payload;
      lost.insert(seq);
      round_lost += p.payload;
      ++stats.lost;
      loss = true;
    }
    // Startup ends when three rounds in a row grow the bandwidth by less than
    // a quarter, or (BBR v2) a round loses more than 2 %: a router's queue
    // shallower than the path's bandwidth-delay product overflows long before
    // the bandwidth stops growing, and startup's 2.9 x window then dropped
    // half a window at once.
    if (round_start && !filled && btl_bw() > 0 && round_loss > 0.02) {
      filled = true;
    } else if (round_start && !filled && !sample_limited) {
      if (const double b = btl_bw(); b >= full_bw * 1.25) {
        full_bw = b;
        full_bw_rounds = 0;
      } else if (++full_bw_rounds >= 3) {
        filled = true;
      }
    }
    if (mode == phase::startup && filled) {
      mode = phase::drain;  // the queue startup built drains at the inverse gain
      pacing_gain = 1 / kStartupGain;
      cwnd_gain = kStartupGain;
    }
    if (mode == phase::drain && static_cast<double>(inflight_bytes) <= bdp()) enter_probe_bw(now);
    if (mode == phase::probe_bw && min_rtt > 0) {
      // Each phase about a round trip: 1.25 probes for more until the pipe
      // shows it (or a loss says no), 0.75 drains what that queued.
      const auto rtt = std::chrono::microseconds(static_cast<long long>(min_rtt * 1000.0));
      const double g = kProbeGains[cycle];
      const bool elapsed = now - cycle_at > rtt;
      bool next = elapsed;
      if (g > 1) {
        next = elapsed && (loss || static_cast<double>(inflight_bytes) >= g * bdp() || now - cycle_at > 4 * rtt);
      } else if (g < 1) {
        next = elapsed || static_cast<double>(inflight_bytes) <= bdp();
      }
      if (next) {
        cycle = (cycle + 1) % std::size(kProbeGains);
        cycle_at = now;
        pacing_gain = kProbeGains[cycle];
      }
    }
    // probe_rtt: the smallest RTT unrefreshed for 10 s, the pipe is emptied
    // to four packets for 200 ms and a round trip, so it is measured again
    // without the queue this stream itself keeps.
    if (rtt_expired && mode != phase::probe_rtt) {
      mode = phase::probe_rtt;
      pacing_gain = 1;
      prior_cwnd = std::max(prior_cwnd, cwnd);
      probe_rtt_until = {};
    }
    if (mode == phase::probe_rtt) {
      if (probe_rtt_until == clock::time_point{} && static_cast<double>(inflight_bytes) <= kMinCwnd) {
        probe_rtt_until = now + kProbeRttTime;
        probe_rtt_round = rounds;
      } else if (probe_rtt_until != clock::time_point{} && rounds > probe_rtt_round && now >= probe_rtt_until) {
        min_rtt_at = now;
        cwnd = std::max(cwnd, prior_cwnd);
        prior_cwnd = 0;
        if (filled) {
          enter_probe_bw(now);
        } else {
          mode = phase::startup;
          pacing_gain = cwnd_gain = kStartupGain;
        }
      }
    }
    // The window: cwnd_gain bandwidth-delay products once the pipe is full;
    // before, it grows by what is delivered (slow start's doubling).
    if (mode == phase::probe_rtt) {
      cwnd = kMinCwnd;
    } else if (newly > 0) {
      const double b = bdp();
      const double target = b > 0 ? cwnd_gain * b + 3 * kMss : cwnd + static_cast<double>(newly);
      if (filled) {
        cwnd = std::min(cwnd + static_cast<double>(newly), target);
      } else if (cwnd < target || delivered < static_cast<std::uint64_t>(10 * kMss)) {
        cwnd += static_cast<double>(newly);
      }
      cwnd = std::clamp(std::min(cwnd, inflight_hi), kMinCwnd, kMaxCwnd);
    }
    pump(now);
    cv.notify_all();
  }

  // The timer: lost packets the pace held back go; a tail probe; a packet
  // unacknowledged past the RTO marks the whole flight lost; a delayed ack
  // goes out.
  void tick(clock::time_point now) {
    if (ack_due != clock::time_point{} && now >= ack_due) send_ack();
    if (inflight.empty()) return;
    pump(now);
    const auto quiet = std::chrono::microseconds(static_cast<long long>(std::max(10000.0, srtt * 2000.0)));
    if (!probed && lost.empty() && last_ack != clock::time_point{} && now - last_ack > quiet) {
      probed = true;
      auto& newest = inflight.rbegin()->second;
      if (!newest.lost) {
        newest.sent = now;
        ++newest.tries;
        ++stats.probes;
        ++stats.retransmits;
        send_wire(newest.wire);
      }
      return;
    }
    // The oldest copy in the pipe; lost ones wait on the window, not the clock.
    auto it = inflight.begin();
    while (it != inflight.end() && it->second.lost) ++it;
    if (it == inflight.end() || now - it->second.sent < std::chrono::milliseconds(rto_ms)) return;
    // A timeout: everything in the pipe is taken for lost and goes again
    // from a window of one packet, regrowing by what is delivered.
    for (auto& [seq, p] : inflight) {
      if (p.lost) continue;
      p.lost = true;
      lost.insert(seq);
      ++stats.lost;
    }
    inflight_bytes = 0;
    cwnd = kMss;
    rto_ms = std::min(rto_ms * 2, kMaxRtoMs);
    ++stats.rtos;
    pump(now);
    cv.notify_all();
  }
};

namespace {

class udp_stream final : public link {
 public:
  explicit udp_stream(std::shared_ptr<udp_hub::stream_state> s) : s_(std::move(s)) {}
  ~udp_stream() override { shutdown(); }

  expected write_all(std::span<const std::uint8_t> bytes) override {
    std::unique_lock lock(s_->m);
    // The writer was away long enough for the pipe to drain below three
    // quarters: what is measured until this flight is delivered says how
    // fast this side wrote, not how fast the path goes.
    if (s_->lost.empty() && static_cast<double>(s_->inflight_bytes) < 0.75 * s_->cwnd) {
      s_->app_limited_until = std::max<std::uint64_t>(1, s_->delivered + s_->inflight_bytes);
    }
    std::size_t at = 0;
    while (at < bytes.size()) {
      const std::size_t n = std::min(kStreamPayload, bytes.size() - at);
      // Room in the congestion window and the peer's buffer, nothing lost
      // still to go again, and the pace (#529); a peer silent this long is
      // gone (the session above notices sooner).
      const auto until = clock::now() + std::chrono::milliseconds(kSilenceMs);
      for (;;) {
        if (s_->down) return err(status::cancelled);
        const auto now = clock::now();
        s_->pump(now);
        // A zero window must not stall forever: with nothing out, one packet may probe it.
        const bool room = s_->lost.empty() && ((static_cast<double>(s_->inflight_bytes + n) <= s_->cwnd &&
                                                s_->inflight_bytes + n <= s_->peer_window) ||
                                               s_->inflight.empty());
        if (room) {
          if (!s_->paced_out(now, kHead + n)) break;
          const auto wake = now + std::chrono::microseconds(static_cast<long long>(
                                      (static_cast<double>(kHead + n) - s_->tokens) / s_->pace_rate() * 1e6) + 1);
          if (s_->cv.wait_until(lock, wake) == std::cv_status::timeout) {
            const double late = std::chrono::duration<double, std::milli>(clock::now() - wake).count();
            s_->oversleep_ms = 0.875 * s_->oversleep_ms + 0.125 * std::max(0.0, late);
          }
          continue;
        }
        if (s_->cv.wait_until(lock, until) == std::cv_status::timeout) return err(status::timeout);
      }
      udp_hub::stream_state::packet p;
      const std::uint32_t seq = s_->next_seq++;
      p.wire.resize(kHead + n);
      p.wire[0] = kStream;
      p.wire[1] = kData;
      put32(p.wire.data() + 2, s_->id);
      put32(p.wire.data() + 6, seq);
      std::memcpy(p.wire.data() + kHead, bytes.data() + at, n);
      p.payload = n;
      p.tries = 1;
      s_->stamp(p, clock::now());
      if (s_->inflight.empty()) s_->last_ack = p.sent;  // the probe's clock starts with the flight
      s_->inflight_bytes += n;
      ++s_->stats.packets;
      s_->stats.max_cwnd = std::max(s_->stats.max_cwnd, s_->cwnd);
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
      const bool was_full = s_->rx.size() + s_->ahead_bytes >= kMaxRx / 2;
      const std::size_t n = std::min(into.size(), s_->rx.size());
      std::copy_n(s_->rx.begin(), n, into.begin());
      s_->rx.erase(s_->rx.begin(), s_->rx.begin() + static_cast<std::ptrdiff_t>(n));
      if (was_full && s_->rx.size() + s_->ahead_bytes < kMaxRx / 2) s_->send_ack();  // the window opened
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

std::optional<udp_hub::stream_stats> udp_hub::stats(std::uint32_t id) {
  std::shared_ptr<stream_state> s;
  {
    std::lock_guard lock(m_);
    if (auto it = streams_.find(id); it != streams_.end()) s = it->second.lock();
  }
  if (!s) return std::nullopt;
  std::lock_guard l(s->m);
  stream_stats out = s->stats;
  out.srtt_ms = s->srtt;
  out.cwnd = s->cwnd;
  out.min_rtt_ms = s->min_rtt;
  out.bandwidth = s->btl_bw();
  out.pacing_rate = s->pace_rate();
  out.inflight = static_cast<double>(s->inflight_bytes);
  out.mode = static_cast<int>(s->mode);
  return out;
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
  if (type == kData && d.size() <= kHead + kStreamPayload) {
    s->on_data(get32(d.data() + 6), d.subspan(kHead), false);
  } else if (type == kFin) {
    s->on_data(get32(d.data() + 6), {}, true);
  } else if (type == kAck && d.size() >= kAckPacket) {
    std::uint64_t sack[kSackWords];
    for (std::size_t w = 0; w < kSackWords; ++w) sack[w] = get64(d.data() + 10 + 8 * w);
    s->on_ack(get32(d.data() + 6), sack, get32(d.data() + 10 + 8 * kSackWords), d.subspan(kAckPacket));
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
