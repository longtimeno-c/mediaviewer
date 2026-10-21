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
  REASON_CONTEXT why{};
  why.Version = POWER_REQUEST_CONTEXT_VERSION;
  why.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
  wchar_t receiving_text[] = L"MediaViewer is receiving files (Driftwire)";
  wchar_t sending_text[] = L"MediaViewer is sending files (Driftwire)";
  why.Reason.SimpleReasonString = receiving ? receiving_text : sending_text;
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
