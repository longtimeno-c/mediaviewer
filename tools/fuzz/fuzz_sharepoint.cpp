// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// libFuzzer harness: what a peer sends Sharepoint before anything is
// authenticated (src/addons/sharepoint, docs/design/28, issue #472): the
// beacon, Bluetooth's INFO, the hello, protocol 2's Noise messages (a
// receiver reading a first message, a sender reading the reply, a receiver
// reading a rejoin), a whole receiver's open fed from the input, and the
// offer (sealed, but the receiver's one structured parse of the sender's
// words). The first byte picks which. Whatever comes in, every call must
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

#include "addons/sharepoint/bt_pipe.h"
#include "addons/sharepoint/channel.h"
#include "addons/sharepoint/noise.h"
#include "addons/sharepoint/transfer.h"
#include "addons/sharepoint/wire.h"

namespace sp = mv::sharepoint;
namespace noise = mv::sharepoint::noise;

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
  switch (data[0] % 8) {
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
      // A whole receiver's open: the hello, then whatever follows it.
      auto l = std::make_shared<input_link>(in);
      auto h = sp::channel::read_hello(*l, std::chrono::steady_clock::now() + std::chrono::seconds(1));
      if (h) (void)sp::channel::open_server(l, std::move(*h), "bbbbbbbbbbbbbbbb", "Fuzz");
      break;
    }
  }
  return 0;
}
