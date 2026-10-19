// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/sharepoint/bt_pipe.h"

#include <algorithm>
#include <chrono>

#include "addons/sharepoint/wire.h"
#include "core/json.h"

namespace mv::sharepoint::bt {

std::string info_json(const std::string& id, const std::string& name) {
  json::writer w;
  w.begin_object().key("v").integer(kBeaconVersion).key("p").integer(kProtocolVersion).key("id").string(id).key("name").string(clean_name(name));
  w.end_object();
  return w.take();
}

bool parse_info(std::span<const std::uint8_t> value, std::string& id, std::string& name, std::uint32_t& protocol) {
  if (value.empty() || value.size() > kMaxBeaconBytes) return false;
  const auto doc = json::parse({reinterpret_cast<const char*>(value.data()), value.size()}, 4);
  if (!doc) return false;
  const auto v = doc->integer("v");
  const std::string* i = doc->str("id");
  const std::string* n = doc->str("name");
  if (!v || *v != static_cast<std::int64_t>(kBeaconVersion) || !i || !valid_id(*i) || !n) return false;
  protocol = protocol_of(doc->integer("p"));
  id = *i;
  name = clean_name(*n);
  return true;
}

bool parse_guid(std::string_view t, guid_parts& out) noexcept {
  // 8-4-4-4-12 hex digits.
  if (t.size() != 36 || t[8] != '-' || t[13] != '-' || t[18] != '-' || t[23] != '-') return false;
  std::uint8_t b[16];
  std::size_t at = 0;
  for (std::size_t i = 0; i < t.size();) {
    if (t[i] == '-') {
      ++i;
      continue;
    }
    const auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    if (i + 1 >= t.size()) return false;
    const int hi = nib(t[i]), lo = nib(t[i + 1]);
    if (hi < 0 || lo < 0 || at >= 16) return false;
    b[at++] = static_cast<std::uint8_t>(hi << 4 | lo);
    i += 2;
  }
  if (at != 16) return false;
  out.d1 = static_cast<std::uint32_t>(b[0]) << 24 | static_cast<std::uint32_t>(b[1]) << 16 |
           static_cast<std::uint32_t>(b[2]) << 8 | b[3];
  out.d2 = static_cast<std::uint16_t>(b[4] << 8 | b[5]);
  out.d3 = static_cast<std::uint16_t>(b[6] << 8 | b[7]);
  for (int i = 0; i < 8; ++i) out.d4[i] = b[8 + i];
  return true;
}

pipe::pipe(std::size_t packet_bytes, send_fn send, closed_fn on_shutdown)
    : packet_bytes_(std::max<std::size_t>(packet_bytes, 20)), send_(std::move(send)),
      on_shutdown_(std::move(on_shutdown)) {}

pipe::~pipe() { shutdown(); }

void pipe::deliver(std::span<const std::uint8_t> packet) {
  if (packet.empty()) return;
  std::lock_guard lock(m_);
  if (down_ || peer_closed_) return;
  if (packet[0] == kPacketClose) {
    peer_closed_ = true;
  } else if (packet[0] == kPacketData) {
    if (in_.size() + packet.size() - 1 > kMaxBuffered) {
      peer_closed_ = true;  // it does not wait for us: end the link
    } else {
      in_.insert(in_.end(), packet.begin() + 1, packet.end());
    }
  }
  cv_.notify_all();
}

void pipe::closed_by_peer() noexcept {
  std::lock_guard lock(m_);
  peer_closed_ = true;
  cv_.notify_all();
}

void pipe::set_packet_bytes(std::size_t n) noexcept {
  std::lock_guard lock(write_m_);
  packet_bytes_ = std::max<std::size_t>(n, 20);
}

expected pipe::write_all(std::span<const std::uint8_t> bytes) {
  std::lock_guard w(write_m_);
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    {
      std::lock_guard lock(m_);
      if (down_) return err(status::cancelled);
      if (peer_closed_) return err(status::io);
    }
    const std::size_t n = std::min(bytes.size() - sent, packet_bytes_ - 1);
    packet_.assign(1, kPacketData);
    packet_.insert(packet_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(sent),
                   bytes.begin() + static_cast<std::ptrdiff_t>(sent + n));
    if (!send_(packet_)) return err(status::io);
    sent += n;
  }
  return {};
}

result<std::size_t> pipe::read_some(std::span<std::uint8_t> into, int timeout_ms) {
  if (into.empty()) return std::size_t{0};
  std::unique_lock lock(m_);
  const auto ready = [&] { return down_ || !in_.empty() || peer_closed_; };
  if (timeout_ms < 0) {
    cv_.wait(lock, ready);
  } else if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
    return err(status::timeout);
  }
  if (down_) return err(status::cancelled);
  if (in_.empty()) return std::size_t{0};  // the other side closed
  const std::size_t n = std::min(into.size(), in_.size());
  std::copy_n(in_.begin(), n, into.begin());
  in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n));
  return n;
}

void pipe::shutdown() noexcept {
  bool tell = false;
  {
    std::lock_guard lock(m_);
    down_ = true;
    tell = !told_;
    told_ = true;
    cv_.notify_all();
  }
  if (tell && on_shutdown_) {
    try {
      on_shutdown_();
    } catch (...) {
    }
  }
}

}  // namespace mv::sharepoint::bt
