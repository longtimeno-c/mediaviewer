// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// power.h on Windows: a power request with a reason, as a media app or a
// download keeps the machine awake (powercfg /requests lists it).
#include "addons/driftwire/power.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace mv::driftwire::power {

namespace {

class request final : public stay_awake {
 public:
  explicit request(HANDLE h) : h_(h) {}
  ~request() override {
    (void)::PowerClearRequest(h_, PowerRequestSystemRequired);
    ::CloseHandle(h_);
  }

 private:
  HANDLE h_;
};

}  // namespace

std::unique_ptr<stay_awake> hold(bool receiving) noexcept {
  try {
    return hold_for(reason(receiving));
  } catch (...) {
    return nullptr;
  }
}

std::unique_ptr<stay_awake> hold_for(const std::string& reason_ascii) noexcept {
  std::wstring text;
  try {
    // ASCII: the owner and the host's reason are literals of ours.
    for (const char c : reason_ascii) text.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  } catch (...) {
    return nullptr;
  }
  REASON_CONTEXT why{};
  why.Version = POWER_REQUEST_CONTEXT_VERSION;
  why.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
  why.Reason.SimpleReasonString = text.data();
  HANDLE h = ::PowerCreateRequest(&why);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  if (!::PowerSetRequest(h, PowerRequestSystemRequired)) {
    ::CloseHandle(h);
    return nullptr;
  }
  try {
    return std::make_unique<request>(h);
  } catch (...) {
    (void)::PowerClearRequest(h, PowerRequestSystemRequired);
    ::CloseHandle(h);
    return nullptr;
  }
}

}  // namespace mv::driftwire::power
