// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: what a peer sends Driftwire before anything is
// authenticated (src/addons/driftwire, docs/design/28, issue #472): the
// beacon, Bluetooth's INFO, the hello, protocol 2's Noise messages (a
// receiver reading a first message, a sender reading the reply, a receiver
// reading a rejoin), a whole receiver's open fed from the input, and the
// offer (sealed, but the receiver's one structured parse of the sender's
// words); and from #472 PR 2, what the internet brings: a pasted code, an
// address, a router's PCP / NAT-PMP / SSDP / HTTP / XML replies, a CPace
// message and an internet hello; and from #472 PR 3, the DHT: bencoding,
// compact node lists and endpoints, word codes, an item opened under a code's
// key; and from #472 PR 4, a remembered-devices record as the secret store
// returns it (damaged, or written by another version); and from #472 PR 5,
// the receiver's internet places as a sender reads them; and from #472's
// relays, a relay server's rendezvous hello, the relay field as typed, and the own-device relay's
// request (as R reads it) and answer (as A reads it). The first byte picks
// which. Whatever comes in, every call must
// return, read nothing past the input and never hang: a link that runs out is
// a closed link.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "addons/driftwire/bt_pipe.h"
#include "addons/driftwire/channel.h"
#include "addons/driftwire/bencode.h"
#include "addons/driftwire/cpace.h"
#include "addons/driftwire/dht.h"
#include "addons/driftwire/dht_item.h"
#include "addons/driftwire/invite.h"
#include "addons/driftwire/known.h"
#include "addons/driftwire/mapping.h"
#include "addons/driftwire/net.h"
#include "addons/driftwire/noise.h"
#include "addons/driftwire/relay.h"
#include "addons/driftwire/splice.h"
#include "addons/driftwire/transfer.h"
#include "addons/driftwire/wire.h"

namespace sp = mv::driftwire;
namespace noise = mv::driftwire::noise;

namespace {

// A link that reads the input and swallows writes; at the end it is closed.
class input_link final : public sp::link {
 public:
  explicit input_link(std::span<const std::uint8_t> in) : in_(in) {}
  mv::expected write_all(std::span<const std::uint8_t>) override {
    if (down_) return mv::err(mv::status::cancelled);
    return {};
  }
  mv::result<std::size_t> read_some(std::span<std::uint8_t> into, int) override {
    if (down_) return mv::err(mv::status::cancelled);
    const std::size_t n = std::min(into.size(), in_.size() - at_);
    if (n == 0) return std::size_t{0};  // the peer closed
    std::memcpy(into.data(), in_.data() + at_, n);
    at_ += n;
    return n;
  }
  void shutdown() noexcept override { down_ = true; }
  sp::transport via() const noexcept override { return sp::transport::wifi; }

