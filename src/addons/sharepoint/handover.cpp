// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/handover.h"

#include <sodium.h>

#include <chrono>
#include <cstring>

namespace mv::sharepoint {

namespace {

using clock = std::chrono::steady_clock;

constexpr char kContext[crypto_kdf_CONTEXTBYTES + 1] = "MVSPhand";
// After a stream is handed on: how long the session has to take it before a
// next meeting starts (a rejoin's handshake over a fresh punch).
constexpr int kTakeMs = 5000;
// A lookup that found nothing: the other side's put is likely on its way.
constexpr int kRetryMs = 500;

}  // namespace

dht::keys handover_keys(std::span<const std::uint8_t, 32> secret) {
  (void)sodium_init();
  static_assert(crypto_kdf_KEYBYTES == 32);
  std::uint8_t part[4][32];
  for (std::uint64_t i = 0; i < 4; ++i) crypto_kdf_derive_from_key(part[i], sizeof part[i], i, kContext, secret.data());
  dht::keys k;
  crypto_sign_seed_keypair(k.public_key.data(), k.secret_key.data(), part[0]);
  std::memcpy(k.box.data(), part[1], 32);
  std::memcpy(k.salt_base.data(), part[2], 32);
  std::memcpy(k.password.data(), part[3], 32);
  sodium_memzero(part, sizeof part);
  return k;
}

dht_handover::dht_handover(std::shared_ptr<channel> ch, options o, std::function<bool()> armed, link_fn on_link,
                           std::function<void()> on_ready)
    : ch_(std::move(ch)),
      opts_(std::move(o)),
      armed_(std::move(armed)),
      on_link_(std::move(on_link)),
      on_ready_(std::move(on_ready)) {
  thread_ = std::thread([this] {
    try {
      run();
    } catch (...) {
    }
  });
}

dht_handover::~dht_handover() {
  {
    std::lock_guard lock(m_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool dht_handover::stopped() const {
  {
    std::lock_guard lock(m_);
    if (stop_) return true;
  }
  return ch_->closed();
}

bool dht_handover::pause(int ms) {
  std::unique_lock lock(m_);
  cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return stop_; });
  return !stop_ && !ch_->closed();
}

void dht_handover::run() {
  auto h = udp_hub::open(0, opts_.loopback);
  if (!h) return;
  std::shared_ptr<udp_hub> hub = std::move(*h);
  struct hub_guard {
    std::shared_ptr<udp_hub>& h;
    ~hub_guard() { h->stop(); }
  } stop_hub{hub};
  const std::function<bool()> halt = [this] { return stopped(); };
  dht::client client(hub, opts_.client);
  // Joined while the local link still works: a drop then costs no bootstrap.
  bool joined = client.start(halt);
  if (joined) {
    ready_.store(true);
    if (on_ready_) on_ready_();
  }
  while (pause(opts_.poll_ms)) {
    if (ch_->link_up() || !armed_()) continue;
    const auto seen = clock::now();
    if (!joined) {
      joined = client.start(halt);
      if (!joined) continue;
      ready_.store(true);
      if (on_ready_) on_ready_();
    } else {
      // This side may be on another network now: how the DHT sees it, again.
      (void)client.refresh(halt);
    }
    // Both ends compute these from the session as it stands; a rejoin
    // elsewhere changes them for the next drop.
    const auto secret = ch_->handover_secret();
    const dht::keys k = handover_keys(secret);
    dht::meet_options mo;
    mo.loopback = opts_.loopback;
    mo.wait_ms = opts_.wait_ms;
    mo.retry_ms = kRetryMs;
    const std::function<bool()> give_up = [this] { return stopped() || ch_->link_up(); };
    auto met = dht::meet(hub, client, k, opts_.me, mo, give_up, {});
    if (!met) continue;
    if (give_up()) {
      (*met)->shutdown();  // back some other way meanwhile
      continue;
    }
    last_meet_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - seen).count());
    meetings_.fetch_add(1);
    on_link_(std::make_unique<bearer_as>(std::move(*met), transport::internet));
    // The session takes it (or not) before a next meeting; then this side's
    // items say "gone".
    const auto until = clock::now() + std::chrono::milliseconds(kTakeMs);
    while (clock::now() < until && !ch_->link_up() && pause(50)) {
    }
    dht::withdraw(client, k, opts_.me, halt);
  }
}

}  // namespace mv::sharepoint
