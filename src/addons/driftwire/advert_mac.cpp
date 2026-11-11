// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include <dns_sd.h>

#include <arpa/inet.h>

#include "addons/driftwire/advert.h"

namespace mv::driftwire::advert {

namespace {

class dns_sd_registration final : public registration {
 public:
  explicit dns_sd_registration(DNSServiceRef ref) : ref_(ref) {}
  ~dns_sd_registration() override { DNSServiceRefDeallocate(ref_); }

 private:
  DNSServiceRef ref_;
};

}  // namespace

std::unique_ptr<registration> publish(std::uint16_t port, const std::vector<std::pair<std::string, std::string>>& txt) {
  std::string record;
  for (const auto& [k, v] : txt) {
    const std::string item = k + "=" + v;
    if (item.size() > 255) return nullptr;
    record.push_back(static_cast<char>(item.size()));
    record += item;
  }
  DNSServiceRef ref = nullptr;
  // No name of ours: a phone needs no machine name, and the daemon picks a
  // free one. No callback: the registration holds with nothing to process.
  const DNSServiceErrorType e =
      DNSServiceRegister(&ref, 0, 0, "MediaViewer", kServiceType, nullptr, nullptr, htons(port),
                         static_cast<std::uint16_t>(record.size()), record.data(), nullptr, nullptr);
  if (e != kDNSServiceErr_NoError || !ref) return nullptr;
  return std::make_unique<dns_sd_registration>(ref);
}

}  // namespace mv::driftwire::advert
