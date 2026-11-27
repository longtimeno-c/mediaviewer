// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/catalogue_port.h"

#include <cstddef>
#include <mutex>
#include <vector>

#include "mediaviewer/mediaviewer_ai.h"

namespace mv::addon::catalogue {

namespace {

// Taken around every call into the pack, so set_provider({}) returns only
// once none is in flight.
std::mutex& lock_of() {
  static std::mutex* m = new std::mutex();  // never destroyed: no order at static exit
  return *m;
}
provider& current() {
  static provider* p = new provider();
  return *p;
}

}  // namespace

void set_provider(provider p) {
  std::lock_guard lock(lock_of());
  current() = std::move(p);
}

provider from_ai(const mv_ai_api* api) {
  const auto has = [api](std::size_t end) { return api && api->struct_size >= end; };
  if (!has(offsetof(mv_ai_api, phone_json) + sizeof(api->phone_json)) || !api->phone_publish || !api->phone_cancel ||
      !api->phone_json) {
    return {};
  }
  provider s;
  s.publish = [api](const std::string& destination) -> expected {
    const mv_status r = api->phone_publish(api->ctx, destination.c_str());
    if (r == MV_OK) return {};
    return err(static_cast<status>(r));
  };
  s.cancel = [api] { (void)api->phone_cancel(api->ctx); };
  s.json = [api]() -> std::string {
    std::vector<char> buf(16 * 1024);
    for (int attempt = 0; attempt < 3; ++attempt) {
      uint32_t needed = 0;
      const mv_status r = api->phone_json(api->ctx, buf.data(), static_cast<uint32_t>(buf.size()), &needed);
      if (r == MV_OK) return std::string(buf.data());
      if (r != MV_ERR_INVALID_ARG || needed <= buf.size()) return {};
      buf.resize(needed + 1024);
    }
    return {};
  };
  return s;
}

expected publish(const std::string& destination) {
  std::lock_guard lock(lock_of());
  if (!current()) return err(status::not_found);
  return current().publish(destination);
}

expected cancel() {
  std::lock_guard lock(lock_of());
  if (!current()) return err(status::not_found);
  current().cancel();
  return {};
}

result<std::string> json() {
  std::lock_guard lock(lock_of());
  if (!current()) return err(status::not_found);
  return current().json();
}

}  // namespace mv::addon::catalogue
