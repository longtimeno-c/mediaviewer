// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// net::machine_name on macOS: the Sharing name ("Tristan's MacBook Air"), the
// one AirDrop shows. SCDynamicStoreCopyComputerName reads a local store and
// never blocks on a name lookup (NSHost can).
#include <CoreFoundation/CoreFoundation.h>
#include <SystemConfiguration/SystemConfiguration.h>

#include <string>

#include "addons/driftwire/net.h"
#include "addons/driftwire/wire.h"

namespace mv::driftwire::net {

std::string machine_name() {
  std::string out;
  if (CFStringRef name = SCDynamicStoreCopyComputerName(nullptr, nullptr)) {
    char buf[512] = {};
    if (CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8)) out = buf;
    CFRelease(name);
  }
  return clean_name(out);
}

}  // namespace mv::driftwire::net
