// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Local search as Phone access's catalogue source (phone/link.h): its
// phone_publish, phone_cancel and phone_json entries, while the pack is
// loaded. Both hosts set it when the pack attaches and clear it before the
// pack goes (addon_abi.cpp, shell/addons_mac.mm). Header-only.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mediaviewer/mediaviewer_ai.h"
#include "phone/link.h"

namespace mv::abi {

inline mv::phone::catalogue_source ai_catalogue_source(const mv_ai_api* api) {
  const auto has = [api](std::size_t end) { return api && api->struct_size >= end; };
  if (!has(offsetof(mv_ai_api, phone_json) + sizeof(api->phone_json)) || !api->phone_publish || !api->phone_cancel ||
      !api->phone_json) {
    return {};
  }
  mv::phone::catalogue_source s;
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

}  // namespace mv::abi
