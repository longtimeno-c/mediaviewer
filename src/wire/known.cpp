// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "wire/known.h"


#include <sodium.h>

#include <algorithm>
#include <cstring>

namespace mv::driftwire::known {

namespace {

constexpr std::uint8_t kVersion = 1;
// #472 relays: a flags byte after each name (bit 0: the person's own
// computer). Written only when a device has a flag, so a build before relays
// still reads a list that has none.
constexpr std::uint8_t kVersionFlags = 2;
constexpr std::uint8_t kFlagOwn = 1;

// Cut to kMaxName bytes on a character boundary.
std::string clip(const std::string& name) {
  if (name.size() <= kMaxName) return name;
  std::size_t n = kMaxName;
  while (n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) --n;
  return name.substr(0, n);
}

}  // namespace

std::array<std::uint8_t, 32> identity(secrets::store* store, bool& kept) {
  if (sodium_init() < 0) {}  // safe to repeat; a bare (void) does not quiet GCC
  std::array<std::uint8_t, 32> key{};
  kept = false;
  if (store) {
    if (auto v = store->get(kIdentityName); v && v->size() == key.size()) {
      std::memcpy(key.data(), v->data(), key.size());
      sodium_memzero(v->data(), v->size());
      kept = true;
      return key;
    }
  }
  randombytes_buf(key.data(), key.size());
  if (store && store->put(kIdentityName, key)) {
    // Read back: a store that says yes and keeps nothing remembers nothing.
    if (auto v = store->get(kIdentityName); v && v->size() == key.size()) {
      kept = std::memcmp(v->data(), key.data(), key.size()) == 0;
      sodium_memzero(v->data(), v->size());
    }
  }
  return key;
}

std::vector<std::uint8_t> encode(const std::vector<device>& devices) {
  const bool flags = std::any_of(devices.begin(), devices.end(), [](const device& d) { return d.own; });
  std::vector<std::uint8_t> out{flags ? kVersionFlags : kVersion, 0};
  for (const auto& d : devices) {
    if (out[1] == kMaxDevices) break;
    const std::string name = clip(d.name);
    out.insert(out.end(), d.key.begin(), d.key.end());
    out.insert(out.end(), d.secret.begin(), d.secret.end());
    out.push_back(static_cast<std::uint8_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    if (flags) out.push_back(d.own ? kFlagOwn : 0);
    ++out[1];
  }
  return out;
}

std::optional<std::vector<device>> decode(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 2 || (bytes[0] != kVersion && bytes[0] != kVersionFlags) || bytes[1] > kMaxDevices) {
    return std::nullopt;
  }
  const bool flags = bytes[0] == kVersionFlags;
  std::vector<device> out;
  std::size_t at = 2;
  for (std::uint8_t i = 0; i < bytes[1]; ++i) {
    if (bytes.size() - at < 65) return std::nullopt;
    device d;
    std::memcpy(d.key.data(), bytes.data() + at, 32);
    std::memcpy(d.secret.data(), bytes.data() + at + 32, 32);
    const std::size_t n = bytes[at + 64];
    at += 65;
    if (n > kMaxName || bytes.size() - at < n) return std::nullopt;
    d.name.assign(reinterpret_cast<const char*>(bytes.data() + at), n);
    at += n;
    if (flags) {
      if (at >= bytes.size() || (bytes[at] & ~kFlagOwn) != 0) return std::nullopt;
      d.own = (bytes[at++] & kFlagOwn) != 0;
    }
    out.push_back(std::move(d));
  }
  if (at != bytes.size()) return std::nullopt;
  return out;
}

std::vector<device> load(secrets::store& store) {
  auto v = store.get(kDevicesName);
  if (!v) return {};
  auto d = decode(*v);
  sodium_memzero(v->data(), v->size());
  return d ? std::move(*d) : std::vector<device>{};
}

std::vector<device> load_named(secrets::store& store, const char* name) {
  auto v = store.get(name);
  if (!v) return {};
  auto d = decode(*v);
  sodium_memzero(v->data(), v->size());
  return d ? std::move(*d) : std::vector<device>{};
}

bool save(secrets::store& store, const std::vector<device>& devices) {
  if (devices.empty()) {
    (void)store.erase(kDevicesName);
    return true;
  }
  auto bytes = encode(devices);
  const bool ok = store.put(kDevicesName, bytes);
  sodium_memzero(bytes.data(), bytes.size());
  return ok;
}

bool forget(secrets::store& store) {
  (void)store.erase(kDevicesName);
  (void)store.erase(kIdentityName);
  (void)store.erase(kRelayKeyName);
  return !store.get(kDevicesName) && !store.get(kIdentityName) && !store.get(kRelayKeyName);
}

std::int64_t hour_of(std::int64_t unix_ms) noexcept {
  return unix_ms >= 0 ? unix_ms / kHourMs : (unix_ms - kHourMs + 1) / kHourMs;
}

}  // namespace mv::driftwire::known
