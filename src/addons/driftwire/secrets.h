// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The OS's secret store, behind a port (issue #472 PR 4, D9): this machine's
// identity key and what it remembers of each known device (its key and the
// pairwise secret) never sit in a plain file.
//
//   secrets_mac.cpp  the data protection keychain, in the app's own access
//                    group (Security.framework, generic passwords under the
//                    service "MediaViewer Driftwire"; #501). None without
//                    the access-group entitlement.
//   secrets_win.cpp  Credential Manager (CredWrite, per user, generic
//                    credentials "MediaViewer Driftwire/<name>"), each
//                    value sealed with DPAPI (#501): any process of the
//                    same user can read or plant the items, but one not
//                    sealed by Driftwire is refused.
//   secrets_file.cpp a file per name in a folder, for the tests and the
//                    portable build (it says so; it is not a secret store)
//
// Values are small (at most kMaxValue bytes: Credential Manager's limit, less DPAPI).
// Every call is on a worker; none is on the UI or render thread.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mv::driftwire::secrets {

// Credential Manager holds 2,560 bytes, and on Windows the value goes in
// sealed by DPAPI, which adds ~300 (its header, salt, two SHA-512 HMACs and
// the AES padding). The devices value is at most 2,082.
inline constexpr std::size_t kMaxValue = 2144;

class store {
 public:
  virtual ~store() = default;
  [[nodiscard]] virtual bool put(const std::string& name, std::span<const std::uint8_t> value) = 0;
  [[nodiscard]] virtual std::optional<std::vector<std::uint8_t>> get(const std::string& name) = 0;
  virtual bool erase(const std::string& name) = 0;
};

// The platform's store; nullptr where there is none.
[[nodiscard]] std::unique_ptr<store> os_store();
// A folder of files (the tests; the portable build).
[[nodiscard]] std::unique_ptr<store> file_store(const std::string& folder);
#if defined(__APPLE__)
// os_store() under another service name (the tests). Removes that service's
// items from the login keychain first; nullptr without the entitlement.
[[nodiscard]] std::unique_ptr<store> keychain_store(const std::string& service);
#endif

}  // namespace mv::driftwire::secrets
