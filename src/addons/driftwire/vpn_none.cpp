// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Where Driftwire has no virtual interface yet (Linux, until its host reaches
// Driftwire: /dev/net/tun with CAP_NET_ADMIN, docs/plans/linux.md): the proxy
// only (vpn.h).
#include "addons/driftwire/vpn.h"

namespace mv::driftwire::vpn {

namespace {

class none final : public os_port {
 public:
  std::string unavailable() override { return "All traffic (VPN) is not on this computer yet: use the proxy."; }
  result<std::unique_ptr<device>> start(const settings&) override { return err(status::unsupported_format); }
  void stop() override {}
};

}  // namespace

std::unique_ptr<os_port> make_os_port() { return std::make_unique<none>(); }

}  // namespace mv::driftwire::vpn