 private:
  std::span<const std::uint8_t> in_;
  std::size_t at_ = 0;
  bool down_ = false;
};

std::array<std::uint8_t, 32> fixed(std::uint8_t v) {
  std::array<std::uint8_t, 32> k;
  k.fill(v);
  return k;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size < 1) return 0;
  const std::span<const std::uint8_t> in(data + 1, size - 1);
  const std::string_view text(reinterpret_cast<const char*>(in.data()), in.size());
  std::vector<std::uint8_t> out, payload;
  switch (data[0] % 28) {
    case 0:
      (void)sp::decode_beacon(in);
      break;
    case 1: {
      std::string id, name;
      std::uint32_t protocol = 0;
      (void)sp::bt::parse_info(in, id, name, protocol);
      break;
    }
    case 2:
      (void)sp::parse_offer(text);
      break;
    case 3: {
      input_link l(in);
      (void)sp::channel::read_hello(l, std::chrono::steady_clock::now() + std::chrono::seconds(1));
      break;
    }
    case 4: {
      // A receiver reading a first message, then answering it.
      noise::handshake resp(noise::xx_hfs(), false, {}, fixed(2));
      if (resp.read_message(in, payload)) (void)resp.write_message({}, out);
      break;
    }
    case 5: {
      // A sender reading the reply to its real first message.
      noise::test_keys k;
      k.e = fixed(3);
      k.e1_seed.emplace();
      k.e1_seed->fill(4);
      noise::handshake init(noise::xx_hfs(), true, {}, fixed(1), std::nullopt, k);
      if (init.write_message({}, out)) (void)init.read_message(in, payload);
      break;
    }
    case 6: {
      // A receiver reading a rejoin that does not hold its secret.
      noise::handshake resp(noise::nn_hfs_psk0(), false, {}, std::nullopt, fixed(7));
      if (resp.read_message(in, payload)) (void)resp.write_message({}, out);
      break;
    }
    case 7: {
      // A whole receiver's open: the hello, then whatever follows it (an
      // internet hello meets a code that waits).
      auto l = std::make_shared<input_link>(in);
      auto h = sp::channel::read_hello(*l, std::chrono::steady_clock::now() + std::chrono::seconds(1));
      sp::channel::options o;
      o.internet_secret = std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6};
      if (h) (void)sp::channel::open_server(l, std::move(*h), "bbbbbbbbbbbbbbbb", "Fuzz", o);
      break;
    }
    case 8:
      if (auto c = sp::invite::decode(text)) (void)sp::invite::encode(*c);
      break;
    case 9:
      if (auto a = sp::net::parse_address(text)) {
        (void)sp::net::address_text(*a);
        (void)sp::net::is_public(*a);
      }
      break;
    case 10: {
      std::array<std::uint8_t, 12> nonce{};
      if (in.size() >= 36) std::memcpy(nonce.data(), in.data() + 24, 12);  // sometimes the right one
      (void)mv::driftwire::mapping::parse_pcp_map(in, nonce);
      (void)mv::driftwire::mapping::parse_nat_pmp_map(in);
      (void)mv::driftwire::mapping::parse_nat_pmp_address(in);
      break;
    }
    case 11:
      (void)mv::driftwire::mapping::parse_ssdp_location(text, mv::driftwire::net::address{false, {192, 168, 1, 1}});
      break;
    case 12:
      (void)mv::driftwire::mapping::parse_description(text, "http://192.168.1.1:5000/rootDesc.xml",
                                                      mv::driftwire::net::address{false, {192, 168, 1, 1}});
      break;
    case 13:
      if (auto r = mv::driftwire::mapping::parse_http(text)) (void)mv::driftwire::mapping::xml_value(r->body, "NewExternalIPAddress");
      break;
    case 14: {
      // A receiver's CPace with a peer message from the input.
      const std::uint8_t prs[] = {1, 2, 3, 4, 5, 6}, ci[] = {'c'};
      std::uint8_t sid[16] = {};
      mv::driftwire::cpace::party p(prs, ci, sid);
      (void)p.finish(in, false, {}, {});
      break;
    }
    case 16:
      if (auto v = mv::driftwire::bencode::decode(text)) (void)mv::driftwire::bencode::encode(*v);
      break;
    case 17:
      (void)mv::driftwire::dht::parse_nodes(text, in.size() % 38 == 0);
      (void)mv::driftwire::dht::parse_compact_endpoint(text);
      break;
    case 18:
      if (auto w = mv::driftwire::dht::parse_words(text)) (void)mv::driftwire::dht::words_text(*w);
      break;
    case 19: {
      // An item's value opened under a code's key (derived once).
      static auto k = mv::driftwire::dht::derive({1, 2, 3, 4}, true);
      if (k) (void)mv::driftwire::dht::open(*k, "salt", text, 1'760'000'000'000);
      break;
    }
    case 20: {
      // An item signature check over hostile bytes.
      std::array<std::uint8_t, 32> key{};
      if (in.size() >= 96) {
        std::memcpy(key.data(), in.data(), 32);
        (void)mv::driftwire::dht::verify(key, text.substr(96, 16), 1, text.substr(96),
                                          in.subspan(32, 64));
      }
      break;
    }
    case 21:
      if (auto d = mv::driftwire::known::decode(in)) (void)mv::driftwire::known::encode(*d);
      break;
    case 22:
      (void)sp::parse_places(text);
      break;
    case 23:
      (void)sp::relay::parse_head(in);
      break;
    case 24:
      for (const auto& p : sp::relay::parse_request(text)) {
        (void)sp::relay::may_reach(p, false);
        (void)sp::relay::may_relay_to(p, sp::relay::fence{});
      }
      break;
    case 25:
      (void)sp::relay::parse_answer(text);
      break;
    case 26: {
      // The relay field as typed (split only: nothing resolves here).
      std::string why;
      (void)sp::relay_server_of(std::string(text), why);
      break;
    }
    case 27: {
      // #472: an item's plaintext (candidates, the hard flag, the TCP port), as
      // if it had opened: padded or cut to the item's size, magic in front.
      std::array<std::uint8_t, mv::driftwire::dht::kPlainBytes> plain{};
      plain[0] = 'M';
      plain[1] = 'V';
      plain[2] = 'S';
      plain[3] = 'D';
      plain[4] = 1;
      std::memcpy(plain.data() + 5, in.data(), std::min(in.size(), plain.size() - 5));
      std::uint64_t written = 0;  // "now" is the item's own time, so the age check lets it through
      for (int i = 7; i >= 0; --i) written = (written << 8) | plain[5 + static_cast<std::size_t>(i)];
      (void)mv::driftwire::dht::parse_plain(plain, static_cast<std::int64_t>(written));
      break;
    }
    case 15: {
      // A sender with a code, answered by the input.
      auto l = std::make_shared<input_link>(in);
      const std::uint8_t secret[] = {1, 2, 3, 4, 5, 6};
      (void)sp::channel::open_internet(l, secret, "aaaaaaaaaaaaaaaa", "Fuzz");
      break;
    }
  }
  return 0;
}
