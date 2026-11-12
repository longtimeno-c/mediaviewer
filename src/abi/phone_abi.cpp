// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// 0.40: Settings -> Phone access (mediaviewer.h, phone/app.h).
#include <mediaviewer/mediaviewer.h>

#include <cstring>
#include <string>

#include "abi/guard.h"
#include "phone/app.h"

namespace {

using mv::status;

status write_out(const std::string& text, char* out, uint32_t cap, uint32_t* needed) {
  const auto need = static_cast<uint32_t>(text.size() + 1);
  if (needed) *needed = need;
  if (!out || cap < need) return status::invalid_arg;
  std::memcpy(out, text.c_str(), need);
  return status::ok;
}

}  // namespace

extern "C" {

MV_API mv_status MV_CALL mv_phone_request(const char* request_json) {
  return static_cast<mv_status>(mv::abi::guard("mv_phone_request", [&]() -> status {
    MV_REQUIRE(request_json, "request_json is required");
    const auto r = mv::phone::app_request(request_json);
    return r ? status::ok : r.error();
  }));
}

MV_API mv_status MV_CALL mv_phone_state_json(char* utf8, uint32_t cap, uint32_t* out_bytes) {
  return static_cast<mv_status>(mv::abi::guard("mv_phone_state_json", [&]() -> status {
    return write_out(mv::phone::app_state_json(), utf8, cap, out_bytes);
  }));
}

MV_API mv_status MV_CALL mv_phone_resume(uint32_t flags) {
  return static_cast<mv_status>(mv::abi::guard("mv_phone_resume", [&]() -> status {
    mv::phone::resume_app((flags & MV_PHONE_SECONDARY) == 0);
    return status::ok;
  }));
}

MV_API mv_status MV_CALL mv_phone_stop(void) {
  return static_cast<mv_status>(mv::abi::guard("mv_phone_stop", [&]() -> status {
    mv::phone::stop_app();
    return status::ok;
  }));
}

}  // extern "C"
