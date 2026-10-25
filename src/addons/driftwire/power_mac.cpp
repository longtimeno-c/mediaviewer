// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// power.h on the Mac: an IOKit assertion against idle system sleep, as
// caffeinate -i takes (pmset -g assertions lists it).
#include "addons/driftwire/power.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

namespace mv::driftwire::power {

namespace {

class assertion final : public stay_awake {
 public:
  explicit assertion(IOPMAssertionID id) : id_(id) {}
  ~assertion() override { (void)IOPMAssertionRelease(id_); }

 private:
  IOPMAssertionID id_;
};

}  // namespace

std::unique_ptr<stay_awake> hold(bool receiving) noexcept {
  CFStringRef why = nullptr;
  try {
    why = CFStringCreateWithCString(kCFAllocatorDefault, reason(receiving).c_str(), kCFStringEncodingUTF8);
  } catch (...) {
    return nullptr;
  }
  if (!why) return nullptr;
  IOPMAssertionID id = kIOPMNullAssertionID;
  const IOReturn made =
      IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleSystemSleep, kIOPMAssertionLevelOn, why, &id);
  CFRelease(why);  // the assertion keeps its own copy
  if (made != kIOReturnSuccess) return nullptr;
  try {
    return std::make_unique<assertion>(id);
  } catch (...) {
    (void)IOPMAssertionRelease(id);
    return nullptr;
  }
}

}  // namespace mv::driftwire::power
