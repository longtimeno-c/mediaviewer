// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's side of relays through the person's own computers (issue
// #472, option 9; relay.h, docs/design/28 "Relays"), apart from engine.cpp:
// the sender reaching a code's receiver through one of its own, the relay
// that answers it, and the "This is my computer" mark.
#include <sodium.h>

#include <algorithm>

#include "addons/driftwire/engine.h"
#include "addons/driftwire/meet.h"

namespace mv::driftwire {

namespace {

std::int64_t unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// What a link met over the DHT needs alive: its socket and the client. The
// client goes first, then the socket stops.
struct met_over_dht {
  std::shared_ptr<udp_hub> hub;
  std::unique_ptr<dht::client> client;
  ~met_over_dht() {
    client.reset();
    if (hub) hub->stop();
  }
};

std::string why_not(relay::answer a, const std::string& who) {
  switch (a) {
    case relay::answer::off:
      return who + " does not relay: turn on \xE2\x80\x9CUse my other computers to relay\xE2\x80\x9D there, and mark "
                   "this computer as yours in its remembered devices.";
    case relay::answer::unreachable:
      return who + " could not reach the other computer either.";
    default:
      return who + " could not relay to the addresses in this code.";
  }
}

}  // namespace

void engine::save_known() {
  std::vector<known::device> copy;
  {
    std::lock_guard lock(m_);
    copy = devices_;
  }
  std::lock_guard s(secrets_m_);
  if (secrets_ && !known::save(*secrets_, copy)) host_.log(1, "driftwire: the remembered devices were not kept");
}

bool engine::own_relay_possible() const {
  std::lock_guard lock(m_);
  return v_.relay && v_.internet && v_.dht &&
         std::any_of(devices_.begin(), devices_.end(), [](const known::device& d) { return d.own; });
}

void engine::set_own(const std::string& fingerprint, bool on) {
  {
    std::lock_guard lock(m_);
    const auto it = std::find_if(devices_.begin(), devices_.end(),
                                 [&](const known::device& d) { return relay::fingerprint(d.key) == fingerprint; });
    if (it == devices_.end() || it->own == on) return;
    it->own = on;
    publish_known_locked();
  }
  save_known();  // this worker's, never the UI thread
  post();
}

std::unique_ptr<link> engine::reach_through_own(const std::vector<net::endpoint>& places, std::string& via,
                                                std::string& why, const std::function<bool()>& give_up) {
  std::vector<known::device> own;
  {
    std::lock_guard lock(m_);
    if (!v_.relay || !v_.internet || !v_.dht) return nullptr;
    for (const auto& d : devices_) {
      if (d.own) own.push_back(d);
    }
  }
  const auto stopped = [&] { return cancel_.load() || stopping_.load() || (give_up && give_up()); };
  const auto say = [this](const std::string& s) {
    {
      std::lock_guard lock(m_);
      v_.out_note = s;
    }
    post_throttled();
  };
  for (const auto& r : own) {
    if (stopped()) return nullptr;
    say("Trying through " + r.name + ", your computer, too\xE2\x80\xA6");
    auto h = udp_hub::open(0, opts_.internet_loopback, opts_.wrap_udp);
    if (!h) continue;
    auto keep = std::make_shared<met_over_dht>();
    keep->hub = std::move(*h);
    keep->client = std::make_unique<dht::client>(keep->hub, dht_options());
    if (!keep->client->start(stopped)) continue;
    // As a send to a remembered device (engine.cpp, run_send): this hour's
    // keys toward r's identity, round after round, for relay_meet_ms at most.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts_.relay_meet_ms);
    const auto out_of_time = [&] { return stopped() || std::chrono::steady_clock::now() >= until; };
    std::unique_ptr<dht::keys> k;
    std::unique_ptr<link> met;
    while (!met && !out_of_time()) {
      const std::int64_t now = unix_ms();
      const std::int64_t hour = known::hour_of(now);
      k = std::make_unique<dht::keys>(known::presence_keys(r, hour, r.key));
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
      dht::meet_options mo;
      mo.loopback = opts_.internet_loopback;
      mo.wait_ms = static_cast<int>(
          std::clamp<std::int64_t>(std::min<std::int64_t>((hour + 1) * known::kHourMs - now, left.count()), 1000,
                                   8 * 60 * 1000));
      mo.punch_ms = known::kPunchMs;
      mo.tcp = true;  // as a send to a remembered device
      auto m = dht::meet(keep->hub, *keep->client, *k, dht::role::sender, mo, out_of_time, {});
      if (m) met = std::move(*m);
    }
    if (!met) {
      why = r.name + ", your computer, did not answer.";
      if (k) {
        const auto brief_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        dht::withdraw(*keep->client, *k, dht::role::sender,
                      [&] { return stopping_.load() || std::chrono::steady_clock::now() >= brief_until; });
      }
      continue;
    }
    std::shared_ptr<link> l(std::move(met));
    {
      std::lock_guard lock(m_);
      if (stopping_.load()) return nullptr;
      opening_.push_back(l);
    }
    channel::options o;
    o.static_key = static_key_;
    std::vector<std::uint8_t> pw(k->password.begin(), k->password.end());
    auto opened = channel::open_internet(l, pw, id_, opts_.my_name, o);
    sodium_memzero(pw.data(), pw.size());
    {
      const auto brief_until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      dht::withdraw(*keep->client, *k, dht::role::sender,
                    [&] { return stopping_.load() || std::chrono::steady_clock::now() >= brief_until; });
    }
    if (!opened || (*opened)->peer_key() != r.key) {
      if (opened) {
        (*opened)->shutdown();
        host_.log(1, "driftwire: a remembered device's key changed");
      }
      forget_opening(l);
      continue;
    }
    std::shared_ptr<channel> ch(std::move(*opened));
    relay::answer said = relay::answer::refused;
    auto through = relay::open_through(ch, places, keep, said);  // a cancel shuts `l` (opening_)
    forget_opening(l);
    if (!through) {
      ch->shutdown();
      if (stopped()) return nullptr;
      why = why_not(said, r.name);
      say(why);
      continue;
    }
    via = r.name;
    return std::move(*through);
  }
  return nullptr;
}

bool engine::maybe_relay(const std::shared_ptr<channel>& ch, const known::device& from, const message& first) {
  if (first.type != msg::relay) return false;
  bool allowed = false;
  {
    std::lock_guard lock(m_);
    // Marked as the person's own here, now (not only when the meeting began).
    const bool own = std::any_of(devices_.begin(), devices_.end(),
                                 [&](const known::device& d) { return d.key == from.key && d.own; });
    allowed = own && v_.relay && v_.internet && v_.dht && !cancel_.load() && !stopping_.load();
    if (allowed) {
      live_ = ch;  // a cancel (Stop) or the destructor ends it
      v_.relaying_for = from.name;
    }
  }
  if (allowed) post();
  relay::serve_options o;
  o.allow_loopback = opts_.internet_loopback;
  o.connect = opts_.connect_internet;
  o.lim = opts_.relay_limits;
  o.stopped = [this] { return cancel_.load() || stopping_.load(); };
  // Whatever happens below (a thread that could not start, memory), the
  // window and the channel are put back.
  struct relaying_guard {
    engine& e;
    const std::shared_ptr<channel>& ch;
    bool served = false;
    ~relaying_guard() {
      ch->shutdown();
      {
        std::lock_guard lock(e.m_);
        e.v_.relaying_for.clear();
        if (served) ++e.v_.relayed;
      }
      e.post();
    }
  } guard{*this, ch};
  try {
    const auto r = relay::serve(ch, first, allowed, o);
    guard.served = r.said == relay::answer::ok;
  } catch (...) {
    host_.log(1, "driftwire: a relay ended with an error");
  }
  if (guard.served) host_.log(2, "driftwire: relayed for another of this person's computers");
  return true;
}

}  // namespace mv::driftwire
