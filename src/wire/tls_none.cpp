// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A build without BoringSSL (tls.h): no HTTPS look; the Tunnel and the relay
// keep their plain port.
#include "wire/tls.h"

namespace mv::driftwire::tls {

bool available() noexcept { return false; }

result<std::unique_ptr<link>> client(std::unique_ptr<link> l, const std::string&, const knock_key*, int) {
  if (l) l->shutdown();
  return err(status::unsupported_format);
}

result<std::unique_ptr<server_context>> make_server_context(const std::string&, const std::string&,
                                                            const std::string&) {
  return err(status::unsupported_format);
}

std::unique_ptr<link> accept(server_context&, std::unique_ptr<link> l, const knock_key&, const decoy&,
                             accepted_as& as, int) {
  as = accepted_as::failed;
  if (l) l->shutdown();
  return nullptr;
}

}  // namespace mv::driftwire::tls
