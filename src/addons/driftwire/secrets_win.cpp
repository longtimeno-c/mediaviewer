// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// secrets.h over Credential Manager: generic credentials, per user, named
// "MediaViewer Driftwire/<name>", persisted on this machine only
// (CRED_PERSIST_LOCAL_MACHINE: not roamed).
//
// #501: any process of the same user can read or write these with CredRead /
// CredEnumerate / CredWrite, with no prompt. So the blob is the value sealed
// with DPAPI (CryptProtectData, this user, CRYPTPROTECT_UI_FORBIDDEN) under
// Driftwire's own entropy: a value is no longer readable by enumerating the
// store, and an item another program wrote in plain (or one from before #501)
// does not open and is refused, not believed. It is not a boundary against a
// program of the same user that does the same (the entropy is in the source).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>
#include <wincrypt.h>  // CryptProtectData (dpapi.h in newer SDKs, included from here)

#include <string>

#include "addons/driftwire/secrets.h"

namespace mv::driftwire::secrets {

namespace {

constexpr char kEntropy[] = "MediaViewer Driftwire secrets v1";

DATA_BLOB entropy_blob() {
  DATA_BLOB e{};
  e.cbData = static_cast<DWORD>(sizeof kEntropy - 1);
  e.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
  return e;
}

// DPAPI's output, freed (and wiped) by LocalFree's owner.
struct local_blob {
  DATA_BLOB b{};
  ~local_blob() {
    if (b.pbData) {
      ::SecureZeroMemory(b.pbData, b.cbData);
      ::LocalFree(b.pbData);
    }
  }
};

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
    if (value.size() > kMaxValue) return false;
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(value.size());
    in.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(value.data()));
    DATA_BLOB e = entropy_blob();
    local_blob sealed;
    if (!::CryptProtectData(&in, L"MediaViewer Driftwire", &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &sealed.b)) {
      return false;
    }
    if (sealed.b.cbData > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
    std::wstring target = target_of(name);
    CREDENTIALW c{};
    c.Type = CRED_TYPE_GENERIC;
    c.TargetName = target.data();
    c.CredentialBlobSize = sealed.b.cbData;
    c.CredentialBlob = sealed.b.pbData;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return ::CredWriteW(&c, 0) != FALSE;
  }
  std::optional<std::vector<std::uint8_t>> get(const std::string& name) override {
    const std::wstring target = target_of(name);
    PCREDENTIALW c = nullptr;
    if (!::CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &c) || !c) return std::nullopt;
    std::optional<std::vector<std::uint8_t>> out;
    DATA_BLOB in{};
    in.cbData = c->CredentialBlobSize;
    in.pbData = c->CredentialBlob;
    DATA_BLOB e = entropy_blob();
    local_blob plain;
    // Not sealed by this user under Driftwire's entropy: refused.
    if (::CryptUnprotectData(&in, nullptr, &e, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain.b) &&
        plain.b.cbData <= kMaxValue) {
      out = std::vector<std::uint8_t>(plain.b.pbData, plain.b.pbData + plain.b.cbData);
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
