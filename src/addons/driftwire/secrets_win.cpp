// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// secrets.h over Credential Manager: generic credentials, per user, named
// "MediaViewer Driftwire/<name>", persisted on this machine only
// (CRED_PERSIST_LOCAL_MACHINE: not roamed). The blob is the value.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <string>

#include "addons/driftwire/secrets.h"

namespace mv::driftwire::secrets {

namespace {

std::wstring target_of(const std::string& name) {
  const std::string full = "MediaViewer Driftwire/" + name;
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, full.data(), static_cast<int>(full.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, full.data(), static_cast<int>(full.size()), w.data(), n);
  return w;
}

class credentials final : public store {
 public:
  bool put(const std::string& name, std::span<const std::uint8_t> value) override {
    if (value.size() > kMaxValue || value.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
    std::wstring target = target_of(name);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.data();
    c.CredentialBlobSize = static_cast<DWORD>(value.size());
    c.CredentialBlob = const_cast<LPBYTE>(reinterpret_cast<const BYTE*>(value.data()));
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return ::CredWriteW(&c, 0) != FALSE;
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    const std::wstring target = target_of(name);
    PCREDENTIALW c = nullptr;
    if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &c) || !c) return std::nullopt;
    std::optional<std::vector<std::uint8_t>> out;
    if (c->CredentialBlobSize <= kMaxValue) {
      out = std::vector<std::uint8_t>(c->CredentialBlob, c->CredentialBlob + c->CredentialBlobSize);
    }
    ::SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
    ::CredFree(c);
    return out;
  }
  bool erase(const std::string& name) override {
    const std::wstring target = target_of(name);
    return ::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
  }
};

}  // namespace

std::unique_ptr<store> os_store() { return std::make_unique<credentials>(); }

}  // namespace mv::driftwire::secrets
