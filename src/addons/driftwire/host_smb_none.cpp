// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// No LAN share here yet (Linux: Samba comes with the Linux host,
// docs/plans/self-host-users-smb.md §5). `host users` still keeps the list.
#include "addons/driftwire/host_smb.h"

namespace mv::driftwire::host_smb {

namespace {

class none_port final : public port {
 public:
  facts probe() override {
    facts f;
    f.supported = false;
    f.why_not = "a network share is not offered on this computer yet (Samba comes with the Linux version)";
    return f;
  }
  owner account(const std::string&) override { return owner::none; }
  std::string make_account(const std::string&, std::string_view) override { return "not offered here"; }
  std::string set_password(const std::string&, std::string_view) override { return "not offered here"; }
  std::string delete_account(const std::string&) override { return "not offered here"; }
  owner share(const std::string&) override { return owner::none; }
  std::string put_share(const share_want&, const std::vector<member>&, const std::vector<std::string>&) override {
    return "not offered here";
  }
  std::string remove_share(const std::string&, const std::vector<std::string>&) override { return {}; }
  std::vector<std::string> our_shares() override { return {}; }
  std::vector<std::string> our_accounts() override { return {}; }
  std::string start_service() override { return "not offered here"; }
};

}  // namespace

std::unique_ptr<port> os_port() { return std::make_unique<none_port>(); }

void note_invoker() {}

std::string check_share_folder(const std::string& path) {
  if (std::string why = refuse_system_folder(path, false); !why.empty()) return why;
  return "a network share is not offered on this computer yet";
}

}  // namespace mv::driftwire::host_smb
