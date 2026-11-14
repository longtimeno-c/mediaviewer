// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// drives::port on Linux: mdadm, later (docs/plans/self-host-drives.md §8). Until
// then it lists nothing and combines nothing, and says so; Separate (each drive
// shared as it is) needs no port and works through `host share`.
#include "addons/driftwire/cli/drives.h"

namespace mv::driftwire::cli::drives {

namespace {

class linux_port final : public port {
 public:
  listing list() override {
    listing l;
    l.pool_ok = false;
    l.error = "combining drives on Linux (mdadm) is not built yet; share each drive with `driftwire host share`";
    return l;
  }
  run_result create(const plan&, bool) override { return {false, "combining drives on Linux (mdadm) is not built yet", {}}; }
  run_result replace(const set_health&, const disk&, bool) override {
    return {false, "combining drives on Linux (mdadm) is not built yet", {}};
  }
  void notify(const std::string&, const std::string&) override {}
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<linux_port>(); }

}  // namespace mv::driftwire::cli::drives
