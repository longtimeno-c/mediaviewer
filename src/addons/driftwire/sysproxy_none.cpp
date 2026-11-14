// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The system proxy where Driftwire does not set one (Linux, for now: GNOME,
// KDE and the environment each keep their own; sysproxy.h). The Tunnel still
// runs; the window says to point apps at its address.
#include "addons/driftwire/sysproxy.h"

namespace mv::driftwire::sysproxy {

namespace {

class none final : public port {
 public:
  std::string unsupported() const override {
    return "This computer's proxy setting is not changed here: set the address below in your apps.";
  }
  result<std::string> set(const std::string&) override { return err(status::io); }
  bool restore(const std::string&, const std::string&) override { return true; }
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<none>(); }

}  // namespace mv::driftwire::sysproxy
