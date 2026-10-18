// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/relay.h"

#include <algorithm>
#include <cstring>

#include "addons/sharepoint/invite.h"
#include "addons/sharepoint/race.h"
#include "addons/sharepoint/transfer.h"
#include "core/json.h"

namespace mv::sharepoint::relay {

namespace {

constexpr const char* kWhy[] = {"ok", "off", "busy", "unreachable", "refused"};

}  // namespace

const char* answer_name(answer a) noexcept {
  const auto i = static_cast<std::size_t>(a);
  return i < std::size(kWhy) ? kWhy[i] : "refused";
}

std::string request_json(const std::vector<net::endpoint>& to) {
  json::writer w;
  w.begin_object().key("places").begin_array();
  for (std::size_t i = 0; i < to.size() && i < invite::kMaxPlaces; ++i) {
    w.begin_object().key("a").string(to[i].address).key("p").integer(to[i].port).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::vector<net::endpoint> parse_request(std::string_view json) {
  auto out = parse_places(json);
  if (out.size() > invite::kMaxPlaces) out.resize(invite::kMaxPlaces);
  return out;
}

std::string answer_json(answer a) {
  json::writer w;
  w.begin_object().key("ok").boolean(a == answer::ok);
  if (a != answer::ok) w.key("why").string(answer_name(a));
  w.end_object();
  return w.take();
}

std::optional<answer> parse_answer(std::string_view text) {
  const auto doc = json::parse(text, 2);
  if (!doc || doc->k != json::kind::object) return std::nullopt;
  const auto ok = doc->boolean("ok");
  if (!ok) return std::nullopt;
  if (*ok) return answer::ok;
  const std::string* why = doc->str("why");
  for (std::size_t i = 1; why && i < std::size(kWhy); ++i) {
    if (*why == kWhy[i]) return static_cast<answer>(i);
  }
  return answer::refused;  // a reason this build does not know: still no
}

bool may_reach(const net::endpoint& to, bool allow_loopback) noexcept {
  const auto ip = net::parse_address(to.address);
  if (!ip || to.port == 0) return false;
  if (net::is_public(*ip)) return true;
  if (!allow_loopback) return false;
  static constexpr std::uint8_t kLoop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  return ip->v6 ? std::memcmp(ip->bytes.data(), kLoop6, 16) == 0 : ip->bytes[0] == 127;
}

void add_relay(link_race& race, const invite::relay_place& r, const connect_fn& connect,
               std::function<bool()> stopped) {
  race.add(
      [r, connect, stopped = std::move(stopped)]() -> result<std::unique_ptr<link>> {
        auto c = connect(r.at, kReachConnectMs);
        if (!c) return err(c.error());
        auto paired = join(std::move(*c), r.token, kPairWaitMs, stopped);
        if (!paired) return err(paired.error());
        return std::unique_ptr<link>(std::make_unique<relayed_link>(std::move(*paired), std::vector<std::uint8_t>{},
                                                                    "relay"));
      },
      kRelayDelayMs);
}

// ---- the link over a channel ----------------------------------------------------------

expected channel_link::write_all(std::span<const std::uint8_t> bytes) {
  while (!bytes.empty()) {
    const auto n = std::min(bytes.size(), kDataChunk);
    if (auto r = ch_->send(msg::relay_data, bytes.first(n)); !r) return r;
    bytes = bytes.subspan(n);
  }
  return {};
}

result<std::size_t> channel_link::read_some(std::span<std::uint8_t> into, int timeout_ms) {
  if (closed_) return std::size_t{0};
  while (at_ >= m_.body.size() || m_.type != msg::relay_data) {
    at_ = 0;
    if (auto r = ch_->receive(m_, timeout_ms); !r) {
      m_.body.clear();
      return err(r.error());
    }
    if (m_.type == msg::cancel) {
      closed_ = true;
      m_.body.clear();
      return std::size_t{0};
    }
    if (m_.type != msg::relay_data) {
      // Nothing else belongs inside a relay: the other end is not one.
      m_.body.clear();
      ch_->shutdown();
      return err(status::corrupt);
    }
  }
  const std::size_t n = std::min(into.size(), m_.body.size() - at_);
  std::memcpy(into.data(), m_.body.data() + at_, n);
  at_ += n;
  return n;
}

// ---- A ---------------------------------------------------------------------------------

result<std::unique_ptr<link>> open_through(std::shared_ptr<channel> ch, const std::vector<net::endpoint>& to,
                                           std::shared_ptr<void> keep, answer& said) {
  said = answer::refused;
  if (to.empty()) return err(status::invalid_arg);
  if (auto r = ch->send(msg::relay, request_json(to)); !r) return err(r.error());
  message m;
  if (auto r = ch->receive(m, kAnswerMs); !r) return err(r.error());
  // R from before relays reads msg::relay as a broken offer and hangs up
  // (above); one that answers anything else is not relaying either.
  const auto a = m.type == msg::relay_answer ? parse_answer(m.text()) : std::nullopt;
  if (!a) return err(status::unsupported_format);
  said = *a;
  if (*a != answer::ok) return err(*a == answer::busy ? status::busy : status::not_found);
  return std::unique_ptr<link>(std::make_unique<channel_link>(std::move(ch), std::move(keep), "relay"));
}

// ---- R ---------------------------------------------------------------------------------

served serve(const std::shared_ptr<channel>& ch, const message& request, bool allowed, const serve_options& o) {
  served out;
  const auto say = [&](answer a) {
    out.said = a;
    (void)ch->send(msg::relay_answer, answer_json(a));
  };
  if (!allowed) {
    say(answer::off);
    ch->flush(2000);
    return out;
  }
  std::vector<net::endpoint> to;
  if (request.type == msg::relay) {
    for (const auto& p : parse_request(request.text())) {
      if (may_reach(p, o.allow_loopback)) to.push_back(p);
    }
  }
  if (to.empty()) {
    say(answer::refused);
    ch->flush(2000);
    return out;
  }
  const connect_fn connect = o.connect ? o.connect : connect_fn([](const net::endpoint& e, int ms) {
    return net::connect(e, ms);
  });
  link_race race;
  start_round(race, to, connect);
  auto onward = race.wait([&] { return (o.stopped && o.stopped()) || ch->closed(); });
  race.join();
  if (!onward) {
    say(answer::unreachable);
    ch->flush(2000);
    return out;
  }
  say(answer::ok);
  if (out.said != answer::ok || ch->closed()) {
    onward->shutdown();
    return out;
  }
  channel_link from_a(ch, nullptr, "relay");
  out.spliced = splice(from_a, *onward, o.lim, o.stopped);
  onward->shutdown();
  return out;
}

}  // namespace mv::sharepoint::relay
